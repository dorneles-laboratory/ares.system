#include <Arduino.h>
#include <WiFi.h>
#include <esp_mac.h>
#include "ares_protocol.h"
#include "ares_config.h"
#include "ares_radio.h"
#include "ares_storage.h"
#include "ares_portal.h"
#include "ares_gateway.h"
#include "ares_wps.h"

// Variáveis globais de identificação dinâmica do nó
char g_ares_node_id[32] = {0};
char g_ares_ssid[32] = {0};
char g_ares_lat[12] = {0};
char g_ares_lon[12] = {0};
char g_ares_altura[10] = {0};

unsigned long lastBeaconTime = 0;
unsigned long lastStatusTime = 0;
unsigned long lastAliveBlinkTime = 0;
unsigned long packetLedTurnOffTime = 0;
float g_ares_battery = 92.0f; // Bateria dinâmica (lida via ADC ou telemetria)

void triggerPacketActivityLed() {
    digitalWrite(PIN_LED_PACKET_ACTIVITY, HIGH);
    packetLedTurnOffTime = millis() + 150; // Pulso de 150ms a cada pacote
}

// Envia confirmação de custódia DTN (ACK) para o emissor liberar a memória flash
void sendCustodyAck(const char* targetMsgId, const char* targetNodeId) {
    AresMessagePacket ack = {};
    snprintf(ack.header.msg_id, sizeof(ack.header.msg_id), "ACK-%s", targetMsgId);
    strncpy(ack.header.source_id, ARES_NODE_ID, sizeof(ack.header.source_id));
    strncpy(ack.header.destination_id, targetNodeId, sizeof(ack.header.destination_id));
    ack.header.priority = ARES_P0_EMERGENCY; // ACK de custódia tem prioridade máxima
    ack.header.direction = ARES_DIR_DOWNSTREAM;
    ack.header.msg_type = ARES_MSG_CUSTODY_ACK;
    ack.header.flags = ARES_FLAG_STORED_IN_WAL;
    ack.header.ttl = 16;
    ack.header.timestamp = millis();
    strncpy(ack.payload.text_message, targetMsgId, sizeof(ack.payload.text_message));

    AresRadio::getInstance().sendBroadcast(&ack);
    Serial.printf("[%s] [DTN CUSTODY] ACK de custódia emitido para pacote %s\n", ARES_NODE_ID, targetMsgId);
}

