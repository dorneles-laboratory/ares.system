#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <mbedtls/md.h>
#include <soc/soc.h>
#include <soc/rtc_cntl_reg.h>
#include <sys/time.h>
#include <time.h>
#include "ares_protocol.h"
#include "ares_config.h"
#include "ares_radio.h"
#include "ares_storage.h"
#include "ares_portal.h"
#include "ares_gateway.h"
#include "ares_wps.h"

// Variáveis globais de identificação e estado dinâmico do nó (Firmware Único Universal)
char g_ares_node_id[32] = {0};
char g_ares_mac_str[18] = {0};
char g_ares_node_sig[17] = {0};
char g_ares_ssid[32] = {0};
char g_ares_lat[16] = "INDEFINIDO";
char g_ares_lon[16] = "INDEFINIDO";
char g_ares_altura[16] = "INDEFINIDO";
uint8_t g_ares_role = ARES_ROLE_RELAY;
float g_ares_battery = -1.0f; // -1.0 = Alimentação Externa (USB) / Sem bateria física conectada

static Preferences g_rolePrefs;
unsigned long lastBeaconTime = 0;
unsigned long lastAliveBlinkTime = 0;
unsigned long packetLedTurnOffTime = 0;
unsigned long gatewayPulsePressStart = 0;
bool gatewayPulseTriggered = false;
bool batteryDegradedMode = false;

/**
 * Calcula assinatura criptográfica exclusiva por nó (HMAC-SHA256 truncado em 16 caracteres hex = 8 bytes)
 * Usa acelerador mbedtls nativo do ESP32.
 */
void computeNodeHmacHex(const char* nodeId, const char* payload, char* outHex17) {
    if (!outHex17) return;
    char inputBuf[128] = {0};
    snprintf(inputBuf, sizeof(inputBuf), "%s:%s", nodeId ? nodeId : "", payload ? payload : "");

    uint8_t hmacOut[32] = {0};
    const mbedtls_md_info_t* mdInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_hmac(mdInfo,
                    (const unsigned char*)ARES_MASTER_SECRET,
                    strlen(ARES_MASTER_SECRET),
                    (const unsigned char*)inputBuf,
                    strlen(inputBuf),
                    hmacOut);

    for (int i = 0; i < 8; i++) {
        snprintf(&outHex17[i * 2], 3, "%02X", hmacOut[i]);
    }
    outHex17[16] = '\0';
}

void triggerPacketActivityLed() {
    digitalWrite(PIN_LED_PACKET_ACTIVITY, HIGH);
    packetLedTurnOffTime = millis() + 150;
}

/**
 * Troca dinamicamente o papel operacional do ESP32 em tempo de execução (sem precisar regravar firmware)
 */
void switchNodeRole(uint8_t newRole, bool persistToNvs) {
    if (newRole < ARES_ROLE_GATEWAY || newRole > ARES_ROLE_DRONE_MULE) {
        newRole = ARES_ROLE_RELAY;
    }

    uint8_t prevRole = g_ares_role;
    g_ares_role = newRole;

    if (persistToNvs) {
        g_rolePrefs.begin("ares_node", false);
        g_rolePrefs.putUChar("role_v2", g_ares_role);
        g_rolePrefs.end();
    }

    Serial.println("\n=======================================================");
    Serial.printf(" 🔄 [ARES DYNAMIC ROLE] Transição de Função: %s (%d) ➔ %s (%d)\n",
                  aresRoleToString(prevRole), prevRole, aresRoleToString(g_ares_role), g_ares_role);
    Serial.println("=======================================================");
    Serial.flush();

    // Ajusta frequência de CPU conforme papel (após esvaziar buffer UART para não corromper caracteres)
    if (g_ares_role == ARES_ROLE_RELAY || g_ares_role == ARES_ROLE_STORAGE) {
        setCpuFrequencyMhz(80);
    } else {
        setCpuFrequencyMhz(160);
    }

    // Se saiu de GATEWAY, encerra conexões de Gateway
    if (prevRole == ARES_ROLE_GATEWAY && g_ares_role != ARES_ROLE_GATEWAY) {
        AresGateway::getInstance().stop();
    }

    // Ativa os serviços do novo papel com potência TX moderada para evitar queda de tensão (Brownout) no USB
    if (g_ares_role == ARES_ROLE_GATEWAY) {
        AresGateway::getInstance().begin();
        WiFi.setTxPower(WIFI_POWER_15dBm);
        AresRadio::getInstance().begin(ARES_ESP_NOW_CHANNEL);
    } else if (g_ares_role == ARES_ROLE_ROUTER) {
        AresCaptivePortal::getInstance().begin(g_ares_ssid);
        WiFi.setTxPower(WIFI_POWER_15dBm);
        AresRadio::getInstance().begin(ARES_ESP_NOW_CHANNEL);
    } else {
        WiFi.mode(WIFI_STA);
        WiFi.setTxPower(WIFI_POWER_15dBm);
        AresRadio::getInstance().begin(ARES_ESP_NOW_CHANNEL);
    }
}

