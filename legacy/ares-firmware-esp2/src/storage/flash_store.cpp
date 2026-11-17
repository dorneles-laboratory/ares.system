#include <Arduino.h>
#include <LittleFS.h>
#include <string.h>
#include "../../include/ares_protocol.h"
#include "../../include/ares_config.h"
#include "../../include/flash_store.h"
#include "../../include/event_log.h"
#include "../../include/node_clock.h"

#define RECORD_MAGIC 0xA7E5            // "Assinatura" de uma gaveta do ARES
#define RECORD_VERSION 1
#define RECORD_FREE 0x00               // Gaveta vazia
#define RECORD_PENDING 0x5A            // Gaveta com mensagem esperando entrega

// Como cada gaveta fica gravada na flash. Os campos foram ordenados para não ter
// "buracos" de alinhamento: 20 bytes de cabeçalho + 156 do pacote + 4 do CRC = 180.
typedef struct {
    uint8_t state;                     // RECORD_FREE ou RECORD_PENDING (fica FORA do CRC)
    uint8_t version;
    uint16_t magic;
    uint32_t order;
    uint32_t receivedAtSec;
    uint8_t previousHop[6];
    uint8_t reserved[2];
    AresMessagePacket packet;
    uint32_t crc;                      // CRC32 de tudo entre "version" e o fim do pacote
} FlashRecord;

static_assert(sizeof(FlashRecord) == 20 + sizeof(AresMessagePacket) + 4, "FlashRecord com bytes de alinhamento inesperados");

// Índice na RAM (STORE_CAPACITY x 20 bytes = ~40 KB com 2.000 gavetas)
static FlashIndexEntry indexTable[STORE_CAPACITY];
static File storeFile;
static uint32_t nextOrder = 1;
static bool storeDebugEnabled = false;
static bool storeReady = false;

void setStoreDebug(bool enabled) {
    storeDebugEnabled = enabled;
}

uint32_t flashStoreRecordSize() {
    return sizeof(FlashRecord);
}

// ---------------------------------------------------------------------------
// Utilitários
// ---------------------------------------------------------------------------

// CRC32 clássico (o mesmo do ZIP), calculado bit a bit. É lento para arquivos grandes,
// mas para 176 bytes leva poucos microssegundos e não depende da versão do núcleo.
static uint32_t crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
    }
    return ~crc;
}

static uint32_t recordCrc(const FlashRecord *r) {
    // Começa no byte 1 (pula o "state") e vai até antes do campo crc
    return crc32(((const uint8_t *) r) + 1, offsetof(FlashRecord, crc) - 1);
}

// Copia até o primeiro '\0' e zera o resto: o campo do rádio pode vir com lixo depois do texto
static void hashField(uint32_t *h, const char *field, size_t size) {
    for (size_t i = 0; i < size && field[i] != '\0'; i++) {
        *h ^= (uint8_t) field[i];
        *h *= 16777619UL;              // FNV-1a: resumo simples e rápido
    }
    *h ^= 0xFF;
    *h *= 16777619UL;
}

static uint32_t headerHash(const AresHeader *h) {
    uint32_t hash = 2166136261UL;
    hashField(&hash, h->source_id, sizeof(h->source_id));
    hashField(&hash, h->msg_id, sizeof(h->msg_id));
    uint32_t ts = (uint32_t) h->timestamp;
    for (int i = 0; i < 4; i++) {
        hash ^= (uint8_t) (ts >> (8 * i));
        hash *= 16777619UL;
    }
    return hash;
}

static bool sameKey(const AresHeader *a, const AresHeader *b) {
    return (uint32_t) a->timestamp == (uint32_t) b->timestamp &&
           strncmp(a->msg_id, b->msg_id, sizeof(a->msg_id)) == 0 &&
           strncmp(a->source_id, b->source_id, sizeof(a->source_id)) == 0;
}

static uint8_t clampPriority(int p) {
    return (p < P0 || p > P4) ? (uint8_t) P4 : (uint8_t) p;
}

// "a" é mais antigo que "b"? Comparação por diferença, segura na volta do millis()
static bool isBefore(uint32_t a, uint32_t b) {
    return (int32_t) (a - b) < 0;
}

