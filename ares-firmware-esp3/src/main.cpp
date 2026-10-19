#include <Arduino.h>
#include <WiFi.h>
#include "ares_protocol.h"
#include "ares_config.h"
#include "esp_now_router.h"
#include "message_store.h"
#include "dedup_cache.h"

extern uint8_t nextHopAddress[];

bool showLogs = false;

// Envio em andamento (uma mensagem por vez até o rádio do próximo salto confirmar)
int inFlightHandle = STORE_NONE;
unsigned long sendStartedAt = 0;

// Contadores para o comando 's'
uint32_t receivedCount = 0;
uint32_t duplicateCount = 0;
uint32_t ttlDroppedCount = 0;
uint32_t storeFullCount = 0;
uint32_t forwardedCount = 0;
uint32_t injectedCount = 0;

// Última mensagem injetada pelo Serial, para o comando 'r' (teste de duplicata)
AresMessagePacket lastInjected;
bool hasLastInjected = false;

static void printMac(const uint8_t *mac) {
    for (int i = 0; i < 6; i++) {
        Serial.printf("%02X", mac[i]);
        if (i < 5) Serial.print(":");
    }
}

// Mostra até o primeiro '\0' sem depender de o campo ter '\0'
static void printField(const char *field, size_t size) {
    Serial.printf("%.*s", (int) size, field);
}

// Descarta duplicatas e ttl esgotado e guarda o resto na fila de prioridades.
// Usado tanto pelos pacotes do rádio quanto pelos injetados pelo Serial.
void handleIncomingPacket(AresMessagePacket *packet, const uint8_t *senderMac) {
    AresHeader *header = &packet->header;
    receivedCount++;

    if (dedupSeen(header)) {
        duplicateCount++;
        if (showLogs) {
            Serial.print("[DUP] ");
            printField(header->msg_id, sizeof(header->msg_id));
            Serial.println(" já foi recebida. Descartada.");
        }
        return;
    }
    dedupRemember(header);

    if (showLogs) {
        Serial.print("[RX] ");
        printField(header->msg_id, sizeof(header->msg_id));
        Serial.print(" | Origem: ");
        printField(header->source_id, sizeof(header->source_id));
        Serial.printf(" | P%d | ttl=%u | Vizinho: ", (int) header->priority, header->ttl);
        printMac(senderMac);
        Serial.println();
    }

    // O ttl conta saltos: cada Router desconta 1 e não repassa quando chega a zero
    if (header->ttl == 0) {
        ttlDroppedCount++;
        if (showLogs) Serial.println("[DROP] ttl esgotado. A mensagem não será repassada.");
        return;
    }
    header->ttl = header->ttl - 1;

    if (!storePut(packet, senderMac, millis())) {
        storeFullCount++;
        if (showLogs) Serial.println("[DROP] Fila cheia só com mensagens tão ou mais urgentes. Descartada.");
        return;
    }

    if (showLogs) {
        Serial.printf("[QUEUE] Guardada. Fila: %d/%d\n", storeUsedSlots(), STORE_SLOTS);
    }
}

// 1. Recepção: tira os pacotes da fila do rádio
void processIncomingFrames() {
    AresReceivedFrame frame;
    while (receiveFrame(&frame)) {
        handleIncomingPacket(&frame.packet, frame.senderMac);
    }
}

// 2. Envio: pega a mensagem mais urgente e a mais antiga, envia ao próximo salto e
// só libera a vaga quando o rádio dele confirmar. Sem confirmação, a mensagem
// continua guardada (store-carry-forward) e é tentada de novo mais tarde.
void processOutgoingMessages() {
    unsigned long now = millis();

    if (inFlightHandle == STORE_NONE) {
        int handle = storeNext(now);
        if (handle == STORE_NONE) return;

        AresStoredMessage *message = storeGet(handle);
        message->inFlight = true;
        message->attempts++;
        inFlightHandle = handle;
        sendStartedAt = now;

        // Só a primeira rodada aparece no log; as seguintes viram uma linha por rodada lá embaixo
        if (showLogs && message->rounds == 0) {
            Serial.print("[TX] ");
            printField(message->packet.header.msg_id, sizeof(message->packet.header.msg_id));
            Serial.printf(" | P%d | enviando ao próximo salto\n", (int) message->packet.header.priority);
        }
        sendToNextHop(&message->packet);
        return;
    }

    AresStoredMessage *message = storeGet(inFlightHandle);
    AresSendStatus status = getSendStatus();

    if (status == SEND_CONFIRMED) {
        forwardedCount++;
        if (showLogs) {
            Serial.print("[ACK] ");
            printField(message->packet.header.msg_id, sizeof(message->packet.header.msg_id));
            Serial.printf(" entregue ao próximo salto (tentativa %u/%d, rodada %u). Vaga liberada.\n",
                          message->attempts, MAX_TX_ATTEMPTS, message->rounds + 1);
        }
        storeMarkDone(inFlightHandle);
        inFlightHandle = STORE_NONE;
        clearSendStatus();
        return;
    }

    bool timedOut = (status == SEND_PENDING) && (now - sendStartedAt >= SEND_CONFIRM_TIMEOUT_MS);
    if (status != SEND_FAILED && !timedOut) return;   // Ainda esperando o callback

    clearSendStatus();
    if (message->attempts >= MAX_TX_ATTEMPTS) {
        if (showLogs) {
            Serial.print("[TX] ");
            printField(message->packet.header.msg_id, sizeof(message->packet.header.msg_id));
            Serial.printf(" | P%d | rodada %u: %d tentativas sem confirmação. Fica guardada e volta em %d ms.\n",
                          (int) message->packet.header.priority, message->rounds + 1,
                          MAX_TX_ATTEMPTS, RETRY_BACKOFF_MS);
        }
        storeDefer(inFlightHandle, now + RETRY_BACKOFF_MS);
        inFlightHandle = STORE_NONE;
        return;
    }

    // Tenta de novo a mesma mensagem, sem log: o resultado da rodada sai numa linha só
    message->attempts++;
    sendStartedAt = now;
    sendToNextHop(&message->packet);
}

