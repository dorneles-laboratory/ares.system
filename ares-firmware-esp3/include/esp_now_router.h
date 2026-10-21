#ifndef ESP_NOW_ROUTER_H
#define ESP_NOW_ROUTER_H

#include <Arduino.h>
#include "ares_protocol.h"

#define RX_FRAME_MAX_LEN sizeof(AresMessagePacket)   // Maior pacote que o Router aceita

// Pacote recebido, copiado pelo callback do rádio para a fila do FreeRTOS
typedef struct {
    uint8_t senderMac[6];              // Quem transmitiu (o vizinho, não necessariamente quem criou)
    int8_t rssi;                       // Força do sinal em dBm (0 = desconhecida)
    uint8_t len;
    uint8_t data[RX_FRAME_MAX_LEN];    // AresMessagePacket ou pacote de controle do ares_router_protocol.h
} AresReceivedFrame;

// Para que serve cada envio: só o resultado de dados e de teste do Gateway interessa ao loop()
typedef enum {
    SEND_KIND_DATA = 1,
    SEND_KIND_GATEWAY_PROBE,
    SEND_KIND_CONTROL                  // Beacon e ACK de custódia: o resultado é ignorado
} AresSendKind;

// Resultado de um envio, informado pelo callback de envio do ESP-NOW
typedef struct {
    uint32_t sendId;
    AresSendKind kind;
    uint8_t destMac[6];
    bool delivered;                    // O rádio do destino confirmou o recebimento
} AresSendResult;

bool initEspNowRouter(const uint8_t *gatewayMac);
// Retorna o id do envio (0 = falhou antes de sair do rádio). O resultado chega por receiveSendResult().
uint32_t sendFrame(const uint8_t *mac, const void *data, size_t len, AresSendKind kind);
bool broadcastFrame(const void *data, size_t len);
bool addRadioPeer(const uint8_t *mac);
void removeRadioPeer(const uint8_t *mac);
// Não bloqueiam: retornam false se a fila estiver vazia
bool receiveFrame(AresReceivedFrame *frame);
bool receiveSendResult(AresSendResult *result);
uint32_t getDroppedFrames();
uint32_t getIgnoredFrames();
void setRadioDebug(bool enabled);

#endif
