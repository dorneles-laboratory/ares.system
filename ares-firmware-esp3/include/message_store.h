#ifndef MESSAGE_STORE_H
#define MESSAGE_STORE_H

#include <stdint.h>
#include "ares_protocol.h"

// Fila de mensagens do Router enquanto o próximo salto não confirma o recebimento.
// Hoje fica na RAM; um módulo de microSD pode implementar as mesmas funções depois
// sem mexer no roteamento do main.cpp.

#define STORE_NONE -1                  // Handle inválido: nenhuma mensagem

typedef struct {
    bool used;
    AresMessagePacket packet;          // Pacote exatamente como será repassado
    uint8_t previousHop[6];            // MAC de quem nos entregou a mensagem
    uint32_t storedAtMs;               // Ordem de chegada: a mais antiga sai primeiro
    uint32_t notBeforeMs;              // Não tentar enviar antes disso (pausa entre rodadas)
    uint8_t attempts;                  // Tentativas na rodada atual
    uint16_t rounds;                   // Rodadas que terminaram sem confirmação
    bool inFlight;                     // Enviada e esperando confirmação: não pode ceder a vaga
} AresStoredMessage;

// Guarda uma cópia. Se estiver cheio, descarta a mais antiga de prioridade MENOR
// para abrir espaço (P0 nunca perde vaga para P4). Retorna false se não coube.
bool storePut(const AresMessagePacket *packet, const uint8_t *previousHop, uint32_t now);
// A próxima a enviar: P0 primeiro e, dentro da mesma prioridade, a mais antiga,
// entre as que já podem ser tentadas. Retorna STORE_NONE se não há nenhuma.
int storeNext(uint32_t now);
AresStoredMessage *storeGet(int handle);
// O próximo salto confirmou: libera a vaga
void storeMarkDone(int handle);
// Devolve à fila para tentar de novo só a partir de untilMs
void storeDefer(int handle, uint32_t untilMs);
// Igual ao storeDefer, mas sem contar rodada sem confirmação (ex.: ainda não há rota)
void storeWait(int handle, uint32_t untilMs);
// Descarta (e registra no log) as mensagens guardadas há mais de maxAgeMs. Retorna quantas saíram.
int storeExpireOld(uint32_t now, uint32_t maxAgeMs);
int storeFreeSlots();
int storeUsedSlots();
void storePrint(uint32_t now);
void setStoreDebug(bool enabled);

#endif
