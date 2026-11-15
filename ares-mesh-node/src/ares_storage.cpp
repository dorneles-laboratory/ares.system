#include "ares_storage.h"

#define ARES_WAL_FILE_PATH "/ares_wal.bin"

AresStorage* AresStorage::instance = nullptr;

AresStorage::AresStorage() : _fsMounted(false), count(0) {
    memset(buffer, 0, sizeof(buffer));
}

AresStorage& AresStorage::getInstance() {
    if (!instance) {
        instance = new AresStorage();
    }
    return *instance;
}

bool AresStorage::begin() {
    if (_fsMounted) return true;

    Serial.println("[ARES DTN WAL] Montando partição Flash LittleFS...");
    if (!LittleFS.begin(true)) {
        Serial.println("[ARES DTN WAL] ❌ Falha crítica ao inicializar LittleFS. Operando em modo volátil de emergência.");
        _fsMounted = false;
        return false;
    }

    _fsMounted = true;
    size_t totalBytes = LittleFS.totalBytes();
    size_t usedBytes = LittleFS.usedBytes();
    Serial.printf("[ARES DTN WAL] ✅ LittleFS montado. Total: %u KB | Usado: %u KB\n",
                  (unsigned int)(totalBytes / 1024), (unsigned int)(usedBytes / 1024));

    // Carrega pacotes que sobreviveram a reboots ou desligamentos
    loadWalFromFlash();
    return true;
}

void AresStorage::loadWalFromFlash() {
    if (!_fsMounted) return;

    if (!LittleFS.exists(ARES_WAL_FILE_PATH)) {
        Serial.println("[ARES DTN WAL] Nenhum arquivo WAL prévio encontrado. Iniciando banco limpo.");
        count = 0;
        return;
    }

    File walFile = LittleFS.open(ARES_WAL_FILE_PATH, "r");
    if (!walFile) {
        Serial.println("[ARES DTN WAL] Erro ao abrir arquivo WAL para leitura.");
        return;
    }

    count = 0;
    while (walFile.available() >= (int)sizeof(WalEntry) && count < MAX_STORED_PACKETS) {
        WalEntry entry;
        if (walFile.read((uint8_t*)&entry, sizeof(WalEntry)) == sizeof(WalEntry)) {
            if (entry.status == WAL_STATUS_CUSTODY) {
                buffer[count] = entry;
                count++;
            }
        }
    }
    walFile.close();

    Serial.printf("[ARES DTN WAL] 🔄 Restauração concluída: %d pacotes recuperados intactos da Flash NVS/LittleFS!\n", count);
}

void AresStorage::syncWalToFlash() {
    if (!_fsMounted) return;

    File walFile = LittleFS.open(ARES_WAL_FILE_PATH, "w");
    if (!walFile) {
        Serial.println("[ARES DTN WAL] Falha ao persistir no arquivo WAL.");
        return;
    }

    for (int i = 0; i < count; i++) {
        if (buffer[i].status == WAL_STATUS_CUSTODY) {
            walFile.write((const uint8_t*)&buffer[i], sizeof(WalEntry));
        }
    }
    walFile.flush();
    walFile.close();
}

bool AresStorage::storePacket(const AresMessagePacket *packet) {
    if (!packet) return false;

    // Se o buffer estiver cheio, aplica o Requisito RS-10 (Preempção Estrita P0..P4)
    if (count >= MAX_STORED_PACKETS) {
        int lowestPrioIndex = -1;
        int maxPriorityVal = -1; // P0 é 0 (máxima), P4 é 4 (mínima)

        for (int i = 0; i < count; i++) {
            if ((int)buffer[i].packet.header.priority > maxPriorityVal) {
                maxPriorityVal = buffer[i].packet.header.priority;
                lowestPrioIndex = i;
            }
        }

        // Se o novo pacote tem prioridade maior que o menos prioritário armazenado
        if (lowestPrioIndex >= 0 && packet->header.priority < buffer[lowestPrioIndex].packet.header.priority) {
            Serial.printf("[ARES DTN WAL] Buffer cheio: Preempção ativada! Descartando P%d (%s) para acolher P%d (%s)\n",
                          buffer[lowestPrioIndex].packet.header.priority, buffer[lowestPrioIndex].packet.header.msg_id,
                          packet->header.priority, packet->header.msg_id);
            removePacketAt(lowestPrioIndex);
        } else {
            Serial.printf("[ARES DTN WAL] Buffer cheio e pacote recebido tem prioridade insuficiente (P%d). Rejeitado.\n",
                          packet->header.priority);
            return false;
        }
    }

    // Armazena novo pacote
    buffer[count].status = WAL_STATUS_CUSTODY;
    memcpy(&buffer[count].packet, packet, sizeof(AresMessagePacket));
    buffer[count].packet.header.flags |= ARES_FLAG_STORED_IN_WAL;
    count++;

    // Grava atomicamente na partição Flash
    syncWalToFlash();

    Serial.printf("[ARES DTN WAL] Pacote %s [P%d] gravado em custódia permanente na Flash. Total em fila: %d\n",
                  packet->header.msg_id, packet->header.priority, count);
    return true;
}

bool AresStorage::confirmCustody(const char *msg_id) {
    if (!msg_id || strlen(msg_id) == 0) return false;

    for (int i = 0; i < count; i++) {
        if (strcmp(buffer[i].packet.header.msg_id, msg_id) == 0) {
            Serial.printf("[ARES DTN WAL] Custody ACK recebido para %s! Liberando slot da Flash.\n", msg_id);
            removePacketAt(i);
            return true;
        }
    }
    return false;
}

const AresMessagePacket* AresStorage::getPacketAt(int index) const {
    if (index >= 0 && index < count && buffer[index].status == WAL_STATUS_CUSTODY) {
        return &buffer[index].packet;
    }
    return nullptr;
}

void AresStorage::removePacketAt(int index) {
    if (index < 0 || index >= count) return;

    for (int i = index; i < count - 1; i++) {
        buffer[i] = buffer[i + 1];
    }
    memset(&buffer[count - 1], 0, sizeof(WalEntry));
    count--;

    syncWalToFlash();
}

void AresStorage::clearAll() {
    memset(buffer, 0, sizeof(buffer));
    count = 0;
    if (_fsMounted && LittleFS.exists(ARES_WAL_FILE_PATH)) {
        LittleFS.remove(ARES_WAL_FILE_PATH);
    }
    Serial.println("[ARES DTN WAL] Banco WAL limpo integralmente.");
}

int AresStorage::drainPacketsToMule(AresMessagePacket *outArray, int maxOut) {
    if (!outArray || maxOut <= 0 || count <= 0) return 0;

    int drained = 0;
    // Drena primeiro os de maior prioridade (P0, depois P1, P2, P3, P4)
    for (int p = ARES_P0_EMERGENCY; p <= ARES_P4_ROUTINE; p++) {
        for (int i = 0; i < count; i++) {
            if (buffer[i].status == WAL_STATUS_CUSTODY && buffer[i].packet.header.priority == p) {
                outArray[drained] = buffer[i].packet;
                drained++;
                if (drained >= maxOut) break;
            }
        }
        if (drained >= maxOut) break;
    }

    return drained;
}
