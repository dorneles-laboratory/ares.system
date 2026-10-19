#include <Arduino.h>
#include "ares_protocol.h"
#include "ares_config.h"

// Declarações externas do rádio
bool initEspNowTransmitter();
bool sendAresMessage(AresMessagePacket *packet);
void setRadioDebug(bool enabled);

unsigned long lastSendTime = 0;
int messageCounter = 1;

void setup() {
    Serial.begin(115200);
    delay(1000);

    // Configura o pino físico de debug com pull-down interno
    pinMode(DEBUG_PIN, INPUT_PULLDOWN);

    // Avalia se o debug está ativo pelas configurações do ares_config.h ou pelo pino físico
    bool showLogs = FORCE_SOFTWARE_DEBUG || (digitalRead(DEBUG_PIN) == HIGH);
    setRadioDebug(showLogs);

    if (showLogs) {
        Serial.println("\n--- Inicializando ARES Node ESP1 (Emissor) [DEBUG ATIVADO] ---");
        Serial.printf("[DEBUG] Tamanho do pacote AresMessagePacket: %u bytes\n", sizeof(AresMessagePacket));
    }
    
    if (!initEspNowTransmitter()) {
        if (showLogs) Serial.println("Erro crítico na inicialização do rádio.");
    }
}

void loop() {
    bool showLogs = FORCE_SOFTWARE_DEBUG || (digitalRead(DEBUG_PIN) == HIGH);
    setRadioDebug(showLogs);

    unsigned long currentMillis = millis();

    if (currentMillis - lastSendTime >= TRANSMISSION_INTERVAL_MS) {
        lastSendTime = currentMillis;

        AresMessagePacket outgoingMessage;

        // 1. Preenchendo o Header
        snprintf(outgoingMessage.header.msg_id, sizeof(outgoingMessage.header.msg_id), "msg-uuid-%d", messageCounter);
        snprintf(outgoingMessage.header.source_id, sizeof(outgoingMessage.header.source_id), "%s", NODE_SOURCE_ID);
        snprintf(outgoingMessage.header.destination_id, sizeof(outgoingMessage.header.destination_id), "GATEWAY-02");
        outgoingMessage.header.priority = P4; 
        outgoingMessage.header.ttl = 64;
        outgoingMessage.header.timestamp = millis();

        // 2. Preenchendo Telemetria Simulada
        outgoingMessage.telemetry.battery_level = 98.5f;
        outgoingMessage.telemetry.storage_avail_kb = 2048;

        // 3. Preenchendo Localização Simulada
        snprintf(outgoingMessage.location.lat, sizeof(outgoingMessage.location.lat), "-29.6842");
        snprintf(outgoingMessage.location.lon, sizeof(outgoingMessage.location.lon), "-53.8069");
        snprintf(outgoingMessage.location.altura, sizeof(outgoingMessage.location.altura), "150.5");

        // 4. Preenchendo a Mensagem de Texto Livre
        snprintf(outgoingMessage.payload.text_message, sizeof(outgoingMessage.payload.text_message), 
                 "ARES #%d", messageCounter);

        bool dispatched = sendAresMessage(&outgoingMessage);

        if (showLogs) {
            if (dispatched) {
                Serial.printf("[LOOP] Pacote #%d despachado com sucesso via ESP-NOW.\n", messageCounter);
            } else {
                Serial.println("[LOOP] Erro ao despachar pacote via rádio.");
            }
        }

        messageCounter++;
    }
}