#ifndef ARES_PROTOCOL_H
#define ARES_PROTOCOL_H

// ============================================================================
// Mensagem de DADOS do ARES (AresMessagePacket, 156 bytes).
//
// ESTE ARQUIVO É IGUAL NOS TRÊS FIRMWARES (ESP1 Storage, ESP2 Gateway, ESP3 Router)
// e os campos são os mesmos do repositório oficial (dorneles-laboratory/ares.system).
// Mudar um campo aqui quebra a compatibilidade com quem não for regravado.
// O script tools/verificar_protocolo.py confere se as três cópias estão idênticas.
//
// Dica: os campos de texto têm tamanho fixo e PRECISAM caber o '\0' do final.
// Ex.: source_id[12] aceita no máximo 11 letras ("ESP1-EMISSOR" tem 12 e seria cortado).
// ============================================================================

#include <Arduino.h>

// Níveis de Prioridade do ARES
typedef enum {
    P0 = 0,  // P0: Emergência / Risco de Vida (sai sempre primeiro e nunca perde vaga)
    P1 = 1,
    P2 = 2,
    P3 = 3,
    P4 = 4   // P4: Telemetria / Tráfego Normal (primeira a ceder vaga quando a memória enche)
} AresPriorityLevel;

// Estrutura do Cabeçalho baseada nas especificações do ARES
typedef struct {
    char msg_id[20];            // ID único da mensagem (no Storage: "<MAC curto>-<sequência>")
    char source_id[12];         // Quem criou a mensagem (máx. 11 caracteres + '\0')
    char destination_id[12];    // Destino (ex: "GATEWAY")
    AresPriorityLevel priority; // P0 ... P4
    uint8_t ttl;                // Saltos restantes: cada nó desconta 1 e não repassa quando chega a 0
    unsigned long timestamp;    // Carimbo de emissão (no Storage: segundos do relógio do nó)
} AresHeader;

// Estrutura da telemetria do nó
typedef struct {
    float battery_level;       // Nível da bateria em % (negativo = desconhecido)
    uint32_t storage_avail_kb; // Armazenamento disponível em KB
} AresTelemetry;

// Estrutura de dados de localização (texto, para não depender de float entre placas)
typedef struct {
    char lat[10];    // Latitude
    char lon[10];    // Longitude ('long' é palavra reservada do C++)
    char altura[8];  // Altura
} AresLocation;

// Estrutura do Payload de Texto Livre
typedef struct {
    char text_message[64]; // Frase ou mensagem enviada
} AresPayloadText;

// Pacote Completo
typedef struct {
    AresHeader header;
    AresTelemetry telemetry;
    AresLocation location;
    AresPayloadText payload;
} AresMessagePacket;

#endif