// ---------------------------------------------------------------------------
// Arquivos de "segmento"
//
// Lição do primeiro teste nas placas: com UM arquivo grande de 351 KB, cada gravação
// levava 6 a 8 s. O LittleFS nunca regrava no lugar: mudar algo no meio de um arquivo
// faz ele copiar o arquivo dali até o fim. Por isso as gavetas ficam divididas em
// arquivos pequenos de 16 gavetas (16 x 180 = 2.880 bytes, menos que um bloco de 4 KB
// da flash). Mudar uma gaveta agora só regrava um bloco, em poucos milissegundos.
// ---------------------------------------------------------------------------

#define STORE_DIR "/ares"
#define OLD_STORE_FILE "/store.bin"     // Formato antigo (arquivo único), apagado se existir
#define SEGMENT_RECORDS 16
#define SEGMENT_COUNT ((STORE_CAPACITY + SEGMENT_RECORDS - 1) / SEGMENT_RECORDS)
#define SEGMENT_BYTES (SEGMENT_RECORDS * sizeof(FlashRecord))

static_assert(SEGMENT_RECORDS * sizeof(FlashRecord) <= 4096, "Um segmento precisa caber num bloco de 4 KB da flash");

static int openSegment = -1;           // Qual segmento está aberto em storeFile (-1 = nenhum)

static void segmentPath(int segment, char *out, size_t size) {
    snprintf(out, size, STORE_DIR "/s%03d.bin", segment);
}

// Deixa aberto o segmento da gaveta pedida (troca de arquivo só quando precisa)
static bool selectSlot(int slot) {
    int segment = slot / SEGMENT_RECORDS;
    if (segment != openSegment) {
        if (storeFile) storeFile.close();
        openSegment = -1;
        char path[24];
        segmentPath(segment, path, sizeof(path));
        storeFile = LittleFS.open(path, "r+");   // r+ = ler e gravar em qualquer posição
        if (!storeFile) return false;
        openSegment = segment;
    }
    return storeFile.seek((size_t) (slot % SEGMENT_RECORDS) * sizeof(FlashRecord));
}

static bool readRecord(int slot, FlashRecord *r) {
    if (!selectSlot(slot)) return false;
    return storeFile.read((uint8_t *) r, sizeof(FlashRecord)) == sizeof(FlashRecord);
}

// Grava e força a ida para a flash (flush). Só depois disso a mensagem está segura.
static bool writeRecord(int slot, const FlashRecord *r) {
    if (!selectSlot(slot)) return false;
    if (storeFile.write((const uint8_t *) r, sizeof(FlashRecord)) != sizeof(FlashRecord)) return false;
    storeFile.flush();
    return true;
}

// Para apagar basta trocar o primeiro byte (state) para FREE
static bool markSlotFree(int slot) {
    if (!selectSlot(slot)) return false;
    uint8_t freeState = RECORD_FREE;
    if (storeFile.write(&freeState, 1) != 1) return false;
    storeFile.flush();
    return true;
}

// ---------------------------------------------------------------------------
// Inicialização
// ---------------------------------------------------------------------------

// Cria os segmentos que faltam (todas as gavetas vazias). Só demora no primeiro boot.
static bool ensureSegments() {
    if (LittleFS.exists(OLD_STORE_FILE)) {
        Serial.println("[STORE] Apagando o arquivo do formato antigo (/store.bin).");
        LittleFS.remove(OLD_STORE_FILE);
    }
    if (!LittleFS.exists(STORE_DIR)) LittleFS.mkdir(STORE_DIR);

    static uint8_t zeros[SEGMENT_BYTES];   // static: não ocupa a pilha do loop()
    memset(zeros, 0, sizeof(zeros));
    int created = 0;
    for (int segment = 0; segment < SEGMENT_COUNT; segment++) {
        char path[24];
        segmentPath(segment, path, sizeof(path));
        size_t current = 0;
        if (LittleFS.exists(path)) {
            File f = LittleFS.open(path, "r");
            if (f) {
                current = f.size();
                f.close();
            }
        }
        if (current >= SEGMENT_BYTES) continue;
        if (created == 0) {
            Serial.printf("[STORE] Preparando a flash: %d arquivos de %u gavetas (só no primeiro boot)",
                          SEGMENT_COUNT, (unsigned) SEGMENT_RECORDS);
        }
        if (LittleFS.totalBytes() - LittleFS.usedBytes() < 2 * 4096) {
            Serial.println("\n[ERRO] Partição de arquivos pequena demais. No Arduino IDE: Ferramentas > Partition Scheme >");
            Serial.println("       'Default 4MB with spiffs'. Ou diminua STORE_CAPACITY no ares_config.h.");
            return false;
        }
        // "a" = acrescenta no fim, preservando as gavetas que já existem
        File f = LittleFS.open(path, "a");
        if (!f || f.write(zeros, SEGMENT_BYTES - current) != SEGMENT_BYTES - current) {
            if (f) f.close();
            Serial.println("\n[ERRO] Falha ao preparar os arquivos na flash.");
            return false;
        }
        f.close();
        created++;
        if (created % 10 == 0) Serial.print(".");
        yield();                       // Deixa o Wi-Fi e o watchdog respirarem
    }
    if (created > 0) Serial.println(" pronto.");
    return true;
}

