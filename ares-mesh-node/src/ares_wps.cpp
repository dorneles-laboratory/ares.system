#include "ares_wps.h"
#include "ares_config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <algorithm>

// ==============================================================================
// 1. SCANNER WI-FI (VARREDURA PASSIVA E FILTRAGEM POR RSSI)
// ==============================================================================
std::vector<WifiApRecord> WifiApScanner::scanTopAccessPoints(int maxAps, int minAps) {
    std::vector<WifiApRecord> apList;

    Serial.println("[WPS Scanner] Iniciando varredura ativa de redes Wi-Fi (incluindo ocultas)...");

    // Executa scan síncrono (async = false) e inclui SSIDs ocultos (show_hidden = true)
    int16_t numNetworks = WiFi.scanNetworks(false, true);

    if (numNetworks < 0) {
        Serial.printf("[WPS Scanner] Falha na varredura Wi-Fi (código: %d).\n", numNetworks);
        return apList;
    }

    if (numNetworks == 0) {
        Serial.println("[WPS Scanner] Nenhuma rede Wi-Fi encontrada ao redor.");
        WiFi.scanDelete();
        return apList;
    }

    Serial.printf("[WPS Scanner] %d redes detectadas ao alcance. Filtrando pontos de acesso...\n", numNetworks);

    // Coleta BSSID, RSSI e Canal de todas as redes encontradas
    for (int i = 0; i < numNetworks; i++) {
        WifiApRecord record;
        record.bssid = WiFi.BSSIDstr(i);
        record.rssi = WiFi.RSSI(i);
        record.channel = WiFi.channel(i);
        apList.push_back(record);
    }

    // BOAS PRÁTICAS: Libera imediatamente os buffers alocados pelo scan no heap do ESP32
    WiFi.scanDelete();

    // Ordena do sinal mais forte (-30 dBm) para o mais fraco (-95 dBm)
    std::sort(apList.begin(), apList.end(), [](const WifiApRecord& a, const WifiApRecord& b) {
        return a.rssi > b.rssi;
    });

    // Mantém os top `maxAps` (5 a 8 pontos de acesso)
    if ((int)apList.size() > maxAps) {
        apList.resize(maxAps);
    }

    Serial.printf("[WPS Scanner] Top %d APs selecionados para triangulação WPS:\n", (int)apList.size());
    for (size_t i = 0; i < apList.size(); i++) {
        Serial.printf("   [%d] BSSID: %s | RSSI: %d dBm | Ch: %d\n",
                      (int)i + 1, apList[i].bssid.c_str(), (int)apList[i].rssi, (int)apList[i].channel);
    }

    return apList;
}

// ==============================================================================
// 2. CLIENTE HTTP/HTTPS PARA API DE GEOLOCALIZAÇÃO
// ==============================================================================
GeolocationClient::GeolocationClient(WpsProvider provider, const char* apiKey)
    : _provider(provider), _apiKey(apiKey ? apiKey : ""), _timeoutMs(10000) {}

GeoLocation GeolocationClient::resolveLocation(const std::vector<WifiApRecord>& aps) {
    if (aps.size() < 2) {
        Serial.println("[WPS Client] Triangulação inviável: são necessários ao menos 2 BSSIDs distintos.");
        GeoLocation empty = {0.0, 0.0, 0.0, false, "", "", 0};
        return empty;
    }

    if (_provider == WPS_PROVIDER_UNWIRED) {
        return queryUnwiredApi(aps);
    } else {
        return queryGoogleApi(aps);
    }
}