// Envia confirmação de custódia DTN (ACK) para o emissor liberar a memória flash
void sendCustodyAck(const char* targetMsgId, const char* targetNodeId) {
    AresMessagePacket ack = {};
    snprintf(ack.header.msg_id, sizeof(ack.header.msg_id), "ACK-%s", targetMsgId);
    strncpy(ack.header.source_id, ARES_NODE_ID, sizeof(ack.header.source_id) - 1);
    strncpy(ack.header.destination_id, targetNodeId, sizeof(ack.header.destination_id) - 1);
    ack.header.priority = ARES_P0_EMERGENCY;
    ack.header.direction = ARES_DIR_DOWNSTREAM;
    ack.header.msg_type = ARES_MSG_CUSTODY_ACK;
    ack.header.flags = ARES_FLAG_STORED_IN_WAL;
    ack.header.ttl = 16;
    ack.header.timestamp = (uint32_t)time(nullptr);
    strncpy(ack.payload.text_message, targetMsgId, sizeof(ack.payload.text_message) - 1);

    AresRadio::getInstance().sendBroadcast(&ack);
    Serial.printf("[%s] [DTN CUSTODY] ACK de custódia emitido para pacote %s\n", ARES_NODE_ID, targetMsgId);
}

// Retransmite pacote na malha ESP-NOW com decremento de TTL
void forwardPacketOnMesh(const AresMessagePacket *packet) {
    if (!packet || packet->header.ttl <= 1) return;
    AresMessagePacket fwd = *packet;
    fwd.header.hop_count++;
    fwd.header.ttl--;
    for (int i = 0; i < 6; i++) {
        if (fwd.hops_trail[i] == 0) {
            fwd.hops_trail[i] = g_ares_role;
            break;
        }
    }
    AresRadio::getInstance().sendBroadcast(&fwd);
}