// Lê os segmentos e monta o índice na RAM. Gavetas com CRC errado (energia caiu
// durante a gravação) são liberadas e registradas.
// Lê um segmento inteiro (16 gavetas) por vez: 125 leituras em vez de 2.000 deixa o
// boot bem mais rápido (no teste, ler gaveta por gaveta levava ~5 s).
static void rebuildIndex() {
    memset(indexTable, 0, sizeof(indexTable));
    nextOrder = 1;
    int recovered = 0;
    int corrupted = 0;
    static FlashRecord segment[SEGMENT_RECORDS];   // static: 2,9 KB fora da pilha
    for (int seg = 0; seg < SEGMENT_COUNT; seg++) {
        if (!selectSlot(seg * SEGMENT_RECORDS)) continue;
        size_t got = storeFile.read((uint8_t *) segment, SEGMENT_BYTES);
        int records = (int) (got / sizeof(FlashRecord));
        for (int i = 0; i < records; i++) {
            int slot = seg * SEGMENT_RECORDS + i;
            if (slot >= STORE_CAPACITY) break;
            const FlashRecord *r = &segment[i];
            if (r->state != RECORD_PENDING) continue;
            if (r->magic != RECORD_MAGIC || r->version != RECORD_VERSION || r->crc != recordCrc(r)) {
                corrupted++;
                markSlotFree(slot);
                eventLog("CORROMPIDA", nullptr, "gaveta com CRC errado liberada no boot");
                continue;
            }
            FlashIndexEntry *e = &indexTable[slot];
            e->used = true;
            e->priority = clampPriority((int) r->packet.header.priority);
            e->order = r->order;
            e->receivedAtSec = r->receivedAtSec;
            e->notBeforeMs = 0;
            e->keyHash = headerHash(&r->packet.header);
            if (r->order >= nextOrder) nextOrder = r->order + 1;
            recovered++;
        }
        yield();
    }
    Serial.printf("[STORE] %d mensagem(ns) recuperada(s) da flash", recovered);
    if (corrupted > 0) Serial.printf(", %d gaveta(s) corrompida(s) liberada(s)", corrupted);
    Serial.println(".");
}

bool flashStoreBegin() {
    // begin(true) = formata a partição se ela não montar (ex.: primeira vez na placa).
    // No primeiro boot é normal aparecer "mount failed (-84)" antes da formatação.
    if (!LittleFS.begin(true)) {
        Serial.println("[ERRO] LittleFS não montou. Confira o Partition Scheme (precisa ter SPIFFS/FS).");
        return false;
    }
    Serial.printf("[STORE] LittleFS montado: %u KB usados de %u KB.\n",
                  (unsigned) (LittleFS.usedBytes() / 1024), (unsigned) (LittleFS.totalBytes() / 1024));
    if (!ensureSegments()) return false;
    unsigned long start = millis();
    rebuildIndex();
    Serial.printf("[STORE] Índice montado em %lu ms.\n", (unsigned long) (millis() - start));
    storeReady = true;
    return true;
}

// ---------------------------------------------------------------------------
// Operações
// ---------------------------------------------------------------------------

