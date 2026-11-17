#ifndef ARES_GATEWAY_SETUP_HTML_H
#define ARES_GATEWAY_SETUP_HTML_H

#include <Arduino.h>

const char ARES_GATEWAY_SETUP_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <title>ARES Gateway • Setup Wi-Fi</title>
  <style>
    :root {
      --bg: #090a0f;
      --card: #11141b;
      --border: rgba(255, 255, 255, 0.08);
      --text: #a1a1aa;
      --text-bright: #ffffff;
      --accent: #3b82f6;
      --success: #10b981;
      --warning: #f59e0b;
      --danger: #ef4444;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
    body { background-color: var(--bg); color: var(--text); padding: 16px; font-size: 13px; max-width: 480px; margin: 0 auto; line-height: 1.5; }
    .header { text-align: center; padding: 14px 0 10px; border-bottom: 1px solid var(--border); margin-bottom: 14px; }
    .badge { display: inline-block; padding: 3px 10px; border-radius: 9999px; font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.5px; }
    .badge-gw { background: rgba(59, 130, 246, 0.15); color: #60a5fa; border: 1px solid rgba(59, 130, 246, 0.3); }
    h1 { font-size: 1.25rem; color: var(--text-bright); margin-top: 6px; font-weight: 700; }
    .meta { font-size: 11px; color: #71717a; margin-top: 4px; font-family: ui-monospace, monospace; }
    .card { background: var(--card); border: 1px solid var(--border); border-radius: 10px; padding: 14px; margin-bottom: 12px; }
    .card-title { font-size: 12px; font-weight: 700; color: var(--text-bright); margin-bottom: 10px; text-transform: uppercase; letter-spacing: 0.5px; display: flex; justify-content: space-between; align-items: center; }
    .status-pill { font-size: 10.5px; padding: 2px 8px; border-radius: 9999px; font-weight: 600; }
    .status-connected { background: rgba(16, 185, 129, 0.15); color: var(--success); border: 1px solid rgba(16, 185, 129, 0.3); }
    .status-connecting { background: rgba(245, 158, 11, 0.15); color: var(--warning); border: 1px solid rgba(245, 158, 11, 0.3); }
    
    label { display: block; font-size: 11px; margin-bottom: 5px; color: #a1a1aa; font-weight: 600; }
    input[type="text"], input[type="password"] {
      width: 100%;
      background: #181d27;
      border: 1px solid var(--border);
      border-radius: 6px;
      padding: 9px 12px;
      color: #fff;
      font-size: 13px;
      margin-bottom: 10px;
      outline: none;
      transition: border 0.15s;
    }
    input:focus { border-color: var(--accent); }
    
    .btn {
      width: 100%;
      background: var(--accent);
      color: #fff;
      border: none;
      border-radius: 6px;
      padding: 10px;
      font-size: 13px;
      font-weight: 600;
      cursor: pointer;
      display: flex;
      justify-content: center;
      align-items: center;
      gap: 6px;
      transition: opacity 0.15s;
    }
    .btn:hover { opacity: 0.9; }
    .btn-secondary { background: #27272a; color: #d4d4d8; margin-bottom: 10px; font-size: 11.5px; padding: 7px; }

    .wifi-list { max-height: 140px; overflow-y: auto; border: 1px solid var(--border); border-radius: 6px; margin-bottom: 10px; background: #0c0e14; }
    .wifi-item { padding: 8px 10px; display: flex; justify-content: space-between; align-items: center; border-bottom: 1px solid rgba(255,255,255,0.03); cursor: pointer; }
    .wifi-item:hover { background: rgba(59, 130, 246, 0.1); color: #fff; }
    .wifi-rssi { font-size: 10px; color: #71717a; font-family: ui-monospace, monospace; }

    .help-text { font-size: 11px; color: #71717a; margin-top: 8px; line-height: 1.4; }
  </style>
</head>
<body>
  <div class="header">
    <span class="badge badge-gw">ARES Gateway Ingress</span>
    <h1>Configuração de Conectividade</h1>
    <div class="meta" id="nodeMeta">ESP32 Gateway • IP: 192.168.4.1</div>
  </div>

  <div class="card">
    <div class="card-title">
      <span>Status da Conexão</span>
      <span class="status-pill status-connecting" id="connStatus">Tentando Conectar...</span>
    </div>
    <div style="font-size: 12px; margin-bottom: 6px;"><strong>Rede Alvo:</strong> <span id="currentSsidDisplay">...</span></div>
    <div style="font-size: 12px; margin-bottom: 6px;"><strong>Canal de Rádio do ESP32:</strong> <span id="currentChanDisplay">1</span></div>
    <div style="font-size: 12px;"><strong>IP na Base:</strong> <span id="currentIpDisplay">Não atribuído</span></div>
  </div>

  <div class="card">
    <div class="card-title">
      <span>Conectar a Nova Rede Wi-Fi</span>
    </div>
    
    <button type="button" class="btn btn-secondary" onclick="scanWifi()">🔍 Escanear Redes Próximas</button>
    <div class="wifi-list" id="wifiList" style="display:none;"></div>

    <form action="/save" method="POST">
      <label for="ssid">Nome da Rede Wi-Fi (SSID)</label>
      <input type="text" id="ssid" name="ssid" required placeholder="Ex: MinhaRede_2.4G" autocomplete="off">

      <label for="pass">Senha do Wi-Fi</label>
      <input type="password" id="pass" name="pass" placeholder="Senha da rede (deixe vazio se aberta)">

      <label for="backend">URL do Backend ARES (Ingress)</label>
      <input type="text" id="backend" name="backend" required placeholder="http://192.168.1.100:5026/api/ares/ingress">

      <label for="m2m">Chave de API M2M (X-Api-Key)</label>
      <input type="text" id="m2m" name="m2m" placeholder="ares-secret-m2m-key-ufsm-2026">

      <button type="submit" class="btn">💾 Salvar e Conectar</button>
    </form>
  </div>

  <div class="help-text">
    💡 <strong>Dica Operacional:</strong> Conecte o ESP32 a uma rede <strong>2.4 GHz</strong>. As credenciais ficam salvas permanentemente na memória flash (NVS). Se a rede cair no futuro, o Gateway tentará reconectar continuamente mantendo este portal aberto para assistência.
  </div>

  <script>
    function updateStatus() {
      fetch('/status')
        .then(r => r.json())
        .then(data => {
          document.getElementById('currentSsidDisplay').textContent = data.ssid || 'Nenhum';
          document.getElementById('currentChanDisplay').textContent = data.channel || '1';
          document.getElementById('currentIpDisplay').textContent = data.ip || 'Não atribuído';
          if (!document.getElementById('ssid').value) document.getElementById('ssid').value = data.ssid || '';
          if (!document.getElementById('backend').value) document.getElementById('backend').value = data.backend || '';
          if (!document.getElementById('m2m').value) document.getElementById('m2m').value = data.m2m || '';
          
          const pill = document.getElementById('connStatus');
          if (data.connected) {
            pill.textContent = '✅ Conectado na Base';
            pill.className = 'status-pill status-connected';
          } else {
            pill.textContent = '🔄 Reconectando...';
            pill.className = 'status-pill status-connecting';
          }
        })
        .catch(() => {});
    }

    function scanWifi() {
      const list = document.getElementById('wifiList');
      list.style.display = 'block';
      list.innerHTML = '<div style="padding:10px;text-align:center;color:#60a5fa;">🔍 Varrendo canais 2.4 GHz... (aguarde ~2s)</div>';
      
      fetch('/scan')
        .then(r => r.json())
        .then(networks => {
          if (!networks.length) {
            list.innerHTML = '<div style="padding:10px;text-align:center;color:#f59e0b;">Nenhuma rede 2.4GHz listada pelo scan.<br><span style="font-size:11px;color:#a1a1aa;">Você pode digitar o nome da rede diretamente no campo abaixo.</span></div>';
            return;
          }
          list.innerHTML = '';
          networks.forEach(net => {
            const div = document.createElement('div');
            div.className = 'wifi-item';
            div.innerHTML = '<span>' + net.ssid + '</span><span class="wifi-rssi">' + net.rssi + ' dBm</span>';
            div.onclick = () => {
              document.getElementById('ssid').value = net.ssid;
              document.getElementById('pass').focus();
            };
            list.appendChild(div);
          });
        })
        .catch(() => {
          list.innerHTML = '<div style="padding:10px;text-align:center;color:#ef4444;">Erro ao processar varredura. Digite o nome da rede abaixo.</div>';
        });
    }

    updateStatus();
    setInterval(updateStatus, 3000);
  </script>
</body>
</html>
)rawliteral";

#endif // ARES_GATEWAY_SETUP_HTML_H