void onPacketReceived(const uint8_t *senderMac, const AresMessagePacket *packet, int rssi) {
    if (!packet) return;

    // Ignora pacotes originados por este próprio nó que ecoaram de outros Relays
    if (strcmp(packet->header.source_id, ARES_NODE_ID) == 0) {
        return;
    }

    // Deduplicação contra loops de inundação RF (antes de acionar o LED)
    if (AresRadio::getInstance().hasSeenMessage(packet->header.msg_id)) {
        return;
    }
    AresRadio::getInstance().markMessageSeen(packet->header.msg_id);

    triggerPacketActivityLed();

    // 0. Sincronização automática de relógio UNIX pela malha RF (dispensa módulo RTC físico)
    if (packet->header.timestamp > 1700000000UL) {
        time_t localNow = time(nullptr);
        if (localNow < 1700000000L || labs((long)packet->header.timestamp - (long)localNow) > 5) {
            struct timeval tv = { .tv_sec = (time_t)packet->header.timestamp, .tv_usec = 0 };
            settimeofday(&tv, nullptr);
        }
    }

    // 1. Confirmação de Custódia DTN
    if (packet->header.msg_type == ARES_MSG_CUSTODY_ACK) {
        Serial.printf("[%s] 📥 [DTN CUSTODY ACK] Confirmação recebida de %s para pacote %s (RSSI: %d dBm)\n",
                      ARES_NODE_ID, packet->header.source_id, packet->payload.text_message, rssi);
        AresStorage::getInstance().confirmCustody(packet->payload.text_message);
        return;
    }

    // =========================================================================
    // 2. PLANO DE CONTROLE INTERNO RF (Telemetria, Comandos de Função e ACKs)
    // =========================================================================
    if (packet->header.msg_type == ARES_MSG_CONTROL_CMD) {
        // Verifica se a ordem de troca de papel é para este nó
        if (strcmp(packet->header.destination_id, ARES_NODE_ID) == 0) {
            char cmdId[24] = {0};
            int newRoleCode = 3;
            char rxSig[17] = {0};

            const char* txt = packet->payload.text_message;
            const char* pCmd = strstr(txt, "CMD:");
            if (pCmd) {
                pCmd += 4;
                int k = 0;
                while (pCmd[k] && pCmd[k] != '|' && k < 23) { cmdId[k] = pCmd[k]; k++; }
            }
            const char* pRole = strstr(txt, "ROLE:");
            if (pRole) {
                newRoleCode = atoi(pRole + 5);
            }
            const char* pSig = strstr(txt, "SIG:");
            if (pSig) {
                pSig += 4;
                int k = 0;
                while (pSig[k] && pSig[k] != '|' && k < 16) { rxSig[k] = pSig[k]; k++; }
            }

            char expectedSig[17] = {0};
            char verifyPayload[32] = {0};
            snprintf(verifyPayload, sizeof(verifyPayload), "SET_ROLE:%d", newRoleCode);
            computeNodeHmacHex(ARES_NODE_ID, verifyPayload, expectedSig);

            if (strcasecmp(rxSig, expectedSig) == 0) {
                Serial.printf("[%s] 🔐 [CONTROL PLANE RF] Ordem autenticada recebida via rádio! Trocando papel para %s (%d)\n",
                              ARES_NODE_ID, aresRoleToString(newRoleCode), newRoleCode);
                switchNodeRole((uint8_t)newRoleCode, true);

                // Emite ACK de controle de volta pela malha até o Gateway
                AresMessagePacket ackPkt = {};
                snprintf(ackPkt.header.msg_id, sizeof(ackPkt.header.msg_id), "CACK-%s", cmdId);
                strncpy(ackPkt.header.source_id, ARES_NODE_ID, sizeof(ackPkt.header.source_id) - 1);
                strncpy(ackPkt.header.destination_id, "GATEWAY", sizeof(ackPkt.header.destination_id) - 1);
                ackPkt.header.priority = ARES_P0_EMERGENCY;
                ackPkt.header.direction = ARES_DIR_UPSTREAM;
                ackPkt.header.msg_type = ARES_MSG_CONTROL_ACK;
                ackPkt.header.ttl = 12;
                ackPkt.header.hop_count = 1;
                ackPkt.header.timestamp = (uint32_t)time(nullptr);
                snprintf(ackPkt.payload.text_message, sizeof(ackPkt.payload.text_message),
                         "ACK:%s|ROLE:%s|SIG:%s", cmdId, aresRoleToString(g_ares_role), expectedSig);
                AresRadio::getInstance().sendBroadcast(&ackPkt);
            } else {
                Serial.printf("[%s] ❌ [CONTROL PLANE RF] Assinatura inválida no comando %s!\n", ARES_NODE_ID, cmdId);
            }
        } else {
            // Não é para este nó: retransmite por radiofrequência para alcançar o nó destino X!
            Serial.printf("[%s] 📡 Retransmitindo comando de controle RF (%s) para %s (hop %d)\n",
                          ARES_NODE_ID, packet->header.msg_id, packet->header.destination_id, packet->header.hop_count + 1);
            forwardPacketOnMesh(packet);
        }
        return;
    }

    if (packet->header.msg_type == ARES_MSG_CONTROL_ACK) {
        if (g_ares_role == ARES_ROLE_GATEWAY) {
            char cmdId[24] = {0};
            char newRole[12] = {0};
            char sig[17] = {0};
            const char* txt = packet->payload.text_message;
            const char* pAck = strstr(txt, "ACK:");
            if (pAck) {
                pAck += 4;
                int k = 0;
                while (pAck[k] && pAck[k] != '|' && k < 23) { cmdId[k] = pAck[k]; k++; }
            }
            const char* pRole = strstr(txt, "ROLE:");
            if (pRole) {
                pRole += 5;
                int k = 0;
                while (pRole[k] && pRole[k] != '|' && k < 11) { newRole[k] = pRole[k]; k++; }
            }
            const char* pSig = strstr(txt, "SIG:");
            if (pSig) {
                pSig += 4;
                int k = 0;
                while (pSig[k] && pSig[k] != '|' && k < 16) { sig[k] = pSig[k]; k++; }
            }
            Serial.printf("[ARES GATEWAY] ✅ Confirmação RF recebida do nó %s: função alterada para %s!\n",
                          packet->header.source_id, newRole);
            AresGateway::getInstance().recordCommandAck(cmdId, packet->header.source_id, newRole, sig);
        } else {
            forwardPacketOnMesh(packet);
        }
        return;
    }

    if (packet->header.msg_type == ARES_MSG_CONTROL_TELEMETRY) {
        if (g_ares_role == ARES_ROLE_GATEWAY) {
            Serial.printf("[%s] 📥 [RX MALHA RF] Telemetria recebida de %s | RSSI: %d dBm | Saltos: %d | %s\n",
                          ARES_NODE_ID, packet->header.source_id, rssi, packet->header.hop_count, packet->payload.text_message);
            AresGateway::getInstance().recordMeshTelemetry(packet, rssi);
        } else if (g_ares_battery < 0.0f || g_ares_battery >= 20.0f) {
            Serial.printf("[%s] 🔄 [RELAY RF] Repassando telemetria do nó %s rumo ao Gateway (RSSI: %d dBm | salto %d ➔ %d)\n",
                          ARES_NODE_ID, packet->header.source_id, rssi, packet->header.hop_count, packet->header.hop_count + 1);
            forwardPacketOnMesh(packet);
        }
        return;
    }

    // =========================================================================
    // 3. PLANO DE DADOS DTN (Mensagens SOS P0 a P4 e Custódia)
    // =========================================================================
    Serial.printf("[%s] 📨 [RX DADOS DTN] Pacote %s de %s ➔ %s | Prioridade: P%d | RSSI: %d dBm | Msg: '%s'\n",
                  ARES_NODE_ID, packet->header.msg_id, packet->header.source_id, packet->header.destination_id,
                  packet->header.priority, rssi, packet->payload.text_message);

    bool isDroneMuleNearby = (strstr(packet->header.source_id, "MULE") != nullptr ||
                              strstr(packet->header.source_id, "DRONE") != nullptr ||
                              packet->header.msg_type == ARES_MSG_BULK_DRAIN_REQ);

    if (isDroneMuleNearby && AresStorage::getInstance().getStoredCount() > 0) {
        AresMessagePacket burstQueue[10];
        int countToDrain = AresStorage::getInstance().drainPacketsToMule(burstQueue, 10);
        for (int i = 0; i < countToDrain; i++) {
            burstQueue[i].header.direction = ARES_DIR_UPSTREAM;
            burstQueue[i].header.hop_count++;
            strncpy(burstQueue[i].header.destination_id, packet->header.source_id, sizeof(burstQueue[i].header.destination_id) - 1);
            AresRadio::getInstance().sendBroadcast(&burstQueue[i]);
            delay(15);
        }
    }

    if (g_ares_role == ARES_ROLE_ROUTER) {
        if (packet->header.direction == ARES_DIR_DOWNSTREAM) {
            if (strcmp(packet->header.destination_id, ARES_NODE_ID) == 0 || strcmp(packet->header.destination_id, "BROADCAST") == 0) {
                Serial.printf("[%s] NOTIFICAÇÃO DA BASE RECEBIDA: %s\n", ARES_NODE_ID, packet->payload.text_message);
                AresCaptivePortal::getInstance().setDownstreamNotification(packet->payload.text_message);
            }
        }
    } else if (g_ares_role == ARES_ROLE_RELAY) {
        if (g_ares_battery >= 0.0f && g_ares_battery < 20.0f && packet->header.priority > ARES_P1_RESCUE) {
            Serial.printf("[%s] [EAR] Bateria crítica (%.1f%%). Recusando tráfego P%d para poupar energia para P0/P1.\n",
                          ARES_NODE_ID, g_ares_battery, packet->header.priority);
            return;
        }
        Serial.printf("[%s] 🔄 [RELAY DADOS] Retransmitindo pacote %s de %s ➔ %s (salto %d ➔ %d)\n",
                      ARES_NODE_ID, packet->header.msg_id, packet->header.source_id,
                      packet->header.destination_id, packet->header.hop_count, packet->header.hop_count + 1);
        forwardPacketOnMesh(packet);
    } else if (g_ares_role == ARES_ROLE_STORAGE) {
        if (AresStorage::getInstance().storePacket(packet)) {
            sendCustodyAck(packet->header.msg_id, packet->header.source_id);
        }
    } else if (g_ares_role == ARES_ROLE_GATEWAY) {
        AresGateway::getInstance().queuePacketForIngress(packet);
        sendCustodyAck(packet->header.msg_id, packet->header.source_id);
    }
}

