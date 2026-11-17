// ============================================================================
// Rádio ESP-NOW do ARES. ESTE ARQUIVO É IGUAL NOS TRÊS FIRMWARES.
// Veja a explicação geral em include/esp_now_radio.h.
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "../../include/ares_protocol.h"
#include "../../include/ares_config.h"
#include "../../include/esp_now_radio.h"

static const uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static bool radioDebugEnabled = false;
static QueueHandle_t rxQueue = nullptr;       // Pacotes recebidos esperando o loop()
static QueueHandle_t resultQueue = nullptr;   // Resultados de envio esperando o loop()

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
// portMUX = trava que funciona entre o loop() e o callback (que roda em outra tarefa/núcleo)
static portMUX_TYPE pendingLock = portMUX_INITIALIZER_UNLOCKED;

// Peers que nós adicionamos, com a última vez que foram usados. O ESP-NOW tem um
// limite de peers (~20 sem criptografia); quando enche, tiramos o usado há mais tempo.
#define TRACKED_PEERS 16
typedef struct {
    bool used;
    uint8_t mac[6];
    uint32_t lastUsedMs;
} TrackedPeer;
static TrackedPeer trackedPeers[TRACKED_PEERS];

void setRadioDebug(bool enabled) {
    radioDebugEnabled = enabled;
}

bool isBroadcastMac(const uint8_t *mac) {
    return memcmp(mac, broadcastMac, 6) == 0;
}

#if ESP_ARDUINO_VERSION_MAJOR < 3
// No núcleo 2.x (o do PlatformIO) o callback de recepção não traz o RSSI. O modo promíscuo
// vê o mesmo quadro antes e anota o sinal de quem transmitiu; o callback do ESP-NOW consulta aqui.
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
// Só copia o pacote para a fila e sai (nada de Serial, HTTP ou flash aqui).
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
        droppedFrames = droppedFrames + 1;   // Fila cheia: o loop() está atrasado
    }
}

// A assinatura dos callbacks mudou no núcleo 3.x (Arduino IDE atual);
// o PlatformIO (platform = espressif32) usa o núcleo 2.x. Os #if escolhem a certa.
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

static TrackedPeer *findTracked(const uint8_t *mac) {
    for (int i = 0; i < TRACKED_PEERS; i++) {
        if (trackedPeers[i].used && memcmp(trackedPeers[i].mac, mac, 6) == 0) return &trackedPeers[i];
    }
    return nullptr;
}

// Tira o peer usado há mais tempo (nunca o broadcast) para abrir espaço
static void evictOldestPeer() {
    TrackedPeer *oldest = nullptr;
    for (int i = 0; i < TRACKED_PEERS; i++) {
        TrackedPeer *p = &trackedPeers[i];
        if (!p->used || isBroadcastMac(p->mac)) continue;
        if (oldest == nullptr || (int32_t) (p->lastUsedMs - oldest->lastUsedMs) < 0) oldest = p;
    }
    if (oldest == nullptr) return;
    if (esp_now_is_peer_exist(oldest->mac)) esp_now_del_peer(oldest->mac);
    oldest->used = false;
}

bool addRadioPeer(const uint8_t *mac) {
    TrackedPeer *tracked = findTracked(mac);
    if (tracked != nullptr) tracked->lastUsedMs = millis();
    if (esp_now_is_peer_exist(mac)) return true;

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, mac, 6);
    // Canal 0 = "o canal em que o rádio está agora". Assim nunca aparece o erro
    // "Peer channel is not equal to the home channel" (visto nos primeiros testes).
    peerInfo.channel = 0;
    peerInfo.ifidx = WIFI_IF_STA;
    peerInfo.encrypt = false;

    esp_err_t result = esp_now_add_peer(&peerInfo);
    if (result == ESP_ERR_ESPNOW_FULL) {
        evictOldestPeer();
        result = esp_now_add_peer(&peerInfo);
    }
    if (result != ESP_OK) return false;

    // Anota na nossa lista (se a lista estiver cheia, o mais antigo dela sai)
    if (tracked == nullptr) {
        for (int i = 0; i < TRACKED_PEERS && tracked == nullptr; i++) {
            if (!trackedPeers[i].used) tracked = &trackedPeers[i];
        }
        if (tracked == nullptr) {
            evictOldestPeer();
            for (int i = 0; i < TRACKED_PEERS && tracked == nullptr; i++) {
                if (!trackedPeers[i].used) tracked = &trackedPeers[i];
            }
        }
        if (tracked != nullptr) {
            tracked->used = true;
            memcpy(tracked->mac, mac, 6);
        }
    }
    if (tracked != nullptr) tracked->lastUsedMs = millis();
    return true;
}

