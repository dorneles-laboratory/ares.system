#include <Arduino.h>
#include <WiFi.h>
#include "ares_protocol.h"
#include "ares_router_protocol.h"
#include "ares_config.h"
#include "esp_now_router.h"
#include "message_store.h"
#include "dedup_cache.h"
#include "neighbor_table.h"
#include "routing.h"

static const uint8_t configuredGatewayMac[6] = GATEWAY_MAC;
uint8_t selfMac[6];

bool showLogs = false;

// --- Envio em andamento: uma mensagem por vez, até o próximo salto confirmar ---
typedef enum {
    TX_IDLE = 0,
    TX_WAIT_RADIO,                     // Esperando o rádio do próximo salto confirmar
    TX_WAIT_CUSTODY                    // Esperando o ACK de custódia do outro Router
} AresTxState;

AresTxState txState = TX_IDLE;
int txHandle = STORE_NONE;
uint8_t txNextHop[6];
bool txToGateway = false;
uint32_t txSendId = 0;
unsigned long txStartedAt = 0;
bool txRadioDone = false;
bool txRadioDelivered = false;
bool txCustodyReceived = false;
unsigned long lastGatewayDeliveryAt = 0;
bool gatewayDeliveredOnce = false;

// --- Tarefas periódicas ---
unsigned long lastBeaconAt = 0;
unsigned long lastProbeAt = 0;
unsigned long lastHousekeepingAt = 0;
uint32_t beaconSequence = 0;
uint32_t probeSendId = 0;

// Última rota mostrada no log, para avisar só quando ela muda
uint8_t loggedHops = 0;
uint8_t loggedNextHop[6] = {0, 0, 0, 0, 0, 0};
bool routeLogged = false;

// Contadores para o comando 's'
uint32_t receivedCount = 0;
uint32_t duplicateCount = 0;
uint32_t ttlDroppedCount = 0;
uint32_t storeFullCount = 0;
uint32_t forwardedToGatewayCount = 0;
uint32_t forwardedToRouterCount = 0;
uint32_t custodyAcksSent = 0;
uint32_t expiredCount = 0;
uint32_t injectedCount = 0;
uint32_t controlIgnoredCount = 0;

// Última mensagem injetada pelo Serial, para o comando 'r' (teste de duplicata)
AresMessagePacket lastInjected;
bool hasLastInjected = false;

static void printMac(const uint8_t *mac) {
    Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Mostra até o primeiro '\0' sem depender de o campo ter '\0'
static void printField(const char *field, size_t size) {
    Serial.printf("%.*s", (int) size, field);
}

static void printMsgId(const AresMessagePacket *packet) {
    printField(packet->header.msg_id, sizeof(packet->header.msg_id));
}

static bool sameMac(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, 6) == 0;
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
// Custódia: confirma ao Router anterior que a mensagem ficou com a gente
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
    if (sendFrame(toMac, &ack, sizeof(ack), SEND_KIND_CONTROL) != 0) {
        custodyAcksSent++;
        if (showLogs) {
            Serial.print("[ACK] Custódia de ");
            printField(header->msg_id, sizeof(header->msg_id));
            Serial.print(" confirmada para ");
            printMac(toMac);
            Serial.println();
        }
    }
}

// Só Routers entendem o ACK de custódia; o ESP1 e o próprio Serial não recebem
static bool expectsCustodyAck(const uint8_t *senderMac) {
    const AresNeighbor *n = neighborFind(senderMac);
    return n != nullptr && n->role == ROLE_ROUTER;
}

// ============================================================================
// 1. Recepção
// ============================================================================

