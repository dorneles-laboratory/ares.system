#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "../../include/ares_protocol.h"
#include "../../include/ares_config.h"
#include "../../include/esp_now_router.h"

uint8_t nextHopAddress[] = GATEWAY_MAC;
static bool radioDebugEnabled = false;
static QueueHandle_t rxQueue = nullptr;

// Alterados dentro dos callbacks (tarefa do Wi-Fi) e lidos no loop()
static volatile AresSendStatus sendStatus = SEND_IDLE;
static volatile uint32_t droppedFrames = 0;   // Fila de recepção cheia
static volatile uint32_t ignoredFrames = 0;   // Tamanho diferente de AresMessagePacket

void setRadioDebug(bool enabled) {
    radioDebugEnabled = enabled;
}

// Callback de recepção: roda na tarefa do Wi-Fi, não no loop().
// Só copia o pacote para a fila e sai (lição do incidente da POC: nada de Serial ou HTTP aqui).
static void queueIncomingFrame(const uint8_t *mac, const uint8_t *data, int len) {
    // Mesma validação do ESP2: só aceita pacotes do tamanho exato do protocolo ARES
    if (len != sizeof(AresMessagePacket)) {
        ignoredFrames = ignoredFrames + 1;
        return;
    }
    AresReceivedFrame frame;
    memcpy(frame.senderMac, mac, 6);
    memcpy(&frame.packet, data, sizeof(AresMessagePacket));
    if (xQueueSend(rxQueue, &frame, 0) != pdTRUE) {
        droppedFrames = droppedFrames + 1;
    }
}

// A assinatura dos callbacks mudou no núcleo 3.x; o PlatformIO (espressif32) usa o 2.x
#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    queueIncomingFrame(info->src_addr, data, len);
}
#else
static void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
    queueIncomingFrame(mac, data, len);
}
#endif

#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 0)
static void onDataSent(const esp_now_send_info_t *info, esp_now_send_status_t status) {
#else
static void onDataSent(const uint8_t *mac, esp_now_send_status_t status) {
#endif
    sendStatus = (status == ESP_NOW_SEND_SUCCESS) ? SEND_CONFIRMED : SEND_FAILED;
}

bool initEspNowRouter() {
    rxQueue = xQueueCreate(RX_QUEUE_LEN, sizeof(AresReceivedFrame));
    if (rxQueue == nullptr) {
        if (radioDebugEnabled) Serial.println("[ERRO] Falha ao criar a fila de recepção.");
        return false;
    }

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    // Força o rádio do Router a operar no mesmo canal do ESP1 e do Gateway/Hotspot
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        if (radioDebugEnabled) Serial.println("[ERRO] Falha ao inicializar o ESP-NOW.");
        return false;
    }

    esp_now_register_recv_cb(onDataRecv);
    esp_now_register_send_cb(onDataSent);

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, nextHopAddress, 6);
    peerInfo.channel = ESPNOW_CHANNEL;
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        if (radioDebugEnabled) Serial.println("[ERRO] Falha ao adicionar o peer do próximo salto.");
        return false;
    }

    if (radioDebugEnabled) {
        Serial.print("[ESP-NOW] Router inicializado. Próximo salto: ");
        for (int i = 0; i < 6; i++) {
            Serial.printf("%02X", nextHopAddress[i]);
            if (i < 5) Serial.print(":");
        }
        Serial.printf(" | Canal: %d\n", ESPNOW_CHANNEL);

        // Broadcast nunca falha no callback de envio, então toda mensagem pareceria entregue
        static const uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        if (memcmp(nextHopAddress, broadcastMac, 6) == 0) {
            Serial.println("[AVISO] GATEWAY_MAC é broadcast (FF:FF:...). As confirmações não valem nada: coloque o MAC do ESP2 no ares_credentials.h.");
        }
    }
    return true;
}

// Envio unicast: o rádio do próximo salto responde automaticamente e o callback
// de envio informa se a confirmação chegou (é a confirmação do rádio, não um ACK do ARES).
bool sendToNextHop(const AresMessagePacket *packet) {
    sendStatus = SEND_PENDING;
    esp_err_t result = esp_now_send(nextHopAddress, (const uint8_t *) packet, sizeof(AresMessagePacket));
    if (result != ESP_OK) {
        sendStatus = SEND_FAILED;
        if (radioDebugEnabled) {
            Serial.print("[ERRO] Falha ao enviar a mensagem. Código de erro: ");
            Serial.println(result);
        }
        return false;
    }
    return true;
}

AresSendStatus getSendStatus() {
    return sendStatus;
}

void clearSendStatus() {
    sendStatus = SEND_IDLE;
}

bool receiveFrame(AresReceivedFrame *frame) {
    return xQueueReceive(rxQueue, frame, 0) == pdTRUE;
}

uint32_t getDroppedFrames() {
    return droppedFrames;
}

uint32_t getIgnoredFrames() {
    return ignoredFrames;
}
