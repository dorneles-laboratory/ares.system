#ifndef DEDUP_CACHE_H
#define DEDUP_CACHE_H

#include "ares_protocol.h"

// Memória curta das últimas mensagens vistas, para não repassar a mesma duas vezes.
// A chave é (source_id, msg_id, timestamp): o ESP1 recomeça o contador do msg_id
// quando reinicia, e o timestamp separa "msg-uuid-1" de antes e depois do reboot.
bool dedupSeen(const AresHeader *header);
void dedupRemember(const AresHeader *header);

#endif
