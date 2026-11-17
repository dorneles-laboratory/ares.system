#ifndef ARES_RADIO_H
#define ARES_RADIO_H

#include <Arduino.h>
#include <esp_idf_version.h>
#include <esp_now.h>
#include <WiFi.h>
#include "ares_protocol.h"
#include "ares_config.h"

typedef void (*AresPacketReceivedCallback)(const uint8_t *senderMac, const AresMessagePacket *packet, int rssi);

class AresRadio {
public:
    static AresRadio& getInstance();

    bool begin(uint8_t channel = ARES_ESP_NOW_CHANNEL);
    bool sendBroadcast(const AresMessagePacket *packet);
    bool sendToMac(const uint8_t *targetMac, const AresMessagePacket *packet);

    void setPacketCallback(AresPacketReceivedCallback callback);
    bool hasSeenMessage(const char *msgId);
    void markMessageSeen(const char *msgId);

    int getLastRssi() const { return lastRssi; }

private:
    AresRadio();

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    static void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status);
    static void onDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int data_len);
#else
    static void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
    static void onDataRecv(const uint8_t *mac_addr, const uint8_t *data, int data_len);
#endif

    static AresRadio* instance;
    AresPacketReceivedCallback packetCallback;
    int lastRssi;

    char seenMsgIds[32][32];
    uint8_t seenIndex;
};

#endif // ARES_RADIO_H
