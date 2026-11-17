#ifndef ESP_NOW_RADIO_H
#define ESP_NOW_RADIO_H

// ============================================================================
// Camada de rádio ESP-NOW do ARES.
// ESTE MÓDULO (esp_now_radio.h + src/radio/esp_now_radio.cpp) É IGUAL NOS TRÊS FIRMWARES.
//
// Regra de ouro (lição do incidente da POC First Breath): os callbacks do ESP-NOW
// rodam na tarefa do Wi-Fi, NÃO no loop(). Dentro deles não se usa Serial, HTTP,
// flash nem nada demorado. Eles só copiam o pacote para uma fila do FreeRTOS e
// o loop() processa depois, com calma.
// ============================================================================

#include <Arduino.h>
#include "ares_protocol.h"

#define RX_FRAME_MAX_LEN sizeof(AresMessagePacket)   // Maior pacote que o ARES aceita

// Pacote recebido, copiado pelo callback do rádio para a fila do FreeRTOS
typedef struct {
    uint8_t senderMac[6];              // Quem transmitiu (o vizinho, não necessariamente quem criou)
    int8_t rssi;                       // Força do sinal em dBm (0 = desconhecida)
    uint8_t len;
    uint8_t data[RX_FRAME_MAX_LEN];    // AresMessagePacket ou pacote de controle do ares_control_protocol.h
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
    bool delivered;                    // O rádio do destino confirmou o recebimento (não é custódia!)
} AresSendResult;

// fixedChannel: canal para travar o rádio (1-13), ou 0 para NÃO mexer no canal.
// O ESP2 usa 0, porque o canal dele é o do Wi-Fi/Hotspot em que está conectado.
bool initEspNowRadio(uint8_t fixedChannel);
// Retorna o id do envio (0 = falhou antes de sair do rádio). O resultado chega por receiveSendResult().
// Adiciona o destino como peer automaticamente, se precisar.
uint32_t sendFrame(const uint8_t *mac, const void *data, size_t len, AresSendKind kind);
bool broadcastFrame(const void *data, size_t len);
// Peers do ESP-NOW: o ESP32 aceita no máximo ~20. Se encher, o mais antigo sai sozinho.
bool addRadioPeer(const uint8_t *mac);
void removeRadioPeer(const uint8_t *mac);
// Não bloqueiam: retornam false se a fila estiver vazia
bool receiveFrame(AresReceivedFrame *frame);
bool receiveSendResult(AresSendResult *result);
uint32_t getDroppedFrames();           // Pacotes perdidos porque a fila de recepção estava cheia
uint32_t getIgnoredFrames();           // Pacotes com tamanho que não é de nenhum pacote ARES
void setRadioDebug(bool enabled);
bool isBroadcastMac(const uint8_t *mac);

#endif
