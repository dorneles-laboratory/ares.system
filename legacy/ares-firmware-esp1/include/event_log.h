#ifndef EVENT_LOG_H
#define EVENT_LOG_H

// ============================================================================
// Registro de eventos do Storage, gravado num arquivo de texto na flash (LittleFS).
//
// A wiki (RS-04) diz que nenhuma mensagem pode sumir em silêncio: toda custódia aceita,
// entrega, expiração ou descarte fica registrada com o motivo. Cada linha é:
//   <segundos do relógio do nó> <EVENTO> <msg_id> <detalhe>
// Quando o arquivo passa de EVENT_LOG_MAX_BYTES, ele vira "events.old" e começa outro.
// ============================================================================

#include "ares_protocol.h"

bool eventLogBegin();
// Grava um evento. "detail" pode ser nullptr.
void eventLog(const char *event, const AresHeader *header, const char *detail);
// Mostra as últimas "lines" linhas no Serial (comando "log")
void eventLogPrintTail(int lines);

#endif
