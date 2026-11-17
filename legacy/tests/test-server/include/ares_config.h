#ifndef ARES_CONFIG_H
#define ARES_CONFIG_H

// Tenta carregar o arquivo de segredos local
#if __has_include("ares_credentials.h")
    #include "ares_credentials.h"
    #pragma message("[ARES CONFIG] Arquivo 'ares_credentials.h' carregado com sucesso.")
#else
    #pragma message("[AVISO ARES CONFIG] 'ares_credentials.h' NAO encontrado. Usando valores PADRAO.")
#endif

// Valores padrão caso o arquivo de segredos não exista
#ifndef WIFI_SSID
    #define WIFI_SSID "DEFAULT_SSID"
#endif

#ifndef WIFI_PASSWORD
    #define WIFI_PASSWORD "DEFAULT_PASSWORD"
#endif

#ifndef INGRESS_SERVER_IP
    #define INGRESS_SERVER_IP "192.168.1.50"
#endif

#define INGRESS_SERVER_PORT 8080
#define GATEWAY_SOURCE_ID "GATEWAY"
#define GATEWAY_DEBUG true

#endif