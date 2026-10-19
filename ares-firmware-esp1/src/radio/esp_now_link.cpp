#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include "../../include/ares_protocol.h"
#include "../../include/ares_config.h"

uint8_t broadcastAddress[] = GATEWAY_MAC;
static bool radioDebugEnabled = false;

void setRadioDebug(bool enabled) {
    radioDebugEnabled = enabled;
}

void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    if (!radioDebugEnabled) return;
    
    Serial.print("[ESP-NOW] Status do Envio: ");
    if (status == ESP_NOW_SEND_SUCCESS) {
        Serial.println("Sucesso na Entrega!");
    } else {
        Serial.println("Falha na Transmissão.");
    }
}

bool initEspNowTransmitter() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    // Força o rádio do ESP1 a operar no mesmo canal 2 do Gateway/Hotspot
    esp_wifi_set_channel(2, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        if (radioDebugEnabled) Serial.println("[ERRO] Falha ao inicializar o ESP-NOW.");
        return false;
    }

    esp_now_register_send_cb(onDataSent);

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = 2; // Canal Wi-Fi padrão (pode ser ajustado conforme necessário)
    peerInfo.encrypt = false;
    
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        if (radioDebugEnabled) Serial.println("[ERRO] Falha ao adicionar o peer de destino.");
        return false;
    }

    if (radioDebugEnabled) Serial.println("[ESP-NOW] Transmissor inicializado com sucesso.");
    return true;
}

bool sendAresMessage(AresMessagePacket *packet) {
    esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *) packet, sizeof(AresMessagePacket));
    if (radioDebugEnabled) {
        Serial.print("[ESP-NOW] Enviando mensagem para o endereço: ");
        for (int i = 0; i < 6; i++) {
            Serial.printf("%02X", broadcastAddress[i]);
            if (i < 5) Serial.print(":");
        }
        Serial.println();
        if (result == ESP_OK) {
            Serial.println("[ESP-NOW] Mensagem enviada com sucesso.");
        } else {
            Serial.print("[ERRO] Falha ao enviar a mensagem. Código de erro: ");
            Serial.println(result);
        }
    }
    return (result == ESP_OK);
}