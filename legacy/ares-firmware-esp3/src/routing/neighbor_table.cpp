#include <string.h>
#include "../../include/ares_config.h"
#include "../../include/neighbor_table.h"

static AresNeighbor neighbors[MAX_NEIGHBORS];

// "a" é mais antigo que "b"? Comparação por diferença, segura na volta do millis()
static bool isOlder(uint32_t a, uint32_t b) {
    return (int32_t) (a - b) < 0;
}

AresNeighbor *neighborFind(const uint8_t *mac) {
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        if (neighbors[i].used && memcmp(neighbors[i].mac, mac, 6) == 0) return &neighbors[i];
    }
    return nullptr;
}

AresNeighbor *neighborAt(int index) {
    if (index < 0 || index >= MAX_NEIGHBORS || !neighbors[index].used) return nullptr;
    return &neighbors[index];
}

AresNeighborUpdate neighborUpdate(const uint8_t *mac, const AresBeaconPacket *beacon, int8_t rssi, uint32_t now) {
    AresNeighborUpdate outcome = NEIGHBOR_UPDATED;
    AresNeighbor *n = neighborFind(mac);
    if (n == nullptr) {
        for (int i = 0; i < MAX_NEIGHBORS && n == nullptr; i++) {
            if (!neighbors[i].used) n = &neighbors[i];
        }
        if (n == nullptr) return NEIGHBOR_TABLE_FULL;
        memset(n, 0, sizeof(*n));
        memcpy(n->mac, mac, 6);
        n->used = true;
        outcome = NEIGHBOR_ADDED;
    }
    n->role = beacon->role;
    n->hopsToGateway = beacon->hopsToGateway;
    memcpy(n->nextHop, beacon->nextHop, 6);
    if (rssi != 0) n->rssi = rssi;
    n->batteryPct = beacon->batteryPct;
    n->queueFree = beacon->queueFree;
    n->lastSeenMs = now;
    return outcome;
}

void neighborRemove(const uint8_t *mac) {
    AresNeighbor *n = neighborFind(mac);
    if (n != nullptr) n->used = false;
}

int neighborExpire(uint32_t now, uint32_t timeoutMs, uint8_t removed[][6], int maxRemoved) {
    int count = 0;
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        AresNeighbor *n = &neighbors[i];
        if (!n->used || isOlder(now, n->lastSeenMs + timeoutMs)) continue;
        if (count < maxRemoved) memcpy(removed[count], n->mac, 6);
        n->used = false;
        count++;
    }
    return count;
}

uint8_t neighborMarkFailure(const uint8_t *mac) {
    AresNeighbor *n = neighborFind(mac);
    if (n == nullptr) return 0;
    if (n->failedRounds < 255) n->failedRounds++;
    return n->failedRounds;
}

void neighborMarkSuccess(const uint8_t *mac) {
    AresNeighbor *n = neighborFind(mac);
    if (n != nullptr) n->failedRounds = 0;
}

int neighborCount() {
    int count = 0;
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        if (neighbors[i].used) count++;
    }
    return count;
}
