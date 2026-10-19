#ifndef ARES_PROTOCOL_H
#define ARES_PROTOCOL_H

#include <Arduino.h>

// Níveis de Prioridade do ARES (Referência conceitual)
typedef enum {
    P0 = 0,  // P0: Emergência / Risco de Vida
    P1 = 1,
    P2 = 2,
    P3 = 3,
    P4 = 4  // P4: Telemetria / Tráfego Normal
} AresPriorityLevel;

// Estrutura do Cabeçalho baseada nas especificações do ARES
typedef struct {
    char msg_id[20];            // UUID string (ex: "123e4567-e89b...")
    char source_id[12];         // MAC Address de origem
    char destination_id[12];    // Destino (ex: "BROADCAST")
    AresPriorityLevel priority; // Ex: "P4"
    uint8_t ttl;                // Time To Live (ex: 64)
    unsigned long timestamp;    // Timestamp de emissão
} AresHeader;

// Estrutura da telemetria do nó
typedef struct {
    float battery_level;       // Nível da bateria (ex: 98.2)
    uint32_t storage_avail_kb; // Armazenamento disponível em KB
} AresTelemetry;

// Estrutura de dados de localização (Ajustado 'long' para 'lon')
typedef struct {
    char lat[10];    // Latitude
    char lon[10];    // Longitude (Evita palavra reservada do C++)
    char altura[8]; // Altura
} AresLocation;

// Estrutura do Payload de Texto Livre
typedef struct {
    char text_message[64]; // Frase ou mensagem enviada
} AresPayloadText;

// Pacote Completo do Nó Emissor
typedef struct {
    AresHeader header;
    AresTelemetry telemetry;
    AresLocation location;
    AresPayloadText payload;
} AresMessagePacket;

#endif