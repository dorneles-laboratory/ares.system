#include <string.h>
#include "../../include/ares_protocol.h"
#include "../../include/ares_config.h"
#include "../../include/dedup_cache.h"

typedef struct {
    bool used;
    char msgId[sizeof(((AresHeader *) 0)->msg_id)];
    char sourceId[sizeof(((AresHeader *) 0)->source_id)];
    unsigned long timestamp;
} SeenMessage;

static SeenMessage seenRing[DEDUP_SIZE];
static int nextPosition = 0;   // Onde a próxima entrada será escrita (sobrescreve a mais antiga)

// Compara só até o primeiro '\0': o resto do campo pode ter lixo de memória do emissor
static bool sameField(const char *a, const char *b, size_t size) {
    return strncmp(a, b, size) == 0;
}

// Copia até o primeiro '\0' e zera o resto, sem depender de o campo ter '\0'
static void copyField(char *dest, const char *src, size_t size) {
    size_t i = 0;
    for (; i < size && src[i] != '\0'; i++) dest[i] = src[i];
    for (; i < size; i++) dest[i] = '\0';
}

bool dedupSeen(const AresHeader *header) {
    for (int i = 0; i < DEDUP_SIZE; i++) {
        const SeenMessage *seen = &seenRing[i];
        if (seen->used &&
            seen->timestamp == header->timestamp &&
            sameField(seen->msgId, header->msg_id, sizeof(seen->msgId)) &&
            sameField(seen->sourceId, header->source_id, sizeof(seen->sourceId))) {
            return true;
        }
    }
    return false;
}

void dedupRemember(const AresHeader *header) {
    SeenMessage *slot = &seenRing[nextPosition];
    slot->used = true;
    copyField(slot->msgId, header->msg_id, sizeof(slot->msgId));
    copyField(slot->sourceId, header->source_id, sizeof(slot->sourceId));
    slot->timestamp = header->timestamp;
    nextPosition = (nextPosition + 1) % DEDUP_SIZE;
}
