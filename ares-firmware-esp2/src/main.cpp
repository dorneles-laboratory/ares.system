// ============================================================================
// ARES - ESP2: GATEWAY
//
// Caminho de uma mensagem dentro do Gateway:
//  1. Chega pelo rádio (callback só copia para a fila; nada de Serial/HTTP lá dentro).
//  2. O loop() confere se é duplicata, GRAVA NA FLASH e manda o ACK de custódia
//     para quem entregou (Router ou Storage). A partir daí a responsabilidade é nossa.
//  3. Quando o Wi-Fi está ok e a tarefa de HTTP está livre, o loop() lê a próxima
//     da flash (P0 primeiro) e entrega para a tarefa fazer o POST.
//  4. Resposta 2xx do servidor: apaga da flash. Falha: espera (2 s, 4 s, 8 s... até 30 s)
//     e tenta de novo. Nada se perde se o servidor ou o Wi-Fi cair.
//
// O que mudou em relação ao Gateway da POC (e por quê):
//  - Antes havia UM pendingPacket: o que chegava durante o POST era descartado.
//    Agora são 2.000 gavetas na flash.
//  - Antes o callback do rádio imprimia no Serial (o mesmo tipo de erro do incidente).
//  - Antes o POST rodava no loop() e travava o rádio por até 3 s. Agora roda numa tarefa.
//  - O Gateway agora manda beacon (0 saltos) e ACK de custódia, então o Router
//    não precisa mais "esperar 1,5 s entre entregas".
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include "ares_protocol.h"
#include "ares_control_protocol.h"
#include "ares_config.h"
#include "esp_now_radio.h"
#include "flash_store.h"
#include "event_log.h"
#include "node_clock.h"
#include "dedup_cache.h"
#include "http_uplink.h"

uint8_t selfMac[6];
bool storageOk = false;

// Mensagem que está com a tarefa de HTTP agora
int uplinkHandle = STORE_NONE;
AresMessagePacket uplinkPacket;
uint32_t httpFailStreak = 0;           // Falhas seguidas (para a pausa crescer)
unsigned long nextUplinkAllowedAt = 0;

unsigned long lastBeaconAt = 0;
unsigned long lastExpireAt = 0;
unsigned long lastHousekeepingAt = 0;
unsigned long lastChannelCheckAt = 0;
uint32_t beaconSequence = 0;
bool lastWifiConnected = false;

// Contadores para o comando 's'
uint32_t receivedCount = 0;
uint32_t duplicateCount = 0;
uint32_t refusedCount = 0;
uint32_t postedCount = 0;
uint32_t postFailCount = 0;
uint32_t expiredCount = 0;
uint32_t unknownFrames = 0;

