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

private:
    AresCaptivePortal();
    static AresCaptivePortal* instance;

    WebServer server;
    DNSServer dnsServer;
    char lastDownstreamMsg[128];

    void setupRoutes();
    void handleRoot();
    void handleStatus();
    void handleSend();
    void handleNotFound();
};

#endif // ARES_PORTAL_H