void onPacketReceived(const uint8_t *senderMac, const AresMessagePacket *packet, int rssi) {
    if (!packet) return;

    // Dispara indicador visual de que uma mensagem passou por este ESP (GPIO 4)
    triggerPacketActivityLed();

    // 1. Processamento de Confirmação de Custódia (RFC 5050 DTN Custody Signal)
    if (packet->header.msg_type == ARES_MSG_CUSTODY_ACK) {
        Serial.printf("[%s] [DTN CUSTODY] ACK recebido da rede para mensagem %s. Liberando da Flash.\n",
                      ARES_NODE_ID, packet->payload.text_message);
        AresStorage::getInstance().confirmCustody(packet->payload.text_message);
        return;
    }

    // Evita loops de retransmissão conferindo o cache de mensagens já vistas
    if (AresRadio::getInstance().hasSeenMessage(packet->header.msg_id)) {
        return;
    }
    AresRadio::getInstance().markMessageSeen(packet->header.msg_id);

    Serial.printf("[%s] Pacote recebido! ID: %s | Origem: %s | Prioridade: P%d | Tipo: %d | RSSI: %d dBm\n",
                  ARES_NODE_ID, packet->header.msg_id, packet->header.source_id, packet->header.priority, packet->header.msg_type, rssi);

    // =========================================================================
    // 2. DETECÇÃO DE DRONE MULE FANET: Drenagem Rápida em Rajada (Bulk Drain)
    // =========================================================================
    bool isDroneMuleNearby = (strstr(packet->header.source_id, "MULE") != nullptr ||
                              strstr(packet->header.source_id, "DRONE") != nullptr ||
                              packet->header.msg_type == ARES_MSG_BULK_DRAIN_REQ);

    if (isDroneMuleNearby && AresStorage::getInstance().getStoredCount() > 0) {
        Serial.printf("[%s] 🚁 [FANET BULK DRAIN] Drone Mule detectado no raio de cobertura! Descarregando fila DTN...\n", ARES_NODE_ID);
        AresMessagePacket burstQueue[10];
        int countToDrain = AresStorage::getInstance().drainPacketsToMule(burstQueue, 10);
        for (int i = 0; i < countToDrain; i++) {
            burstQueue[i].header.direction = ARES_DIR_UPSTREAM;
            burstQueue[i].header.hop_count++;
            strncpy(burstQueue[i].header.destination_id, packet->header.source_id, sizeof(burstQueue[i].header.destination_id));
            AresRadio::getInstance().sendBroadcast(&burstQueue[i]);
            delay(15); // Espaçamento mínimo para evitar colisão de RF no ar
        }
        Serial.printf("[%s] 🚁 [FANET BULK DRAIN] %d pacotes prioritários transferidos com sucesso para o Drone!\n", ARES_NODE_ID, countToDrain);
    }

    // =========================================================================
    // 3. TRATAMENTO CONFORME O PAPEL DO NÓ (GATEWAY, ROUTER, RELAY, STORAGE)
    // =========================================================================
#if (NODE_ROLE == ARES_ROLE_ROUTER)
    // Se for mensagem downstream destinada a nós ou em broadcast
    if (packet->header.direction == ARES_DIR_DOWNSTREAM) {
        if (strcmp(packet->header.destination_id, ARES_NODE_ID) == 0 || strcmp(packet->header.destination_id, "BROADCAST") == 0) {
            Serial.printf("[%s] NOTIFICAÇÃO DA BASE RECEBIDA: %s\n", ARES_NODE_ID, packet->payload.text_message);
            AresCaptivePortal::getInstance().setDownstreamNotification(packet->payload.text_message);
        }
    }

#elif (NODE_ROLE == ARES_ROLE_RELAY)
    // Energy-Aware Routing (EAR): Se a bateria estiver abaixo de 20%, recusa tráfego P3/P4
    if (g_ares_battery < 20.0f && packet->header.priority > ARES_P1_RESCUE) {
        Serial.printf("[%s] [EAR] Bateria crítica (%.1f%%). Recusando repasse de tráfego rotineiro P%d para poupar energia para P0/P1.\n",
                      ARES_NODE_ID, g_ares_battery, packet->header.priority);
        return;
    }

    // Retransmite imediatamente com incremento de salto
    AresMessagePacket fwdPacket = *packet;
    fwdPacket.header.hop_count++;
    fwdPacket.header.ttl--;
    if (fwdPacket.header.ttl > 0) {
        for (int i = 0; i < 6; i++) {
            if (fwdPacket.hops_trail[i] == 0) {
                fwdPacket.hops_trail[i] = NODE_ROLE;
                break;
            }
        }
        AresRadio::getInstance().sendBroadcast(&fwdPacket);
        Serial.printf("[%s] Pacote retransmitido para o próximo salto (hop %d)\n", ARES_NODE_ID, fwdPacket.header.hop_count);
    }

#elif (NODE_ROLE == ARES_ROLE_STORAGE)
    // Guarda em custódia DTN local persistente no LittleFS
    if (AresStorage::getInstance().storePacket(packet)) {
        // Envia ACK de custódia de volta ao salto anterior
        sendCustodyAck(packet->header.msg_id, packet->header.source_id);
    }

#elif (NODE_ROLE == ARES_ROLE_GATEWAY)
    // Nó Gateway enfileira para HTTP Ingress e confirma custódia
    AresGateway::getInstance().queuePacketForIngress(packet);
    sendCustodyAck(packet->header.msg_id, packet->header.source_id);
#endif
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    // 0. Configura LEDs de status de hardware
    // GPIO 2: Heartbeat indicador de que o ESP está ligado e funcionando
    // GPIO 4: Atividade de tráfego indicando que uma mensagem passou pelo ESP
    pinMode(PIN_LED_SYSTEM_ALIVE, OUTPUT);
    pinMode(PIN_LED_PACKET_ACTIVITY, OUTPUT);
    digitalWrite(PIN_LED_SYSTEM_ALIVE, HIGH);
    digitalWrite(PIN_LED_PACKET_ACTIVITY, LOW);

    // Lê o MAC address de hardware único diretamente do eFuse
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macStr[18] = {0};
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Identificação 100% dinâmica: cada ESP32 adquire seu nome único via hardware MAC
    snprintf(g_ares_node_id, sizeof(g_ares_node_id), "ESP32_%02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(g_ares_ssid, sizeof(g_ares_ssid), "ARES-AP-%02X%02X", mac[4], mac[5]);

    // 0. Gestão Dinâmica de Energia (Energy-Aware):
    // Nós puramente repetidores/armazenamento operam a 80MHz para economizar ~40% de consumo de bateria
#if (NODE_ROLE == ARES_ROLE_RELAY || NODE_ROLE == ARES_ROLE_STORAGE)
    setCpuFrequencyMhz(80);
    Serial.println("[ARES POWER] Escalonamento de frequência: 80 MHz ativado (Modo Baixo Consumo).");
#else
    setCpuFrequencyMhz(160);
#endif

    // Inicializa o banco de custódia DTN permanente com LittleFS Flash WAL
    AresStorage::getInstance().begin();

    // 1. Inicializa Gerenciador de Geolocalização (carrega do NVS ou fallback)
    AresLocationManager::getInstance().begin();

    // Se houver provisionamento configurado ou nó Gateway com internet, executa WPS
#if (NODE_ROLE == ARES_ROLE_GATEWAY)
    if (strlen(GOOGLE_GEOLOCATION_API_KEY) > 0 || strlen(UNWIRED_API_KEY) > 0) {
        WpsProvider provider = (strlen(GOOGLE_GEOLOCATION_API_KEY) > 0) ? WPS_PROVIDER_GOOGLE : WPS_PROVIDER_UNWIRED;
        const char* key = (provider == WPS_PROVIDER_GOOGLE) ? GOOGLE_GEOLOCATION_API_KEY : UNWIRED_API_KEY;
        AresLocationManager::getInstance().performWpsUpdate(GATEWAY_WIFI_SSID, GATEWAY_WIFI_PASS, provider, key);
    }
#else
    if (strlen(PROVISIONING_WIFI_SSID) > 0 && (strlen(GOOGLE_GEOLOCATION_API_KEY) > 0 || strlen(UNWIRED_API_KEY) > 0)) {
        WpsProvider provider = (strlen(GOOGLE_GEOLOCATION_API_KEY) > 0) ? WPS_PROVIDER_GOOGLE : WPS_PROVIDER_UNWIRED;
        const char* key = (provider == WPS_PROVIDER_GOOGLE) ? GOOGLE_GEOLOCATION_API_KEY : UNWIRED_API_KEY;
        AresLocationManager::getInstance().performWpsUpdate(PROVISIONING_WIFI_SSID, PROVISIONING_WIFI_PASS, provider, key);
    }
#endif

    Serial.println("\n=======================================================");
    Serial.printf(" ⭐ ARES RESILIENCE MESH NODE — %s\n", ARES_NODE_ID);
    Serial.printf("    MAC de Hardware: %s\n", macStr);
    Serial.printf("    Papel do Nó: %d | Canal: %d\n", NODE_ROLE, ARES_ESP_NOW_CHANNEL);
    Serial.printf("    Localização: Lat %s, Lon %s (%s)\n", ARES_NODE_LAT, ARES_NODE_LON, ARES_NODE_ALTURA);
    Serial.println("=======================================================");

    // 1. Configurações de Wi-Fi e Rádio
#if (NODE_ROLE == ARES_ROLE_ROUTER)
    AresCaptivePortal::getInstance().begin(g_ares_ssid);
    AresRadio::getInstance().begin(ARES_ESP_NOW_CHANNEL);

#elif (NODE_ROLE == ARES_ROLE_GATEWAY)
    WiFi.mode(WIFI_AP_STA);
    AresGateway::getInstance().begin();
    AresRadio::getInstance().begin(ARES_ESP_NOW_CHANNEL);

#else
    // Relays e Storages usam apenas modo Station para o rádio ESP-NOW
    WiFi.disconnect(true);
    AresRadio::getInstance().begin(ARES_ESP_NOW_CHANNEL);
#endif

    // Registra a função de recepção de pacotes
    AresRadio::getInstance().setPacketCallback(onPacketReceived);
    Serial.printf("[ARES] Inicialização dinâmica concluída com sucesso para %s. Pronto para operação.\n", ARES_NODE_ID);
}

void loop() {
    unsigned long now = millis();

    // 1. Pulso Heartbeat no GPIO 2 (indica que o ESP32 está vivo e operando)
    if (now - lastAliveBlinkTime >= 1000) {
        lastAliveBlinkTime = now;
        digitalWrite(PIN_LED_SYSTEM_ALIVE, !digitalRead(PIN_LED_SYSTEM_ALIVE));
    }

    // 2. Desliga o LED de atividade (GPIO 4) após o pulso visual de 150ms
    if (packetLedTurnOffTime > 0 && now >= packetLedTurnOffTime) {
        digitalWrite(PIN_LED_PACKET_ACTIVITY, LOW);
        packetLedTurnOffTime = 0;
    }

    // Loop do Portal Cativo nos nós Router
#if (NODE_ROLE == ARES_ROLE_ROUTER)
    AresCaptivePortal::getInstance().handleClient();
#endif

    // Processamento da fila de Ingress HTTP no Gateway
#if (NODE_ROLE == ARES_ROLE_GATEWAY)
    AresGateway::getInstance().processIngressQueue();
#endif

    // Disparo de Beacons periódicos de telemetria e presença
    if (now - lastBeaconTime >= BEACON_INTERVAL_MS) {
        lastBeaconTime = now;

        AresMessagePacket beacon = {};
        snprintf(beacon.header.msg_id, sizeof(beacon.header.msg_id), "%s-BCN-%lu", ARES_NODE_ID, now / 1000);
        strncpy(beacon.header.source_id, ARES_NODE_ID, sizeof(beacon.header.source_id));
        strncpy(beacon.header.destination_id, "BROADCAST", sizeof(beacon.header.destination_id));
        beacon.header.priority = ARES_P4_ROUTINE;
        beacon.header.direction = ARES_DIR_UPSTREAM;
        beacon.header.msg_type = ARES_MSG_DATA;
        beacon.header.flags = (g_ares_battery < 20.0f) ? ARES_FLAG_ENERGY_LOW : 0;
        beacon.header.ttl = 32;
        beacon.header.hop_count = 0;
        beacon.header.timestamp = now;

        beacon.telemetry.battery_level = (NODE_ROLE == ARES_ROLE_GATEWAY) ? 100.0f : g_ares_battery;
        beacon.telemetry.storage_avail_kb = AresStorage::getInstance().getStoredCount() * sizeof(AresMessagePacket);
        beacon.telemetry.last_rssi = -60;

        strncpy(beacon.location.lat, ARES_NODE_LAT, sizeof(beacon.location.lat));
        strncpy(beacon.location.lon, ARES_NODE_LON, sizeof(beacon.location.lon));
        strncpy(beacon.location.altura, ARES_NODE_ALTURA, sizeof(beacon.location.altura));

        snprintf(beacon.payload.text_message, sizeof(beacon.payload.text_message),
                 "Heartbeat %s: Role %d | Fila DTN: %d", ARES_NODE_ID, NODE_ROLE, AresStorage::getInstance().getStoredCount());

        AresRadio::getInstance().sendBroadcast(&beacon);
        triggerPacketActivityLed();

#if (NODE_ROLE == ARES_ROLE_STORAGE)
        // Se houver pacotes acumulados, drena pacotes se houver drone por perto
        if (AresStorage::getInstance().getStoredCount() > 0) {
            Serial.printf("[%s] DTN Storage ativo: %d pacotes retidos na fila aguardando Drone Mule.\n",
                          ARES_NODE_ID, AresStorage::getInstance().getStoredCount());
        }
#endif
    }
}
