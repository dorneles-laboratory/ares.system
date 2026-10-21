#ifndef NEIGHBOR_TABLE_H
#define NEIGHBOR_TABLE_H

#include <stdint.h>
#include "ares_router_protocol.h"

// Vizinhos descobertos pelos beacons. Tamanho fixo (MAX_NEIGHBORS), sem malloc.
typedef struct {
    bool used;
    uint8_t mac[6];
    uint8_t role;                      // AresNodeRole
    uint8_t hopsToGateway;             // Como o vizinho anunciou
    uint8_t nextHop[6];                // Por onde o vizinho chega ao Gateway
    int8_t rssi;                       // Sinal do último beacon (0 = desconhecido)
    uint8_t batteryPct;
    uint8_t queueFree;
    uint32_t lastSeenMs;
    uint8_t failedRounds;              // Rodadas de envio seguidas sem confirmação
} AresNeighbor;

typedef enum {
    NEIGHBOR_UPDATED = 0,
    NEIGHBOR_ADDED,
    NEIGHBOR_TABLE_FULL
} AresNeighborUpdate;

AresNeighborUpdate neighborUpdate(const uint8_t *mac, const AresBeaconPacket *beacon, int8_t rssi, uint32_t now);
AresNeighbor *neighborFind(const uint8_t *mac);
// Para percorrer a tabela: retorna nullptr nas posições vazias
AresNeighbor *neighborAt(int index);
void neighborRemove(const uint8_t *mac);
// Remove quem ficou sem beacon por mais de timeoutMs. Copia os MACs removidos para "removed"
// (até maxRemoved) e retorna quantos saíram.
int neighborExpire(uint32_t now, uint32_t timeoutMs, uint8_t removed[][6], int maxRemoved);
// Retorna quantas rodadas seguidas já falharam com esse vizinho
uint8_t neighborMarkFailure(const uint8_t *mac);
void neighborMarkSuccess(const uint8_t *mac);
int neighborCount();

#endif
