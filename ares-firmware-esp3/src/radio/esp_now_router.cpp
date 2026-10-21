#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "../../include/ares_protocol.h"
#include "../../include/ares_config.h"
#include "../../include/esp_now_router.h"

static const uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static bool radioDebugEnabled = false;
static QueueHandle_t rxQueue = nullptr;
static QueueHandle_t resultQueue = nullptr;

// Alterados dentro dos callbacks (tarefa do Wi-Fi) e lidos no loop()
static volatile uint32_t droppedFrames = 0;   // Fila de recepção cheia
static volatile uint32_t ignoredFrames = 0;   // Tamanho que não é de nenhum pacote ARES

// Envios aguardando o callback, na ordem em que saíram. O ESP-NOW chama o callback
// de envio na mesma ordem, então o primeiro da fila é sempre o envio confirmado agora.
#define PENDING_SENDS 16
typedef struct {
    uint32_t sendId;
    AresSendKind kind;
    uint8_t destMac[6];
} PendingSend;
static PendingSend pendingSends[PENDING_SENDS];
static int pendingHead = 0;
static int pendingCount = 0;
static uint32_t nextSendId = 1;
static portMUX_TYPE pendingLock = portMUX_INITIALIZER_UNLOCKED;

void setRadioDebug(bool enabled) {
    radioDebugEnabled = enabled;
}

#if ESP_ARDUINO_VERSION_MAJOR < 3
// No núcleo 2.x o callback de recepção não traz o RSSI. O modo promíscuo vê o mesmo
// quadro antes e anota o sinal de quem transmitiu; o callback do ESP-NOW consulta aqui.
#define RSSI_CACHE_SIZE 8
typedef struct {
    uint8_t mac[6];
    int8_t rssi;
} RssiEntry;
static RssiEntry rssiCache[RSSI_CACHE_SIZE];
static int rssiNext = 0;

static void onPromiscuousPacket(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *) buf;
    const uint8_t *senderMac = pkt->payload + 10;   // Endereço 2 do cabeçalho 802.11
    for (int i = 0; i < RSSI_CACHE_SIZE; i++) {
        if (memcmp(rssiCache[i].mac, senderMac, 6) == 0) {
            rssiCache[i].rssi = pkt->rx_ctrl.rssi;
            return;
        }
    }
    memcpy(rssiCache[rssiNext].mac, senderMac, 6);
    rssiCache[rssiNext].rssi = pkt->rx_ctrl.rssi;
    rssiNext = (rssiNext + 1) % RSSI_CACHE_SIZE;
}

static int8_t lookupRssi(const uint8_t *mac) {
    for (int i = 0; i < RSSI_CACHE_SIZE; i++) {
        if (memcmp(rssiCache[i].mac, mac, 6) == 0) return rssiCache[i].rssi;
    }
    return 0;
}
#endif

// Callback de recepção: roda na tarefa do Wi-Fi, não no loop().
// Só copia o pacote para a fila e sai (lição do incidente da POC: nada de Serial ou HTTP aqui).
static void queueIncomingFrame(const uint8_t *mac, const uint8_t *data, int len, int8_t rssi) {
    if (len <= 0 || len > (int) RX_FRAME_MAX_LEN) {
        ignoredFrames = ignoredFrames + 1;
        return;
    }
    AresReceivedFrame frame;
    memcpy(frame.senderMac, mac, 6);
    frame.rssi = rssi;
    frame.len = (uint8_t) len;
    memcpy(frame.data, data, len);
    if (xQueueSend(rxQueue, &frame, 0) != pdTRUE) {
        droppedFrames = droppedFrames + 1;
    }
}

// A assinatura dos callbacks mudou no núcleo 3.x; o PlatformIO (espressif32) usa o 2.x
#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    queueIncomingFrame(info->src_addr, data, len, info->rx_ctrl ? info->rx_ctrl->rssi : 0);
}
#else
static void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
    queueIncomingFrame(mac, data, len, lookupRssi(mac));
}
#endif

#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 0)
static void onDataSent(const esp_now_send_info_t *info, esp_now_send_status_t status) {
#else
static void onDataSent(const uint8_t *mac, esp_now_send_status_t status) {
#endif
    PendingSend sent = {};
    bool found = false;
    portENTER_CRITICAL(&pendingLock);
    if (pendingCount > 0) {
        sent = pendingSends[pendingHead];
        pendingHead = (pendingHead + 1) % PENDING_SENDS;
        pendingCount--;
        found = true;
    }
    portEXIT_CRITICAL(&pendingLock);

    // Beacons e ACKs não precisam de resposta; broadcast sempre "dá certo" e não diz nada
    if (!found || sent.kind == SEND_KIND_CONTROL) return;

    AresSendResult result;
    result.sendId = sent.sendId;
    result.kind = sent.kind;
    memcpy(result.destMac, sent.destMac, 6);
    result.delivered = (status == ESP_NOW_SEND_SUCCESS);
    xQueueSend(resultQueue, &result, 0);
}

bool addRadioPeer(const uint8_t *mac) {
    if (esp_now_is_peer_exist(mac)) return true;
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, mac, 6);
    peerInfo.channel = ESPNOW_CHANNEL;
    peerInfo.encrypt = false;
    return esp_now_add_peer(&peerInfo) == ESP_OK;
}

