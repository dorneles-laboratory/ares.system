#include "ares_gateway.h"
#include "ares_radio.h"
#include "ares_storage.h"
#include "ares_wps.h"
#include "ares_gateway_setup_html.h"
#include "esp_wifi_types.h"
#include <sys/time.h>
#include <time.h>

AresGateway* AresGateway::instance = nullptr;

static const char* getWifiDisconnectReasonString(uint8_t reason, String& humanHint) {
    switch (reason) {
        case WIFI_REASON_AUTH_FAIL: // 202
            humanHint = "SENHA INCORRETA! O ponto de acesso rejeitou a chave de autenticacao.";
            return "AUTH_FAIL (202: Senha Incorreta)";

        case 15: // WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
        case WIFI_REASON_HANDSHAKE_TIMEOUT: // 204
            humanHint = "TIMEOUT NA NEGOCIAÇÃO DE CHAVES WPA2: Quase sempre indica SENHA ERRADA ou incompatibilidade WPA2/WPA3.";
            return "HANDSHAKE_TIMEOUT (Falha na chave de segurança)";

        case WIFI_REASON_NO_AP_FOUND: // 201
            humanHint = "REDE NAO ENCONTRADA: O SSID nao foi localizado. Verifique se o Hotspot esta ligado e em 2.4 GHz.";
            return "NO_AP_FOUND (201: Rede Inexistente ou Fora de Alcance)";

        case WIFI_REASON_BEACON_TIMEOUT: // 200
            humanHint = "SINAL PERDIDO: Beacons do roteador nao foram ouvidos a tempo (sinal fraco ou interferencia).";
            return "BEACON_TIMEOUT (200: Sinal Perdido)";

        case WIFI_REASON_ASSOC_FAIL: // 203
            humanHint = "FALHA DE ASSOCIACAO: O roteador rejeitou o ESP32 (limite de clientes ou filtro MAC).";
            return "ASSOC_FAIL (203: Associacao Rejeitada)";

        case WIFI_REASON_CONNECTION_FAIL: // 205
            humanHint = "FALHA DE CONEXAO: Nao foi possivel estabelecer enlace com o ponto de acesso.";
            return "CONNECTION_FAIL (205: Falha de Enlace)";

        case WIFI_REASON_AUTH_EXPIRE: // 2
            humanHint = "AUTENTICACAO EXPIRADA: Sessao encerrada pelo roteador.";
            return "AUTH_EXPIRE (2: Sessao Expirada)";

        case WIFI_REASON_ASSOC_EXPIRE: // 4
            humanHint = "ASSOCIACAO EXPIRADA: Tempo de conexao inativa esgotado.";
            return "ASSOC_EXPIRE (4: Tempo Expirado)";

        case WIFI_REASON_ASSOC_TOOMANY: // 5
            humanHint = "LIMITE DE DISPOSITIVOS: O ponto de acesso atingiu o numero maximo de clientes.";
            return "ASSOC_TOOMANY (5: Limite de Clientes)";

        case WIFI_REASON_ASSOC_LEAVE: // 8
            humanHint = "DESASSOCIADO: O nó ou ponto de acesso solicitou desconexão.";
            return "ASSOC_LEAVE (8: Desassociado)";

        default:
            humanHint = "Codigo padrao 802.11 da pilha sem fio.";
            return "DESCONHECIDO";
    }
}

AresGateway::AresGateway()
    : server(80),
      pendingCount(0),
      lastWifiRetryTime(0),
      lastPostRetryTime(0),
      lastHeartbeatTime(0),
      apActive(false),
      isWifiConnected(false),
      begun(false),
      lastDisconnectReason(0) {
    memset(pendingQueue, 0, sizeof(pendingQueue));
    memset(remoteNodes, 0, sizeof(remoteNodes));
    memset(pendingAcks, 0, sizeof(pendingAcks));
}

AresGateway& AresGateway::getInstance() {
    if (!instance) {
        instance = new AresGateway();
    }
    return *instance;
}

void AresGateway::loadConfig() {
    prefs.begin("ares_gw", false);
    currentSsid = prefs.getString("ssid", GATEWAY_WIFI_SSID);
    currentPass = prefs.getString("pass", GATEWAY_WIFI_PASS);
    backendUrl  = prefs.getString("backend", BACKEND_INGRESS_URL);
    m2mKey      = prefs.getString("m2m", M2M_API_KEY);
    // Migra automaticamente o IP antigo inválido (192.168.1.100) para o IP do Hotspot (192.168.137.1)
    if (backendUrl.indexOf("192.168.1.100") != -1) {
        backendUrl = BACKEND_INGRESS_URL;
        prefs.putString("backend", backendUrl);
    }
    prefs.end();

    Serial.println("[ARES GATEWAY] Configurações de rede ativas:");
    Serial.printf("               SSID Alvo: '%s'\n", currentSsid.c_str());
    Serial.printf("               Backend:   '%s'\n", backendUrl.c_str());
}

