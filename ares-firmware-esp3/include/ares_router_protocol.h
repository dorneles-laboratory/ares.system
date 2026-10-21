#ifndef ARES_ROUTER_PROTOCOL_H
#define ARES_ROUTER_PROTOCOL_H

#include <stdint.h>
#include "ares_protocol.h"

// Pacotes de controle trocados só entre Routers. As mensagens de dados continuam sendo o
// AresMessagePacket do ares_protocol.h, sem nenhuma mudança. Os pacotes de controle têm
// outro tamanho, então o ESP2 os descarta pela validação de tamanho que ele já faz.

#define ARES_CTRL_MAGIC_0 'A'
#define ARES_CTRL_MAGIC_1 'R'
#define ARES_CTRL_VERSION 1

typedef enum {
    CTRL_BEACON = 1,                   // "Estou aqui e fico a N saltos do Gateway"
    CTRL_CUSTODY_ACK = 2               // "Guardei a sua mensagem, pode liberar a vaga"
} AresControlType;

typedef enum {
    ROLE_ROUTER = 2,
    ROLE_GATEWAY = 3                   // Reservado para quando o ESP2 também anunciar beacons
} AresNodeRole;

#define HOPS_UNREACHABLE 255           // Sem rota até o Gateway
#define BATTERY_UNKNOWN 255

typedef struct __attribute__((packed)) {
    uint8_t magic[2];                  // 'A','R': separa controle de lixo de rádio
    uint8_t version;
    uint8_t type;                      // AresControlType
} AresControlHeader;

typedef struct __attribute__((packed)) {
    AresControlHeader header;
    uint8_t role;                      // AresNodeRole
    uint8_t hopsToGateway;             // 1 = alcança o ESP2 direto; HOPS_UNREACHABLE = sem rota
    uint8_t batteryPct;                // 0-100 ou BATTERY_UNKNOWN
    uint8_t queueFree;                 // Vagas livres na fila (quem está cheio não deve receber)
    uint8_t nextHop[6];                // Por onde este Router chega ao Gateway (split horizon)
    uint32_t sequence;                 // Contador de beacons deste Router
} AresBeaconPacket;

typedef struct __attribute__((packed)) {
    AresControlHeader header;
    char msgId[20];                    // Mesma chave da deduplicação: msg_id + source_id + timestamp
    char sourceId[12];
    uint32_t timestamp;
} AresCustodyAckPacket;

// Garantias em tempo de compilação: os pacotes de controle nunca podem ter o tamanho de uma
// mensagem de dados, senão o ESP2 tentaria convertê-los em JSON
static_assert(sizeof(AresBeaconPacket) != sizeof(AresMessagePacket), "Beacon não pode ter o tamanho do AresMessagePacket");
static_assert(sizeof(AresCustodyAckPacket) != sizeof(AresMessagePacket), "ACK não pode ter o tamanho do AresMessagePacket");
static_assert(sizeof(((AresCustodyAckPacket *) 0)->msgId) == sizeof(((AresHeader *) 0)->msg_id), "msgId deve casar com AresHeader.msg_id");
static_assert(sizeof(((AresCustodyAckPacket *) 0)->sourceId) == sizeof(((AresHeader *) 0)->source_id), "sourceId deve casar com AresHeader.source_id");

#endif
