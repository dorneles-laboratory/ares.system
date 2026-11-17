#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "ares_config.h"

void setup() {
    Serial.begin(115200);
    delay(1000);

    if (GATEWAY_DEBUG) {
        Serial.println("\n--- Inicializando ARES Node ESP2 (Gateway) ---");
        Serial.printf("[DEBUG] SSID configurado: %s\n", WIFI_SSID);
        Serial.printf("[DEBUG] Servidor Ingress IP: %s\n", INGRESS_SERVER_IP);
    }

    // 1. Configura explicitamente o modo Station e limpa configurações antigas
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    delay(100);

    if (GATEWAY_DEBUG) {
        Serial.println("\n--- DESCOBRINDO ENDEREÇO MAC ---");
        Serial.println(WiFi.macAddress());
        Serial.println("--------------------------------");
        Serial.println();

        // Escaneia as redes disponíveis para ver se a do celular aparece
        int n = WiFi.scanNetworks();
        Serial.printf("[WiFi] Redes encontradas: %d\n", n);
        for (int i = 0; i < n; ++i) {
            Serial.printf("  [%d] %s (RSSI: %d)\n", i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i));
            delay(105);
        }
    }

    // 2. Inicia a conexão com a rede Wi-Fi / Hotspot
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    if (GATEWAY_DEBUG) Serial.print("[WiFi] Conectando à rede");

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
        }
    } else {
        if (GATEWAY_DEBUG) {
            Serial.println("\n[AVISO] Falha ao conectar no Wi-Fi.");
            Serial.printf("[AVISO] Status atual do WiFi: %d\n", WiFi.status());
        }
    }
}

void loop() {
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        String url = String("http://") + INGRESS_SERVER_IP + ":8080/";
        
        Serial.printf("[HTTP] Enviando teste para: %s\n", url.c_str());
        
        http.begin(url);
        http.addHeader("Content-Type", "application/json");
        
        int httpResponseCode = http.POST("{\"teste\":\"ola_do_esp32\"}");

        if (httpResponseCode > 0) {
            Serial.printf("[HTTP] Resposta do Notebook! Código: %d\n", httpResponseCode);
            String payload = http.getString();
            Serial.printf("[HTTP] Resposta corpo: %s\n", payload.c_str());
        } else {
            Serial.printf("[HTTP] Falha no envio. Erro: %s\n", http.errorToString(httpResponseCode).c_str());
        }

        http.end();
    } else {
        Serial.println("[AVISO] Wi-Fi desconectado!");
    }

    delay(5000); // Aguarda 5 segundos para o próximo teste
}