GeoLocation GeolocationClient::queryGoogleApi(const std::vector<WifiApRecord>& aps) {
    GeoLocation result = {0.0, 0.0, 0.0, false, "", "", 0};

    // Monta o endpoint Google Maps Geolocation API
    String url = "https://www.googleapis.com/geolocation/v1/geolocate";
    if (_apiKey.length() > 0) {
        url += "?key=" + _apiKey;
    }

    // Serialização com ArduinoJson v7
    JsonDocument doc;
    doc["considerIp"] = false; // Força triangulação puramente via Wi-Fi (WPS)

    JsonArray wifiArray = doc["wifiAccessPoints"].to<JsonArray>();
    for (const auto& ap : aps) {
        JsonObject apObj = wifiArray.add<JsonObject>();
        apObj["macAddress"] = ap.bssid;
        apObj["signalStrength"] = ap.rssi;
        apObj["channel"] = ap.channel;
    }

    String requestBody;
    serializeJson(doc, requestBody);

    WiFiClientSecure client;
    // -------------------------------------------------------------------------
    // AVISO DE SEGURANÇA / BOAS PRÁTICAS:
    // Em nós de campo offline ou sem sincronização de relógio NTP/RTC, a validação
    // estrita de certificado SSL falhará devido ao ano base (1970).
    // Para protótipo e testes, usamos setInsecure().
    // Em produção com hora sincronizada, substitua por:
    // client.setCACert(GOOGLE_GTS_ROOT_R1_CA);
    // -------------------------------------------------------------------------
    client.setInsecure();

    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(_timeoutMs);
    http.addHeader("Content-Type", "application/json");
    // Atribuição de requisição obrigatória Google Maps Platform
    http.addHeader("X-Goog-Maps-Solution-ID", "gmp_git_agentskills_v1");

    Serial.printf("[WPS Client] Enviando POST HTTPS para Google Geolocation API (%u bytes)...\n", requestBody.length());
    int httpResponseCode = http.POST(requestBody);

    if (httpResponseCode == HTTP_CODE_OK) {
        String responsePayload = http.getString();
        Serial.println("[WPS Client] Resposta 200 OK recebida do Google Maps Platform.");

        JsonDocument responseDoc;
        DeserializationError err = deserializeJson(responseDoc, responsePayload);

        if (!err) {
            result.lat = responseDoc["location"]["lat"] | 0.0;
            result.lng = responseDoc["location"]["lng"] | 0.0;
            result.accuracy = responseDoc["accuracy"] | 999.0;
            result.valid = (result.lat != 0.0 || result.lng != 0.0);
            result.timestamp = millis();

            snprintf(result.formatted_lat, sizeof(result.formatted_lat), "%.5f", result.lat);
            snprintf(result.formatted_lng, sizeof(result.formatted_lng), "%.5f", result.lng);

            Serial.printf("[WPS Client] Geoposicionamento obtido com sucesso!\n");
            Serial.printf("   📍 Latitude:  %s\n", result.formatted_lat);
            Serial.printf("   📍 Longitude: %s\n", result.formatted_lng);
            Serial.printf("   🎯 Precisão:  %.1f metros\n", result.accuracy);
        } else {
            Serial.printf("[WPS Client] Erro ao parsear JSON de resposta: %s\n", err.c_str());
        }
    } else {
        Serial.printf("[WPS Client] Falha na requisição HTTP: código %d (%s)\n",
                      httpResponseCode, http.errorToString(httpResponseCode).c_str());
        if (httpResponseCode > 0) {
            String errBody = http.getString();
            Serial.printf("[WPS Client] Resposta de erro: %s\n", errBody.c_str());
        }
    }

    http.end();
    return result;
}

