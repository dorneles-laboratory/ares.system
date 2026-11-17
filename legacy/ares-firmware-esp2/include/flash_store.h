#ifndef FLASH_STORE_H
#define FLASH_STORE_H

// ============================================================================
// Armazenamento das mensagens na FLASH INTERNA do ESP32 (LittleFS), sem microSD.
//
// Como funciona (o "porquê" de cada escolha):
//  - STORE_CAPACITY "gavetas" (slots) de tamanho fixo, divididas em arquivos pequenos
//    "/ares/s000.bin", "/ares/s001.bin"... de 16 gavetas cada (cabem num bloco de 4 KB).
//    Arquivos pequenos porque o LittleFS copia o arquivo inteiro a partir do ponto
//    alterado: com um arquivo único de 351 KB cada gravação levava 6 a 8 s.
//    Tudo é criado no primeiro boot: nunca falta espaço no meio e não há malloc no loop().
//  - Cada gaveta tem um CRC32 (uma "soma de verificação"). Se a energia cair no meio
//    de uma gravação, o CRC não bate e a gaveta é tratada como vazia no próximo boot.
//    Isso é seguro: o ACK de custódia só é enviado DEPOIS da gravação terminar, então
//    quem mandou a mensagem ainda a tem e vai mandar de novo.
//  - Na RAM fica só um índice pequeno (~20 bytes por gaveta: prioridade, ordem, idade).
//    Ele é reconstruído lendo o arquivo a cada boot. O pacote inteiro só é lido da
//    flash na hora de enviar.
//  - Prioridade: sai primeiro a P0 e, na mesma prioridade, a mais antiga.
//  - Memória cheia: a mensagem nova toma a gaveta da mais antiga de prioridade MENOR
//    (P4 primeiro, depois P3...). P0 nunca perde a vaga. Tudo vai para o registro de eventos.
//  - O LittleFS já espalha as gravações pela flash (wear leveling), o que aumenta a vida útil.
// ============================================================================

#include <stdint.h>
#include "ares_protocol.h"

#define STORE_NONE -1                  // Handle inválido: nenhuma mensagem

// O que fica na RAM para cada gaveta
typedef struct {
    bool used;
    bool inFlight;                     // Enviada e esperando confirmação: não pode ceder a vaga
    uint8_t priority;                  // 0-4 (valor inválido vindo do rádio vira 4)
    uint8_t attempts;                  // Tentativas na rodada atual
    uint16_t rounds;                   // Rodadas que terminaram sem confirmação
    uint32_t order;                    // Ordem de chegada (maior = mais nova)
    uint32_t receivedAtSec;            // Relógio do nó quando foi guardada (para a validade)
    uint32_t notBeforeMs;              // Não tentar enviar antes disso (pausa entre rodadas)
    uint32_t keyHash;                  // Resumo de (source_id, msg_id, timestamp) para achar duplicatas rápido
} FlashIndexEntry;

typedef enum {
    PUT_OK = 0,
    PUT_OK_EVICTED,                    // Guardada, mas outra menos urgente foi descartada para abrir espaço
    PUT_DUPLICATE,                     // Já estava guardada
    PUT_FULL,                          // Cheia só com mensagens tão ou mais urgentes
    PUT_FLASH_ERROR                    // Falha ao gravar na flash
} FlashPutResult;

// Monta o LittleFS, cria o arquivo se precisar e reconstrói o índice. Chamar no setup().
bool flashStoreBegin();
// Guarda a mensagem e só retorna depois de gravada de verdade na flash
FlashPutResult flashStorePut(const AresMessagePacket *packet, const uint8_t *previousHop);
// A próxima a enviar (P0 primeiro, depois a mais antiga), entre as que já podem ser tentadas
int flashStoreNext(uint32_t nowMs);
FlashIndexEntry *flashStoreEntry(int handle);
// Lê o pacote guardado na gaveta (e de quem ele veio, se previousHop não for nullptr)
bool flashStoreRead(int handle, AresMessagePacket *packet, uint8_t *previousHop);
// O próximo salto aceitou a custódia: apaga a gaveta
bool flashStoreRemove(int handle);
// Devolve à fila para tentar de novo a partir de untilMs. countRound = conta como rodada sem confirmação.
void flashStoreDefer(int handle, uint32_t untilMs, bool countRound);
// Apaga (e registra) as mensagens guardadas há mais de maxAgeSec. Retorna quantas saíram.
int flashStoreExpireOld(uint32_t nowSec, uint32_t maxAgeSec);
int flashStoreUsed();
int flashStoreFree();
uint32_t flashStoreRecordSize();
void flashStorePrint(uint32_t nowSec);
// Apaga TUDO (comando "format sim" no Serial)
bool flashStoreFormat();
void setStoreDebug(bool enabled);

#endif
