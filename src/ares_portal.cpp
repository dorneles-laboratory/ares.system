#include "ares_portal.h"
#include "ares_radio.h"
#include "ares_storage.h"
#include <ArduinoJson.h>

AresCaptivePortal* AresCaptivePortal::instance = nullptr;

AresCaptivePortal::AresCaptivePortal() : server(80) {
    memset(lastDownstreamMsg, 0, sizeof(lastDownstreamMsg));
}

AresCaptivePortal& AresCaptivePortal::getInstance() {
    if (!instance) {
        instance = new AresCaptivePortal();
    }
    return *instance;
}

bool AresCaptivePortal::begin(const char *ssid) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(CAPTIVE_PORTAL_IP, CAPTIVE_PORTAL_IP, CAPTIVE_PORTAL_NETMASK);
    bool apStarted = WiFi.softAP(ssid, nullptr, ARES_ESP_NOW_CHANNEL);

    if (!apStarted) {
        Serial.println("[ARES PORTAL] Falha ao iniciar SoftAP!");
        return false;
    }

    Serial.printf("[ARES PORTAL] SoftAP ativo: %s | IP: 192.168.4.1\n", ssid);

    // Redireciona todas as requisições DNS para o IP local (Captive Portal Trap)
    dnsServer.start(DNS_PORT, "*", CAPTIVE_PORTAL_IP);

    setupRoutes();
    server.begin();
    return true;
}

void AresCaptivePortal::setupRoutes() {
    server.on("/", HTTP_GET, [this]() { handleRoot(); });
    server.on("/api/status", HTTP_GET, [this]() { handleStatus(); });
    server.on("/api/send", HTTP_POST, [this]() { handleSend(); });

    // Armadilhas comuns de Captive Portal do Android e iOS
    server.on("/generate_204", HTTP_GET, [this]() { handleRoot(); });
    server.on("/gen_204", HTTP_GET, [this]() { handleRoot(); });
    server.on("/hotspot-detect.html", HTTP_GET, [this]() { handleRoot(); });
    server.on("/canonical.html", HTTP_GET, [this]() { handleRoot(); });
    server.on("/ncsi.txt", HTTP_GET, [this]() { handleRoot(); });

    server.onNotFound([this]() { handleNotFound(); });
}

void AresCaptivePortal::handleRoot() {
    server.send(200, "text/html", FPSTR(ARES_CAPTIVE_INDEX_HTML));
}

void AresCaptivePortal::handleStatus() {
    JsonDocument doc;
    doc["nodeId"] = ARES_NODE_ID;
    doc["battery"] = 94; // Simula nível de bateria de teste
    if (strlen(lastDownstreamMsg) > 0) {
        doc["downstreamMsg"] = lastDownstreamMsg;
    }

    String jsonResponse;
    serializeJson(doc, jsonResponse);
    server.send(200, "application/json", jsonResponse);
}

void AresCaptivePortal::handleSend() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"Corpo vazio\"}");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        server.send(400, "application/json", "{\"error\":\"JSON invalido\"}");
        return;
    }

    int priority = doc["priority"] | 4;
    const char *text = doc["text"] | "";

    // Constrói o pacote binário ARES
    AresMessagePacket packet = {};
    snprintf(packet.header.msg_id, sizeof(packet.header.msg_id), "%s-%lu", ARES_NODE_ID, millis());
    strncpy(packet.header.source_id, ARES_NODE_ID, sizeof(packet.header.source_id));
    strncpy(packet.header.destination_id, "BASE", sizeof(packet.header.destination_id));
    packet.header.priority = (uint8_t)priority;
    packet.header.direction = ARES_DIR_UPSTREAM;
    packet.header.ttl = 64;
    packet.header.hop_count = 1;
    packet.header.timestamp = millis();

    packet.telemetry.battery_level = g_ares_battery;
    packet.telemetry.storage_avail_kb = AresStorage::getInstance().getAvailableSpaceKb();
    packet.telemetry.last_rssi = AresRadio::getInstance().getLastRssi();

    strncpy(packet.location.lat, ARES_NODE_LAT, sizeof(packet.location.lat));
    strncpy(packet.location.lon, ARES_NODE_LON, sizeof(packet.location.lon));
    strncpy(packet.location.altura, ARES_NODE_ALTURA, sizeof(packet.location.altura));

    strncpy(packet.payload.text_message, text, sizeof(packet.payload.text_message));

    packet.hops_trail[0] = g_ares_role;

    // 1. Armazena localmente em custódia DTN
    AresStorage::getInstance().storePacket(&packet);

    // 2. Dispara imediatamente via ESP-NOW para qualquer nó vizinho que esteja escutando
    digitalWrite(PIN_LED_PACKET_ACTIVITY, HIGH);
    AresRadio::getInstance().sendBroadcast(&packet);

    JsonDocument resDoc;
    resDoc["status"] = "success";
    resDoc["msg_id"] = packet.header.msg_id;

    String resJson;
    serializeJson(resDoc, resJson);
    server.send(200, "application/json", resJson);
}

void AresCaptivePortal::handleNotFound() {
    // Redireciona qualquer requisição desconhecida para a raiz (Captive Trap)
    server.sendHeader("Location", String("http://192.168.4.1/"), true);
    server.send(302, "text/plain", "");
}

void AresCaptivePortal::handleClient() {
    dnsServer.processNextRequest();
    server.handleClient();
}

void AresCaptivePortal::setDownstreamNotification(const char *msg) {
    if (msg) {
        strncpy(lastDownstreamMsg, msg, sizeof(lastDownstreamMsg) - 1);
    }
}