GeoLocation GeolocationClient::queryUnwiredApi(const std::vector<WifiApRecord>& aps) {
    GeoLocation result = {0.0, 0.0, 0.0, false, "", "", 0};
    String url = "https://us1.unwiredlabs.com/v2/process.php";

    // Serialização com ArduinoJson v7 para o formato UnwiredLabs
    JsonDocument doc;
    doc["token"] = _apiKey;
    doc["address"] = 0;

    JsonArray wifiArray = doc["wifi"].to<JsonArray>();
    for (const auto& ap : aps) {
        JsonObject apObj = wifiArray.add<JsonObject>();
        apObj["bssid"] = ap.bssid;
        apObj["channel"] = ap.channel;
        apObj["signal"] = ap.rssi;
    }

    String requestBody;
    serializeJson(doc, requestBody);

    WiFiClientSecure client;
    client.setInsecure(); // Prototipagem / Sem validação estrita de data

    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(_timeoutMs);
    http.addHeader("Content-Type", "application/json");

    Serial.printf("[WPS Client] Enviando POST HTTPS para UnwiredLabs API (%u bytes)...\n", requestBody.length());
    int httpResponseCode = http.POST(requestBody);

    if (httpResponseCode == HTTP_CODE_OK) {
        String responsePayload = http.getString();
        JsonDocument responseDoc;
        DeserializationError err = deserializeJson(responseDoc, responsePayload);

        if (!err && String((const char*)responseDoc["status"]) == "ok") {
            result.lat = responseDoc["lat"] | 0.0;
            result.lng = responseDoc["lon"] | 0.0;
            result.accuracy = responseDoc["accuracy"] | 999.0;
            result.valid = (result.lat != 0.0 || result.lng != 0.0);
            result.timestamp = millis();

            snprintf(result.formatted_lat, sizeof(result.formatted_lat), "%.5f", result.lat);
            snprintf(result.formatted_lng, sizeof(result.formatted_lng), "%.5f", result.lng);

            Serial.printf("[WPS Client] Posição obtida via UnwiredLabs: Lat %s, Lng %s (Precisão: %.1fm)\n",
                          result.formatted_lat, result.formatted_lng, result.accuracy);
        } else {
            Serial.printf("[WPS Client] Falha ou status de erro UnwiredLabs: %s\n", responseDoc["message"] | "Erro desconhecido");
        }
    } else {
        Serial.printf("[WPS Client] HTTP Erro UnwiredLabs: %d\n", httpResponseCode);
    }

    http.end();
    return result;
}

// ==============================================================================
// 3. GERENCIADOR DE LOCALIZAÇÃO DO NÓ ARES (SCAN + API + NVS STORAGE)
// ==============================================================================
AresLocationManager::AresLocationManager() : _initialized(false) {
    memset(&_currentLocation, 0, sizeof(_currentLocation));
}

void AresLocationManager::begin() {
    if (_initialized) return;

    // Tenta carregar a última localização persistida na memória flash NVS
    GeoLocation savedLoc;
    if (loadFromNvs(savedLoc)) {
        _currentLocation = savedLoc;
        snprintf(g_ares_lat, sizeof(g_ares_lat), "%s", savedLoc.formatted_lat);
        snprintf(g_ares_lon, sizeof(g_ares_lon), "%s", savedLoc.formatted_lng);
        Serial.printf("[AresLocationManager] Localização prévia restaurada da Flash NVS: Lat %s, Lon %s (Precisão: %.1fm)\n",
                      g_ares_lat, g_ares_lon, _currentLocation.accuracy);
    } else {
        // Fallback para coordenadas padrão de contingência
        snprintf(g_ares_lat, sizeof(g_ares_lat), "%s", NODE_LAT);
        snprintf(g_ares_lon, sizeof(g_ares_lon), "%s", NODE_LON);
        Serial.printf("[AresLocationManager] Nenhuma posição NVS encontrada. Usando contingência inicial: Lat %s, Lon %s\n",
                      g_ares_lat, g_ares_lon);
    }

    _initialized = true;
}

void AresLocationManager::saveToNvs(const GeoLocation& loc) {
    if (!loc.valid) return;

    Preferences prefs;
    if (prefs.begin("ares_geo", false)) {
        prefs.putDouble("lat", loc.lat);
        prefs.putDouble("lng", loc.lng);
        prefs.putDouble("acc", loc.accuracy);
        prefs.putString("flat", loc.formatted_lat);
        prefs.putString("flng", loc.formatted_lng);
        prefs.putBool("valid", true);
        prefs.putUInt("ts", loc.timestamp);
        prefs.end();
        Serial.printf("[AresLocationManager] Posição geográfica persistida com sucesso na memória NVS!\n");
    }
}