// Descarta duplicatas e ttl esgotado e guarda o resto na fila de prioridades.
// Usado tanto pelos pacotes do rádio quanto pelos injetados pelo Serial.
void handleDataPacket(AresMessagePacket *packet, const uint8_t *senderMac) {
    AresHeader *header = &packet->header;
    bool ackSender = expectsCustodyAck(senderMac);
    receivedCount++;

    if (dedupSeen(header)) {
        duplicateCount++;
        if (showLogs) {
            Serial.print("[DUP] ");
            printMsgId(packet);
            Serial.println(" já foi recebida. Descartada.");
        }
        // O ACK anterior pode ter se perdido: confirma de novo para o vizinho parar de reenviar
        if (ackSender) sendCustodyAck(senderMac, header);
        return;
    }

    if (showLogs) {
        Serial.print("[RX] ");
        printMsgId(packet);
        Serial.print(" | Origem: ");
        printField(header->source_id, sizeof(header->source_id));
        Serial.printf(" | P%d | ttl=%u | Vizinho: ", (int) header->priority, header->ttl);
        printMac(senderMac);
        Serial.println();
    }

    // O ttl conta saltos: cada Router desconta 1 e não repassa quando chega a zero
    if (header->ttl == 0) {
        dedupRemember(header);
        ttlDroppedCount++;
        if (showLogs) Serial.println("[DROP] ttl esgotado. A mensagem não será repassada.");
        // A custódia foi aceita e encerrada aqui (fica no log); o vizinho pode liberar a vaga
        if (ackSender) sendCustodyAck(senderMac, header);
        return;
    }
    header->ttl = header->ttl - 1;

    if (!storePut(packet, senderMac, millis())) {
        // Sem ACK: quem enviou continua com a mensagem e tenta de novo depois
        storeFullCount++;
        if (showLogs) Serial.println("[DROP] Fila cheia só com mensagens tão ou mais urgentes. Recusada.");
        return;
    }
    dedupRemember(header);

    if (showLogs) {
        Serial.printf("[QUEUE] Guardada. Fila: %d/%d\n", storeUsedSlots(), STORE_SLOTS);
    }
    if (ackSender) sendCustodyAck(senderMac, header);
}

void handleBeacon(const uint8_t *senderMac, const AresBeaconPacket *beacon, int8_t rssi) {
    if (sameMac(senderMac, selfMac)) return;

    AresNeighborUpdate outcome = neighborUpdate(senderMac, beacon, rssi, millis());
    if (outcome == NEIGHBOR_TABLE_FULL) {
        if (showLogs) {
            Serial.print("[NBR] Tabela de vizinhos cheia. Ignorando ");
            printMac(senderMac);
            Serial.println();
        }
        return;
    }
    if (outcome == NEIGHBOR_ADDED) {
        addRadioPeer(senderMac);
        if (showLogs) {
            Serial.print("[NBR] Novo vizinho: ");
            printMac(senderMac);
            if (beacon->hopsToGateway == HOPS_UNREACHABLE) {
                Serial.print(" | sem rota até o Gateway");
            } else {
                Serial.printf(" | %u salto(s) até o Gateway", beacon->hopsToGateway);
            }
            Serial.printf(" | RSSI %d dBm | Vagas: %u\n", rssi, beacon->queueFree);
        }
    }
}

void handleCustodyAck(const uint8_t *senderMac, const AresCustodyAckPacket *ack) {
    if (txState == TX_IDLE || txToGateway || !sameMac(senderMac, txNextHop)) return;
    AresStoredMessage *message = storeGet(txHandle);
    if (message == nullptr) return;
    const AresHeader *header = &message->packet.header;
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

void processIncomingFrames() {
    AresReceivedFrame frame;
    while (receiveFrame(&frame)) {
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
            controlIgnoredCount++;
        }
    }
}

// ============================================================================
// 2. Resultados dos envios (confirmação do rádio)
// ============================================================================

void processSendResults() {
    AresSendResult result;
    while (receiveSendResult(&result)) {
        if (result.kind == SEND_KIND_GATEWAY_PROBE && result.sendId == probeSendId) {
            probeSendId = 0;
            if (result.delivered) {
                routingGatewayConfirmed(millis());
            } else {
                routingGatewayFailed();
            }
        } else if (result.kind == SEND_KIND_DATA && result.sendId == txSendId) {
            txRadioDone = true;
            txRadioDelivered = result.delivered;
        }
    }
}

// ============================================================================
// 3. Envio com confirmação e custódia (store-carry-forward)
// ============================================================================

