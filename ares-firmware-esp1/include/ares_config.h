#ifndef ARES_CONFIG_H
#define ARES_CONFIG_H

// Tenta carregar o arquivo de segredos local
#if __has_include("ares_credentials.h")
    #include "ares_credentials.h"
    #pragma message("[ARES CONFIG] Arquivo 'ares_credentials.h' carregado com sucesso.")
#else
    #pragma message("[AVISO ARES CONFIG] 'ares_credentials.h' NAO encontrado. Usando valores PADRAO.")
#endif

// MAC Address de destino do Gateway (Substitua pelo MAC real do ESP2)
#ifndef GATEWAY_MAC
    #define GATEWAY_MAC {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}
#endif

// --- CONFIGURAÇÕES DE REDE E HARDWARE ---
#define NODE_SOURCE_ID "ESP1-EMISSOR"
#define TRANSMISSION_INTERVAL_MS 5000 // Intervalo de envio em milissegundos (3s)

// --- CONFIGURAÇÕES DE DEBUG ---
#define DEBUG_PIN 4                    // Pino físico para ativar o modo debug (Ligar em 3.3V)
#define FORCE_SOFTWARE_DEBUG true      // true = logs ativados por código, false = silenciado (salvo se o pino físico estiver ativo)

#endif