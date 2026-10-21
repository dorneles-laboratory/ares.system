#include <string.h>
#include "../../include/ares_config.h"
#include "../../include/ares_router_protocol.h"
#include "../../include/neighbor_table.h"
#include "../../include/routing.h"

static uint8_t gatewayAddress[6];
static uint8_t selfAddress[6];
static uint32_t gatewayConfirmedAt = 0;
static bool gatewayEverConfirmed = false;

void routingInit(const uint8_t *gatewayMac, const uint8_t *selfMac) {
    memcpy(gatewayAddress, gatewayMac, 6);
    memcpy(selfAddress, selfMac, 6);
}

const uint8_t *routingGatewayMac() {
    return gatewayAddress;
}

void routingGatewayConfirmed(uint32_t now) {
    gatewayConfirmedAt = now;
    gatewayEverConfirmed = true;
}

void routingGatewayFailed() {
    gatewayEverConfirmed = false;
}

bool routingGatewayAlive(uint32_t now) {
    return gatewayEverConfirmed && (now - gatewayConfirmedAt) < GATEWAY_ALIVE_MS;
}

uint32_t routingGatewayLastConfirmed() {
    return gatewayConfirmedAt;
}

// Valores desconhecidos entram como medianos para não ganhar nem perder desempates
static int rssiScore(const AresNeighbor *n) {
    return n->rssi == 0 ? -100 : n->rssi;
}

static int batteryScore(const AresNeighbor *n) {
    return n->batteryPct == BATTERY_UNKNOWN ? 50 : n->batteryPct;
}

// O vizinho pode levar mensagens até o Gateway?
static bool isUsable(const AresNeighbor *n, const uint8_t *excludeMac) {
    if (n->role != ROLE_ROUTER && n->role != ROLE_GATEWAY) return false;
    if (n->hopsToGateway >= MAX_ROUTE_HOPS) return false;
    if (n->queueFree == 0) return false;
    // Split horizon: quem chega ao Gateway passando por nós não serve de caminho para nós
    if (memcmp(n->nextHop, selfAddress, 6) == 0) return false;
    if (excludeMac != nullptr && memcmp(n->mac, excludeMac, 6) == 0) return false;
    return true;
}

static bool isBetter(const AresNeighbor *a, const AresNeighbor *b) {
    if (a->hopsToGateway != b->hopsToGateway) return a->hopsToGateway < b->hopsToGateway;
    if (rssiScore(a) != rssiScore(b)) return rssiScore(a) > rssiScore(b);
    if (batteryScore(a) != batteryScore(b)) return batteryScore(a) > batteryScore(b);
    return a->queueFree > b->queueFree;
}

static const AresNeighbor *bestNeighbor(const uint8_t *excludeMac) {
    const AresNeighbor *best = nullptr;
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        const AresNeighbor *n = neighborAt(i);
        if (n == nullptr || !isUsable(n, excludeMac)) continue;
        if (best == nullptr || isBetter(n, best)) best = n;
    }
    return best;
}

uint8_t routingMyHops(uint32_t now) {
    if (routingGatewayAlive(now)) return 1;
    const AresNeighbor *best = bestNeighbor(nullptr);
    if (best == nullptr) return HOPS_UNREACHABLE;
    int hops = best->hopsToGateway + 1;
    return hops >= MAX_ROUTE_HOPS ? HOPS_UNREACHABLE : (uint8_t) hops;
}

bool routingChooseNextHop(uint32_t now, const uint8_t *excludeMac, uint8_t outMac[6], bool *isGateway) {
    // O Gateway é o destino final: entregar direto sempre vence, mesmo que a mensagem tenha vindo dele
    if (routingGatewayAlive(now)) {
        memcpy(outMac, gatewayAddress, 6);
        *isGateway = true;
        return true;
    }
    const AresNeighbor *best = bestNeighbor(excludeMac);
    if (best == nullptr) return false;
    memcpy(outMac, best->mac, 6);
    *isGateway = false;
    return true;
}
