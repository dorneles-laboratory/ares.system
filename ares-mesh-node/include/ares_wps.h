#ifndef ARES_WPS_H
#define ARES_WPS_H

#include <Arduino.h>
#include <vector>

// ==============================================================================
// PROVEDORES DE GEOLOCALIZAÇÃO SUPORTADOS
// ==============================================================================
enum WpsProvider {
    WPS_PROVIDER_GOOGLE = 1,    // Google Maps Geolocation API
    WPS_PROVIDER_UNWIRED = 2    // UnwiredLabs LocationAPI
};

// ==============================================================================
// ESTRUTURA TIPADA DE GEOLOCALIZAÇÃO RETORNADA
// ==============================================================================
struct GeoLocation {
    double lat;             // Latitude (ex: -29.71340)
    double lng;             // Longitude (ex: -53.71580)
    double accuracy;        // Raio de precisão em metros (ex: 25.0)
    bool valid;             // True se obtido com sucesso
    char formatted_lat[12]; // String formatada para o protocolo ARES
    char formatted_lng[12]; // String formatada para o protocolo ARES
    uint32_t timestamp;     // Momento da leitura (millis ou epoch)
};

// Informações básicas de um ponto de acesso Wi-Fi vizinho
struct WifiApRecord {
    String bssid;           // Formato "XX:XX:XX:XX:XX:XX"
    int32_t rssi;           // Intensidade de sinal em dBm (ex: -55)
    int32_t channel;        // Canal Wi-Fi (1 a 13)
};

// ==============================================================================
// 1. SCANNER WI-FI (VARREDURA PASSIVA E FILTRAGEM DE RSSI)
// ==============================================================================
class WifiApScanner {
public:
    // Executa varredura (incluindo redes ocultas) e filtra de 5 a 8 APs mais fortes
    static std::vector<WifiApRecord> scanTopAccessPoints(int maxAps = 8, int minAps = 2);
};

// ==============================================================================
// 2. CLIENTE HTTP/HTTPS PARA API DE GEOLOCALIZAÇÃO
// ==============================================================================
class GeolocationClient {
public:
    GeolocationClient(WpsProvider provider = WPS_PROVIDER_GOOGLE, const char* apiKey = "");

    void setProvider(WpsProvider provider) { _provider = provider; }
    void setApiKey(const char* apiKey) { _apiKey = apiKey; }
    void setTimeoutMs(uint32_t timeoutMs) { _timeoutMs = timeoutMs; }

    // Envia os APs coletados e retorna a GeoLocation resolvida
    GeoLocation resolveLocation(const std::vector<WifiApRecord>& aps);

private:
    WpsProvider _provider;
    String _apiKey;
    uint32_t _timeoutMs;

    GeoLocation queryGoogleApi(const std::vector<WifiApRecord>& aps);
    GeoLocation queryUnwiredApi(const std::vector<WifiApRecord>& aps);
};

// ==============================================================================
// 3. GERENCIADOR DE LOCALIZAÇÃO DO NÓ ARES (SCAN + API + NVS STORAGE)
// ==============================================================================
class AresLocationManager {
public:
    static AresLocationManager& getInstance() {
        static AresLocationManager instance;
        return instance;
    }

    // Inicializa carregando a última posição válida salva na memória flash NVS
    void begin();

    // Executa o ciclo completo de WPS:
    // 1. Conecta temporariamente a um Wi-Fi com internet (se credenciais fornecidas)
    // 2. Escaneia redes vizinhas
    // 3. Consulta a API na nuvem
    // 4. Salva a nova coordenada na NVS e atualiza as variáveis globais da malha
    bool performWpsUpdate(const char* wifiSsid = nullptr, 
                          const char* wifiPass = nullptr,
                          WpsProvider provider = WPS_PROVIDER_GOOGLE,
                          const char* apiKey = nullptr);

    // Retorna a localização corrente (resolvida via WPS ou da NVS ou fallback)
    const GeoLocation& getCurrentLocation() const { return _currentLocation; }

    // Salva / Recupera do NVS (Non-Volatile Storage)
    void saveToNvs(const GeoLocation& loc);
    bool loadFromNvs(GeoLocation& loc);

private:
    AresLocationManager();
    GeoLocation _currentLocation;
    bool _initialized;
};

#endif // ARES_WPS_H