static void startAttempt(AresStoredMessage *message, unsigned long now) {
    message->attempts++;
    txState = TX_WAIT_RADIO;
    txStartedAt = now;
    txRadioDone = false;
    txRadioDelivered = false;
    txCustodyReceived = false;
    txSendId = sendFrame(txNextHop, &message->packet, sizeof(AresMessagePacket), SEND_KIND_DATA);
    if (txSendId == 0) {
        txRadioDone = true;    // Nem saiu do rádio: conta como tentativa sem confirmação
        txRadioDelivered = false;
    }
}

static void finishDelivered(AresStoredMessage *message) {
    if (txToGateway) {
        forwardedToGatewayCount++;
        lastGatewayDeliveryAt = millis();
        gatewayDeliveredOnce = true;
    } else {
        forwardedToRouterCount++;
        neighborMarkSuccess(txNextHop);
    }
    if (showLogs) {
        Serial.print("[ACK] ");
        printMsgId(&message->packet);
        Serial.print(txToGateway ? " entregue ao Gateway " : " aceita (custódia) pelo Router ");
        printMac(txNextHop);
        Serial.printf(" (tentativa %u/%d, rodada %u). Vaga liberada.\n",
                      message->attempts, MAX_TX_ATTEMPTS, message->rounds + 1);
    }
    storeMarkDone(txHandle);
    txHandle = STORE_NONE;
    txState = TX_IDLE;
}

