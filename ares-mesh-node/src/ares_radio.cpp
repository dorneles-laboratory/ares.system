#include "ares_radio.h"

AresRadio* AresRadio::instance = nullptr;

AresRadio::AresRadio() : packetCallback(nullptr), lastRssi(-60), seenIndex(0) {
    memset(seenMsgIds, 0, sizeof(seenMsgIds));
}

AresRadio& AresRadio::getInstance() {
    if (!instance) {
        instance = new AresRadio();
    }
    return *instance;
}

bool AresRadio::begin(uint8_t channel) {
    // Inicializa ESP-NOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("[ARES RADIO] Erro ao inicializar ESP-NOW!");
        return false;
    }

    esp_now_register_send_cb(AresRadio::onDataSent);
    esp_now_register_recv_cb(AresRadio::onDataRecv);

    // Adiciona peer de broadcast padrão
    esp_now_peer_info_t peerInfo = {};
    uint8_t broadcastMac[] = BROADCAST_MAC;
    memcpy(peerInfo.peer_addr, broadcastMac, 6);
    peerInfo.channel = channel;
    peerInfo.encrypt = false;

    if (!esp_now_is_peer_exist(broadcastMac)) {
        if (esp_now_add_peer(&peerInfo) != ESP_OK) {
            Serial.println("[ARES RADIO] Falha ao adicionar peer de broadcast!");
            return false;
        }
    }

    Serial.printf("[ARES RADIO] ESP-NOW ativo no canal %d. MAC Local: %s\n", channel, WiFi.macAddress().c_str());
    return true;
}

bool AresRadio::sendBroadcast(const AresMessagePacket *packet) {
    uint8_t broadcastMac[] = BROADCAST_MAC;
    return sendToMac(broadcastMac, packet);
}

bool AresRadio::sendToMac(const uint8_t *targetMac, const AresMessagePacket *packet) {
    esp_err_t result = esp_now_send(targetMac, (uint8_t*)packet, sizeof(AresMessagePacket));
    if (result == ESP_OK) {
        return true;
    } else {
        Serial.printf("[ARES RADIO] Erro no envio ESP-NOW: %d\n", result);
        return false;
    }
}

void AresRadio::setPacketCallback(AresPacketReceivedCallback callback) {
    packetCallback = callback;
}

bool AresRadio::hasSeenMessage(const char *msgId) {
    if (!msgId || strlen(msgId) == 0) return false;
    for (int i = 0; i < 32; i++) {
        if (strncmp(seenMsgIds[i], msgId, 32) == 0) {
            return true;
        }
    }
    return false;
}

void AresRadio::markMessageSeen(const char *msgId) {
    if (!msgId || strlen(msgId) == 0) return;
    strncpy(seenMsgIds[seenIndex], msgId, 32);
    seenIndex = (seenIndex + 1) % 32;
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
void AresRadio::onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
    // Status callback em ESP-IDF 5+
}

void AresRadio::onDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int data_len) {
    if (data_len < (int)sizeof(AresMessagePacket)) {
        return;
    }

    const AresMessagePacket *packet = (const AresMessagePacket*)data;
    if (instance && instance->packetCallback && recv_info) {
        int rssi = recv_info->rx_ctrl ? recv_info->rx_ctrl->rssi : -60;
        instance->lastRssi = rssi;
        instance->packetCallback(recv_info->src_addr, packet, rssi);
    }
}
#else
void AresRadio::onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    // Status callback em ESP-IDF 4
}

void AresRadio::onDataRecv(const uint8_t *mac_addr, const uint8_t *data, int data_len) {
    if (data_len < (int)sizeof(AresMessagePacket)) {
        return;
    }

    const AresMessagePacket *packet = (const AresMessagePacket*)data;
    if (instance && instance->packetCallback) {
        instance->packetCallback(mac_addr, packet, instance->lastRssi);
    }
}
#endif
