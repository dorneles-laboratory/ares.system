#include <string.h>
#include "../../include/ares_config.h"
#include "../../include/ares_control_protocol.h"
#include "../../include/neighbor_table.h"
#include "../../include/next_hop.h"

// Valores desconhecidos entram como medianos para não ganhar nem perder desempates
static int rssiScore(const AresNeighbor *n) {
    return n->rssi == 0 ? -100 : n->rssi;
}

static int batteryScore(const AresNeighbor *n) {
    return n->batteryPct == BATTERY_UNKNOWN ? 50 : n->batteryPct;
}

// O vizinho pode levar mensagens até o Gateway?
static bool isUsable(const AresNeighbor *n, const uint8_t *excludeMac) {
    if (n->role != ROLE_ROUTER && n->role != ROLE_GATEWAY) return false;   // Outro Storage não serve
    if (n->hopsToGateway >= MAX_ROUTE_HOPS) return false;                   // Ele mesmo está sem rota
    if (n->queueFree == 0) return false;                                     // Está cheio
    if (excludeMac != nullptr && memcmp(n->mac, excludeMac, 6) == 0) return false;
    return true;
}

static bool isBetter(const AresNeighbor *a, const AresNeighbor *b) {
    if (a->hopsToGateway != b->hopsToGateway) return a->hopsToGateway < b->hopsToGateway;
    if (rssiScore(a) != rssiScore(b)) return rssiScore(a) > rssiScore(b);
    return batteryScore(a) > batteryScore(b);
}

bool nextHopChoose(const uint8_t *excludeMac, uint8_t outMac[6], bool *isGateway) {
    const AresNeighbor *best = nullptr;
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        const AresNeighbor *n = neighborAt(i);
        if (n == nullptr || !isUsable(n, excludeMac)) continue;
        if (best == nullptr || isBetter(n, best)) best = n;
    }
    if (best != nullptr) {
        memcpy(outMac, best->mac, 6);
        *isGateway = (best->role == ROLE_GATEWAY);
        return true;
    }
#ifdef FIXED_NEXT_HOP_MAC
    static const uint8_t fixedMac[6] = FIXED_NEXT_HOP_MAC;
    if (excludeMac == nullptr || memcmp(fixedMac, excludeMac, 6) != 0) {
        memcpy(outMac, fixedMac, 6);
        *isGateway = false;
        return true;
    }
#endif
    return false;
}