void setup() {
    // Desativa o detector de Brownout hipersensível para evitar reset por pico de corrente de RF no cabo USB
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    Serial.begin(115200);
    delay(800);

    pinMode(PIN_LED_SYSTEM_ALIVE, OUTPUT);
    pinMode(PIN_LED_PACKET_ACTIVITY, OUTPUT);
    pinMode(PIN_GATEWAY_BOOT_BTN, INPUT_PULLUP);
    pinMode(PIN_GATEWAY_PULSE, INPUT_PULLUP);
    digitalWrite(PIN_LED_SYSTEM_ALIVE, HIGH);
    digitalWrite(PIN_LED_PACKET_ACTIVITY, LOW);

    // 1. Lê o MAC address único de fábrica do eFuse
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(g_ares_mac_str, sizeof(g_ares_mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    snprintf(g_ares_node_id, sizeof(g_ares_node_id), "ESP32_%02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(g_ares_ssid, sizeof(g_ares_ssid), "ARES-AP-%02X%02X", mac[4], mac[5]);

    // 2. Calcula a assinatura criptográfica exclusiva de hardware deste nó
    computeNodeHmacHex(g_ares_node_id, "IDENTITY", g_ares_node_sig);

    // 3. Todos os nós nascem como RELAY por padrão (salvo se alterado por pulso físico ou ordem RF)
    g_rolePrefs.begin("ares_node", false);
    g_ares_role = g_rolePrefs.getUChar("role_v2", ARES_ROLE_RELAY);
    if (digitalRead(PIN_GATEWAY_BOOT_BTN) == LOW || digitalRead(PIN_GATEWAY_PULSE) == LOW) {
        Serial.println("[HARDWARE TRIGGER] ⚡ Pulso detectado na porta de Gateway (GPIO 0 / GPIO 13)! Promovendo a GATEWAY inicial.");
        g_ares_role = ARES_ROLE_GATEWAY;
        g_rolePrefs.putUChar("role_v2", g_ares_role);
    }
    g_rolePrefs.end();

    AresStorage::getInstance().begin();
    AresLocationManager::getInstance().begin();

    Serial.println("\n=======================================================");
    Serial.printf(" ⭐ ARES UNIFIED MESH NODE — %s\n", ARES_NODE_ID);
    Serial.printf("    MAC de Fábrica:   %s\n", g_ares_mac_str);
    Serial.printf("    Assinatura Nó:    %s (HMAC-SHA256)\n", g_ares_node_sig);
    Serial.printf("    Papel Inicial:    %s (%d) | Canal: %d\n", aresRoleToString(g_ares_role), g_ares_role, ARES_ESP_NOW_CHANNEL);
    Serial.printf("    Bateria:          %s\n", (g_ares_battery < 0.0f ? "NÃO CONECTADA (Alimentação Externa / USB)" : "Monitorando ADC"));
    Serial.printf("    Localização:      Lat %s, Lon %s (%s)\n", ARES_NODE_LAT, ARES_NODE_LON, ARES_NODE_ALTURA);
    Serial.println("    Dica: Segure o botão BOOT (GPIO 0) ou pulso no GPIO 13");
    Serial.println("          para promover este nó a GATEWAY inicial.");
    Serial.println("=======================================================");

    AresRadio::getInstance().setPacketCallback(onPacketReceived);
    switchNodeRole(g_ares_role, false);
}

void loop() {
    unsigned long now = millis();

    // 1. Comandos seriais disponíveis em todos os nós (role, wifi, backend, scan, bat, hb, s)
    AresGateway::getInstance().checkSerialCommands();

    // 2. Monitora pulso físico no botão BOOT (GPIO 0) ou porta GPIO 13 para promover a GATEWAY em tempo real
    bool pinPressed = (digitalRead(PIN_GATEWAY_BOOT_BTN) == LOW || digitalRead(PIN_GATEWAY_PULSE) == LOW);
    if (pinPressed) {
        if (gatewayPulsePressStart == 0) {
            gatewayPulsePressStart = now;
        } else if (!gatewayPulseTriggered && (now - gatewayPulsePressStart >= 500)) {
            gatewayPulseTriggered = true;
            Serial.println("\n[HARDWARE TRIGGER] ⚡ Pulso físico confirmado (GPIO 0 / GPIO 13)! Assumindo função GATEWAY inicial...");
            switchNodeRole(ARES_ROLE_GATEWAY, true);
        }
    } else {
        gatewayPulsePressStart = 0;
        gatewayPulseTriggered = false;
    }

    // 3. Adaptação Autônoma por Bateria (apenas quando há bateria conectada e < 20%)
    if (g_ares_battery >= 0.0f && g_ares_battery < 20.0f && !batteryDegradedMode &&
        (g_ares_role == ARES_ROLE_ROUTER || g_ares_role == ARES_ROLE_STORAGE)) {
        batteryDegradedMode = true;
        Serial.printf("\n[ENERGY-AWARE] 🪫 Bateria crítica (%.1f%%)! Rebaixando função de %s para RELAY de baixo consumo.\n",
                      g_ares_battery, aresRoleToString(g_ares_role));
        switchNodeRole(ARES_ROLE_RELAY, false);
    } else if ((g_ares_battery < 0.0f || g_ares_battery >= 25.0f) && batteryDegradedMode) {
        batteryDegradedMode = false;
    }

    // 4. Pulso Heartbeat visual no GPIO 2
    if (now - lastAliveBlinkTime >= 1000) {
        lastAliveBlinkTime = now;
        digitalWrite(PIN_LED_SYSTEM_ALIVE, !digitalRead(PIN_LED_SYSTEM_ALIVE));
    }

    if (packetLedTurnOffTime > 0 && now >= packetLedTurnOffTime) {
        digitalWrite(PIN_LED_PACKET_ACTIVITY, LOW);
        packetLedTurnOffTime = 0;
    }

    // 5. Executa tarefas específicas da função atual do nó
    if (g_ares_role == ARES_ROLE_ROUTER) {
        AresCaptivePortal::getInstance().handleClient();
    } else if (g_ares_role == ARES_ROLE_GATEWAY) {
        AresGateway::getInstance().processIngressQueue();
    }

    // 6. Emite telemetria assinada do Plano de Controle Interno RF a cada BEACON_INTERVAL_MS
    if (now - lastBeaconTime >= BEACON_INTERVAL_MS) {
        lastBeaconTime = now;

        char rolePayload[32] = {0};
        char teleSig[17] = {0};
        snprintf(rolePayload, sizeof(rolePayload), "ROLE:%s", aresRoleToString(g_ares_role));
        computeNodeHmacHex(ARES_NODE_ID, rolePayload, teleSig);

        time_t epochNow = time(nullptr);
        uint32_t pktTs = (epochNow > 1700000000L) ? (uint32_t)epochNow : (now / 1000);

        AresMessagePacket telePkt = {};
        snprintf(telePkt.header.msg_id, sizeof(telePkt.header.msg_id), "%s-T-%lu", ARES_NODE_ID, now / 1000);
        strncpy(telePkt.header.source_id, ARES_NODE_ID, sizeof(telePkt.header.source_id) - 1);
        strncpy(telePkt.header.destination_id, "GATEWAY", sizeof(telePkt.header.destination_id) - 1);
        telePkt.header.priority = ARES_P4_ROUTINE;
        telePkt.header.direction = ARES_DIR_UPSTREAM;
        telePkt.header.msg_type = ARES_MSG_CONTROL_TELEMETRY;
        telePkt.header.flags = ((g_ares_battery >= 0.0f && g_ares_battery < 20.0f) ? ARES_FLAG_ENERGY_LOW : 0) | ARES_FLAG_AUTHENTICATED;
        telePkt.header.ttl = 16;
        telePkt.header.hop_count = 1;
        telePkt.header.timestamp = pktTs;

        telePkt.telemetry.battery_level = g_ares_battery;
        telePkt.telemetry.storage_avail_kb = AresStorage::getInstance().getAvailableSpaceKb();
        telePkt.telemetry.last_rssi = AresRadio::getInstance().getLastRssi();

        strncpy(telePkt.location.lat, ARES_NODE_LAT, sizeof(telePkt.location.lat) - 1);
        strncpy(telePkt.location.lon, ARES_NODE_LON, sizeof(telePkt.location.lon) - 1);
        strncpy(telePkt.location.altura, ARES_NODE_ALTURA, sizeof(telePkt.location.altura) - 1);

        snprintf(telePkt.payload.text_message, sizeof(telePkt.payload.text_message),
                 "ROLE:%s|SIG:%s|MAC:%s",
                 aresRoleToString(g_ares_role), teleSig, g_ares_mac_str);

        AresRadio::getInstance().markMessageSeen(telePkt.header.msg_id);
        AresRadio::getInstance().sendBroadcast(&telePkt);
        triggerPacketActivityLed();
        Serial.printf("[%s] 📡 [TX BEACON RF] Emitindo presença na malha | Função: %s | Flash Livre: %u KB | HMAC: %s\n",
                      ARES_NODE_ID, aresRoleToString(g_ares_role), telePkt.telemetry.storage_avail_kb, teleSig);
    }
}
