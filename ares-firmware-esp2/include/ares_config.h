#ifndef ARES_CONFIG_H
#define ARES_CONFIG_H

// ============================================================================
// Configuração do ESP2 (GATEWAY).
//
// O Gateway recebe as mensagens pelo rádio (do Router ou direto do Storage), GUARDA
// NA FLASH (igual ao Storage), confirma a custódia e envia cada uma por HTTP POST
// ao servidor Ingress. Só apaga da flash quando o servidor responde 2xx (200 OK).
// ============================================================================

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
    #define INGRESS_SERVER_IP "192.168.1.50"   // Pode ser trocado sem regravar: comando "ip" no Serial
#endif

#define INGRESS_SERVER_PORT 8080
#define GATEWAY_SOURCE_ID "ESP2-GATEWAY"
#define GATEWAY_DEBUG true

// --- RÁDIO ---
// O Gateway NÃO escolhe o canal: ele usa o canal do Wi-Fi/Hotspot em que conecta.
// Este é o canal em que o Storage e o Router estão travados. Se o Hotspot estiver em
// outro, o Gateway avisa no Serial (erro "perda de sincronismo" da wiki).
#define ESPNOW_CHANNEL 2
#define RADIO_LOW_TX_POWER false
#define RX_QUEUE_LEN 16
#define BEACON_INTERVAL_MS 5000        // O Gateway se anuncia (0 saltos) para Router e Storage o acharem

// --- ARMAZENAMENTO NA FLASH (igual ao Storage) ---
// Guarda o que chegou enquanto o Wi-Fi/servidor está fora (RF-NG-15: não rejeitar sem uplink).
#define STORE_CAPACITY 2000
#define MESSAGE_MAX_AGE_SEC (7UL * 24UL * 60UL * 60UL)
#define EXPIRE_CHECK_INTERVAL_MS 60000
#define CLOCK_SAVE_INTERVAL_MS 60000   // Salva o relógio do nó na NVS a cada 1 min
#define EVENT_LOG_MAX_BYTES 32768
#define DEDUP_SIZE 64                  // Últimas entregues ao servidor lembradas (idempotência, RF-NG-04)

// --- ENVIO HTTP ---
#define HTTP_TIMEOUT_MS 3000           // Espera máxima pela resposta do servidor
#define HTTP_RETRY_MIN_MS 2000         // Primeira pausa depois de um POST que falhou
#define HTTP_RETRY_MAX_MS 30000        // A pausa dobra a cada falha seguida, até este máximo
#define WIFI_RETRY_INTERVAL_MS 30000   // Sem Wi-Fi: tenta conectar de novo a cada 30 s
#define WIFI_CONNECT_TIMEOUT_MS 20000  // Tempo máximo esperando o Wi-Fi no boot

// --- COMANDOS PELO SERIAL ---
#define SERIAL_LINE_IDLE_MS 100

#endif