// Rodada inteira sem confirmação: guarda a mensagem e desconfia do enlace
static void finishRoundFailed(AresStoredMessage *message, unsigned long now) {
    if (showLogs) {
        Serial.print("[TX] ");
        printMsgId(&message->packet);
        Serial.printf(" | P%d | rodada %u: %d tentativas sem confirmação de ", (int) message->packet.header.priority,
                      message->rounds + 1, MAX_TX_ATTEMPTS);
        printMac(txNextHop);
        Serial.printf(". Fica guardada e volta em %d ms.\n", RETRY_BACKOFF_MS);
    }
    storeDefer(txHandle, now + RETRY_BACKOFF_MS);

    if (txToGateway) {
        routingGatewayFailed();
        if (showLogs) Serial.println("[ROUTE] O Gateway não respondeu. Procurando outro caminho.");
    } else if (neighborMarkFailure(txNextHop) >= NEIGHBOR_FAIL_ROUNDS) {
        neighborRemove(txNextHop);
        removeRadioPeer(txNextHop);
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
        int handle = storeNext(now);
        if (handle == STORE_NONE) return;
        AresStoredMessage *message = storeGet(handle);

        if (!routingChooseNextHop(now, message->previousHop, txNextHop, &txToGateway)) {
            // Sem caminho agora: a mensagem espera guardada e outras podem tentar
            storeWait(handle, now + RETRY_BACKOFF_MS);
            return;
        }
        // O ESP2 só tem espaço para um pacote enquanto faz o POST: entregas em sequência
        // seriam confirmadas pelo rádio e descartadas por ele, então espaçamos
        if (txToGateway && gatewayDeliveredOnce && now - lastGatewayDeliveryAt < GATEWAY_SEND_SPACING_MS) return;

        txHandle = handle;
        message->inFlight = true;
        message->attempts = 0;
        if (showLogs && message->rounds == 0) {
            Serial.print("[TX] ");
            printMsgId(&message->packet);
            Serial.printf(" | P%d | enviando para ", (int) message->packet.header.priority);
            printMac(txNextHop);
            Serial.println(txToGateway ? " (Gateway)" : " (Router)");
        }
        startAttempt(message, now);
        return;
    }

    AresStoredMessage *message = storeGet(txHandle);
    if (message == nullptr) {
        txState = TX_IDLE;
        return;
    }

    // O ACK de custódia pode chegar antes de o loop() ver a confirmação do rádio
    if (txCustodyReceived) {
        finishDelivered(message);
        return;
    }

    bool attemptFailed = false;
    if (txState == TX_WAIT_RADIO) {
        if (txRadioDone) {
            if (!txRadioDelivered) {
                attemptFailed = true;
            } else if (txToGateway) {
                // O ESP2 não manda ACK de custódia: a confirmação do rádio é o máximo que ele dá
                routingGatewayConfirmed(now);
                finishDelivered(message);
                return;
            } else {
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
    if (message->attempts >= MAX_TX_ATTEMPTS) {
        finishRoundFailed(message, now);
    } else {
        startAttempt(message, now);
    }
}

// ============================================================================
// 4. Vizinhos, beacons e rota
// ============================================================================

void sendBeacon(unsigned long now) {
    AresBeaconPacket beacon;
    memset(&beacon, 0, sizeof(beacon));
    beacon.header.magic[0] = ARES_CTRL_MAGIC_0;
    beacon.header.magic[1] = ARES_CTRL_MAGIC_1;
    beacon.header.version = ARES_CTRL_VERSION;
    beacon.header.type = CTRL_BEACON;
    beacon.role = ROLE_ROUTER;
    beacon.hopsToGateway = routingMyHops(now);
    beacon.batteryPct = readBatteryPct();
    beacon.queueFree = (uint8_t) storeFreeSlots();
    bool isGateway;
    if (!routingChooseNextHop(now, nullptr, beacon.nextHop, &isGateway)) memset(beacon.nextHop, 0, 6);
    beacon.sequence = ++beaconSequence;
    broadcastFrame(&beacon, sizeof(beacon));
}

// Testa o enlace com o ESP2 quando não há dados indo para ele. É um pacote pequeno: o
// rádio do ESP2 confirma o recebimento e o firmware dele descarta pelo tamanho.
void probeGateway(unsigned long now) {
    bool recentlyConfirmed = routingGatewayAlive(now) &&
                             (now - routingGatewayLastConfirmed()) < GATEWAY_ALIVE_MS / 2;
    if (recentlyConfirmed) return;
    // Um teste anterior sem resposta é simplesmente substituído (o id antigo passa a ser ignorado)
    AresBeaconPacket probe;
    memset(&probe, 0, sizeof(probe));
    probe.header.magic[0] = ARES_CTRL_MAGIC_0;
    probe.header.magic[1] = ARES_CTRL_MAGIC_1;
    probe.header.version = ARES_CTRL_VERSION;
    probe.header.type = CTRL_BEACON;
    probe.role = ROLE_ROUTER;
    probe.hopsToGateway = routingMyHops(now);
    probe.batteryPct = readBatteryPct();
    probe.queueFree = (uint8_t) storeFreeSlots();
    probe.sequence = beaconSequence;
    probeSendId = sendFrame(routingGatewayMac(), &probe, sizeof(probe), SEND_KIND_GATEWAY_PROBE);
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

// Mostra a rota só quando ela muda (recalculada a cada volta do loop)
void logRouteChanges(unsigned long now) {
    uint8_t hops = routingMyHops(now);
    uint8_t nextHop[6] = {0, 0, 0, 0, 0, 0};
    bool isGateway = false;
    bool hasRoute = routingChooseNextHop(now, nullptr, nextHop, &isGateway);
    if (routeLogged && hops == loggedHops && sameMac(nextHop, loggedNextHop)) return;

    routeLogged = true;
    loggedHops = hops;
    memcpy(loggedNextHop, nextHop, 6);
    if (!showLogs) return;
    if (!hasRoute || hops == HOPS_UNREACHABLE) {
        Serial.printf("[ROUTE] Sem rota até o Gateway. As mensagens ficam guardadas (%d na fila).\n", storeUsedSlots());
    } else {
        Serial.printf("[ROUTE] Rota: %u salto(s) até o Gateway, próximo salto ", hops);
        printMac(nextHop);
        Serial.println(isGateway ? " (o próprio Gateway)" : " (Router vizinho)");
    }
}

// ============================================================================
// 5. Comandos pelo Monitor Serial (115200)
// ============================================================================

void printStatus() {
    unsigned long now = millis();
    Serial.println("\n--- STATUS DO ESP3 (ROUTER) ---");
    Serial.print("MAC deste Router: ");
    printMac(selfMac);
    Serial.print("\nGateway (ESP2): ");
    printMac(routingGatewayMac());
    if (routingGatewayAlive(now)) {
        Serial.printf(" | alcançável (confirmou há %lu s)", (unsigned long) ((now - routingGatewayLastConfirmed()) / 1000));
    } else {
        Serial.print(" | sem contato direto");
    }
    Serial.printf(" | Canal: %d\n", ESPNOW_CHANNEL);
    uint8_t hops = routingMyHops(now);
    if (hops == HOPS_UNREACHABLE) {
        Serial.println("Rota: nenhuma (mensagens ficam guardadas)");
    } else {
        Serial.printf("Rota: %u salto(s) até o Gateway\n", hops);
    }
    Serial.printf("Vizinhos: %d/%d\n", neighborCount(), MAX_NEIGHBORS);
    Serial.printf("Recebidas: %lu | Duplicadas: %lu | Injetadas: %lu\n",
                  (unsigned long) receivedCount, (unsigned long) duplicateCount, (unsigned long) injectedCount);
    Serial.printf("Entregues: ao Gateway=%lu | a outro Router=%lu | ACKs de custódia enviados=%lu\n",
                  (unsigned long) forwardedToGatewayCount, (unsigned long) forwardedToRouterCount,
                  (unsigned long) custodyAcksSent);
    Serial.printf("Descartadas: ttl=%lu | expiradas=%lu | fila cheia=%lu | rádio ocupado=%lu | desconhecidas=%lu\n",
                  (unsigned long) ttlDroppedCount, (unsigned long) expiredCount, (unsigned long) storeFullCount,
                  (unsigned long) getDroppedFrames(), (unsigned long) (getIgnoredFrames() + controlIgnoredCount));
    Serial.printf("Fila: %d/%d\n", storeUsedSlots(), STORE_SLOTS);
}

void printNeighbors() {
    unsigned long now = millis();
    Serial.printf("[NBR] %d vizinho(s):\n", neighborCount());
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        const AresNeighbor *n = neighborAt(i);
        if (n == nullptr) continue;
        Serial.print("  ");
        printMac(n->mac);
        if (n->hopsToGateway == HOPS_UNREACHABLE) {
            Serial.print(" | sem rota");
        } else {
            Serial.printf(" | %u salto(s)", n->hopsToGateway);
        }
        Serial.printf(" | RSSI %d dBm | bateria ", n->rssi);
        if (n->batteryPct == BATTERY_UNKNOWN) {
            Serial.print("?");
        } else {
            Serial.printf("%u%%", n->batteryPct);
        }
        Serial.printf(" | vagas %u | visto há %lu s | falhas %u\n", n->queueFree,
                      (unsigned long) ((now - n->lastSeenMs) / 1000), n->failedRounds);
    }
}

// Monta uma mensagem no formato do ESP1 e a trata como se tivesse chegado pelo rádio.
// Serve para testar fila, prioridade e reenvio com uma placa só (modo simulação).
void injectTestMessage(int priority, const char *text) {
    AresMessagePacket packet;
    memset(&packet, 0, sizeof(packet));
    injectedCount++;
    snprintf(packet.header.msg_id, sizeof(packet.header.msg_id), "inj-%lu", (unsigned long) injectedCount);
    snprintf(packet.header.source_id, sizeof(packet.header.source_id), "ESP3-TESTE");
    snprintf(packet.header.destination_id, sizeof(packet.header.destination_id), "GATEWAY-02");
    packet.header.priority = (AresPriorityLevel) priority;
    packet.header.ttl = 64;
    packet.header.timestamp = millis();
    snprintf(packet.payload.text_message, sizeof(packet.payload.text_message), "%s", text);

    memcpy(&lastInjected, &packet, sizeof(packet));
    hasLastInjected = true;
    handleDataPacket(&packet, selfMac);
}

// Lê "AA:BB:CC:DD:EE:FF" (também aceita minúsculas)
static bool parseMac(const char *text, uint8_t mac[6]) {
    unsigned int b[6];
    if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return false;
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t) b[i];
    return true;
}

// "v AA:BB:CC:DD:EE:FF saltos [rssi]": finge que esse vizinho mandou um beacon
void simulateBeacon(const char *args) {
    char macText[18];
    int hops = 0;
    int rssi = -60;
    uint8_t mac[6];
    if (sscanf(args, "%17s %d %d", macText, &hops, &rssi) < 2 || !parseMac(macText, mac)) {
        Serial.println("Uso: v AA:BB:CC:DD:EE:FF saltos [rssi]   (ex.: v 02:00:00:00:00:AA 1 -50)");
        return;
    }
    AresBeaconPacket beacon;
    memset(&beacon, 0, sizeof(beacon));
    beacon.role = ROLE_ROUTER;
    beacon.hopsToGateway = (hops < 0 || hops > 255) ? HOPS_UNREACHABLE : (uint8_t) hops;
    beacon.batteryPct = BATTERY_UNKNOWN;
    beacon.queueFree = STORE_SLOTS;
    handleBeacon(mac, &beacon, (int8_t) rssi);
}

void printHelp() {
    Serial.println("Comandos: s = status | n = vizinhos | q = fila | p0 ... p4 texto = injeta mensagem");
    Serial.println("          r = repete a última injetada | v MAC saltos [rssi] = simula beacon de um vizinho");
}

void runCommand(char *line) {
    if (line[0] == 'p' && line[1] >= '0' && line[1] <= '4' && (line[2] == '\0' || line[2] == ' ')) {
        const char *text = (line[2] == ' ') ? line + 3 : "teste";
        injectTestMessage(line[1] - '0', text);
    } else if (strcmp(line, "r") == 0) {
        // Reenvia a última injetada, igual, para testar o descarte de duplicatas
        if (hasLastInjected) {
            AresMessagePacket copy;
            memcpy(&copy, &lastInjected, sizeof(copy));
            handleDataPacket(&copy, selfMac);
        } else {
            Serial.println("Nenhuma mensagem injetada ainda. Use p0 ... p4 primeiro.");
        }
    } else if (line[0] == 'v' && line[1] == ' ') {
        simulateBeacon(line + 2);
    } else if (strcmp(line, "s") == 0) {
        printStatus();
    } else if (strcmp(line, "n") == 0) {
        printNeighbors();
    } else if (strcmp(line, "q") == 0) {
        storePrint(millis());
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

    // Avalia se o debug está ativo pelas configurações do ares_config.h ou pelo pino físico
    showLogs = FORCE_SOFTWARE_DEBUG || (digitalRead(DEBUG_PIN) == HIGH);
    setRadioDebug(showLogs);
    setStoreDebug(showLogs);

    if (showLogs) {
        Serial.println("\n--- Inicializando ARES Node ESP3 (Router) [DEBUG ATIVADO] ---");
        Serial.printf("[DEBUG] Tamanho do pacote AresMessagePacket: %u bytes\n", sizeof(AresMessagePacket));
    }

    if (!initEspNowRouter(configuredGatewayMac)) {
        if (showLogs) Serial.println("Erro crítico na inicialização do rádio.");
    }
    WiFi.macAddress(selfMac);
    routingInit(configuredGatewayMac, selfMac);

    // O MAC deste Router é o que vai no GATEWAY_MAC do ares_credentials.h do ESP1
    Serial.println("\n--- DESCOBRINDO ENDEREÇO MAC ---");
    Serial.print("MAC Address deste Router: ");
    Serial.println(WiFi.macAddress());
    printHelp();

    // Primeiro anúncio e primeiro teste do Gateway já no boot, sem esperar o intervalo
    lastBeaconAt = millis();
    lastProbeAt = millis();
    sendBeacon(lastBeaconAt);
    probeGateway(lastProbeAt);
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
        expiredCount += storeExpireOld(now, MESSAGE_MAX_AGE_MS);
    }
    if (now - lastBeaconAt >= BEACON_INTERVAL_MS) {
        lastBeaconAt = now;
        sendBeacon(now);
    }
    if (now - lastProbeAt >= GATEWAY_PROBE_INTERVAL_MS) {
        lastProbeAt = now;
        probeGateway(now);
    }

    logRouteChanges(now);
    processOutgoingMessages();
    processSerialCommands();
}
