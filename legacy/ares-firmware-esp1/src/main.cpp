// ============================================================================
// ARES - ESP1: nó STORAGE
//
// O que este nó faz, em ordem:
//  1. CRIA mensagens: telemetria P4 automática e texto digitado no Monitor Serial (p0 ... p4).
//  2. RECEBE mensagens de outros nós pelo rádio (ele também é um ponto de guarda).
//  3. GUARDA tudo na flash do próprio ESP32 (LittleFS). Se a energia cair, nada se perde.
//  4. ENTREGA uma por vez ao próximo salto (Router ou Gateway), P0 primeiro.
//  5. Só APAGA da flash quando o próximo salto manda o ACK de custódia
//     ("guardei, pode liberar"). Sem ACK, tenta de novo mais tarde.
//
// Organização dos arquivos:
//  - src/radio/esp_now_radio.cpp    -> rádio ESP-NOW (igual nos 3 firmwares)
//  - src/storage/flash_store.cpp    -> gavetas de mensagens na flash
//  - src/storage/event_log.cpp      -> registro de eventos (nada some em silêncio)
//  - src/storage/node_clock.cpp     -> relógio e numeração que sobrevivem ao reinício
//  - src/routing/neighbor_table.cpp -> vizinhos descobertos por beacon
//  - src/routing/next_hop.cpp       -> para quem entregar
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include "ares_protocol.h"
#include "ares_control_protocol.h"
#include "ares_config.h"
#include "esp_now_radio.h"
#include "flash_store.h"
#include "event_log.h"
#include "node_clock.h"
#include "dedup_cache.h"
#include "neighbor_table.h"
#include "next_hop.h"

uint8_t selfMac[6];
char sourceId[sizeof(((AresHeader *) 0)->source_id)];   // Ex.: "STG-A1B2C3"
bool showLogs = false;
bool storageOk = false;

// --- Envio em andamento: uma mensagem por vez, até o próximo salto confirmar ---
// Isto é uma "máquina de estados": em vez de esperar com delay() (que trava tudo),
// o loop() passa por aqui a cada volta e vê se já é hora de avançar.
typedef enum {
    TX_IDLE = 0,                       // Nada sendo enviado
    TX_WAIT_RADIO,                     // Esperando o rádio do próximo salto confirmar
    TX_WAIT_CUSTODY                    // Esperando o ACK de custódia (Router ou Gateway)
} AresTxState;

AresTxState txState = TX_IDLE;
int txHandle = STORE_NONE;
AresMessagePacket txPacket;            // Cópia lida da flash da mensagem em envio
uint8_t txNextHop[6];
bool txToGateway = false;
uint32_t txSendId = 0;
unsigned long txStartedAt = 0;
bool txRadioDone = false;
bool txRadioDelivered = false;
bool txCustodyReceived = false;

// --- Tarefas periódicas (sempre com millis(), nunca com delay()) ---
unsigned long lastBeaconAt = 0;
unsigned long lastTelemetryAt = 0;
unsigned long lastExpireAt = 0;
unsigned long lastHousekeepingAt = 0;
uint32_t beaconSequence = 0;
bool hadRouteLogged = false;
bool hadRoute = false;

// --- Contadores para o comando 's' ---
uint32_t createdCount = 0;
uint32_t receivedCount = 0;
uint32_t duplicateCount = 0;
uint32_t ttlDroppedCount = 0;
uint32_t refusedFullCount = 0;
uint32_t deliveredCount = 0;
uint32_t expiredCount = 0;
uint32_t custodyAcksSent = 0;
uint32_t unknownFrames = 0;

// ============================================================================
// Utilitários de impressão
// ============================================================================

static void printMac(const uint8_t *mac) {
    Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Mostra até o primeiro '\0' sem depender de o campo ter '\0'
static void printField(const char *field, size_t size) {
    Serial.printf("%.*s", (int) size, field);
}

static void printMsgId(const AresHeader *header) {
    printField(header->msg_id, sizeof(header->msg_id));
}

static bool sameMac(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, 6) == 0;
}

