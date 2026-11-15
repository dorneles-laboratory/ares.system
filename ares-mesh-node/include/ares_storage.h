#ifndef ARES_STORAGE_H
#define ARES_STORAGE_H

#include <Arduino.h>
#include <LittleFS.h>
#include "ares_protocol.h"
#include "ares_config.h"

// Status do pacote no arquivo de log contínuo (WAL)
enum WalEntryStatus {
    WAL_STATUS_EMPTY = 0,
    WAL_STATUS_CUSTODY = 1,      // Sob custódia ativa (aguardando próximo salto / drone)
    WAL_STATUS_TRANSFERRED = 2   // Custódia transferida com sucesso (ACK recebido)
};

struct WalEntry {
    uint8_t status;
    AresMessagePacket packet;
};

class AresStorage {
public:
    static AresStorage& getInstance();

    // Inicializa o sistema de arquivos LittleFS e carrega o banco WAL
    bool begin();

    // Armazena pacote na flash (LittleFS) com preempção estrita P0..P4
    bool storePacket(const AresMessagePacket *packet);

    // Confirma transferência de custódia (quando ACK é recebido)
    bool confirmCustody(const char *msg_id);

    // Retorna número de pacotes em custódia ativa
    int getStoredCount() const { return count; }

    const AresMessagePacket* getPacketAt(int index) const;
    void removePacketAt(int index);
    void clearAll();

    // Drena pacotes em lote para o Drone Mule (FANET Bulk Drain) ordenados por prioridade (P0 primeiro)
    int drainPacketsToMule(AresMessagePacket *outArray, int maxOut);

private:
    AresStorage();
    static AresStorage* instance;

    bool _fsMounted;
    WalEntry buffer[MAX_STORED_PACKETS];
    int count; // Total em custódia ativa

    void syncWalToFlash();
    void loadWalFromFlash();
};

#endif // ARES_STORAGE_H
