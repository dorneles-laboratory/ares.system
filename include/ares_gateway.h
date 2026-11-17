#ifndef ARES_GATEWAY_H
#define ARES_GATEWAY_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "ares_protocol.h"
#include "ares_config.h"

struct AresRemoteMeshNodeCache {
    char node_id[20];
    char mac_address[18];
    char role[12];
    char signature[17];
    char lat[12];
    char lon[12];
    float battery_level;
    uint32_t storage_avail_kb;
    int rssi;
    uint8_t hops;
    bool dirty;
};

struct AresCommandAckCache {
    char cmd_id[24];
    char node_id[20];
    char new_role[12];
    char signature[17];
    bool pending;
};

class AresGateway {
public:
    static AresGateway& getInstance();

    bool begin();
    void stop();
    void queuePacketForIngress(const AresMessagePacket *packet);
    void recordMeshTelemetry(const AresMessagePacket *packet, int rssi);
    void recordCommandAck(const char* cmdId, const char* nodeId, const char* newRole, const char* sig);
    void processIngressQueue();
    void checkWifiConnection();
    void checkSerialCommands();
    bool sendHeartbeat();

    void onWifiConnected();
    void onWifiDisconnected(uint8_t reason);

private:
    AresGateway();
    static AresGateway* instance;

    WebServer server;
    DNSServer dnsServer;
    Preferences prefs;

    AresMessagePacket pendingQueue[10];
    int pendingCount;
    AresRemoteMeshNodeCache remoteNodes[12];
    AresCommandAckCache pendingAcks[6];
    unsigned long lastWifiRetryTime;
    unsigned long lastPostRetryTime;
    unsigned long lastHeartbeatTime;
    bool apActive;
    bool isWifiConnected;
    bool begun;
    uint8_t lastDisconnectReason;

    String currentSsid;
    String currentPass;
    String backendUrl;
    String m2mKey;

    void loadConfig();
    void saveConfig(const String& ssid, const String& pass, const String& backend, const String& m2m);
    void startSetupPortal();
    void setupWebRoutes();
    void handleRoot();
    void handleSave();
    void handleStatus();
    void handleScan();
    void handleNotFound();
    void tryResolveLocation();
    void processDownstreamAndControlResponse(JsonDocument& resDoc);
    bool sendHttpPost(const AresMessagePacket *packet);
};

#endif // ARES_GATEWAY_H