static void macToText(const uint8_t *mac, char *out, size_t size) {
    snprintf(out, size, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Bateria em % pelo divisor de tensão (3,3 V = 0%, 4,2 V = 100%), ou desconhecida
static uint8_t readBatteryPct() {
#if BATTERY_ADC_PIN >= 0
    float volts = analogReadMilliVolts(BATTERY_ADC_PIN) * BATTERY_DIVIDER_RATIO / 1000.0f;
    int pct = (int) ((volts - 3.3f) * 100.0f / (4.2f - 3.3f));
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (uint8_t) pct;
#else
    return BATTERY_UNKNOWN;
#endif
}

// ============================================================================
// 1. Criação de mensagens (telemetria e texto do Serial)
// ============================================================================

void createMessage(int priority, const char *text) {
    AresMessagePacket packet;
    memset(&packet, 0, sizeof(packet));   // Zera tudo: nada de lixo de memória indo pelo rádio

    // ID que nunca repete: 6 letras do MAC + sequência que sobrevive ao reinício
    uint32_t seq = nodeNextSequence();
    snprintf(packet.header.msg_id, sizeof(packet.header.msg_id), "%02X%02X%02X-%lu",
             selfMac[3], selfMac[4], selfMac[5], (unsigned long) seq);
    snprintf(packet.header.source_id, sizeof(packet.header.source_id), "%s", sourceId);
    snprintf(packet.header.destination_id, sizeof(packet.header.destination_id), "%s", MESSAGE_DESTINATION);
    packet.header.priority = (AresPriorityLevel) priority;
    packet.header.ttl = DEFAULT_TTL;
    packet.header.timestamp = nodeClockNow();

    uint8_t battery = readBatteryPct();
    packet.telemetry.battery_level = (battery == BATTERY_UNKNOWN) ? -1.0f : (float) battery;
    packet.telemetry.storage_avail_kb = (uint32_t) flashStoreFree() * flashStoreRecordSize() / 1024;

    snprintf(packet.location.lat, sizeof(packet.location.lat), "%s", LOCATION_LAT);
    snprintf(packet.location.lon, sizeof(packet.location.lon), "%s", LOCATION_LON);
    snprintf(packet.location.altura, sizeof(packet.location.altura), "%s", LOCATION_ALT);
    snprintf(packet.payload.text_message, sizeof(packet.payload.text_message), "%s", text);

    FlashPutResult result = flashStorePut(&packet, selfMac);
    if (result == PUT_OK || result == PUT_OK_EVICTED) {
        createdCount++;
        eventLog("CRIADA", &packet.header, text);
        if (showLogs) {
            Serial.print("[NEW] ");
            printMsgId(&packet.header);
            Serial.printf(" | P%d | \"%s\" | estado: PENDENTE | memória: %d/%d\n", priority, text,
                          flashStoreUsed(), STORE_CAPACITY);
        }
    } else {
        eventLog("RECUSADA", &packet.header, result == PUT_FULL ? "memória cheia de mensagens mais urgentes" : "erro na flash");
        if (showLogs) {
            Serial.println(result == PUT_FULL ? "[DROP] Memória cheia só com mensagens tão ou mais urgentes. Não foi criada."
                                              : "[ERRO] Falha ao gravar na flash. Mensagem não criada.");
        }
    }
}

void createTelemetry() {
    char text[sizeof(((AresPayloadText *) 0)->text_message)];
    snprintf(text, sizeof(text), "Telemetria %s", sourceId);
    createMessage(P4, text);
}

// ============================================================================
// 2. Recepção pelo rádio
// ============================================================================

static void sendCustodyAck(const uint8_t *toMac, const AresHeader *header) {
    AresCustodyAckPacket ack;
    memset(&ack, 0, sizeof(ack));
    ack.header.magic[0] = ARES_CTRL_MAGIC_0;
    ack.header.magic[1] = ARES_CTRL_MAGIC_1;
    ack.header.version = ARES_CTRL_VERSION;
    ack.header.type = CTRL_CUSTODY_ACK;
    memcpy(ack.msgId, header->msg_id, sizeof(ack.msgId));
    memcpy(ack.sourceId, header->source_id, sizeof(ack.sourceId));
    ack.timestamp = (uint32_t) header->timestamp;
    if (sendFrame(toMac, &ack, sizeof(ack), SEND_KIND_CONTROL) != 0) custodyAcksSent++;
}

// Outro nó nos entregou uma mensagem: guardamos na flash e SÓ DEPOIS confirmamos a custódia
void handleDataPacket(AresMessagePacket *packet, const uint8_t *senderMac) {
    AresHeader *header = &packet->header;
    receivedCount++;

    if (dedupSeen(header)) {
        // Já entregamos esta antes: o ACK anterior pode ter se perdido, confirma de novo
        duplicateCount++;
        sendCustodyAck(senderMac, header);
        return;
    }
    if (header->ttl == 0) {
        ttlDroppedCount++;
        eventLog("DESCARTADA", header, "ttl esgotado");
        if (showLogs) {
            Serial.print("[DROP] ");
            printMsgId(header);
            Serial.println(" chegou com ttl 0. Registrada e descartada.");
        }
        sendCustodyAck(senderMac, header);   // A custódia termina aqui (registrada no log)
        return;
    }
    header->ttl = header->ttl - 1;           // Este nó conta como um salto

    FlashPutResult result = flashStorePut(packet, senderMac);
    if (result == PUT_DUPLICATE) {
        duplicateCount++;
        if (showLogs) {
            Serial.print("[DUP] ");
            printMsgId(header);
            Serial.println(" já está guardada. ACK reenviado.");
        }
        sendCustodyAck(senderMac, header);
        return;
    }
    if (result == PUT_FULL || result == PUT_FLASH_ERROR) {
        // Sem ACK: quem enviou continua com a mensagem e tenta de novo depois
        refusedFullCount++;
        eventLog("RECUSADA", header, result == PUT_FULL ? "memória cheia" : "erro na flash");
        if (showLogs) Serial.println("[DROP] Não coube na memória. Recusada sem ACK (o vizinho continua com ela).");
        return;
    }

    char from[18];
    macToText(senderMac, from, sizeof(from));
    eventLog("CUSTODIA_ACEITA", header, from);
    if (showLogs) {
        Serial.print("[RX] ");
        printMsgId(header);
        Serial.print(" de ");
        printMac(senderMac);
        Serial.printf(" | P%d | gravada na flash, ACK enviado. Memória: %d/%d\n", (int) header->priority,
                      flashStoreUsed(), STORE_CAPACITY);
    }
    sendCustodyAck(senderMac, header);
}

void handleBeacon(const uint8_t *senderMac, const AresBeaconPacket *beacon, int8_t rssi) {
    if (sameMac(senderMac, selfMac)) return;
    AresNeighborUpdate outcome = neighborUpdate(senderMac, beacon, rssi, millis());
    if (outcome == NEIGHBOR_ADDED) {
        addRadioPeer(senderMac);
        if (showLogs) {
            Serial.print("[NBR] Novo vizinho: ");
            printMac(senderMac);
            const char *role = beacon->role == ROLE_GATEWAY ? "Gateway" : beacon->role == ROLE_ROUTER ? "Router" : "Storage";
            Serial.printf(" (%s)", role);
            if (beacon->hopsToGateway == HOPS_UNREACHABLE) {
                Serial.print(" | sem rota até o Gateway");
            } else {
                Serial.printf(" | %u salto(s) até o Gateway", beacon->hopsToGateway);
            }
            Serial.printf(" | RSSI %d dBm\n", rssi);
        }
    }
}

// O ACK precisa ser do vizinho para quem enviamos E da mensagem que está em envio
void handleCustodyAck(const uint8_t *senderMac, const AresCustodyAckPacket *ack) {
    if (txState == TX_IDLE || !sameMac(senderMac, txNextHop)) return;
    const AresHeader *header = &txPacket.header;
    if (strncmp(ack->msgId, header->msg_id, sizeof(ack->msgId)) == 0 &&
        strncmp(ack->sourceId, header->source_id, sizeof(ack->sourceId)) == 0 &&
        ack->timestamp == (uint32_t) header->timestamp) {
        txCustodyReceived = true;
    }
}

static bool isControlPacket(const AresReceivedFrame *frame, uint8_t type, size_t size) {
    if (frame->len != size) return false;
    const AresControlHeader *header = (const AresControlHeader *) frame->data;
    return header->magic[0] == ARES_CTRL_MAGIC_0 && header->magic[1] == ARES_CTRL_MAGIC_1 &&
           header->version == ARES_CTRL_VERSION && header->type == type;
}

// O tamanho do pacote diz o que ele é: 156 bytes = mensagem; tamanhos menores = controle
void processIncomingFrames() {
    AresReceivedFrame frame;
    // Limite por volta: com uma rajada de pacotes, o resto do loop() não fica parado
    for (int i = 0; i < 8 && receiveFrame(&frame); i++) {
        if (frame.len == sizeof(AresMessagePacket)) {
            AresMessagePacket packet;
            memcpy(&packet, frame.data, sizeof(packet));
            handleDataPacket(&packet, frame.senderMac);
        } else if (isControlPacket(&frame, CTRL_BEACON, sizeof(AresBeaconPacket))) {
            AresBeaconPacket beacon;
            memcpy(&beacon, frame.data, sizeof(beacon));
            handleBeacon(frame.senderMac, &beacon, frame.rssi);
        } else if (isControlPacket(&frame, CTRL_CUSTODY_ACK, sizeof(AresCustodyAckPacket))) {
            AresCustodyAckPacket ack;
            memcpy(&ack, frame.data, sizeof(ack));
            handleCustodyAck(frame.senderMac, &ack);
        } else {
            unknownFrames++;
        }
    }
}

void processSendResults() {
    AresSendResult result;
    while (receiveSendResult(&result)) {
        if (result.kind == SEND_KIND_DATA && result.sendId == txSendId) {
            txRadioDone = true;
            txRadioDelivered = result.delivered;
        }
    }
}

// ============================================================================
// 3. Entrega com custódia (store-carry-forward)
// ============================================================================

static void startAttempt(unsigned long now) {
    FlashIndexEntry *entry = flashStoreEntry(txHandle);
    if (entry != nullptr) entry->attempts++;
    txState = TX_WAIT_RADIO;
    txStartedAt = now;
    txRadioDone = false;
    txRadioDelivered = false;
    txCustodyReceived = false;
    txSendId = sendFrame(txNextHop, &txPacket, sizeof(AresMessagePacket), SEND_KIND_DATA);
    if (txSendId == 0) {
        txRadioDone = true;            // Nem saiu do rádio: conta como tentativa sem confirmação
        txRadioDelivered = false;
    }
}

static void finishDelivered() {
    deliveredCount++;
    neighborMarkSuccess(txNextHop);
    dedupRemember(&txPacket.header);   // Se a mesma voltar para nós, sabemos que já foi entregue
    char detail[40];
    macToText(txNextHop, detail, sizeof(detail));
    strncat(detail, txToGateway ? " (Gateway)" : " (Router)", sizeof(detail) - strlen(detail) - 1);
    eventLog("ENTREGUE", &txPacket.header, detail);
    flashStoreRemove(txHandle);        // Agora sim pode apagar da flash
    if (showLogs) {
        Serial.print("[ACK] ");
        printMsgId(&txPacket.header);
        Serial.printf(" | estado: CUSTÓDIA ACEITA pelo %s ", txToGateway ? "Gateway" : "Router");
        printMac(txNextHop);
        Serial.printf(". Apagada da flash. Memória: %d/%d\n", flashStoreUsed(), STORE_CAPACITY);
    }
    txHandle = STORE_NONE;
    txState = TX_IDLE;
}

// Rodada inteira sem confirmação: guarda a mensagem e desconfia do enlace
static void finishRoundFailed(unsigned long now) {
    FlashIndexEntry *entry = flashStoreEntry(txHandle);
    if (showLogs) {
        Serial.print("[TX] ");
        printMsgId(&txPacket.header);
        Serial.printf(" | %d tentativas sem confirmação de ", MAX_TX_ATTEMPTS);
        printMac(txNextHop);
        Serial.printf(". Continua guardada e volta em %d ms.\n", RETRY_BACKOFF_MS);
    }
    flashStoreDefer(txHandle, now + RETRY_BACKOFF_MS, true);
    if (entry != nullptr && entry->rounds == 1) eventLog("SEM_CONFIRMACAO", &txPacket.header, "vai tentar de novo");

    if (neighborMarkFailure(txNextHop) >= NEIGHBOR_FAIL_ROUNDS) {
        neighborRemove(txNextHop);
        if (showLogs) {
            Serial.print("[NBR] Enlace com ");
            printMac(txNextHop);
            Serial.printf(" perdido (%d rodadas sem confirmação). Vizinho removido.\n", NEIGHBOR_FAIL_ROUNDS);
        }
    }
    txHandle = STORE_NONE;
    txState = TX_IDLE;
}

void processOutgoingMessages() {
    unsigned long now = millis();

    if (txState == TX_IDLE) {
        int handle = flashStoreNext(now);
        if (handle == STORE_NONE) return;

        uint8_t previousHop[6];
        if (!flashStoreRead(handle, &txPacket, previousHop)) {
            // A gaveta não confere (flash com defeito?): registra e libera para não travar a fila
            eventLog("CORROMPIDA", nullptr, "falha ao ler a gaveta na hora do envio");
            flashStoreRemove(handle);
            return;
        }
        if (!nextHopChoose(previousHop, txNextHop, &txToGateway)) {
            // Ninguém para entregar agora: espera guardada, sem contar como falha
            flashStoreDefer(handle, now + RETRY_BACKOFF_MS, false);
            return;
        }
        txHandle = handle;
        FlashIndexEntry *entry = flashStoreEntry(handle);
        entry->inFlight = true;
        entry->attempts = 0;
        if (showLogs && entry->rounds == 0) {
            Serial.print("[TX] ");
            printMsgId(&txPacket.header);
            Serial.printf(" | P%d | enviando para ", (int) txPacket.header.priority);
            printMac(txNextHop);
            Serial.println(txToGateway ? " (Gateway)" : " (Router)");
        }
        startAttempt(now);
        return;
    }

    FlashIndexEntry *entry = flashStoreEntry(txHandle);
    if (entry == nullptr) {            // Foi apagada por outro motivo (ex.: format)
        txState = TX_IDLE;
        return;
    }

    // O ACK de custódia pode chegar antes de o loop() ver a confirmação do rádio
    if (txCustodyReceived) {
        finishDelivered();
        return;
    }

    bool attemptFailed = false;
    if (txState == TX_WAIT_RADIO) {
        if (txRadioDone) {
            if (!txRadioDelivered) {
                attemptFailed = true;
            } else {
                // O rádio do vizinho recebeu. Isso NÃO é custódia: agora esperamos o ACK
                txState = TX_WAIT_CUSTODY;
                txStartedAt = now;
            }
        } else if (now - txStartedAt >= SEND_CONFIRM_TIMEOUT_MS) {
            attemptFailed = true;
        }
    } else if (txState == TX_WAIT_CUSTODY && now - txStartedAt >= CUSTODY_TIMEOUT_MS) {
        attemptFailed = true;
    }

    if (!attemptFailed) return;
    if (entry->attempts >= MAX_TX_ATTEMPTS) {
        finishRoundFailed(now);
    } else {
        startAttempt(now);
    }
}

// ============================================================================
// 4. Vizinhos e beacon
// ============================================================================

void sendBeacon() {
    AresBeaconPacket beacon;
    memset(&beacon, 0, sizeof(beacon));
    beacon.header.magic[0] = ARES_CTRL_MAGIC_0;
    beacon.header.magic[1] = ARES_CTRL_MAGIC_1;
    beacon.header.version = ARES_CTRL_VERSION;
    beacon.header.type = CTRL_BEACON;
    beacon.role = ROLE_STORAGE;
    beacon.hopsToGateway = HOPS_UNREACHABLE;   // O Storage não oferece caminho até o Gateway
    beacon.batteryPct = readBatteryPct();
    int freeSlots = flashStoreFree();
    beacon.queueFree = (uint8_t) (freeSlots > 255 ? 255 : freeSlots);
    bool isGateway;
    if (!nextHopChoose(nullptr, beacon.nextHop, &isGateway)) memset(beacon.nextHop, 0, 6);
    beacon.sequence = ++beaconSequence;
    broadcastFrame(&beacon, sizeof(beacon));
}

void expireNeighbors(unsigned long now) {
    uint8_t removed[MAX_NEIGHBORS][6];
    int count = neighborExpire(now, NEIGHBOR_TIMEOUT_MS, removed, MAX_NEIGHBORS);
    for (int i = 0; i < count && i < MAX_NEIGHBORS; i++) {
        removeRadioPeer(removed[i]);
        if (showLogs) {
            Serial.print("[NBR] Vizinho ");
            printMac(removed[i]);
            Serial.printf(" sumiu (sem beacon há %d s). Removido.\n", NEIGHBOR_TIMEOUT_MS / 1000);
        }
    }
}

// Avisa só quando muda: "tenho para quem entregar" <-> "não tenho"
void logRouteChanges() {
    uint8_t mac[6];
    bool isGateway = false;
    bool hasRoute = nextHopChoose(nullptr, mac, &isGateway);
    if (hadRouteLogged && hasRoute == hadRoute) return;
    hadRouteLogged = true;
    hadRoute = hasRoute;
    if (!showLogs) return;
    if (hasRoute) {
        Serial.print("[ROUTE] Entregando para ");
        printMac(mac);
        Serial.println(isGateway ? " (Gateway)" : " (Router)");
    } else {
        Serial.printf("[ROUTE] Ninguém para entregar. As mensagens ficam guardadas na flash (%d).\n", flashStoreUsed());
    }
}

// ============================================================================
// 5. Comandos pelo Monitor Serial (115200, qualquer final de linha)
// ============================================================================

void printStatus() {
    Serial.println("\n--- STATUS DO ESP1 (STORAGE) ---");
    Serial.print("MAC: ");
    printMac(selfMac);
    Serial.printf(" | ID: %s | Boot nº %lu | Relógio do nó: %lu s\n", sourceId,
                  (unsigned long) nodeBootCount(), (unsigned long) nodeClockNow());
    Serial.printf("Flash: %s | Memória: %d/%d mensagens (%u bytes cada)\n", storageOk ? "OK" : "COM ERRO",
                  flashStoreUsed(), STORE_CAPACITY, (unsigned) flashStoreRecordSize());
    Serial.printf("Vizinhos: %d/%d | RAM livre: %u bytes\n", neighborCount(), MAX_NEIGHBORS, (unsigned) ESP.getFreeHeap());
    Serial.printf("Criadas: %lu | Recebidas: %lu | Entregues: %lu | ACKs enviados: %lu\n", (unsigned long) createdCount,
                  (unsigned long) receivedCount, (unsigned long) deliveredCount, (unsigned long) custodyAcksSent);
    Serial.printf("Duplicadas: %lu | ttl esgotado: %lu | recusadas (cheia): %lu | expiradas: %lu\n",
                  (unsigned long) duplicateCount, (unsigned long) ttlDroppedCount, (unsigned long) refusedFullCount,
                  (unsigned long) expiredCount);
    Serial.printf("Rádio: fila cheia=%lu | pacotes desconhecidos=%lu\n", (unsigned long) getDroppedFrames(),
                  (unsigned long) (getIgnoredFrames() + unknownFrames));
}

void printNeighbors() {
    unsigned long now = millis();
    Serial.printf("[NBR] %d vizinho(s):\n", neighborCount());
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        const AresNeighbor *n = neighborAt(i);
        if (n == nullptr) continue;
        Serial.print("  ");
        printMac(n->mac);
        const char *role = n->role == ROLE_GATEWAY ? "Gateway" : n->role == ROLE_ROUTER ? "Router" : "Storage";
        Serial.printf(" | %s", role);
        if (n->hopsToGateway == HOPS_UNREACHABLE) {
            Serial.print(" | sem rota");
        } else {
            Serial.printf(" | %u salto(s)", n->hopsToGateway);
        }
        Serial.printf(" | RSSI %d dBm | vagas %u | visto há %lu s | falhas %u\n", n->rssi, n->queueFree,
                      (unsigned long) ((now - n->lastSeenMs) / 1000), n->failedRounds);
    }
}

void printHelp() {
    Serial.println("Comandos: p0 ... p4 texto = cria mensagem (ex.: p0 Socorro, 3 pessoas no telhado)");
    Serial.println("          t = telemetria agora | s = status | n = vizinhos | q = mensagens guardadas");
    Serial.println("          log = últimos eventos | format sim = APAGA todas as mensagens da flash");
}

void runCommand(char *line) {
    if (line[0] == 'p' && line[1] >= '0' && line[1] <= '4' && (line[2] == '\0' || line[2] == ' ')) {
        const char *text = (line[2] == ' ') ? line + 3 : "teste";
        createMessage(line[1] - '0', text);
    } else if (strcmp(line, "t") == 0) {
        createTelemetry();
    } else if (strcmp(line, "s") == 0) {
        printStatus();
    } else if (strcmp(line, "n") == 0) {
        printNeighbors();
    } else if (strcmp(line, "q") == 0) {
        flashStorePrint(nodeClockNow());
    } else if (strcmp(line, "log") == 0) {
        eventLogPrintTail(20);
    } else if (strcmp(line, "format sim") == 0) {
        txState = TX_IDLE;
        txHandle = STORE_NONE;
        Serial.println("[STORE] Apagando todas as mensagens e o registro de eventos...");
        storageOk = flashStoreFormat();
    } else if (strcmp(line, "format") == 0) {
        Serial.println("Isso apaga TODAS as mensagens guardadas. Para confirmar, digite: format sim");
    } else if (line[0] != '\0') {
        printHelp();
    }
}

// A linha termina no Enter ou, se o Monitor estiver em "No line ending", depois de um
// instante sem receber nada.
void processSerialCommands() {
    static char line[80];
    static int length = 0;
    static unsigned long lastCharAt = 0;

    while (Serial.available()) {
        char c = Serial.read();
        lastCharAt = millis();
        if (c == '\n' || c == '\r') {
            line[length] = '\0';
            runCommand(line);
            length = 0;
        } else if (length < (int) sizeof(line) - 1) {
            line[length++] = c;
        }
    }
    if (length > 0 && millis() - lastCharAt >= SERIAL_LINE_IDLE_MS) {
        line[length] = '\0';
        runCommand(line);
        length = 0;
    }
}

// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(1000);

    // Configura o pino físico de debug com pull-down interno
    pinMode(DEBUG_PIN, INPUT_PULLDOWN);
    showLogs = FORCE_SOFTWARE_DEBUG || (digitalRead(DEBUG_PIN) == HIGH);
    setRadioDebug(showLogs);
    setStoreDebug(showLogs);

    Serial.println("\n--- Inicializando ARES Node ESP1 (Storage) ---");
    Serial.printf("[DEBUG] Tamanho do pacote AresMessagePacket: %u bytes\n", (unsigned) sizeof(AresMessagePacket));

    // 1. Relógio e numeração (NVS) antes de tudo: o resto usa
    nodeClockBegin();

    // 2. Rádio (também liga o Wi-Fi, que é o que dá o MAC)
    if (!initEspNowRadio(ESPNOW_CHANNEL)) {
        Serial.println("[ERRO] Falha crítica na inicialização do rádio.");
    }
    WiFi.macAddress(selfMac);
    snprintf(sourceId, sizeof(sourceId), "%s%02X%02X%02X", NODE_SOURCE_PREFIX, selfMac[3], selfMac[4], selfMac[5]);

    // 3. Flash: monta o LittleFS e recupera o que estava guardado antes de desligar
    storageOk = flashStoreBegin();
    if (!storageOk) {
        Serial.println("[ERRO] Sem armazenamento na flash: este nó não vai criar nem aceitar mensagens.");
    }
    eventLogBegin();
    eventLog("BOOT", nullptr, storageOk ? "flash OK" : "flash com erro");

    Serial.println("\n--- DESCOBRINDO ENDEREÇO MAC ---");
    Serial.print("MAC Address deste Storage: ");
    Serial.println(WiFi.macAddress());
    Serial.printf("ID das mensagens (source_id): %s | Canal: %d\n", sourceId, ESPNOW_CHANNEL);
    printHelp();

    lastBeaconAt = millis();
    lastTelemetryAt = millis();
    lastExpireAt = millis();
    sendBeacon();
}

void loop() {
    showLogs = FORCE_SOFTWARE_DEBUG || (digitalRead(DEBUG_PIN) == HIGH);
    setRadioDebug(showLogs);
    setStoreDebug(showLogs);

    unsigned long now = millis();

    processIncomingFrames();
    processSendResults();

    if (now - lastHousekeepingAt >= 1000) {
        lastHousekeepingAt = now;
        expireNeighbors(now);
        nodeClockMaybeSave();
    }
    if (now - lastExpireAt >= EXPIRE_CHECK_INTERVAL_MS) {
        lastExpireAt = now;
        expiredCount += flashStoreExpireOld(nodeClockNow(), MESSAGE_MAX_AGE_SEC);
    }
    if (now - lastBeaconAt >= BEACON_INTERVAL_MS) {
        lastBeaconAt = now;
        sendBeacon();
    }
#if TELEMETRY_INTERVAL_MS > 0
    if (storageOk && now - lastTelemetryAt >= TELEMETRY_INTERVAL_MS) {
        lastTelemetryAt = now;
        createTelemetry();
    }
#endif

    logRouteChanges();
    if (storageOk) processOutgoingMessages();
    processSerialCommands();
}
