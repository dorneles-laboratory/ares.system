#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include "../../include/ares_protocol.h"
#include "../../include/ares_config.h"

// Ponteiro de função ou callback externo para processar o pacote recebido
static void (*onPacketReceivedCallback)(const uint8_t *mac, const AresMessagePacket *packet, int data_len) = nullptr;

// Callback nativo do ESP-NOW ao receber dados
void onDataRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int len) {
    if (GATEWAY_DEBUG) {
        Serial.printf("[ESP-NOW RX] Pacote recebido de MAC: %02X:%02X:%02X:%02X:%02X:%02X | Tamanho: %d bytes\n",
                      mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5], len);
    }

    // Valida se o tamanho bate com a nossa estrutura de protocolo
    if (len == sizeof(AresMessagePacket)) {
        AresMessagePacket receivedPacket;
        memcpy(&receivedPacket, incomingData, sizeof(AresMessagePacket));

        // Se houver um callback registrado, repassa o pacote
        if (onPacketReceivedCallback != nullptr) {
            onPacketReceivedCallback(mac_addr, &receivedPacket, len);
        }
    } else {
        if (GATEWAY_DEBUG) {
            Serial.println("[AVISO] Tamanho do pacote incompatível com o protocolo ARES.");
        }
    }
}

// Inicializa o ESP-NOW para o Gateway (Receptor)
bool initEspNowReceiver(void (*callback)(const uint8_t *mac, const AresMessagePacket *packet, int data_len)) {
    onPacketReceivedCallback = callback;

    // O Gateway precisa estar em modo Station (ou AP+STA) para usar o ESP-NOW de forma estável junto com o Wi-Fi
    WiFi.mode(WIFI_STA);

    if (esp_now_init() != ESP_OK) {
        if (GATEWAY_DEBUG) Serial.println("[ERRO] Falha ao inicializar o ESP-NOW no Gateway.");
        return false;
    }

    esp_now_register_recv_cb(onDataRecv);

    if (GATEWAY_DEBUG) Serial.println("[ESP-NOW] Receptor inicializado e escutando na frequência.");
    return true;
}