void AresGateway::saveConfig(const String& ssid, const String& pass, const String& backend, const String& m2m) {
    prefs.begin("ares_gw", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.putString("backend", backend);
    prefs.putString("m2m", m2m);
    prefs.end();

    currentSsid = ssid;
    currentPass = pass;
    backendUrl  = backend;
    m2mKey      = m2m;
    lastDisconnectReason = 0;

    Serial.println("\n[ARES GATEWAY] ✅ Novas credenciais gravadas permanentemente na NVS!");
    Serial.printf("               Novo SSID: '%s'\n", currentSsid.c_str());
    Serial.printf("               Backend:   '%s'\n\n", backendUrl.c_str());
}

void AresGateway::startSetupPortal() {
    if (apActive) return;

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    WiFi.softAP("ARES-GATEWAY-SETUP", "");

    dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));
    setupWebRoutes();
    server.begin();
    apActive = true;

    Serial.println("=======================================================");
    Serial.println(" 📡 [ARES GATEWAY] PORTAL DE CONFIGURAÇÃO WI-FI ATIVO");
    Serial.println("    Conecte seu celular ou notebook na rede Wi-Fi:");
    Serial.println("    SSID: ARES-GATEWAY-SETUP (Rede Aberta)");
    Serial.println("    Acesse no navegador: http://192.168.4.1");
    Serial.println("    (Ou configure direto pelo Serial: 'wifi <SSID> <SENHA>')");
    Serial.println("=======================================================");
}

void AresGateway::setupWebRoutes() {
    server.on("/", HTTP_GET, [this]() { handleRoot(); });
    server.on("/status", HTTP_GET, [this]() { handleStatus(); });
    server.on("/scan", HTTP_GET, [this]() { handleScan(); });
    server.on("/save", HTTP_POST, [this]() { handleSave(); });

    // Rotas de Captive Portal (iOS, Android, Windows)
    server.on("/generate_204", HTTP_GET, [this]() { handleRoot(); });
    server.on("/canonical.html", HTTP_GET, [this]() { handleRoot(); });
    server.on("/hotspot-detect.html", HTTP_GET, [this]() { handleRoot(); });
    server.on("/connecttest.txt", HTTP_GET, [this]() { handleRoot(); });
    server.on("/redirect", HTTP_GET, [this]() { handleRoot(); });
    server.onNotFound([this]() { handleNotFound(); });
}

void AresGateway::handleRoot() {
    server.send_P(200, "text/html", ARES_GATEWAY_SETUP_HTML);
}