static int findFreeSlot() {
    for (int i = 0; i < STORE_CAPACITY; i++) {
        if (!indexTable[i].used) return i;
    }
    return STORE_NONE;
}

// A mais antiga da prioridade mais baixa que seja MENOS urgente que "priority"
static int findVictim(uint8_t priority) {
    int victim = STORE_NONE;
    for (int i = 0; i < STORE_CAPACITY; i++) {
        const FlashIndexEntry *e = &indexTable[i];
        if (!e->used || e->inFlight || e->priority <= priority) continue;
        if (victim == STORE_NONE || e->priority > indexTable[victim].priority ||
            (e->priority == indexTable[victim].priority && isBefore(e->order, indexTable[victim].order))) {
            victim = i;
        }
    }
    return victim;
}

static bool isAlreadyStored(const AresHeader *header, uint32_t hash) {
    static FlashRecord r;
    for (int i = 0; i < STORE_CAPACITY; i++) {
        const FlashIndexEntry *e = &indexTable[i];
        if (!e->used || e->keyHash != hash) continue;
        // O resumo bateu: confirma lendo da flash (resumos iguais de mensagens diferentes são raros, mas existem)
        if (readRecord(i, &r) && sameKey(&r.packet.header, header)) return true;
    }
    return false;
}

FlashPutResult flashStorePut(const AresMessagePacket *packet, const uint8_t *previousHop) {
    if (!storeReady) return PUT_FLASH_ERROR;
    uint32_t hash = headerHash(&packet->header);
    if (isAlreadyStored(&packet->header, hash)) return PUT_DUPLICATE;

    uint8_t priority = clampPriority((int) packet->header.priority);
    bool evicted = false;
    int slot = findFreeSlot();
    if (slot == STORE_NONE) {
        slot = findVictim(priority);
        if (slot == STORE_NONE) return PUT_FULL;
        // Registra QUAL mensagem saiu antes de sobrescrever (RS-04: nada some em silêncio)
        AresMessagePacket victim;
        char detail[48];
        if (flashStoreRead(slot, &victim, nullptr)) {
            snprintf(detail, sizeof(detail), "P%u cedeu a vaga para uma P%u", indexTable[slot].priority, priority);
            eventLog("DESCARTADA", &victim.header, detail);
            if (storeDebugEnabled) {
                Serial.printf("[DROP] %.*s %s (memória cheia)\n", (int) sizeof(victim.header.msg_id),
                              victim.header.msg_id, detail);
            }
        }
        evicted = true;
    }

    static FlashRecord r;
    memset(&r, 0, sizeof(r));
    r.state = RECORD_PENDING;
    r.version = RECORD_VERSION;
    r.magic = RECORD_MAGIC;
    r.order = nextOrder++;
    r.receivedAtSec = nodeClockNow();
    if (previousHop != nullptr) memcpy(r.previousHop, previousHop, 6);
    memcpy(&r.packet, packet, sizeof(AresMessagePacket));
    r.crc = recordCrc(&r);

    if (!writeRecord(slot, &r)) {
        indexTable[slot].used = false;  // Se era vítima, já foi registrada; a gaveta fica livre
        return PUT_FLASH_ERROR;
    }

    FlashIndexEntry *e = &indexTable[slot];
    memset(e, 0, sizeof(*e));
    e->used = true;
    e->priority = priority;
    e->order = r.order;
    e->receivedAtSec = r.receivedAtSec;
    e->notBeforeMs = millis();
    e->keyHash = hash;
    return evicted ? PUT_OK_EVICTED : PUT_OK;
}

int flashStoreNext(uint32_t nowMs) {
    int best = STORE_NONE;
    for (int i = 0; i < STORE_CAPACITY; i++) {
        const FlashIndexEntry *e = &indexTable[i];
        if (!e->used || e->inFlight || isBefore(nowMs, e->notBeforeMs)) continue;
        if (best == STORE_NONE || e->priority < indexTable[best].priority ||
            (e->priority == indexTable[best].priority && isBefore(e->order, indexTable[best].order))) {
            best = i;
        }
    }
    return best;
}

FlashIndexEntry *flashStoreEntry(int handle) {
    if (handle < 0 || handle >= STORE_CAPACITY || !indexTable[handle].used) return nullptr;
    return &indexTable[handle];
}

