#ifndef ARES_CONTROL_PROTOCOL_H
#define ARES_CONTROL_PROTOCOL_H

// ============================================================================
// Pacotes de CONTROLE do ARES (beacon e ACK de custódia).
//
// ESTE ARQUIVO É IGUAL NOS TRÊS FIRMWARES (ESP1 Storage, ESP2 Gateway, ESP3 Router).
// Se mudar aqui, copie para os outros dois. O script tools/verificar_protocolo.py
// confere se as cópias estão idênticas.
//
// As mensagens de DADOS continuam sendo o AresMessagePacket do ares_protocol.h.
// O que separa um tipo do outro é o TAMANHO do pacote: os pacotes de controle
// nunca podem ter o tamanho do AresMessagePacket (garantido pelos static_assert lá embaixo).
// ============================================================================

#include <stdint.h>
#include "ares_protocol.h"

#define ARES_CTRL_MAGIC_0 'A'
#define ARES_CTRL_MAGIC_1 'R'
#define ARES_CTRL_VERSION 1

typedef enum {
    CTRL_BEACON = 1,                   // "Estou aqui, sou tal papel e fico a N saltos do Gateway"
    CTRL_CUSTODY_ACK = 2               // "Guardei a sua mensagem, pode liberar a vaga"
} AresControlType;

// Papel de quem mandou o beacon. Quem escolhe o próximo salto só usa ROUTER e GATEWAY:
// o Storage guarda mensagens, mas não as leva até o Gateway por outros nós.
typedef enum {
    ROLE_STORAGE = 1,                  // ESP1: cria/guarda mensagens na flash e entrega à rede
    ROLE_ROUTER = 2,                   // ESP3: repassa mensagens em direção ao Gateway
    ROLE_GATEWAY = 3                   // ESP2: destino final no rádio, envia por HTTP ao servidor
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
    uint8_t hopsToGateway;             // 0 = sou o Gateway; 1 = alcanço o ESP2 direto; HOPS_UNREACHABLE = sem rota
    uint8_t batteryPct;                // 0-100 ou BATTERY_UNKNOWN
    uint8_t queueFree;                 // Vagas livres na fila (quem está cheio não deve receber); máx. 255
    uint8_t nextHop[6];                // Por onde este nó chega ao Gateway (split horizon)
    uint32_t sequence;                 // Contador de beacons deste nó
} AresBeaconPacket;

typedef struct __attribute__((packed)) {
    AresControlHeader header;
    char msgId[20];                    // Mesma chave da deduplicação: msg_id + source_id + timestamp
    char sourceId[12];
    uint32_t timestamp;
} AresCustodyAckPacket;

// Garantias em tempo de compilação: os pacotes de controle nunca podem ter o tamanho de uma
// mensagem de dados, senão o receptor tentaria tratá-los como mensagem
static_assert(sizeof(AresBeaconPacket) != sizeof(AresMessagePacket), "Beacon não pode ter o tamanho do AresMessagePacket");
static_assert(sizeof(AresCustodyAckPacket) != sizeof(AresMessagePacket), "ACK não pode ter o tamanho do AresMessagePacket");
static_assert(sizeof(((AresCustodyAckPacket *) 0)->msgId) == sizeof(((AresHeader *) 0)->msg_id), "msgId deve casar com AresHeader.msg_id");
static_assert(sizeof(((AresCustodyAckPacket *) 0)->sourceId) == sizeof(((AresHeader *) 0)->source_id), "sourceId deve casar com AresHeader.source_id");
// ESP-NOW v1 (o do ESP32 comum) aceita no máximo 250 bytes por envio
static_assert(sizeof(AresMessagePacket) <= 250, "AresMessagePacket passou do limite de 250 bytes do ESP-NOW");

#endif
