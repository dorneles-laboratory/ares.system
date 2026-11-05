#ifndef HTTP_UPLINK_H
#define HTTP_UPLINK_H

// ============================================================================
// Envio HTTP ao servidor Ingress, numa TAREFA separada do loop().
//
// Por quê: um POST pode levar até 3 s (HTTP_TIMEOUT_MS). Se ele rodasse no loop(),
// o Gateway ficaria 3 s sem tratar o rádio e sem mandar ACKs de custódia, e os
// Routers achariam que o Gateway sumiu. Com uma tarefa do FreeRTOS só para o HTTP:
//  - o loop() continua recebendo, gravando na flash e confirmando custódia;
//  - a tarefa pega UMA mensagem por vez (um "job"), faz o POST e devolve o resultado.
// O loop() é o único que mexe na flash; a tarefa só recebe uma CÓPIA do pacote.
// (Lição do incidente da POC: HTTP nunca dentro do callback do rádio.)
// ============================================================================

#include <stdint.h>
#include "ares_protocol.h"

typedef struct {
    int handle;                        // Gaveta da flash de onde veio (o loop() usa na volta)
    AresMessagePacket packet;
    uint8_t fromMac[6];                // Último salto (Router ou Storage), vai no JSON
    uint32_t storedAtSec;              // Quando chegou ao Gateway (relógio do nó)
} UplinkJob;

typedef struct {
    int handle;
    int httpCode;                      // 200 = sucesso; negativo = erro de conexão (ex.: -1 = recusada)
} UplinkResult;

// Cria a tarefa e as filas. Chamar no setup(), depois de conectar o Wi-Fi.
bool uplinkBegin();
// A tarefa está livre para um novo job?
bool uplinkIsIdle();
bool uplinkSubmit(const UplinkJob *job);
// Não bloqueia: false se não há resultado novo
bool uplinkPollResult(UplinkResult *result);
// IP do servidor (salvo na NVS: muda pelo comando "ip" sem regravar o firmware)
void uplinkLoadServerIp();
void uplinkSetServerIp(const char *ip);
void uplinkGetServerIp(char *out, size_t size);

#endif