void removeRadioPeer(const uint8_t *mac) {
    if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);
}

bool initEspNowRouter(const uint8_t *gatewayMac) {
    rxQueue = xQueueCreate(RX_QUEUE_LEN, sizeof(AresReceivedFrame));
    resultQueue = xQueueCreate(PENDING_SENDS, sizeof(AresSendResult));
    if (rxQueue == nullptr || resultQueue == nullptr) {
        if (radioDebugEnabled) Serial.println("[ERRO] Falha ao criar as filas do rádio.");
        return false;
    }

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    // Força o rádio do Router a operar no mesmo canal do ESP1 e do Gateway/Hotspot
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

#if ESP_ARDUINO_VERSION_MAJOR < 3
    wifi_promiscuous_filter_t filter = {};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(onPromiscuousPacket);
    esp_wifi_set_promiscuous(true);
#endif

    if (esp_now_init() != ESP_OK) {
        if (radioDebugEnabled) Serial.println("[ERRO] Falha ao inicializar o ESP-NOW.");
        return false;
    }

    esp_now_register_recv_cb(onDataRecv);
    esp_now_register_send_cb(onDataSent);

    if (!addRadioPeer(broadcastMac) || !addRadioPeer(gatewayMac)) {
        if (radioDebugEnabled) Serial.println("[ERRO] Falha ao adicionar os peers de broadcast e do Gateway.");
        return false;
    }

    if (radioDebugEnabled) {
        Serial.printf("[ESP-NOW] Router inicializado. Gateway: %02X:%02X:%02X:%02X:%02X:%02X | Canal: %d\n",
                      gatewayMac[0], gatewayMac[1], gatewayMac[2], gatewayMac[3], gatewayMac[4], gatewayMac[5],
                      ESPNOW_CHANNEL);

        // Broadcast nunca falha no callback de envio, então toda mensagem pareceria entregue
        if (memcmp(gatewayMac, broadcastMac, 6) == 0) {
            Serial.println("[AVISO] GATEWAY_MAC é broadcast (FF:FF:...). As confirmações não valem nada: coloque o MAC do ESP2 no ares_credentials.h.");
        }
    }
    return true;
}

// Envio unicast: o rádio do destino responde automaticamente e o callback de envio
// informa se a confirmação chegou (é a confirmação do rádio, não a custódia do ARES).
uint32_t sendFrame(const uint8_t *mac, const void *data, size_t len, AresSendKind kind) {
    uint32_t sendId;
    portENTER_CRITICAL(&pendingLock);
    if (pendingCount >= PENDING_SENDS) {
        portEXIT_CRITICAL(&pendingLock);
        return 0;
    }
    sendId = nextSendId++;
    if (nextSendId == 0) nextSendId = 1;
    int slot = (pendingHead + pendingCount) % PENDING_SENDS;
    pendingSends[slot].sendId = sendId;
    pendingSends[slot].kind = kind;
    memcpy(pendingSends[slot].destMac, mac, 6);
    pendingCount++;
    portEXIT_CRITICAL(&pendingLock);

    esp_err_t result = esp_now_send(mac, (const uint8_t *) data, len);
    if (result != ESP_OK) {
        // Não vai ter callback para este envio: tira da fila (ele é sempre o último)
        portENTER_CRITICAL(&pendingLock);
        if (pendingCount > 0) pendingCount--;
        portEXIT_CRITICAL(&pendingLock);
        if (radioDebugEnabled) {
            Serial.print("[ERRO] Falha ao enviar pelo rádio. Código de erro: ");
            Serial.println(result);
        }
        return 0;
    }
    return sendId;
}

bool broadcastFrame(const void *data, size_t len) {
    return sendFrame(broadcastMac, data, len, SEND_KIND_CONTROL) != 0;
}

bool receiveFrame(AresReceivedFrame *frame) {
    return xQueueReceive(rxQueue, frame, 0) == pdTRUE;
}

bool receiveSendResult(AresSendResult *result) {
    return xQueueReceive(resultQueue, result, 0) == pdTRUE;
}

uint32_t getDroppedFrames() {
    return droppedFrames;
}

uint32_t getIgnoredFrames() {
    return ignoredFrames;
}