void printStatus() {
    Serial.println("\n--- STATUS DO ESP3 (ROUTER) ---");
    Serial.print("MAC deste Router: ");
    Serial.println(WiFi.macAddress());
    Serial.print("Próximo salto (Gateway): ");
    printMac(nextHopAddress);
    Serial.printf(" | Canal: %d\n", ESPNOW_CHANNEL);
    Serial.printf("Recebidas: %lu | Duplicadas: %lu | Repassadas: %lu | Injetadas: %lu\n",
                  (unsigned long) receivedCount, (unsigned long) duplicateCount,
                  (unsigned long) forwardedCount, (unsigned long) injectedCount);
    Serial.printf("Descartadas: ttl=%lu | fila cheia=%lu | rádio ocupado=%lu | tamanho errado=%lu\n",
                  (unsigned long) ttlDroppedCount, (unsigned long) storeFullCount,
                  (unsigned long) getDroppedFrames(), (unsigned long) getIgnoredFrames());
    Serial.printf("Fila: %d/%d\n", storeUsedSlots(), STORE_SLOTS);
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

    uint8_t ownMac[6];
    WiFi.macAddress(ownMac);
    handleIncomingPacket(&packet, ownMac);
}

void runCommand(char *line) {
    // "p0 texto" ... "p4 texto": injeta uma mensagem com essa prioridade
    if (line[0] == 'p' && line[1] >= '0' && line[1] <= '4' && (line[2] == '\0' || line[2] == ' ')) {
        const char *text = (line[2] == ' ') ? line + 3 : "teste";
        injectTestMessage(line[1] - '0', text);
    } else if (strcmp(line, "r") == 0) {
        // Reenvia a última injetada, igual, para testar o descarte de duplicatas
        if (hasLastInjected) {
            AresMessagePacket copy;
            memcpy(&copy, &lastInjected, sizeof(copy));
            uint8_t ownMac[6];
            WiFi.macAddress(ownMac);
            handleIncomingPacket(&copy, ownMac);
        } else {
            Serial.println("Nenhuma mensagem injetada ainda. Use p0 ... p4 primeiro.");
        }
    } else if (strcmp(line, "s") == 0) {
        printStatus();
    } else if (strcmp(line, "q") == 0) {
        storePrint(millis());
    } else if (line[0] != '\0') {
        Serial.println("Comandos: s = status | q = fila | p0 ... p4 texto = injeta mensagem | r = repete a última injetada");
    }
}

// 3. Comandos pelo Monitor Serial (115200). A linha termina no Enter ou, se o
// Monitor estiver em "No line ending", depois de um instante sem receber nada.
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

    if (!initEspNowRouter()) {
        if (showLogs) Serial.println("Erro crítico na inicialização do rádio.");
    }

    // O MAC deste Router é o que vai no GATEWAY_MAC do ares_credentials.h do ESP1
    Serial.println("\n--- DESCOBRINDO ENDEREÇO MAC ---");
    Serial.print("MAC Address deste Router: ");
    Serial.println(WiFi.macAddress());
    Serial.println("Comandos: s = status | q = fila | p0 ... p4 texto = injeta mensagem | r = repete a última injetada");
}

void loop() {
    showLogs = FORCE_SOFTWARE_DEBUG || (digitalRead(DEBUG_PIN) == HIGH);
    setRadioDebug(showLogs);
    setStoreDebug(showLogs);

    processIncomingFrames();
    processOutgoingMessages();
    processSerialCommands();
}
