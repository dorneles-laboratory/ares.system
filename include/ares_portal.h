#ifndef ARES_PORTAL_H
#define ARES_PORTAL_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "ares_protocol.h"
#include "ares_config.h"
#include "ares_captive_html.h"

class AresCaptivePortal {
public:
    static AresCaptivePortal& getInstance();

    bool begin(const char *ssid);
    void handleClient();
    void setDownstreamNotification(const char *msg);
    // [PORTAL] Chamado quando chega um ACK de custódia para um pedido enviado por este portal
    void onCustodyAck(const char *msgId, bool fromGateway);

private:
    AresCaptivePortal();
    static AresCaptivePortal* instance;

    WebServer server;
    DNSServer dnsServer;
    char lastDownstreamMsg[128];

    // [PORTAL] Situação dos últimos pedidos enviados, para o celular mostrar se chegaram
    // 1 = enviado pelo rádio | 2 = guardado por outro nó da rede | 3 = confirmado pela base (Gateway)
    struct SentMsg { char id[32]; uint8_t state; };
    static const int MAX_SENT = 12;
    SentMsg sent[MAX_SENT];
    int sentNext;

    void setupRoutes();
    void handleRoot();
    void handleStatus();
    void handleSend();
    void handleNotFound();
};

#endif // ARES_PORTAL_H
