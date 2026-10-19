#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "ares_protocol.h"
#include "ares_config.h"

// Declaração do receptor de rádio
bool initEspNowReceiver(void (*callback)(const uint8_t *mac, const AresMessagePacket *packet, int data_len));

// Buffers globais para separar a interrupção de rádio do envio HTTP (Thread Segura)
AresMessagePacket pendingPacket;
volatile bool hasPendingPacket = false;
uint8_t lastSenderMac[6];
int lastPacketLen = 0;

// 1. Callback de rádio (RÁPIDA): Apenas salva o pacote na memória e libera a placa de rede
void handleAresIncomingPacket(const uint8_t *mac, const AresMessagePacket *packet, int data_len) {
    if (!hasPendingPacket) {
        memcpy(&pendingPacket, packet, sizeof(AresMessagePacket));
        memcpy(lastSenderMac, mac, 6);
        lastPacketLen = data_len;
        hasPendingPacket = true;
    }
}

// 2. Função de Envio HTTP (Executada na Thread Principal, fora da interrupção)
void sendHttpPost(const AresMessagePacket *packet) {
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        
        // Monta a URL (ex: http://10.174.176.89:8080/)
        String serverPath = String("http://") + INGRESS_SERVER_IP + ":" + String(INGRESS_SERVER_PORT) + "/";
        
        if (GATEWAY_DEBUG) Serial.printf("[HTTP] Tentando conectar em: %s\n", serverPath.c_str());
        
        http.begin(serverPath);
        http.addHeader("Content-Type", "application/json");
        http.setTimeout(3000); 

        // Criação do JSON
        JsonDocument doc; 
        doc["msg_id"] = packet->header.msg_id;
        doc["source_id"] = packet->header.source_id;
        doc["priority"] = packet->header.priority;
        doc["battery_level"] = packet->telemetry.battery_level;
        doc["lat"] = packet->location.lat;
        doc["lon"] = packet->location.lon;
        doc["text_message"] = packet->payload.text_message;

        String jsonBody;
        serializeJson(doc, jsonBody);

        // Disparo da requisição HTTP POST
        int httpResponseCode = http.POST(jsonBody);

        if (httpResponseCode > 0) {
            Serial.printf("[HTTP] POST enviado com sucesso! Código de resposta: %d\n", httpResponseCode);
            String payload_resposta = http.getString();
            if (payload_resposta.length() > 0) {
                Serial.printf("[HTTP] Resposta do Servidor: %s\n", payload_resposta.c_str());
            }
        } else {
            Serial.printf("[HTTP] Falha no POST. Código numérico: %d | Erro descritivo: %s\n", 
                          httpResponseCode, http.errorToString(httpResponseCode).c_str());
        }

        http.end();
    } else {
        Serial.println("[AVISO] Wi-Fi desconectado no momento do envio.");
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    if (GATEWAY_DEBUG) {
        Serial.println("\n--- Inicializando ARES Node ESP2 (Gateway) ---");
        Serial.printf("[DEBUG] SSID configurado: %s\n", WIFI_SSID);
        Serial.printf("[DEBUG] Servidor Ingress IP: %s\n", INGRESS_SERVER_IP);
    }

    // Limpa configurações de Wi-Fi antigas
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    delay(100);

    if (GATEWAY_DEBUG) {
        Serial.println("\n--- DESCOBRINDO ENDEREÇO MAC ---");
        Serial.println(WiFi.macAddress());
        Serial.println("--------------------------------\n");

        // Escaneamento de Redes (Exibe SSIDs, Canais e RSSI)
        int n = WiFi.scanNetworks();
        Serial.printf("[WiFi] Redes encontradas: %d\n", n);
        for (int i = 0; i < n; ++i) {
            Serial.printf("  [%d] %s (Canal: %d, RSSI: %d)\n", i + 1, WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i));
            delay(105); // Delay seguro para evitar interrupções no rádio
        }
    }

    // Conexão com o Hotspot
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    if (GATEWAY_DEBUG) Serial.print("[WiFi] Conectando à rede...");
    
    unsigned long startAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 20000) {
        delay(500);
        if (GATEWAY_DEBUG) Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        if (GATEWAY_DEBUG) {
            Serial.println("\n[WiFi] Conectado com sucesso!");
            Serial.print("[WiFi] IP do Gateway: ");
            Serial.println(WiFi.localIP());
            Serial.print("[WiFi] Canal atual: ");
            Serial.println(WiFi.channel()); // Imprime o canal operacional
        }
    } else {
        if (GATEWAY_DEBUG) {
            Serial.println("\n[AVISO] Falha ao conectar no Wi-Fi do Hotspot.");
        }
    }

    // Inicializa o Receptor ESP-NOW
    if (initEspNowReceiver(handleAresIncomingPacket)) {
        if (GATEWAY_DEBUG) Serial.println("[ESP-NOW] Receptor inicializado e escutando na frequência.");
    } else {
        if (GATEWAY_DEBUG) Serial.println("[ERRO] Falha crítica ao iniciar o rádio receptor.");
    }
}

void loop() {
    // A mágica acontece aqui: O HTTP só é acionado fora da interrupção do rádio
    if (hasPendingPacket) {
        if (GATEWAY_DEBUG) {
            Serial.printf("\n[ESP-NOW RX] Pacote recebido de MAC: %02X:%02X:%02X:%02X:%02X:%02X | Tamanho: %d bytes\n",
                          lastSenderMac[0], lastSenderMac[1], lastSenderMac[2], 
                          lastSenderMac[3], lastSenderMac[4], lastSenderMac[5], lastPacketLen);
            
            Serial.println("\n--- [ARES GATEWAY] Pacote Processado na Thread Principal ---");
            Serial.printf("  ID da Mensagem : %s\n", pendingPacket.header.msg_id);
            Serial.printf("  Origem (Nó)    : %s\n", pendingPacket.header.source_id);
            Serial.printf("  Prioridade     : P%d\n", pendingPacket.header.priority);
            Serial.printf("  Bateria        : %.2f%%\n", pendingPacket.telemetry.battery_level);
            Serial.printf("  Coordenadas    : Lat %s, Lon %s\n", pendingPacket.location.lat, pendingPacket.location.lon);
            Serial.printf("  Payload Text   : %s\n", pendingPacket.payload.text_message);
            Serial.println("-----------------------------------------------------");
        }

        // Faz o despacho do pacote via POST HTTP
        sendHttpPost(&pendingPacket);
        
        // Libera a flag para aceitar o próximo pacote do ESP1
        hasPendingPacket = false; 
    }

    // Um pequeno delay para estabilidade do Watchdog Timer (WDT)
    delay(10);
}