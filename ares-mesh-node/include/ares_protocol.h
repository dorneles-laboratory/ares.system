#ifndef ARES_PROTOCOL_H
#define ARES_PROTOCOL_H

#include <Arduino.h>

// ==============================================================================
// Níveis de Prioridade ARES (RFC ARES DTN-01)
// ==============================================================================
typedef enum {
    ARES_P0_EMERGENCY = 0, // P0: Emergência Extrema / Risco à Vida
    ARES_P1_RESCUE    = 1, // P1: Feridos Graves / Resgate Prioritário
    ARES_P2_SUPPLIES  = 2, // P2: Água / Mantimentos / Medicamentos
    ARES_P3_LOGISTICS = 3, // P3: Apoio Operacional / Abrigo
    ARES_P4_ROUTINE   = 4  // P4: Telemetria e Mensagens Gerais
} AresPriorityLevel;

// Papéis operacionais dos nós ARES
typedef enum {
    ARES_ROLE_GATEWAY    = 1, // Gateway: Ingress conectado à Base IP / Nuvem
    ARES_ROLE_ROUTER     = 2, // Router: Entrada com Captive Portal SOS
    ARES_ROLE_RELAY      = 3, // Relay: Repetidor e retransmissão ad-hoc
    ARES_ROLE_STORAGE    = 4, // Storage: Store-and-Forward persistente DTN
    ARES_ROLE_DRONE_MULE = 5  // Drone Mule: FANET móvel para drenagem aérea oportunística
} AresNodeRole;

#define ARES_ROLE_EDGE_SOS   ARES_ROLE_ROUTER
#define ARES_ROLE_EDGE_FIELD ARES_ROLE_ROUTER

// Direção da mensagem
typedef enum {
    ARES_DIR_UPSTREAM   = 1, // Do campo para a Base
    ARES_DIR_DOWNSTREAM = 2  // Da Base para o campo
} AresMessageDirection;

// Tipos de mensagem e controle DTN
typedef enum {
    ARES_MSG_DATA            = 1, // Dados normais / Alertas SOS / Telemetria
    ARES_MSG_CUSTODY_ACK     = 2, // Confirmação de custódia DTN (ACK de entrega)
    ARES_MSG_BULK_DRAIN_REQ  = 3, // Solicitação do Drone FANET para esvaziamento em rajada
    ARES_MSG_BULK_DRAIN_DONE = 4  // Finalização da rajada de drenagem
} AresMessageType;

// Flags de status de nó e protocolo
#define ARES_FLAG_ENERGY_LOW     0x01 // Bateria < 20% (EAR: recusa tráfego P3/P4)
#define ARES_FLAG_STORED_IN_WAL  0x02 // Pacote persistido em LittleFS flash
#define ARES_FLAG_AUTHENTICATED  0x04 // Integridade/Autenticidade verificada

// ==============================================================================
// Estruturas do Pacote Binário ARES (Compacto para transmissão via ESP-NOW)
// Tamanho total alinhado e otimizado para MTU do ESP-NOW (<= 250 bytes)
// ==============================================================================

#pragma pack(push, 1)

typedef struct {
    char msg_id[32];               // Identificador único (UUID ou prefixo)
    char source_id[20];            // MAC ou ID do nó emissor ("ESP32_ROUTER", "ESP32_GATEWAY", etc.)
    char destination_id[20];       // Destino ("BASE", "BROADCAST", etc.)
    uint8_t priority;              // AresPriorityLevel (0 a 4)
    uint8_t direction;             // AresMessageDirection (1 = Up, 2 = Down)
    uint8_t msg_type;              // AresMessageType (1=Data, 2=CustodyACK, 3=BulkDrainReq, 4=BulkDrainDone)
    uint8_t flags;                 // Bitmask (ARES_FLAG_*)
    uint8_t ttl;                   // Time-to-Live (decrementado a cada salto)
    uint8_t hop_count;             // Contador de saltos
    uint32_t timestamp;            // Timestamp UNIX aproximado ou millis
} AresHeader;

typedef struct {
    float battery_level;           // Nível de bateria (0.0 a 100.0%)
    uint32_t storage_avail_kb;     // Espaço livre em memória
    int8_t last_rssi;              // RSSI do último enlace
} AresTelemetry;

typedef struct {
    char lat[10];                  // Ex: "-29.7134"
    char lon[10];                  // Ex: "-53.7158"
    char altura[8];                // Ex: "110m"
} AresLocation;

typedef struct {
    char text_message[96];         // Mensagem de texto livre (até 95 chars + null)
} AresPayload;

// Pacote binário unificado transmitido pelo rádio ESP-NOW
typedef struct {
    AresHeader header;
    AresTelemetry telemetry;
    AresLocation location;
    AresPayload payload;
    uint8_t hops_trail[6];         // Registra sequência de IDs dos nós percorridos (ex: [1, 2, 3, 4, 6])
} AresMessagePacket;

#pragma pack(pop)

#endif // ARES_PROTOCOL_H
