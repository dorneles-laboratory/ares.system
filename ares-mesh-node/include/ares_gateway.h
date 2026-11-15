#ifndef ARES_GATEWAY_H
#define ARES_GATEWAY_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "ares_protocol.h"
#include "ares_config.h"

class AresGateway {
public:
    static AresGateway& getInstance();

    bool begin();
    void queuePacketForIngress(const AresMessagePacket *packet);
    void processIngressQueue();

private:
    AresGateway();
    static AresGateway* instance;

    AresMessagePacket pendingQueue[10];
    int pendingCount;

    bool sendHttpPost(const AresMessagePacket *packet);
};

#endif // ARES_GATEWAY_H
