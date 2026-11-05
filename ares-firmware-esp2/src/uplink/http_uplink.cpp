#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "../../include/ares_protocol.h"
#include "../../include/ares_config.h"
#include "../../include/http_uplink.h"

static QueueHandle_t jobQueue = nullptr;      // loop() -> tarefa (1 job por vez)
static QueueHandle_t resultQueue = nullptr;   // tarefa -> loop()
static volatile bool busy = false;
static char serverIp[40] = INGRESS_SERVER_IP;
static portMUX_TYPE ipLock = portMUX_INITIALIZER_UNLOCKED;
static unsigned long lastWifiTryAt = 0;

// Campo de texto de tamanho fixo -> String, parando no primeiro '\0' (pode não ter '\0')
static String fieldText(const char *field, size_t size) {
    size_t len = strnlen(field, size);
    String s;
    s.reserve(len);
    for (size_t i = 0; i < len; i++) s += field[i];
    return s;
}

void uplinkLoadServerIp() {
    Preferences prefs;
    prefs.begin("ares", true);        // true = só leitura
    String saved = prefs.getString("server_ip", "");
    prefs.end();
    if (saved.length() > 0) uplinkSetServerIp(saved.c_str());
}

void uplinkSetServerIp(const char *ip) {
    portENTER_CRITICAL(&ipLock);
    snprintf(serverIp, sizeof(serverIp), "%s", ip);
    portEXIT_CRITICAL(&ipLock);
}

void uplinkGetServerIp(char *out, size_t size) {
    portENTER_CRITICAL(&ipLock);
    snprintf(out, size, "%s", serverIp);
    portEXIT_CRITICAL(&ipLock);
}

// Monta o JSON. Mantém os campos do JSON antigo e acrescenta os que faltavam.
static String buildJson(const UplinkJob *job) {
    const AresMessagePacket *p = &job->packet;
    char fromMac[18];
    snprintf(fromMac, sizeof(fromMac), "%02X:%02X:%02X:%02X:%02X:%02X", job->fromMac[0], job->fromMac[1],
             job->fromMac[2], job->fromMac[3], job->fromMac[4], job->fromMac[5]);

    JsonDocument doc;
    doc["msg_id"] = fieldText(p->header.msg_id, sizeof(p->header.msg_id));
    doc["source_id"] = fieldText(p->header.source_id, sizeof(p->header.source_id));
    doc["destination_id"] = fieldText(p->header.destination_id, sizeof(p->header.destination_id));
    doc["priority"] = (int) p->header.priority;
    doc["ttl"] = p->header.ttl;
    doc["timestamp"] = (unsigned long) p->header.timestamp;
    doc["battery_level"] = p->telemetry.battery_level;
    doc["storage_avail_kb"] = p->telemetry.storage_avail_kb;
    doc["lat"] = fieldText(p->location.lat, sizeof(p->location.lat));
    doc["lon"] = fieldText(p->location.lon, sizeof(p->location.lon));
    doc["altura"] = fieldText(p->location.altura, sizeof(p->location.altura));
    doc["text_message"] = fieldText(p->payload.text_message, sizeof(p->payload.text_message));
    doc["gateway_id"] = GATEWAY_SOURCE_ID;
    doc["last_hop_mac"] = fromMac;
    doc["gateway_rx_clock_s"] = (unsigned long) job->storedAtSec;

    String body;
    serializeJson(doc, body);
    return body;
}

static int postJob(const UplinkJob *job) {
    char ip[40];
    uplinkGetServerIp(ip, sizeof(ip));
    String url = String("http://") + ip + ":" + String(INGRESS_SERVER_PORT) + "/";

    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(url)) return -1;
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(buildJson(job));

    if (GATEWAY_DEBUG) {
        if (code > 0) {
            Serial.printf("[HTTP] POST %.*s -> %s | resposta %d\n", (int) sizeof(job->packet.header.msg_id),
                          job->packet.header.msg_id, url.c_str(), code);
        } else {
            // -1 com "connection refused" = firewall do Windows ou perfil de rede Público (veja o README)
            Serial.printf("[HTTP] Falha no POST para %s | código %d (%s)\n", url.c_str(), code,
                          http.errorToString(code).c_str());
        }
    }
    http.end();
    return code;
}

// Sem Wi-Fi: tenta de novo de tempos em tempos (o Hotspot pode ter reiniciado).
// Cuidado: enquanto procura a rede, o rádio passa por todos os canais e o ESP-NOW
// fica surdo. Por isso, se não conectar, o rádio é devolvido ao canal do ESP-NOW.
static void keepWifiAlive() {
    if (WiFi.status() == WL_CONNECTED) return;
    if (millis() - lastWifiTryAt < WIFI_RETRY_INTERVAL_MS) return;
    lastWifiTryAt = millis();
    // Credenciais não preenchidas: nem tenta (só atrapalharia o rádio)
    if (strcmp(WIFI_SSID, "DEFAULT_SSID") == 0 || strcmp(WIFI_SSID, "SEU_SSID_AQUI") == 0) return;
    if (GATEWAY_DEBUG) Serial.println("[WiFi] Desconectado. Tentando reconectar (as mensagens continuam guardadas na flash)...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    // Esta é a tarefa do HTTP, então pode esperar aqui sem travar o loop() nem o rádio
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) vTaskDelay(pdMS_TO_TICKS(250));
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect();
        esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
        if (GATEWAY_DEBUG) Serial.printf("[WiFi] Não conectou. Rádio de volta ao canal %d; nova tentativa em %d s.\n",
                                         ESPNOW_CHANNEL, WIFI_RETRY_INTERVAL_MS / 1000);
    }
}

static void uplinkTask(void *arg) {
    (void) arg;
    static UplinkJob job;              // static: 200 bytes fora da pilha da tarefa
    for (;;) {
        keepWifiAlive();
        // Espera até 200 ms por um job; sem job, volta a cuidar do Wi-Fi
        if (xQueueReceive(jobQueue, &job, pdMS_TO_TICKS(200)) == pdTRUE) {
            UplinkResult result;
            result.handle = job.handle;
            result.httpCode = (WiFi.status() == WL_CONNECTED) ? postJob(&job) : -100;
            xQueueSend(resultQueue, &result, portMAX_DELAY);
            busy = false;
        }
    }
}

bool uplinkBegin() {
    jobQueue = xQueueCreate(1, sizeof(UplinkJob));
    resultQueue = xQueueCreate(2, sizeof(UplinkResult));
    if (jobQueue == nullptr || resultQueue == nullptr) return false;
    lastWifiTryAt = millis();
    // 8 KB de pilha: HTTPClient + ArduinoJson precisam de bastante. Núcleo 1, junto com o loop().
    return xTaskCreatePinnedToCore(uplinkTask, "ares_uplink", 8192, nullptr, 1, nullptr, 1) == pdPASS;
}

bool uplinkIsIdle() {
    return !busy;
}

bool uplinkSubmit(const UplinkJob *job) {
    if (busy) return false;
    busy = true;
    if (xQueueSend(jobQueue, job, 0) != pdTRUE) {
        busy = false;
        return false;
    }
    return true;
}

bool uplinkPollResult(UplinkResult *result) {
    return xQueueReceive(resultQueue, result, 0) == pdTRUE;
}
