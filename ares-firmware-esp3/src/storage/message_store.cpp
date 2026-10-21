#include <Arduino.h>
#include <string.h>
#include "../../include/ares_protocol.h"
#include "../../include/ares_config.h"
#include "../../include/message_store.h"

// Um único conjunto de vagas com o campo de prioridade, em vez de cinco arrays:
// funciona como cinco filas (storeNext sempre escolhe P0 antes de P1...), mas uma
// prioridade vazia não prende vagas que outra poderia usar.
static AresStoredMessage slots[STORE_SLOTS];
static bool storeDebugEnabled = false;

void setStoreDebug(bool enabled) {
    storeDebugEnabled = enabled;
}

// Prioridade para ordenar a fila. Um valor fora de P0-P4 vindo do rádio é tratado
// como P4, sem alterar o pacote que será repassado.
static int priorityOf(const AresStoredMessage *m) {
    int p = (int) m->packet.header.priority;
    return (p < P0 || p > P4) ? P4 : p;
}

static int priorityOfPacket(const AresMessagePacket *packet) {
    int p = (int) packet->header.priority;
    return (p < P0 || p > P4) ? P4 : p;
}

// "a" é mais antigo que "b"? Comparação por diferença, segura na volta do millis()
static bool isOlder(uint32_t a, uint32_t b) {
    return (int32_t) (a - b) < 0;
}

static int findFreeSlot() {
    for (int i = 0; i < STORE_SLOTS; i++) {
        if (!slots[i].used) return i;
    }
    return STORE_NONE;
}

// A mais antiga da prioridade mais baixa (número maior) que seja menos urgente que "priority"
static int findVictim(int priority) {
    int victim = STORE_NONE;
    for (int i = 0; i < STORE_SLOTS; i++) {
        const AresStoredMessage *m = &slots[i];
        if (!m->used || m->inFlight || priorityOf(m) <= priority) continue;
        if (victim == STORE_NONE ||
            priorityOf(m) > priorityOf(&slots[victim]) ||
            (priorityOf(m) == priorityOf(&slots[victim]) && isOlder(m->storedAtMs, slots[victim].storedAtMs))) {
            victim = i;
        }
    }
    return victim;
}

bool storePut(const AresMessagePacket *packet, const uint8_t *previousHop, uint32_t now) {
    int i = findFreeSlot();
    if (i == STORE_NONE) {
        i = findVictim(priorityOfPacket(packet));
        if (i == STORE_NONE) return false;
        if (storeDebugEnabled) Serial.printf("[DROP] %.*s P%d: fila cheia, cedeu a vaga para uma mensagem mais urgente\n",
                      (int) sizeof(slots[i].packet.header.msg_id), slots[i].packet.header.msg_id,
                      priorityOf(&slots[i]));
    }
    AresStoredMessage *m = &slots[i];
    memcpy(&m->packet, packet, sizeof(AresMessagePacket));
    if (previousHop != nullptr) {
        memcpy(m->previousHop, previousHop, 6);
    } else {
        memset(m->previousHop, 0, 6);
    }
    m->storedAtMs = now;
    m->notBeforeMs = now;
    m->attempts = 0;
    m->rounds = 0;
    m->inFlight = false;
    m->used = true;
    return true;
}

int storeNext(uint32_t now) {
    int best = STORE_NONE;
    for (int i = 0; i < STORE_SLOTS; i++) {
        const AresStoredMessage *m = &slots[i];
        if (!m->used || isOlder(now, m->notBeforeMs)) continue;
        if (best == STORE_NONE ||
            priorityOf(m) < priorityOf(&slots[best]) ||
            (priorityOf(m) == priorityOf(&slots[best]) && isOlder(m->storedAtMs, slots[best].storedAtMs))) {
            best = i;
        }
    }
    return best;
}

AresStoredMessage *storeGet(int handle) {
    if (handle < 0 || handle >= STORE_SLOTS || !slots[handle].used) return nullptr;
    return &slots[handle];
}

void storeMarkDone(int handle) {
    if (storeGet(handle) != nullptr) slots[handle].used = false;
}

void storeDefer(int handle, uint32_t untilMs) {
    AresStoredMessage *m = storeGet(handle);
    if (m == nullptr) return;
    m->notBeforeMs = untilMs;
    m->attempts = 0;
    if (m->rounds < UINT16_MAX) m->rounds++;
    m->inFlight = false;
}

void storeWait(int handle, uint32_t untilMs) {
    AresStoredMessage *m = storeGet(handle);
    if (m == nullptr) return;
    m->notBeforeMs = untilMs;
    m->attempts = 0;
    m->inFlight = false;
}

int storeExpireOld(uint32_t now, uint32_t maxAgeMs) {
    int removed = 0;
    for (int i = 0; i < STORE_SLOTS; i++) {
        AresStoredMessage *m = &slots[i];
        if (!m->used || m->inFlight || (now - m->storedAtMs) < maxAgeMs) continue;
        // Nada sai da fila em silêncio (RS-04): toda expiração fica no log
        if (storeDebugEnabled) {
            Serial.printf("[DROP] %.*s P%d: expirou depois de %lu s guardada sem entrega\n",
                          (int) sizeof(m->packet.header.msg_id), m->packet.header.msg_id,
                          priorityOf(m), (unsigned long) ((now - m->storedAtMs) / 1000));
        }
        m->used = false;
        removed++;
    }
    return removed;
}

int storeFreeSlots() {
    int count = 0;
    for (int i = 0; i < STORE_SLOTS; i++) {
        if (!slots[i].used) count++;
    }
    return count;
}

int storeUsedSlots() {
    return STORE_SLOTS - storeFreeSlots();
}

void storePrint(uint32_t now) {
    int perPriority[5] = {0, 0, 0, 0, 0};
    for (int i = 0; i < STORE_SLOTS; i++) {
        if (slots[i].used) perPriority[priorityOf(&slots[i])]++;
    }
    Serial.printf("[QUEUE] %d/%d vagas ocupadas | P0=%d P1=%d P2=%d P3=%d P4=%d\n",
                  storeUsedSlots(), STORE_SLOTS,
                  perPriority[0], perPriority[1], perPriority[2], perPriority[3], perPriority[4]);
    for (int p = P0; p <= P4; p++) {
        for (int i = 0; i < STORE_SLOTS; i++) {
            const AresStoredMessage *m = &slots[i];
            if (!m->used || priorityOf(m) != p) continue;
            Serial.printf("  P%d %.*s (origem %.*s) | ttl=%u | na fila há %lus | rodadas sem confirmação: %u\n", p,
                          (int) sizeof(m->packet.header.msg_id), m->packet.header.msg_id,
                          (int) sizeof(m->packet.header.source_id), m->packet.header.source_id,
                          m->packet.header.ttl,
                          (unsigned long) ((now - m->storedAtMs) / 1000), m->rounds);
        }
    }
}
