#ifndef ARES_CONFIG_H
#define ARES_CONFIG_H

// ============================================================================
// Configuração do ESP1 (nó STORAGE).
//
// O Storage cria mensagens (telemetria automática e texto digitado no Serial),
// também aceita mensagens de outros nós pelo rádio, GUARDA TUDO NA FLASH DO PRÓPRIO
// ESP32 (LittleFS, sem microSD) e só apaga uma mensagem depois que o próximo nó
// (Router ou Gateway) confirma que ficou com ela (ACK de custódia).
// ============================================================================

// Tenta carregar o arquivo de segredos local (opcional no Storage)
#if __has_include("ares_credentials.h")
    #include "ares_credentials.h"
    #pragma message("[ARES CONFIG] Arquivo 'ares_credentials.h' carregado com sucesso.")
#else
    #pragma message("[AVISO ARES CONFIG] 'ares_credentials.h' NAO encontrado. Usando valores PADRAO.")
#endif

// --- IDENTIDADE ---
// O source_id vira "STG-" + os 3 últimos bytes do MAC (ex.: "STG-A1B2C3"): único por placa
// e com 10 caracteres, cabendo no source_id[12] (o antigo "ESP1-EMISSOR" tinha 12 e era cortado).
#define NODE_SOURCE_PREFIX "STG-"
#define MESSAGE_DESTINATION "GATEWAY"
#define DEFAULT_TTL 16                 // Saltos que a mensagem pode dar (mesmo valor do MAX_ROUTE_HOPS do Router)

// --- RÁDIO ---
#define ESPNOW_CHANNEL 2               // TEM que ser o mesmo canal do Router e do Wi-Fi/Hotspot do ESP2
#define RADIO_LOW_TX_POWER false       // true = potência baixa (bancada / problema de brownout na USB)
#define RX_QUEUE_LEN 16                // Pacotes esperando o loop() processar

// --- TELEMETRIA AUTOMÁTICA (P4) ---
// Cada mensagem é gravada na flash. 30 s dá ~2.900 gravações/dia, tranquilo para a flash.
// Use 0 para desligar e mandar só o que você digitar no Serial.
#define TELEMETRY_INTERVAL_MS 30000
// Sem GNSS: posição fixa de onde o nó foi instalado (texto, como no protocolo)
#define LOCATION_LAT "-29.6842"
#define LOCATION_LON "-53.8069"
#define LOCATION_ALT "150.5"

// --- ARMAZENAMENTO NA FLASH (LittleFS, partição "spiffs" do esquema padrão de 4 MB) ---
// 2.000 mensagens (RF-NU-17) x 180 bytes = ~352 KB de flash e ~40 KB de RAM para o índice.
// O ESP32 comum (sem PSRAM) aguenta bem. Não passe de ~5.000 sem medir a RAM livre.
#define STORE_CAPACITY 2000
#define MESSAGE_MAX_AGE_SEC (7UL * 24UL * 60UL * 60UL) // Validade: 7 dias guardada (RF-NR-05)
#define EXPIRE_CHECK_INTERVAL_MS 60000                 // De quanto em quanto tempo procura vencidas
#define CLOCK_SAVE_INTERVAL_MS 60000                   // Salva o relógio do nó na NVS a cada 1 min
#define EVENT_LOG_MAX_BYTES 32768                      // Registro de eventos: ao passar disso, vira .old

// --- VIZINHOS E PRÓXIMO SALTO ---
#define BEACON_INTERVAL_MS 5000        // O Storage também se anuncia (para o Router mandar o ACK de custódia)
#define NEIGHBOR_TIMEOUT_MS 15000      // Vizinho sem beacon por 3 intervalos sai da tabela
#define MAX_NEIGHBORS 8
#define NEIGHBOR_FAIL_ROUNDS 2         // Rodadas de envio seguidas sem confirmação que derrubam um vizinho
#define MAX_ROUTE_HOPS 16              // Vizinho anunciando mais que isso conta como "sem rota"

// --- ENVIO E CUSTÓDIA ---
#define SEND_CONFIRM_TIMEOUT_MS 300    // Espera máxima pela confirmação do rádio
#define CUSTODY_TIMEOUT_MS 800         // Espera máxima pelo ACK de custódia do Router/Gateway
#define MAX_TX_ATTEMPTS 3              // Tentativas seguidas antes de devolver a mensagem à fila
#define RETRY_BACKOFF_MS 3000          // Pausa antes de tentar de novo uma mensagem devolvida
#define DEDUP_SIZE 64                  // Últimas mensagens entregues lembradas para descartar duplicatas

// --- ENERGIA ---
#define BATTERY_ADC_PIN -1             // Pino do divisor de tensão da bateria; -1 = sem medição
#define BATTERY_DIVIDER_RATIO 2.0f     // Tensão da bateria / tensão no pino (divisor 1:1 = 2.0)

// --- COMANDOS PELO SERIAL ---
#define SERIAL_LINE_IDLE_MS 100        // Sem Enter: a linha vale depois desse tempo sem receber nada

// --- CONFIGURAÇÕES DE DEBUG ---
#define DEBUG_PIN 4                    // Pino físico para ativar o modo debug (Ligar em 3.3V)
#define FORCE_SOFTWARE_DEBUG true      // true = logs ativados por código, false = silenciado (salvo se o pino físico estiver ativo)

#endif
