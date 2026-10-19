#ifndef ARES_CONFIG_H
#define ARES_CONFIG_H

// Tenta carregar o arquivo de segredos local
#if __has_include("ares_credentials.h")
    #include "ares_credentials.h"
    #pragma message("[ARES CONFIG] Arquivo 'ares_credentials.h' carregado com sucesso.")
#else
    #pragma message("[AVISO ARES CONFIG] 'ares_credentials.h' NAO encontrado. Usando valores PADRAO.")
#endif

// MAC Address do próximo salto (o ESP2 Gateway). Substitua pelo MAC real no ares_credentials.h
#ifndef GATEWAY_MAC
    #define GATEWAY_MAC {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}
#endif

// --- CONFIGURAÇÕES DE RÁDIO ---
#define ESPNOW_CHANNEL 2               // Mesmo canal fixo do ESP1 (o ESP2 segue o canal do Hotspot)

// --- CONFIGURAÇÕES DE ROTEAMENTO ---
#define SEND_CONFIRM_TIMEOUT_MS 300    // Espera máxima pela confirmação do rádio de um envio
#define MAX_TX_ATTEMPTS 3              // Tentativas seguidas antes de devolver a mensagem à fila
#define RETRY_BACKOFF_MS 3000          // Pausa antes de tentar de novo uma mensagem devolvida

// --- MEMÓRIA (tamanhos fixos, sem malloc no loop) ---
#define STORE_SLOTS 24                 // Mensagens guardadas, somando P0-P4
#define DEDUP_SIZE 64                  // Últimas mensagens lembradas para descartar duplicatas
#define RX_QUEUE_LEN 16                // Pacotes esperando o loop() processar

// --- COMANDOS PELO SERIAL ---
#define SERIAL_LINE_IDLE_MS 100        // Sem Enter: a linha vale depois desse tempo sem receber nada

// --- CONFIGURAÇÕES DE DEBUG ---
#define DEBUG_PIN 4                    // Pino físico para ativar o modo debug (Ligar em 3.3V)
#define FORCE_SOFTWARE_DEBUG true      // true = logs ativados por código, false = silenciado (salvo se o pino físico estiver ativo)

#endif
