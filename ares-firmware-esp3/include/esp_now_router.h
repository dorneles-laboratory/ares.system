#ifndef ESP_NOW_ROUTER_H
#define ESP_NOW_ROUTER_H

#include <Arduino.h>
#include "ares_protocol.h"

// Pacote recebido, copiado pelo callback do rádio para a fila do FreeRTOS
typedef struct {
    uint8_t senderMac[6];          // Quem transmitiu (o vizinho, não necessariamente quem criou)
    AresMessagePacket packet;
} AresReceivedFrame;

// Resultado do último envio unicast, informado pelo callback de envio do ESP-NOW
typedef enum {
    SEND_IDLE = 0,       // Nenhum envio aguardando confirmação
    SEND_PENDING,        // Enviado, esperando o callback
    SEND_CONFIRMED,      // O rádio do próximo salto confirmou o recebimento
    SEND_FAILED          // O rádio não recebeu confirmação
} AresSendStatus;

bool initEspNowRouter();
bool sendToNextHop(const AresMessagePacket *packet);
AresSendStatus getSendStatus();
void clearSendStatus();
// Não bloqueia: retorna false se não há pacote na fila
bool receiveFrame(AresReceivedFrame *frame);
uint32_t getDroppedFrames();
uint32_t getIgnoredFrames();
void setRadioDebug(bool enabled);

#endif