bool flashStoreRead(int handle, AresMessagePacket *packet, uint8_t *previousHop) {
    if (flashStoreEntry(handle) == nullptr) return false;
    static FlashRecord r;
    if (!readRecord(handle, &r) || r.state != RECORD_PENDING || r.crc != recordCrc(&r)) return false;
    memcpy(packet, &r.packet, sizeof(AresMessagePacket));
    if (previousHop != nullptr) memcpy(previousHop, r.previousHop, 6);
    return true;
}

bool flashStoreRemove(int handle) {
    if (flashStoreEntry(handle) == nullptr) return false;
    bool ok = markSlotFree(handle);
    indexTable[handle].used = false;
    return ok;
}

void flashStoreDefer(int handle, uint32_t untilMs, bool countRound) {
    FlashIndexEntry *e = flashStoreEntry(handle);
    if (e == nullptr) return;
    e->notBeforeMs = untilMs;
    e->attempts = 0;
    e->inFlight = false;
    if (countRound && e->rounds < UINT16_MAX) e->rounds++;
}

int flashStoreExpireOld(uint32_t nowSec, uint32_t maxAgeSec) {
    int removed = 0;
    for (int i = 0; i < STORE_CAPACITY; i++) {
        FlashIndexEntry *e = &indexTable[i];
        if (!e->used || e->inFlight || nowSec - e->receivedAtSec < maxAgeSec) continue;
        AresMessagePacket packet;
        char detail[48];
        snprintf(detail, sizeof(detail), "guardada há %lu s sem entrega", (unsigned long) (nowSec - e->receivedAtSec));
        if (flashStoreRead(i, &packet, nullptr)) {
            eventLog("EXPIRADA", &packet.header, detail);
            if (storeDebugEnabled) {
                Serial.printf("[DROP] %.*s expirou: %s\n", (int) sizeof(packet.header.msg_id), packet.header.msg_id, detail);
            }
        }
        flashStoreRemove(i);
        removed++;
    }
    return removed;
}

int flashStoreUsed() {
    int count = 0;
    for (int i = 0; i < STORE_CAPACITY; i++) {
        if (indexTable[i].used) count++;
    }
    return count;
}

int flashStoreFree() {
    return STORE_CAPACITY - flashStoreUsed();
}

void flashStorePrint(uint32_t nowSec) {
    int perPriority[5] = {0, 0, 0, 0, 0};
    for (int i = 0; i < STORE_CAPACITY; i++) {
        if (indexTable[i].used) perPriority[indexTable[i].priority]++;
    }
    Serial.printf("[STORE] %d/%d gavetas ocupadas | P0=%d P1=%d P2=%d P3=%d P4=%d\n", flashStoreUsed(), STORE_CAPACITY,
                  perPriority[0], perPriority[1], perPriority[2], perPriority[3], perPriority[4]);
    // Mostra no máximo 20 por prioridade para não travar o Serial com 2.000 linhas
    for (int p = P0; p <= P4; p++) {
        int shown = 0;
        for (int i = 0; i < STORE_CAPACITY && shown < 20; i++) {
            const FlashIndexEntry *e = &indexTable[i];
            if (!e->used || e->priority != p) continue;
            AresMessagePacket packet;
            if (!flashStoreRead(i, &packet, nullptr)) continue;
            Serial.printf("  P%d %.*s | \"%.*s\" | guardada há %lus | rodadas sem confirmação: %u%s\n", p,
                          (int) sizeof(packet.header.msg_id), packet.header.msg_id,
                          (int) sizeof(packet.payload.text_message), packet.payload.text_message,
                          (unsigned long) (nowSec - e->receivedAtSec), e->rounds, e->inFlight ? " | enviando" : "");
            shown++;
        }
        if (shown == 20 && perPriority[p] > 20) Serial.printf("  ... e mais %d P%d\n", perPriority[p] - 20, p);
    }
}

bool flashStoreFormat() {
    storeReady = false;
    if (storeFile) storeFile.close();
    openSegment = -1;
    LittleFS.format();
    if (!LittleFS.begin(true)) return false;
    return flashStoreBegin();
}
