#include <Arduino.h>
#include <LittleFS.h>
#include "../../include/ares_config.h"
#include "../../include/event_log.h"
#include "../../include/node_clock.h"

#define EVENT_FILE "/events.log"
#define EVENT_FILE_OLD "/events.old"

bool eventLogBegin() {
    return true;   // O LittleFS já foi montado pelo flashStoreBegin()
}

// Se o arquivo ficou grande, troca: events.log -> events.old (o .old anterior é apagado)
static void rotateIfNeeded() {
    File f = LittleFS.open(EVENT_FILE, "r");
    if (!f) return;
    size_t size = f.size();
    f.close();
    if (size < EVENT_LOG_MAX_BYTES) return;
    LittleFS.remove(EVENT_FILE_OLD);
    LittleFS.rename(EVENT_FILE, EVENT_FILE_OLD);
}

void eventLog(const char *event, const AresHeader *header, const char *detail) {
    rotateIfNeeded();
    // "a" = append: escreve no fim. Abrir, escrever e fechar garante que a linha
    // fica gravada mesmo se a energia cair logo depois.
    File f = LittleFS.open(EVENT_FILE, "a");
    if (!f) return;
    char line[160];
    snprintf(line, sizeof(line), "%lu %s %.*s %s\n",
             (unsigned long) nodeClockNow(), event,
             header ? (int) sizeof(header->msg_id) : 1, header ? header->msg_id : "-",
             detail ? detail : "");
    f.print(line);
    f.close();
}

void eventLogPrintTail(int lines) {
    File f = LittleFS.open(EVENT_FILE, "r");
    if (!f) {
        Serial.println("[LOG] Nenhum evento registrado ainda.");
        return;
    }
    // Conta as linhas para pular direto para as últimas (o arquivo tem no máximo 32 KB)
    int total = 0;
    while (f.available()) {
        if (f.read() == '\n') total++;
    }
    int skip = total > lines ? total - lines : 0;
    f.seek(0);
    Serial.printf("[LOG] Últimos %d de %d eventos (segundos do relógio do nó, evento, msg_id, detalhe):\n",
                  total - skip, total);
    int current = 0;
    while (f.available()) {
        char c = f.read();
        if (current >= skip) Serial.write(c);
        if (c == '\n') current++;
    }
    f.close();
}
