#ifndef ARES_CONFIG_H
#define ARES_CONFIG_H

// Tenta carregar o arquivo de segredos local
#if __has_include("ares_credentials.h")
    #include "ares_credentials.h"
    #pragma message("[ARES CONFIG] Arquivo 'ares_credentials.h' carregado com sucesso.")
#else
    #pragma message("[AVISO ARES CONFIG] 'ares_credentials.h' NAO encontrado. Usando valores PADRAO.")
#endif

// MAC Address do ESP2 (Gateway). Substitua pelo MAC real no ares_credentials.h.
// Todos os Routers usam o mesmo: quem alcança o ESP2 entrega direto, os outros vão por vizinhos.
#ifndef GATEWAY_MAC
    #define GATEWAY_MAC {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}
#endif

// --- CONFIGURAÇÕES DE RÁDIO ---
#define ESPNOW_CHANNEL 2               // Mesmo canal fixo do ESP1 (o ESP2 segue o canal do Hotspot)

// --- DESCOBERTA DE VIZINHOS ---
#define BEACON_INTERVAL_MS 5000        // Cada Router anuncia a si mesmo a cada 5 s (broadcast)
#define NEIGHBOR_TIMEOUT_MS 15000      // Vizinho sem beacon por 3 intervalos sai da tabela
#define MAX_NEIGHBORS 8                // Tamanho fixo da tabela (o ESP-NOW aceita ~20 peers)
#define NEIGHBOR_FAIL_ROUNDS 2         // Rodadas de envio seguidas sem confirmação que derrubam um vizinho

// --- ROTEAMENTO ---
#define GATEWAY_ALIVE_MS 15000         // ESP2 é considerado alcançável por esse tempo após a última confirmação do rádio
#define GATEWAY_PROBE_INTERVAL_MS 5000 // Sem tráfego para o ESP2, testa o enlace com ele nesse intervalo
#define MAX_ROUTE_HOPS 16              // Distância maior que isso conta como "sem rota" (evita contagem ao infinito)

// --- ENVIO E CUSTÓDIA ---
#define SEND_CONFIRM_TIMEOUT_MS 300    // Espera máxima pela confirmação do rádio de um envio
#define CUSTODY_TIMEOUT_MS 500         // Espera máxima pelo ACK de custódia de outro Router
#define MAX_TX_ATTEMPTS 3              // Tentativas seguidas antes de devolver a mensagem à fila
#define RETRY_BACKOFF_MS 3000          // Pausa antes de tentar de novo uma mensagem devolvida
#define GATEWAY_SEND_SPACING_MS 1500   // Intervalo mínimo entre entregas ao ESP2: ele guarda um pacote por vez e
                                       // descarta o que chega durante o POST, mesmo com o rádio confirmando

// --- MEMÓRIA (tamanhos fixos, sem malloc no loop) ---
#define STORE_SLOTS 24                 // Mensagens guardadas, somando P0-P4
#define DEDUP_SIZE 64                  // Últimas mensagens lembradas para descartar duplicatas
#define RX_QUEUE_LEN 16                // Pacotes esperando o loop() processar
#define MESSAGE_MAX_AGE_MS (7UL * 24UL * 60UL * 60UL * 1000UL) // Validade local de uma mensagem guardada (RF-NR-05: 7 dias)

// --- ENERGIA ---
#define BATTERY_ADC_PIN -1             // Pino do divisor de tensão da bateria; -1 = sem medição (anuncia "desconhecida")
#define BATTERY_DIVIDER_RATIO 2.0f     // Tensão da bateria / tensão no pino (divisor 1:1 = 2.0)

// --- COMANDOS PELO SERIAL ---
#define SERIAL_LINE_IDLE_MS 100        // Sem Enter: a linha vale depois desse tempo sem receber nada

// --- CONFIGURAÇÕES DE DEBUG ---
#define DEBUG_PIN 4                    // Pino físico para ativar o modo debug (Ligar em 3.3V)
#define FORCE_SOFTWARE_DEBUG true      // true = logs ativados por código, false = silenciado (salvo se o pino físico estiver ativo)

#endif