bool AresLocationManager::loadFromNvs(GeoLocation& loc) {
    Preferences prefs;
    if (!prefs.begin("ares_geo", true)) {
        return false;
    }

    bool valid = prefs.getBool("valid", false);
    if (!valid) {
        prefs.end();
        return false;
    }

    loc.lat = prefs.getDouble("lat", 0.0);
    loc.lng = prefs.getDouble("lng", 0.0);
    loc.accuracy = prefs.getDouble("acc", 999.0);
    String flat = prefs.getString("flat", "");
    String flng = prefs.getString("flng", "");
    loc.valid = true;
    loc.timestamp = prefs.getUInt("ts", 0);

    memset(loc.formatted_lat, 0, sizeof(loc.formatted_lat));
    memset(loc.formatted_lng, 0, sizeof(loc.formatted_lng));
    strncpy(loc.formatted_lat, flat.c_str(), sizeof(loc.formatted_lat) - 1);
    strncpy(loc.formatted_lng, flng.c_str(), sizeof(loc.formatted_lng) - 1);

    prefs.end();
    return true;
}

bool AresLocationManager::performWpsUpdate(const char* wifiSsid,
                                          const char* wifiPass,
                                          WpsProvider provider,
                                          const char* apiKey) {
    Serial.println("\n[AresLocationManager] ======================================");
    Serial.println("[AresLocationManager] Iniciando ciclo dinâmico de geolocalização WPS...");

    // 1. Executa a varredura das redes Wi-Fi ao redor (BSSIDs + RSSI)
    std::vector<WifiApRecord> aps = WifiApScanner::scanTopAccessPoints(8, 2);

    if (aps.size() < 2) {
        Serial.println("[AresLocationManager] Redes insuficientes ao redor para WPS.");
        return false;
    }

    // 2. Se credenciais de Wi-Fi de acesso à Internet foram passadas e ainda não estamos conectados:
    bool temporaryConnection = false;
    if (wifiSsid && strlen(wifiSsid) > 0 && WiFi.status() != WL_CONNECTED) {
        Serial.printf("[AresLocationManager] Conectando temporariamente ao Wi-Fi '%s' para consulta à nuvem...\n", wifiSsid);
        WiFi.begin(wifiSsid, wifiPass ? wifiPass : "");

        unsigned long startConnect = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - startConnect < 10000)) {
            delay(250);
            Serial.print(".");
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[AresLocationManager] Conectado à Internet! IP: %s\n", WiFi.localIP().toString().c_str());
            temporaryConnection = true;
        } else {
            Serial.println("[AresLocationManager] Falha ao conectar ao Wi-Fi com internet.");
            return false;
        }
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[AresLocationManager] Sem conectividade IP ativa para consultar a API.");
        return false;
    }

    // 3. Consulta a API de Geolocalização
    const char* keyToUse = apiKey ? apiKey : "";
    GeolocationClient client(provider, keyToUse);
    GeoLocation loc = client.resolveLocation(aps);

    if (loc.valid) {
        _currentLocation = loc;
        snprintf(g_ares_lat, sizeof(g_ares_lat), "%s", loc.formatted_lat);
        snprintf(g_ares_lon, sizeof(g_ares_lon), "%s", loc.formatted_lng);

        // 4. Salva permanentemente no NVS para resistir a reboots e operações offline
        saveToNvs(loc);

        Serial.printf("[AresLocationManager] Localização dinâmica aplicada ao Nó ARES: Lat %s | Lon %s\n",
                      g_ares_lat, g_ares_lon);
    }

    // 5. Se foi uma conexão temporária de campo, desconecta para retornar ao rádio mesh
    if (temporaryConnection) {
        Serial.println("[AresLocationManager] Desconectando da rede de upload e retornando ao canal ARES...");
        WiFi.disconnect(true);
        delay(200);
    }

    return loc.valid;
}