static void printMac(const uint8_t *mac) {
    Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void macToText(const uint8_t *mac, char *out, size_t size) {
    snprintf(out, size, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static uint8_t currentChannel() {
    uint8_t channel = 0;
    wifi_second_chan_t second;
    esp_wifi_get_channel(&channel, &second);
    return channel;
}

// ============================================================================
// 1. Recepção pelo rádio
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
    sendFrame(toMac, &ack, sizeof(ack), SEND_KIND_CONTROL);
}

void handleDataPacket(AresMessagePacket *packet, const uint8_t *senderMac) {
    AresHeader *header = &packet->header;
    receivedCount++;

    if (GATEWAY_DEBUG) {
        Serial.printf("\n[ESP-NOW RX] %.*s de ", (int) sizeof(header->msg_id), header->msg_id);
        printMac(senderMac);
        Serial.printf(" | Origem: %.*s | P%d | ttl=%u\n", (int) sizeof(header->source_id), header->source_id,
                      (int) header->priority, header->ttl);
    }

    // Já enviada ao servidor (idempotência, RF-NG-04): não manda de novo, só reconfirma
    if (dedupSeen(header)) {
        duplicateCount++;
        if (GATEWAY_DEBUG) Serial.println("[DUP] Já entregue ao servidor. ACK reenviado.");
        sendCustodyAck(senderMac, header);
        return;
    }

    // O Gateway é o destino: o ttl não é descontado aqui e não descartamos por ttl 0
    FlashPutResult result = flashStorePut(packet, senderMac);
    if (result == PUT_DUPLICATE) {
        duplicateCount++;
        if (GATEWAY_DEBUG) Serial.println("[DUP] Já está guardada esperando o POST. ACK reenviado.");
        sendCustodyAck(senderMac, header);
        return;
    }
    if (result == PUT_FULL || result == PUT_FLASH_ERROR) {
        refusedCount++;   // Sem ACK: o Router continua com a mensagem e tenta depois
        eventLog("RECUSADA", header, result == PUT_FULL ? "memória cheia" : "erro na flash");
        if (GATEWAY_DEBUG) Serial.println("[DROP] Não coube na flash. Recusada sem ACK (o vizinho continua com ela).");
        return;
    }

    char from[18];
    macToText(senderMac, from, sizeof(from));
    eventLog("CUSTODIA_ACEITA", header, from);
    sendCustodyAck(senderMac, header);
    if (GATEWAY_DEBUG) {
        Serial.printf("[STORE] Gravada na flash e custódia confirmada. Esperando POST: %d\n", flashStoreUsed());
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
    for (int i = 0; i < 8 && receiveFrame(&frame); i++) {
        if (frame.len == sizeof(AresMessagePacket)) {
            AresMessagePacket packet;
            memcpy(&packet, frame.data, sizeof(packet));
            handleDataPacket(&packet, frame.senderMac);
        } else if (isControlPacket(&frame, CTRL_BEACON, sizeof(AresBeaconPacket))) {
            // Beacons de Router/Storage (e o "teste de enlace" do Router): o Gateway não
            // precisa de tabela de vizinhos, então só ignora. O rádio já confirmou o recebimento.
        } else if (isControlPacket(&frame, CTRL_CUSTODY_ACK, sizeof(AresCustodyAckPacket))) {
            // O Gateway não envia mensagens pelo rádio, então não espera ACK de ninguém
        } else {
            unknownFrames++;
        }
    }
    // Resultados de envio (beacons e ACKs) não interessam ao Gateway: só esvazia a fila
    AresSendResult ignored;
    while (receiveSendResult(&ignored)) {
    }
}

// ============================================================================
// 2. Envio ao servidor (via tarefa de HTTP)
// ============================================================================

void processUplink() {
    unsigned long now = millis();

    // a) Chegou o resultado de um POST?
    UplinkResult result;
    if (uplinkPollResult(&result) && result.handle == uplinkHandle) {
        if (result.httpCode >= 200 && result.httpCode < 300) {
            postedCount++;
            httpFailStreak = 0;
            dedupRemember(&uplinkPacket.header);
            eventLog("ENTREGUE_SERVIDOR", &uplinkPacket.header, nullptr);
            flashStoreRemove(uplinkHandle);
            if (GATEWAY_DEBUG) {
                Serial.printf("[HTTP] %.*s entregue ao servidor. Restam na flash: %d\n",
                              (int) sizeof(uplinkPacket.header.msg_id), uplinkPacket.header.msg_id, flashStoreUsed());
            }
        } else {
            postFailCount++;
            httpFailStreak++;
            // Pausa que dobra a cada falha: 2 s, 4 s, 8 s, 16 s, 30 s (não martela um servidor fora do ar)
            uint32_t pause = HTTP_RETRY_MIN_MS;
            for (uint32_t i = 1; i < httpFailStreak && pause < HTTP_RETRY_MAX_MS; i++) pause *= 2;
            if (pause > HTTP_RETRY_MAX_MS) pause = HTTP_RETRY_MAX_MS;
            nextUplinkAllowedAt = now + pause;
            flashStoreDefer(uplinkHandle, now + pause, true);
            if (GATEWAY_DEBUG) Serial.printf("[HTTP] Continua guardada. Nova tentativa em %lu s.\n", (unsigned long) (pause / 1000));
        }
        uplinkHandle = STORE_NONE;
    }

    // b) Se a tarefa está livre e há Wi-Fi, manda a próxima
    if (uplinkHandle != STORE_NONE || !uplinkIsIdle()) return;
    if (WiFi.status() != WL_CONNECTED) return;
    if ((int32_t) (now - nextUplinkAllowedAt) < 0) return;

    int handle = flashStoreNext(now);
    if (handle == STORE_NONE) return;

    static UplinkJob job;
    if (!flashStoreRead(handle, &job.packet, job.fromMac)) {
        eventLog("CORROMPIDA", nullptr, "falha ao ler a gaveta antes do POST");
        flashStoreRemove(handle);
        return;
    }
    FlashIndexEntry *entry = flashStoreEntry(handle);
    job.handle = handle;
    job.storedAtSec = entry->receivedAtSec;
    memcpy(&uplinkPacket, &job.packet, sizeof(uplinkPacket));
    if (uplinkSubmit(&job)) {
        entry->inFlight = true;
        uplinkHandle = handle;
    }
}

// ============================================================================
// 3. Beacon, canal e Wi-Fi
// ============================================================================

void sendBeacon() {
    AresBeaconPacket beacon;
    memset(&beacon, 0, sizeof(beacon));
    beacon.header.magic[0] = ARES_CTRL_MAGIC_0;
    beacon.header.magic[1] = ARES_CTRL_MAGIC_1;
    beacon.header.version = ARES_CTRL_VERSION;
    beacon.header.type = CTRL_BEACON;
    beacon.role = ROLE_GATEWAY;
    beacon.hopsToGateway = 0;          // Eu SOU o Gateway
    beacon.batteryPct = BATTERY_UNKNOWN;
    int freeSlots = flashStoreFree();
    beacon.queueFree = (uint8_t) (freeSlots > 255 ? 255 : freeSlots);
    beacon.sequence = ++beaconSequence;
    broadcastFrame(&beacon, sizeof(beacon));
}

// Erro conhecido da wiki: o Hotspot muda de canal e Storage/Router (travados no canal 2)
// deixam de ouvir o Gateway. Não dá para consertar sozinho, mas dá para AVISAR bem alto.
void checkChannel() {
    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected != lastWifiConnected) {
        lastWifiConnected = connected;
        if (connected) {
            Serial.printf("[WiFi] Conectado. IP do Gateway: %s | Canal: %u\n", WiFi.localIP().toString().c_str(), currentChannel());
            eventLog("WIFI", nullptr, "conectado");
        } else {
            Serial.println("[WiFi] Conexão perdida. As mensagens ficam na flash até voltar.");
            eventLog("WIFI", nullptr, "desconectado");
        }
    }
    if (connected && currentChannel() != ESPNOW_CHANNEL) {
        Serial.printf("[AVISO] O Hotspot está no canal %u, mas Storage e Router usam o canal %d (ESPNOW_CHANNEL).\n",
                      currentChannel(), ESPNOW_CHANNEL);
        Serial.println("        Eles NÃO vão ouvir este Gateway. Fixe o Hotspot no canal certo ou mude o ESPNOW_CHANNEL deles.");
    }
}

void connectWifi() {
    // Limpa configurações de Wi-Fi antigas
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    delay(100);

    Serial.println("\n--- DESCOBRINDO ENDEREÇO MAC ---");
    Serial.println(WiFi.macAddress());
    Serial.println("--------------------------------\n");

    if (GATEWAY_DEBUG) {
        // Lista as redes com o canal de cada uma: ajuda a achar o canal do Hotspot
        int n = WiFi.scanNetworks();
        Serial.printf("[WiFi] Redes encontradas: %d\n", n);
        for (int i = 0; i < n; ++i) {
            Serial.printf("  [%d] %s (Canal: %d, RSSI: %d)%s\n", i + 1, WiFi.SSID(i).c_str(), (int) WiFi.channel(i),
                          (int) WiFi.RSSI(i), WiFi.SSID(i) == WIFI_SSID ? "  <- rede configurada" : "");
        }
        WiFi.scanDelete();
    }

    // Sem economia de energia no rádio: com ela, o ESP32 "cochila" entre os beacons do
    // roteador e perde pacotes ESP-NOW que chegam nesse intervalo.
    WiFi.setSleep(false);
    // A reconexão é feita pela tarefa de HTTP (keepWifiAlive), que devolve o rádio ao
    // canal do ESP-NOW quando não consegue. A automática do ESP32 ficaria varrendo canais.
    WiFi.setAutoReconnect(false);
    if (strcmp(WIFI_SSID, "DEFAULT_SSID") == 0 || strcmp(WIFI_SSID, "SEU_SSID_AQUI") == 0) {
        // Sem ares_credentials.h preenchido: nem tenta (economiza ~20 s de boot).
        // O Gateway funciona só no rádio, guardando tudo na flash.
        Serial.println("[AVISO] Wi-Fi não configurado (ares_credentials.h). Modo só rádio: as mensagens ficam na flash.");
        WiFi.disconnect();
        esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
        return;
    }
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("[WiFi] Conectando à rede...");
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[AVISO] Sem Wi-Fi agora. O Gateway continua recebendo e guardando; tenta de novo a cada 30 s.");
        // Sem Wi-Fi o canal ficaria "solto": trava no canal do ESP-NOW para continuar ouvindo
        WiFi.disconnect();
        esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    }
}

// ============================================================================
// 4. Comandos pelo Monitor Serial
// ============================================================================

void printStatus() {
    char ip[40];
    uplinkGetServerIp(ip, sizeof(ip));
    Serial.println("\n--- STATUS DO ESP2 (GATEWAY) ---");
    Serial.print("MAC: ");
    printMac(selfMac);
    Serial.printf(" | Canal: %u (Storage/Router: %d)\n", currentChannel(), ESPNOW_CHANNEL);
    Serial.printf("Wi-Fi: %s", WiFi.status() == WL_CONNECTED ? "conectado" : "DESCONECTADO");
    if (WiFi.status() == WL_CONNECTED) Serial.printf(" | IP %s | RSSI %d dBm", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    Serial.printf("\nServidor: http://%s:%d/\n", ip, INGRESS_SERVER_PORT);
    Serial.printf("Flash: %s | Esperando POST: %d/%d | RAM livre: %u bytes\n", storageOk ? "OK" : "COM ERRO",
                  flashStoreUsed(), STORE_CAPACITY, (unsigned) ESP.getFreeHeap());
    Serial.printf("Recebidas: %lu | Duplicadas: %lu | Recusadas: %lu | Expiradas: %lu\n", (unsigned long) receivedCount,
                  (unsigned long) duplicateCount, (unsigned long) refusedCount, (unsigned long) expiredCount);
    Serial.printf("POST ok: %lu | POST com falha: %lu | Rádio: fila cheia=%lu, desconhecidos=%lu\n",
                  (unsigned long) postedCount, (unsigned long) postFailCount, (unsigned long) getDroppedFrames(),
                  (unsigned long) (getIgnoredFrames() + unknownFrames));
}

void printHelp() {
    Serial.println("Comandos: s = status | q = mensagens esperando POST | log = últimos eventos");
    Serial.println("          ip 192.168.0.10 = troca o IP do servidor (fica salvo) | format sim = APAGA a flash");
}

void runCommand(char *line) {
    if (strcmp(line, "s") == 0) {
        printStatus();
    } else if (strcmp(line, "q") == 0) {
        flashStorePrint(nodeClockNow());
    } else if (strcmp(line, "log") == 0) {
        eventLogPrintTail(20);
    } else if (strncmp(line, "ip ", 3) == 0 && strlen(line) > 3) {
        // Erro da wiki: o DHCP do Hotspot troca o IP do notebook. Agora não precisa regravar.
        uplinkSetServerIp(line + 3);
        Preferences prefs;
        prefs.begin("ares", false);
        prefs.putString("server_ip", line + 3);
        prefs.end();
        httpFailStreak = 0;
        nextUplinkAllowedAt = 0;
        Serial.printf("[HTTP] Servidor agora é http://%s:%d/ (salvo na NVS)\n", line + 3, INGRESS_SERVER_PORT);
    } else if (strcmp(line, "format sim") == 0) {
        uplinkHandle = STORE_NONE;
        Serial.println("[STORE] Apagando todas as mensagens e o registro de eventos...");
        storageOk = flashStoreFormat();
    } else if (line[0] != '\0') {
        printHelp();
    }
}

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
    setRadioDebug(GATEWAY_DEBUG);
    setStoreDebug(GATEWAY_DEBUG);

    Serial.println("\n--- Inicializando ARES Node ESP2 (Gateway) ---");
    Serial.printf("[DEBUG] SSID configurado: %s\n", WIFI_SSID);

    nodeClockBegin();
    uplinkLoadServerIp();
    char ip[40];
    uplinkGetServerIp(ip, sizeof(ip));
    Serial.printf("[DEBUG] Servidor Ingress: http://%s:%d/\n", ip, INGRESS_SERVER_PORT);

    // 1. Wi-Fi primeiro: ele define o canal que o ESP-NOW vai usar
    connectWifi();
    WiFi.macAddress(selfMac);

    // 2. Flash: recupera o que ficou guardado sem POST antes de desligar
    storageOk = flashStoreBegin();
    eventLog("BOOT", nullptr, storageOk ? "flash OK" : "flash com erro");

    // 3. Rádio ESP-NOW no canal atual (0 = não mexe no canal do Wi-Fi)
    if (initEspNowRadio(0)) {
        Serial.println("[ESP-NOW] Receptor inicializado e escutando.");
    } else {
        Serial.println("[ERRO] Falha crítica ao iniciar o rádio receptor.");
    }

    // 4. Tarefa de HTTP
    if (!uplinkBegin()) Serial.println("[ERRO] Não consegui criar a tarefa de HTTP.");

    lastWifiConnected = WiFi.status() == WL_CONNECTED;
    checkChannel();
    printHelp();
    sendBeacon();
    lastBeaconAt = millis();
    lastExpireAt = millis();
}

void loop() {
    unsigned long now = millis();

    processIncomingFrames();
    if (storageOk) processUplink();

    if (now - lastHousekeepingAt >= 1000) {
        lastHousekeepingAt = now;
        nodeClockMaybeSave();
    }
    if (now - lastBeaconAt >= BEACON_INTERVAL_MS) {
        lastBeaconAt = now;
        sendBeacon();
    }
    if (now - lastChannelCheckAt >= 10000) {
        lastChannelCheckAt = now;
        checkChannel();
    }
    if (now - lastExpireAt >= EXPIRE_CHECK_INTERVAL_MS) {
        lastExpireAt = now;
        expiredCount += flashStoreExpireOld(nodeClockNow(), MESSAGE_MAX_AGE_SEC);
    }
    processSerialCommands();

    // Pequena pausa: deixa a tarefa de HTTP e o Wi-Fi trabalharem (e alimenta o watchdog)
    delay(2);
}