void AresGateway::handleStatus() {
    JsonDocument doc;
    bool conn = (WiFi.status() == WL_CONNECTED);
    doc["connected"] = conn;
    doc["ssid"] = currentSsid;
    doc["ip"] = conn ? WiFi.localIP().toString() : "";
    doc["channel"] = (int)WiFi.channel();
    doc["backend"] = backendUrl;
    doc["m2m"] = m2mKey;

    if (!conn && lastDisconnectReason > 0) {
        String hint;
        doc["last_error"] = getWifiDisconnectReasonString(lastDisconnectReason, hint);
        doc["error_hint"] = hint;
    }

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void AresGateway::handleScan() {
    Serial.println("[ARES GATEWAY] 🔍 Solicitada varredura de redes Wi-Fi 2.4 GHz...");

    int16_t n = WiFi.scanNetworks(false, false, false, 120);
    Serial.printf("[ARES GATEWAY] Varredura finalizada. Redes encontradas: %d\n", n);

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    if (n > 0) {
        for (int i = 0; i < n; ++i) {
            String s = WiFi.SSID(i);
            if (s.length() > 0) {
                bool exists = false;
                for (JsonVariant v : arr) {
                    if (v["ssid"].as<String>() == s) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    JsonObject obj = arr.add<JsonObject>();
                    obj["ssid"] = s;
                    obj["rssi"] = WiFi.RSSI(i);
                }
            }
        }
    }
    WiFi.scanDelete();

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void AresGateway::handleSave() {
    if (!server.hasArg("ssid")) {
        server.send(400, "text/plain", "SSID obrigatorio");
        return;
    }

    String newSsid = server.arg("ssid");
    String newPass = server.hasArg("pass") ? server.arg("pass") : "";
    String newBackend = server.hasArg("backend") ? server.arg("backend") : BACKEND_INGRESS_URL;
    String newM2m = server.hasArg("m2m") ? server.arg("m2m") : M2M_API_KEY;

    saveConfig(newSsid, newPass, newBackend, newM2m);

    String feedback = R"rawliteral(
    <!DOCTYPE html>
    <html lang="pt-BR">
    <head>
      <meta charset="UTF-8">
      <meta name="viewport" content="width=device-width, initial-scale=1.0">
      <meta http-equiv="refresh" content="5; url=/">
      <title>Configurações Salvas</title>
      <style>
        body { background: #090a0f; color: #fff; font-family: sans-serif; text-align: center; padding: 40px 20px; }
        .box { background: #11141b; border: 1px solid rgba(255,255,255,0.1); border-radius: 12px; padding: 24px; max-width: 400px; margin: 0 auto; }
        h2 { color: #10b981; margin-bottom: 12px; }
        p { color: #a1a1aa; font-size: 14px; margin-bottom: 16px; line-height: 1.5; }
        a { color: #3b82f6; text-decoration: none; font-weight: bold; }
      </style>
    </head>
    <body>
      <div class="box">
        <h2>✅ Configurações Salvas!</h2>
        <p>Conectando à rede <strong>)rawliteral" + newSsid + R"rawliteral(</strong>...</p>
        <p>Acompanhe os logs detalhados pelo Monitor Serial.</p>
        <p><a href="/">Atualizar Painel</a></p>
      </div>
    </body>
    </html>
    )rawliteral";

    server.send(200, "text/html", feedback);

    // Inicia tentativa de conexão com novos dados
    isWifiConnected = false;
    Serial.printf("\n[ARES GATEWAY] 🔄 Disparando conexão com nova rede: '%s' ...\n", currentSsid.c_str());
    WiFi.disconnect();
    WiFi.begin(currentSsid.c_str(), currentPass.c_str());
    lastWifiRetryTime = millis();
}

void AresGateway::handleNotFound() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
}

void AresGateway::onWifiConnected() {
    isWifiConnected = true;
    lastDisconnectReason = 0;

    Serial.println("\n=======================================================");
    Serial.println(" [ARES GATEWAY] ✅ CONEXÃO WI-FI ESTABELECIDA COM SUCESSO!");
    Serial.printf("                Rede Conectada:  '%s'\n", currentSsid.c_str());
    Serial.printf("                Endereço IP:     %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("                Gateway Padrão:  %s\n", WiFi.gatewayIP().toString().c_str());
    Serial.printf("                Máscara de Rede: %s\n", WiFi.subnetMask().toString().c_str());
    Serial.printf("                Canal de Rádio:  %d\n", (int)WiFi.channel());
    Serial.printf("                Intensidade RSSI:%d dBm\n", (int)WiFi.RSSI());
    Serial.println("=======================================================\n");

    // Desativa SoftAP para economizar RF e evitar colisões
    if (apActive) {
        WiFi.softAPdisconnect(true);
        apActive = false;
        Serial.println("[ARES GATEWAY] Portal de emergência 'ARES-GATEWAY-SETUP' desativado (Wi-Fi online).");
    }

    // Tenta obter geolocalização somente após conexão com a internet
    tryResolveLocation();

    // Força envio imediato do Heartbeat no loop principal para registrar o Gateway como ONLINE no Dashboard
    lastHeartbeatTime = 0;
}

void AresGateway::onWifiDisconnected(uint8_t reason) {
    isWifiConnected = false;
    lastDisconnectReason = reason;

    String hint;
    const char* reasonStr = getWifiDisconnectReasonString(reason, hint);

    Serial.println("\n-------------------------------------------------------");
    Serial.printf(" [WIFI EVENT] ⚠️ DESCONEXÃO DETECTADA NA REDE '%s'\n", currentSsid.c_str());
    Serial.printf("              Código de Erro: %d (%s)\n", reason, reasonStr);
    Serial.printf("              Diagnóstico:    %s\n", hint.c_str());
    if (reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
        Serial.println("              👉 DICA: A senha pode estar incorreta. Digite no serial:");
        Serial.printf("                       wifi %s <SENHA_CORRETA>\n", currentSsid.c_str());
    } else if (reason == WIFI_REASON_NO_AP_FOUND) {
        Serial.println("              👉 DICA: Certifique-se de que o Hotspot está ligado e em 2.4 GHz.");
    }
    Serial.println("-------------------------------------------------------\n");
}

void AresGateway::tryResolveLocation() {
    // Se a localização já estiver preenchida (ex: pela NVS), não sobrescreve
    if (strcmp(g_ares_lat, "INDEFINIDO") != 0 && g_ares_lat[0] != '\0') {
        Serial.printf("[AresLocationManager] Localização ativa da NVS: Lat %s, Lon %s\n", g_ares_lat, g_ares_lon);
        return;
    }

    if (strlen(GOOGLE_GEOLOCATION_API_KEY) > 0 || strlen(UNWIRED_API_KEY) > 0) {
        Serial.println("[AresLocationManager] Conectado à internet. Consultando serviço de geolocalização WPS...");
        WpsProvider provider = (strlen(GOOGLE_GEOLOCATION_API_KEY) > 0) ? WPS_PROVIDER_GOOGLE : WPS_PROVIDER_UNWIRED;
        const char* key = (provider == WPS_PROVIDER_GOOGLE) ? GOOGLE_GEOLOCATION_API_KEY : UNWIRED_API_KEY;
        AresLocationManager::getInstance().performWpsUpdate(nullptr, nullptr, provider, key);
    } else {
        Serial.println("[AresLocationManager] Nenhuma chave de API de geolocalização configurada. Posição permanece INDEFINIDO.");
    }
}

bool AresGateway::begin() {
    loadConfig();

    // 1. Registro detalhado de eventos nativos do driver Wi-Fi
    WiFi.onEvent([this](WiFiEvent_t event, WiFiEventInfo_t info) {
        switch (event) {
            case ARDUINO_EVENT_WIFI_STA_START:
                Serial.println("[WIFI EVENT] Rádio Station (STA) inicializado.");
                break;
            case ARDUINO_EVENT_WIFI_STA_CONNECTED:
                Serial.printf("[WIFI EVENT] 🔗 Enlace de rádio estabelecido com '%s'! Aguardando IP (DHCP)...\n", currentSsid.c_str());
                break;
            case ARDUINO_EVENT_WIFI_STA_GOT_IP:
                this->onWifiConnected();
                break;
            case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
                this->onWifiDisconnected(info.wifi_sta_disconnected.reason);
                break;
            default:
                break;
        }
    });

    // 2. Inicia em modo STA limpo primeiro (sem SoftAP competindo pelo canal)
    WiFi.mode(WIFI_STA);

    Serial.println("\n-------------------------------------------------------");
    Serial.printf("[ARES GATEWAY] 📡 Conectando à rede Wi-Fi da Base: '%s' ...\n", currentSsid.c_str());
    Serial.println("-------------------------------------------------------");
    WiFi.begin(currentSsid.c_str(), currentPass.c_str());

    // 3. Aguarda até 14 segundos para permitir handshake e atribuição de IP via DHCP
    unsigned long startTry = millis();
    int dots = 0;
    while (WiFi.status() != WL_CONNECTED && (millis() - startTry < 14000)) {
        delay(500);
        Serial.print(".");
        dots++;
        if (dots % 20 == 0) Serial.println();
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        if (!isWifiConnected) {
            onWifiConnected();
        }
        begun = true;
        return true;
    } else {
        Serial.printf("[ARES GATEWAY] ⚠️ Tempo limite esgotado sem IP para a rede '%s'.\n", currentSsid.c_str());
        if (lastDisconnectReason > 0) {
            String hint;
            const char* reasonStr = getWifiDisconnectReasonString(lastDisconnectReason, hint);
            Serial.printf("[ARES GATEWAY] 🛑 Causa da falha: %s\n", reasonStr);
            Serial.printf("[ARES GATEWAY]    Detalhes: %s\n", hint.c_str());
        }
        // Sobe o portal de contingência
        startSetupPortal();
        lastWifiRetryTime = millis();
        begun = true;
        return false;
    }
}

void AresGateway::stop() {
    if (apActive) {
        dnsServer.stop();
        server.stop();
        WiFi.softAPdisconnect(true);
        apActive = false;
    }
    WiFi.disconnect(false);
    isWifiConnected = false;
    begun = false;
}

void AresGateway::checkWifiConnection() {
    // SE CONECTADO: Não tenta reconectar e atualiza flag
    if (WiFi.status() == WL_CONNECTED) {
        if (!isWifiConnected) {
            onWifiConnected();
        }
        return;
    }

    // Se desconectado
    isWifiConnected = false;
    unsigned long now = millis();
    if (now - lastWifiRetryTime >= 20000) {
        lastWifiRetryTime = now;
        Serial.println("\n[ARES GATEWAY] 🔄 Iniciando tentativa de reconexão Wi-Fi...");
        Serial.printf("               Rede Alvo: '%s'\n", currentSsid.c_str());
        if (lastDisconnectReason > 0) {
            String hint;
            const char* reasonStr = getWifiDisconnectReasonString(lastDisconnectReason, hint);
            Serial.printf("               Último erro registrado: %s (%s)\n", reasonStr, hint.c_str());
        }
        WiFi.begin(currentSsid.c_str(), currentPass.c_str());
    }
}

void AresGateway::checkSerialCommands() {
    if (!Serial.available()) return;

    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() == 0) return;

    if (cmd.startsWith("wifi ")) {
        int firstSpace = cmd.indexOf(' ');
        int secondSpace = cmd.indexOf(' ', firstSpace + 1);

        String newSsid, newPass;
        if (secondSpace != -1) {
            newSsid = cmd.substring(firstSpace + 1, secondSpace);
            newPass = cmd.substring(secondSpace + 1);
        } else {
            newSsid = cmd.substring(firstSpace + 1);
            newPass = "";
        }
        newSsid.trim();
        newPass.trim();

        Serial.printf("\n[SERIAL CMD] ⚙️ Reconfigurando Wi-Fi: SSID='%s', Senha='%s'\n",
                      newSsid.c_str(), (newPass.length() > 0 ? "******" : "[Sem Senha]"));
        saveConfig(newSsid, newPass, backendUrl, m2mKey);
        isWifiConnected = false;
        WiFi.disconnect();
        WiFi.begin(currentSsid.c_str(), currentPass.c_str());
        lastWifiRetryTime = millis();
    } else if (cmd.startsWith("backend ")) {
        String newUrl = cmd.substring(8);
        newUrl.trim();
        Serial.printf("\n[SERIAL CMD] ⚙️ Atualizando Backend Ingress para '%s'...\n", newUrl.c_str());
        saveConfig(currentSsid, currentPass, newUrl, m2mKey);
    } else if (cmd == "scan") {
        Serial.println("\n[SERIAL SCAN] 🔍 Varrendo redes Wi-Fi 2.4 GHz disponíveis...");
        WiFi.disconnect(false);
        delay(150);
        int n = WiFi.scanNetworks(false, false, false, 150);
        if (n <= 0) {
            Serial.println("[SERIAL SCAN] ❌ Nenhuma rede 2.4 GHz encontrada ao redor!");
        } else {
            Serial.printf("[SERIAL SCAN] ✅ %d redes encontradas ao alcance:\n", n);
            for (int i = 0; i < n; i++) {
                Serial.printf("  [%2d] SSID: '%-25s' | Canal: %-2d | RSSI: %d dBm | %s\n",
                              i + 1,
                              WiFi.SSID(i).c_str(),
                              (int)WiFi.channel(i),
                              (int)WiFi.RSSI(i),
                              (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "Aberta" : "Protegida"));
            }
        }
        WiFi.scanDelete();
        Serial.println();
    } else if (cmd.startsWith("role ")) {
        String rStr = cmd.substring(5);
        rStr.trim();
        uint8_t newRole = aresStringToRole(rStr.c_str());
        Serial.printf("\n[SERIAL CMD] ⚙️ Alterando papel operacional localmente para %s (%d)...\n",
                      aresRoleToString(newRole), newRole);
        switchNodeRole(newRole, true);
    } else if (cmd.startsWith("bat ")) {
        float newBat = cmd.substring(4).toFloat();
        if (newBat >= 0.0f && newBat <= 100.0f) {
            g_ares_battery = newBat;
            Serial.printf("\n[SERIAL CMD] 🔋 Bateria atualizada para %.1f%%\n", g_ares_battery);
        }
    } else if (cmd == "hb") {
        Serial.println("\n[SERIAL CMD] 📡 Disparando Heartbeat HTTP imediato para a Base...");
        sendHeartbeat();
    } else if (cmd == "s") {
        time_t nowSec = time(nullptr);
        Serial.println("\n-------------------------------------------------------");
        Serial.println(" [STATUS ATUAL DO NÓ UNIFICADO ARES]");
        Serial.printf("  ID de Hardware:     %s (MAC: %s)\n", ARES_NODE_ID, g_ares_mac_str);
        Serial.printf("  Assinatura HMAC:    %s\n", g_ares_node_sig);
        Serial.printf("  Papel Atual:        %s (%d)\n", aresRoleToString(g_ares_role), g_ares_role);
        if (g_ares_battery < 0.0f) {
            Serial.println("  Bateria:            NÃO CONECTADA (Alimentação Externa / USB)");
        } else {
            Serial.printf("  Bateria:            %.1f%%\n", g_ares_battery);
        }
        Serial.printf("  Flash Livre (WAL):  %u KB\n", AresStorage::getInstance().getAvailableSpaceKb());
        Serial.printf("  Hora Sincronizada:  %lu (UNIX)\n", (unsigned long)nowSec);
        Serial.printf("  Conectado ao Wi-Fi: %s\n", (WiFi.status() == WL_CONNECTED ? "✅ SIM" : "❌ NÃO"));
        Serial.printf("  SSID Configurado:   '%s'\n", currentSsid.c_str());
        Serial.printf("  IP na Base:         %s\n", (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "Não atribuído"));
        Serial.printf("  Backend URL:        %s\n", backendUrl.c_str());
        Serial.printf("  Canal de Rádio:     %d\n", (int)WiFi.channel());
        Serial.printf("  Pacotes na Fila:    %d\n", pendingCount);
        if (lastDisconnectReason > 0) {
            String hint;
            const char* reasonStr = getWifiDisconnectReasonString(lastDisconnectReason, hint);
            Serial.printf("  Último Erro Wi-Fi:  %s\n", reasonStr);
        }
        Serial.println("-------------------------------------------------------\n");
    }
}

void AresGateway::recordMeshTelemetry(const AresMessagePacket *packet, int rssi) {
    if (!packet || strlen(packet->header.source_id) == 0) return;
    if (strcmp(packet->header.source_id, ARES_NODE_ID) == 0) return;

    int slot = -1;
    for (int i = 0; i < 12; i++) {
        if (strncmp(remoteNodes[i].node_id, packet->header.source_id, sizeof(remoteNodes[i].node_id)) == 0) {
            slot = i;
            break;
        }
        if (slot == -1 && remoteNodes[i].node_id[0] == '\0') {
            slot = i;
        }
    }
    if (slot == -1) slot = 0;

    strncpy(remoteNodes[slot].node_id, packet->header.source_id, sizeof(remoteNodes[slot].node_id) - 1);
    remoteNodes[slot].battery_level = packet->telemetry.battery_level;
    remoteNodes[slot].storage_avail_kb = packet->telemetry.storage_avail_kb;
    remoteNodes[slot].rssi = rssi;
    remoteNodes[slot].hops = packet->header.hop_count > 0 ? packet->header.hop_count : 1;
    strncpy(remoteNodes[slot].lat, packet->location.lat, sizeof(remoteNodes[slot].lat) - 1);
    strncpy(remoteNodes[slot].lon, packet->location.lon, sizeof(remoteNodes[slot].lon) - 1);

    // Extrai ROLE:<role>|SIG:<sig>|MAC:<mac> do payload de controle se presente
    strncpy(remoteNodes[slot].role, "RELAY", sizeof(remoteNodes[slot].role) - 1);
    strncpy(remoteNodes[slot].mac_address, packet->header.source_id, sizeof(remoteNodes[slot].mac_address) - 1);
    remoteNodes[slot].signature[0] = '\0';

    const char* txt = packet->payload.text_message;
    const char* rolePos = strstr(txt, "ROLE:");
    if (rolePos) {
        rolePos += 5;
        int k = 0;
        while (rolePos[k] && rolePos[k] != '|' && k < 11) {
            remoteNodes[slot].role[k] = rolePos[k];
            k++;
        }
        remoteNodes[slot].role[k] = '\0';
    }
    const char* sigPos = strstr(txt, "SIG:");
    if (sigPos) {
        sigPos += 4;
        int k = 0;
        while (sigPos[k] && sigPos[k] != '|' && k < 16) {
            remoteNodes[slot].signature[k] = sigPos[k];
            k++;
        }
        remoteNodes[slot].signature[k] = '\0';
    }
    const char* macPos = strstr(txt, "MAC:");
    if (macPos) {
        macPos += 4;
        int k = 0;
        while (macPos[k] && macPos[k] != '|' && k < 17) {
            remoteNodes[slot].mac_address[k] = macPos[k];
            k++;
        }
        remoteNodes[slot].mac_address[k] = '\0';
    }

    remoteNodes[slot].dirty = true;
}

void AresGateway::recordCommandAck(const char* cmdId, const char* nodeId, const char* newRole, const char* sig) {
    for (int i = 0; i < 6; i++) {
        if (!pendingAcks[i].pending) {
            strncpy(pendingAcks[i].cmd_id, cmdId ? cmdId : "", sizeof(pendingAcks[i].cmd_id) - 1);
            strncpy(pendingAcks[i].node_id, nodeId ? nodeId : "", sizeof(pendingAcks[i].node_id) - 1);
            strncpy(pendingAcks[i].new_role, newRole ? newRole : "RELAY", sizeof(pendingAcks[i].new_role) - 1);
            strncpy(pendingAcks[i].signature, sig ? sig : "", sizeof(pendingAcks[i].signature) - 1);
            pendingAcks[i].pending = true;
            lastHeartbeatTime = 0; // Dispara heartbeat rápido para confirmar no Dashboard
            break;
        }
    }
}

void AresGateway::queuePacketForIngress(const AresMessagePacket *packet) {
    if (!packet) return;
    if (pendingCount < 10) {
        memcpy(&pendingQueue[pendingCount], packet, sizeof(AresMessagePacket));
        pendingCount++;
        Serial.printf("[ARES GATEWAY] Pacote %s adicionado à fila de Ingress (Total na fila: %d)\n",
                      packet->header.msg_id, pendingCount);
    } else {
        Serial.printf("[ARES GATEWAY] ⚠️ Fila cheia! Descartando pacote menos prioritário para receber %s\n",
                      packet->header.msg_id);
    }
}

void AresGateway::processDownstreamAndControlResponse(JsonDocument& resDoc) {
    // 1. Sincroniza o relógio interno UNIX do ESP32 (dispensa módulo RTC físico)
    uint32_t srvTs = resDoc["server_timestamp"] | 0;
    if (srvTs > 1700000000UL) {
        struct timeval tv = { .tv_sec = (time_t)srvTs, .tv_usec = 0 };
        settimeofday(&tv, nullptr);
    }

    // 2. Processa comandos assinados do Plano de Controle (ex: SET_ROLE para o Gateway ou nós sem internet)
    if (resDoc["pendingCommands"].is<JsonArray>()) {
        JsonArray cmds = resDoc["pendingCommands"].as<JsonArray>();
        for (JsonObject cmd : cmds) {
            const char* cmdId = cmd["cmd_id"] | "CMD-0";
            const char* targetId = cmd["target_node_id"] | "";
            const char* roleName = cmd["role"] | "RELAY";
            int roleCode = cmd["role_code"] | 3;
            const char* sig = cmd["signature"] | "";
            uint32_t ts = cmd["timestamp"] | srvTs;

            if (strlen(targetId) == 0) continue;

            if (strcmp(targetId, ARES_NODE_ID) == 0) {
                char expectedSig[17] = {0};
                char payloadToVerify[32] = {0};
                snprintf(payloadToVerify, sizeof(payloadToVerify), "SET_ROLE:%d", roleCode);
                computeNodeHmacHex(ARES_NODE_ID, payloadToVerify, expectedSig);

                if (strcasecmp(sig, expectedSig) == 0) {
                    Serial.printf("[CONTROL PLANE] 🔐 Comando autenticado para este nó (%s): trocando função para %s!\n",
                                  ARES_NODE_ID, roleName);
                    switchNodeRole((uint8_t)roleCode, true);
                    recordCommandAck(cmdId, ARES_NODE_ID, roleName, expectedSig);
                } else {
                    Serial.printf("[CONTROL PLANE] ❌ Assinatura HMAC inválida no comando %s (esperado %s, recebido %s)!\n",
                                  cmdId, expectedSig, sig);
                }
            } else {
                // Propaga o comando por radiofrequência (ESP-NOW) para alcançar o nó X sem internet!
                AresMessagePacket ctrlPkt = {};
                snprintf(ctrlPkt.header.msg_id, sizeof(ctrlPkt.header.msg_id), "%s", cmdId);
                strncpy(ctrlPkt.header.source_id, ARES_NODE_ID, sizeof(ctrlPkt.header.source_id) - 1);
                strncpy(ctrlPkt.header.destination_id, targetId, sizeof(ctrlPkt.header.destination_id) - 1);
                ctrlPkt.header.priority = ARES_P0_EMERGENCY;
                ctrlPkt.header.direction = ARES_DIR_DOWNSTREAM;
                ctrlPkt.header.msg_type = ARES_MSG_CONTROL_CMD;
                ctrlPkt.header.flags = ARES_FLAG_AUTHENTICATED;
                ctrlPkt.header.ttl = 12;
                ctrlPkt.header.hop_count = 1;
                ctrlPkt.header.timestamp = ts;

                snprintf(ctrlPkt.payload.text_message, sizeof(ctrlPkt.payload.text_message),
                         "CMD:%s|ROLE:%d|SIG:%s|TS:%lu",
                         cmdId, roleCode, sig, (unsigned long)ts);

                AresRadio::getInstance().sendBroadcast(&ctrlPkt);
                Serial.printf("[CONTROL PLANE] 📡 Propagando ordem RF (SET_ROLE=%s) pela malha para o nó %s [CMD %s]\n",
                              roleName, targetId, cmdId);
            }
        }
    }

    // 3. Processa mensagens de texto downstream da Base
    if (resDoc["downstreamMessages"].is<JsonArray>()) {
        JsonArray downMsgs = resDoc["downstreamMessages"].as<JsonArray>();
        for (JsonObject down : downMsgs) {
            const char *dest = down["destinationNodeId"] | "BROADCAST";
            const char *msgText = down["textMessage"] | "";
            int prio = down["priority"] | 0;

            AresMessagePacket downPacket = {};
            snprintf(downPacket.header.msg_id, sizeof(downPacket.header.msg_id), "DOWN-%lu", millis());
            strncpy(downPacket.header.source_id, ARES_NODE_ID, sizeof(downPacket.header.source_id) - 1);
            strncpy(downPacket.header.destination_id, dest, sizeof(downPacket.header.destination_id) - 1);
            downPacket.header.priority = prio;
            downPacket.header.direction = ARES_DIR_DOWNSTREAM;
            downPacket.header.msg_type = ARES_MSG_DATA;
            downPacket.header.ttl = 64;
            downPacket.header.hop_count = 1;
            strncpy(downPacket.payload.text_message, msgText, sizeof(downPacket.payload.text_message) - 1);

            AresRadio::getInstance().sendBroadcast(&downPacket);
            Serial.printf("[ARES GATEWAY] Mensagem downstream despachada para malha: %s -> %s\n", dest, msgText);
        }
    }
}

bool AresGateway::sendHeartbeat() {
    if (WiFi.status() != WL_CONNECTED) return false;

    String hbUrl = backendUrl;
    int idx = hbUrl.lastIndexOf("/ingress");
    if (idx != -1) {
        hbUrl = hbUrl.substring(0, idx) + "/heartbeat";
    } else if (!hbUrl.endsWith("/heartbeat")) {
        hbUrl += "/heartbeat";
    }

    HTTPClient http;
    http.begin(hbUrl.c_str());
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Api-Key", m2mKey.c_str());
    http.setTimeout(3500);

    char hbSig[17] = {0};
    computeNodeHmacHex(ARES_NODE_ID, "HB", hbSig);

    JsonDocument doc;
    doc["gateway_id"] = ARES_NODE_ID;
    doc["mac_address"] = g_ares_mac_str;
    doc["role"] = aresRoleToString(g_ares_role);
    doc["battery_level"] = g_ares_battery;
    doc["storage_avail_kb"] = AresStorage::getInstance().getAvailableSpaceKb();
    doc["wifi_rssi"] = (int)WiFi.RSSI();
    doc["wifi_channel"] = (int)WiFi.channel();
    doc["ip_address"] = WiFi.localIP().toString();
    doc["uptime_s"] = (uint32_t)(millis() / 1000);
    doc["lat"] = ARES_NODE_LAT;
    doc["lon"] = ARES_NODE_LON;
    doc["altura"] = ARES_NODE_ALTURA;
    doc["signature"] = hbSig;

    JsonArray meshArr = doc["mesh_nodes"].to<JsonArray>();
    for (int i = 0; i < 12; i++) {
        if (remoteNodes[i].dirty && remoteNodes[i].node_id[0] != '\0') {
            JsonObject mn = meshArr.add<JsonObject>();
            mn["node_id"] = remoteNodes[i].node_id;
            mn["mac_address"] = remoteNodes[i].mac_address;
            mn["role"] = remoteNodes[i].role;
            mn["battery_level"] = remoteNodes[i].battery_level;
            mn["storage_avail_kb"] = remoteNodes[i].storage_avail_kb;
            mn["rssi"] = remoteNodes[i].rssi;
            mn["hops"] = remoteNodes[i].hops;
            mn["lat"] = remoteNodes[i].lat;
            mn["lon"] = remoteNodes[i].lon;
            mn["signature"] = remoteNodes[i].signature;
            JsonArray trail = mn["hops_trail"].to<JsonArray>();
            trail.add(remoteNodes[i].node_id);
            trail.add(ARES_NODE_ID);
            remoteNodes[i].dirty = false;
        }
    }

    JsonArray acksArr = doc["command_acks"].to<JsonArray>();
    for (int i = 0; i < 6; i++) {
        if (pendingAcks[i].pending) {
            JsonObject ack = acksArr.add<JsonObject>();
            ack["cmd_id"] = pendingAcks[i].cmd_id;
            ack["node_id"] = pendingAcks[i].node_id;
            ack["new_role"] = pendingAcks[i].new_role;
            ack["status"] = "APPLIED";
            ack["signature"] = pendingAcks[i].signature;
            pendingAcks[i].pending = false;
        }
    }

    String body;
    serializeJson(doc, body);

    int httpCode = http.POST(body);
    if (httpCode >= 200 && httpCode < 300) {
        String res = http.getString();
        JsonDocument resDoc;
        if (deserializeJson(resDoc, res) == DeserializationError::Ok) {
            processDownstreamAndControlResponse(resDoc);
        }
        Serial.printf("[ARES GATEWAY] 💓 Heartbeat OK (%d) -> %s | Nó %s ONLINE no Dashboard!\n",
                      httpCode, hbUrl.c_str(), ARES_NODE_ID);
        http.end();
        return true;
    } else {
        Serial.printf("[ARES GATEWAY] ⚠️ Falha no Heartbeat HTTP (%d) para %s\n", httpCode, hbUrl.c_str());
        http.end();
        return false;
    }
}

void AresGateway::processIngressQueue() {
    // 1. Atende requisições do Portal de Setup (se ativo)
    if (apActive) {
        dnsServer.processNextRequest();
        server.handleClient();
    }

    // 2. Monitora estado do Wi-Fi com logs diagnósticos
    checkWifiConnection();

    // Se não estiver conectado ao Wi-Fi, retém pacotes e aguarda conexão
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    unsigned long now = millis();

    // 3. Dispara Heartbeat periódico (a cada 10s ou imediatamente após conectar)
    if (lastHeartbeatTime == 0 || (now - lastHeartbeatTime >= GATEWAY_HEARTBEAT_MS)) {
        lastHeartbeatTime = now;
        sendHeartbeat();
    }

    if (pendingCount <= 0) return;

    // Intervalo de segurança para retentativa de pacotes de dados
    if (now - lastPostRetryTime < 2000) {
        return;
    }

    // Processa o primeiro da fila
    AresMessagePacket *pkt = &pendingQueue[0];
    bool success = sendHttpPost(pkt);

    if (success) {
        for (int i = 0; i < pendingCount - 1; i++) {
            pendingQueue[i] = pendingQueue[i + 1];
        }
        pendingCount--;
        Serial.printf("[ARES GATEWAY] Pacote despachado e removido da fila. Restantes: %d\n", pendingCount);
    } else {
        lastPostRetryTime = now;
        Serial.printf("[ARES GATEWAY] ⚠️ Falha ao despachar pacote %s ao backend. Retendo na fila para retentativa.\n",
                      pkt->header.msg_id);
    }
}

bool AresGateway::sendHttpPost(const AresMessagePacket *packet) {
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    http.begin(backendUrl.c_str());
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Api-Key", m2mKey.c_str());
    http.setTimeout(4000);

    char pktSig[17] = {0};
    computeNodeHmacHex(packet->header.source_id, packet->header.msg_id, pktSig);

    JsonDocument doc;
    doc["msg_id"] = packet->header.msg_id;
    doc["source_id"] = packet->header.source_id;
    doc["destination_id"] = packet->header.destination_id;
    doc["priority"] = packet->header.priority;
    doc["ttl"] = packet->header.ttl;
    doc["hop_count"] = packet->header.hop_count;
    doc["battery_level"] = packet->telemetry.battery_level;
    doc["storage_avail_kb"] = packet->telemetry.storage_avail_kb;
    doc["rssi"] = packet->telemetry.last_rssi;
    doc["lat"] = packet->location.lat;
    doc["lon"] = packet->location.lon;
    doc["altura"] = packet->location.altura;
    doc["text_message"] = packet->payload.text_message;
    doc["timestamp"] = (double)packet->header.timestamp;
    doc["gateway_id"] = ARES_NODE_ID;
    doc["signature"] = pktSig;

    // Hops trail real (sem criar nós fantasmas HOP_x)
    JsonArray hopsArr = doc["hops_trail"].to<JsonArray>();
    hopsArr.add(packet->header.source_id);
    if (strcmp(packet->header.source_id, ARES_NODE_ID) != 0) {
        hopsArr.add(ARES_NODE_ID);
    }

    String body;
    serializeJson(doc, body);

    int httpCode = http.POST(body);
    if (httpCode >= 200 && httpCode < 300) {
        String res = http.getString();
        Serial.printf("[ARES GATEWAY] HTTP POST Sucesso (%d): %s\n", httpCode, res.c_str());

        JsonDocument resDoc;
        if (deserializeJson(resDoc, res) == DeserializationError::Ok) {
            processDownstreamAndControlResponse(resDoc);
        }
        http.end();
        return true;
    } else {
        Serial.printf("[ARES GATEWAY] Erro no HTTP POST: %d\n", httpCode);
        http.end();
        return false;
    }
}
