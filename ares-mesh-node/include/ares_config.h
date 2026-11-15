#ifndef ARES_CONFIG_H
#define ARES_CONFIG_H

#include "ares_protocol.h"

// ==============================================================================
// CONFIGURAÇÕES GLOBAIS DO NÓ ARES (100% DINÂMICAS PARA N ESP32s)
// O identificador do nó é derivado automaticamente do MAC Address de hardware
// (ex: ESP32_4A1C8E). Se NODE_ID for definido em tempo de compilação, ele prevalece.
// ==============================================================================

extern char g_ares_node_id[32];
extern char g_ares_ssid[32];
extern char g_ares_lat[12];
extern char g_ares_lon[12];
extern char g_ares_altura[10];

#ifdef NODE_ID
    #define ARES_NODE_ID NODE_ID
#else
    #define ARES_NODE_ID (g_ares_node_id[0] != '\0' ? g_ares_node_id : "ESP32_DYNAMIC")
#endif

#define ARES_NODE_LAT (g_ares_lat[0] != '\0' ? g_ares_lat : NODE_LAT)
#define ARES_NODE_LON (g_ares_lon[0] != '\0' ? g_ares_lon : NODE_LON)
#define ARES_NODE_ALTURA (g_ares_altura[0] != '\0' ? g_ares_altura : NODE_ALTURA)

#ifndef NODE_ROLE
    #define NODE_ROLE ARES_ROLE_RELAY
#endif

#ifndef NODE_LAT
    #define NODE_LAT "-29.7134"
#endif

#ifndef NODE_LON
    #define NODE_LON "-53.7158"
#endif

#ifndef NODE_ALTURA
    #define NODE_ALTURA "105m"
#endif

// ==============================================================================
// CONFIGURAÇÕES DE GEOLOCALIZAÇÃO DINÂMICA VIA WI-FI (WPS)
// ==============================================================================
#ifndef GOOGLE_GEOLOCATION_API_KEY
    #define GOOGLE_GEOLOCATION_API_KEY "" // Insira sua chave ou passe via build_flags
#endif

#ifndef UNWIRED_API_KEY
    #define UNWIRED_API_KEY ""
#endif

#ifndef PROVISIONING_WIFI_SSID
    #define PROVISIONING_WIFI_SSID ""
#endif

#ifndef PROVISIONING_WIFI_PASS
    #define PROVISIONING_WIFI_PASS ""
#endif

// ==============================================================================
// CONFIGURAÇÕES DE RÁDIO ESP-NOW E WI-FI
// ==============================================================================
#define ARES_ESP_NOW_CHANNEL 1
#define FORCE_SOFTWARE_DEBUG true
#define BROADCAST_MAC {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}

// ==============================================================================
// CONFIGURAÇÕES DO PORTAL CATIVO (SSID Dinâmico por ESP32)
// ==============================================================================
#define CAPTIVE_PORTAL_IP         IPAddress(192, 168, 4, 1)
#define CAPTIVE_PORTAL_NETMASK    IPAddress(255, 255, 255, 0)
#define DNS_PORT                  53

// ==============================================================================
// CONFIGURAÇÕES DE GATEWAY (Conexão WAN com o Servidor ARES)
// ==============================================================================
#define GATEWAY_WIFI_SSID         "ARES_BASE_WIFI"
#define GATEWAY_WIFI_PASS         "aresresiliencia2026"
#define BACKEND_INGRESS_URL       "http://192.168.1.100:5026/api/ares/ingress"
#define M2M_API_KEY               "ares-secret-m2m-key-ufsm-2026"

// ==============================================================================
// PINAGEM DE LEDS INDICADORES DE STATUS
// ==============================================================================
#define PIN_LED_SYSTEM_ALIVE     2   // GPIO 2: LED Onboard / Indicador de que o ESP está funcionando
#define PIN_LED_PACKET_ACTIVITY  4   // GPIO 4: LED Indicador de que uma mensagem passou pelo ESP (15, 4, 5, 18, 19, 21, 22, 23)

// ==============================================================================
// CONFIGURAÇÕES DO STORE-AND-FORWARD (DTN)
// ==============================================================================
#define MAX_STORED_PACKETS        50
#define PACKET_RETENTION_MS       (3600 * 1000) // 1 hora de retenção em memória
#define BEACON_INTERVAL_MS        5000          // 5 segundos entre beacons

#endif // ARES_CONFIG_H
