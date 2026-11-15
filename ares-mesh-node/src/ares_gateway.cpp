#include "ares_gateway.h"
#include "ares_radio.h"

AresGateway* AresGateway::instance = nullptr;

AresGateway::AresGateway() : pendingCount(0) {
    memset(pendingQueue, 0, sizeof(pendingQueue));
}

AresGateway& AresGateway::getInstance() {
    if (!instance) {
        instance = new AresGateway();
    }
    return *instance;
}

bool AresGateway::begin() {
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(GATEWAY_WIFI_SSID, GATEWAY_WIFI_PASS);

    Serial.printf("[ARES GATEWAY] Conectando ao Wi-Fi da Base: %s ...\n", GATEWAY_WIFI_SSID);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 15) {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[ARES GATEWAY] Conectado! IP da Base: %s\n", WiFi.localIP().toString().c_str());
        return true;
    } else {
        Serial.println("\n[ARES GATEWAY] Aviso: Não conectado ao Wi-Fi da Base (modo offline ou simulado).");
        return false;
    }
}

void AresGateway::queuePacketForIngress(const AresMessagePacket *packet) {
    if (!packet) return;
    if (pendingCount < 10) {
        memcpy(&pendingQueue[pendingCount], packet, sizeof(AresMessagePacket));
        pendingCount++;
    }
}

void AresGateway::processIngressQueue() {
    if (pendingCount <= 0) return;

    // Processa o primeiro da fila
    AresMessagePacket pkt = pendingQueue[0];
    for (int i = 0; i < pendingCount - 1; i++) {
        pendingQueue[i] = pendingQueue[i + 1];
    }
    pendingCount--;

    sendHttpPost(&pkt);
}

bool AresGateway::sendHttpPost(const AresMessagePacket *packet) {
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    http.begin(BACKEND_INGRESS_URL);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Api-Key", M2M_API_KEY);
    http.setTimeout(3500);

    JsonDocument doc;
    doc["msg_id"] = packet->header.msg_id;
    doc["source_id"] = packet->header.source_id;
    doc["destination_id"] = packet->header.destination_id;
    doc["priority"] = packet->header.priority;
    doc["ttl"] = packet->header.ttl;
    doc["hop_count"] = packet->header.hop_count;
    doc["battery_level"] = packet->telemetry.battery_level;
    doc["storage_avail_kb"] = packet->telemetry.storage_avail_kb;
    doc["rssi"] = packet->telemetry.last_rssi;
    doc["lat"] = packet->location.lat;
    doc["lon"] = packet->location.lon;
    doc["altura"] = packet->location.altura;
    doc["text_message"] = packet->payload.text_message;
    doc["timestamp"] = (double)packet->header.timestamp;
    doc["gateway_id"] = ARES_NODE_ID;

    // Hops trail
    JsonArray hopsArr = doc["hops_trail"].to<JsonArray>();
    hopsArr.add(packet->header.source_id);
    for (int i = 0; i < 6; i++) {
        if (packet->hops_trail[i] > 0) {
            char hopLabel[16];
            snprintf(hopLabel, sizeof(hopLabel), "HOP_%d", packet->hops_trail[i]);
            hopsArr.add(hopLabel);
        }
    }
    hopsArr.add(ARES_NODE_ID);

    String body;
    serializeJson(doc, body);

    int httpCode = http.POST(body);
    if (httpCode >= 200 && httpCode < 300) {
        String res = http.getString();
        Serial.printf("[ARES GATEWAY] HTTP POST Sucesso (%d): %s\n", httpCode, res.c_str());

        // Processa eventuais mensagens downstream retornadas pelo backend
        JsonDocument resDoc;
        if (deserializeJson(resDoc, res) == DeserializationError::Ok) {
            JsonArray downMsgs = resDoc["downstreamMessages"].as<JsonArray>();
            for (JsonObject down : downMsgs) {
                const char *dest = down["destinationNodeId"] | "BROADCAST";
                const char *msgText = down["textMessage"] | "";
                int prio = down["priority"] | 0;

                AresMessagePacket downPacket = {};
                snprintf(downPacket.header.msg_id, sizeof(downPacket.header.msg_id), "DOWN-%lu", millis());
                strncpy(downPacket.header.source_id, ARES_NODE_ID, sizeof(downPacket.header.source_id));
                strncpy(downPacket.header.destination_id, dest, sizeof(downPacket.header.destination_id));
                downPacket.header.priority = prio;
                downPacket.header.direction = ARES_DIR_DOWNSTREAM;
                downPacket.header.ttl = 64;
                downPacket.header.hop_count = 1;
                strncpy(downPacket.payload.text_message, msgText, sizeof(downPacket.payload.text_message));

                // Dispara na malha downstream
                AresRadio::getInstance().sendBroadcast(&downPacket);
                Serial.printf("[ARES GATEWAY] Mensagem downstream despachada para malha: %s -> %s\n", dest, msgText);
            }
        }
        http.end();
        return true;
    } else {
        Serial.printf("[ARES GATEWAY] Erro no HTTP POST: %d\n", httpCode);
        http.end();
        return false;
    }
}