void removeRadioPeer(const uint8_t *mac) {
    if (isBroadcastMac(mac)) return;
    if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);
    TrackedPeer *tracked = findTracked(mac);
    if (tracked != nullptr) tracked->used = false;
}

bool initEspNowRadio(uint8_t fixedChannel) {
    rxQueue = xQueueCreate(RX_QUEUE_LEN, sizeof(AresReceivedFrame));
    resultQueue = xQueueCreate(PENDING_SENDS, sizeof(AresSendResult));
    if (rxQueue == nullptr || resultQueue == nullptr) {
        if (radioDebugEnabled) Serial.println("[ERRO] Falha ao criar as filas do rádio.");
        return false;
    }

    if (fixedChannel != 0) {
        // Storage e Router: só ESP-NOW, sem conectar em Wi-Fi. Trava o canal.
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        esp_wifi_set_channel(fixedChannel, WIFI_SECOND_CHAN_NONE);
#if ESP_ARDUINO_VERSION_MAJOR < 3
        // RSSI pelo modo promíscuo (veja onPromiscuousPacket). Só faz sentido sem Wi-Fi conectado.
        wifi_promiscuous_filter_t filter = {};
        filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
        esp_wifi_set_promiscuous_filter(&filter);
        esp_wifi_set_promiscuous_rx_cb(onPromiscuousPacket);
        esp_wifi_set_promiscuous(true);
#endif
    }

#if RADIO_LOW_TX_POWER
    // Potência baixa (8,5 dBm) para bancada: menos consumo de pico na USB do notebook,
    // o que evitou o "brownout" (reinício por queda de tensão) visto em uma das placas.
    esp_wifi_set_max_tx_power(34);   // Unidade: 0,25 dBm -> 34 = 8,5 dBm
#endif

    if (esp_now_init() != ESP_OK) {
        if (radioDebugEnabled) Serial.println("[ERRO] Falha ao inicializar o ESP-NOW.");
        return false;
    }

    esp_now_register_recv_cb(onDataRecv);
    esp_now_register_send_cb(onDataSent);

    if (!addRadioPeer(broadcastMac)) {
        if (radioDebugEnabled) Serial.println("[ERRO] Falha ao adicionar o peer de broadcast.");
        return false;
    }

    if (radioDebugEnabled) {
        uint8_t channel = 0;
        wifi_second_chan_t second;
        esp_wifi_get_channel(&channel, &second);
        Serial.printf("[ESP-NOW] Rádio inicializado no canal %u.\n", channel);
    }
    return true;
}

// Envio unicast: o rádio do destino responde automaticamente e o callback de envio
// informa se a confirmação chegou (é a confirmação do RÁDIO, não a custódia do ARES).
uint32_t sendFrame(const uint8_t *mac, const void *data, size_t len, AresSendKind kind) {
    if (!addRadioPeer(mac)) {
        if (radioDebugEnabled) Serial.println("[ERRO] Não consegui adicionar o destino como peer do ESP-NOW.");
        return 0;
    }

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
            Serial.printf("[ERRO] Falha ao enviar pelo rádio. Código de erro: 0x%X\n", (unsigned) result);
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
