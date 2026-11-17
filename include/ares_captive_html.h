#ifndef ARES_CAPTIVE_HTML_H
#define ARES_CAPTIVE_HTML_H

#include <Arduino.h>

const char ARES_CAPTIVE_INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <title>ARES • Rede de Resiliência e Emergência</title>
  <style>
    :root {
      --bg: #090a0f;
      --card: #11141b;
      --border: rgba(255, 255, 255, 0.08);
      --text: #a1a1aa;
      --text-bright: #ffffff;
      --p0: #ef4444;
      --p1: #f97316;
      --p2: #eab308;
      --p3: #3b82f6;
      --p4: #10b981;
      --accent: #3b82f6;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
    body { background-color: var(--bg); color: var(--text); padding: 14px; font-size: 13px; max-width: 480px; margin: 0 auto; line-height: 1.4; }
    .header { text-align: center; padding: 12px 0 8px; border-bottom: 1px solid var(--border); margin-bottom: 10px; }
    .badge { display: inline-block; padding: 2px 8px; border-radius: 9999px; font-size: 10.5px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.3px; }
    .badge-dtn { background: rgba(255, 255, 255, 0.05); color: #a1a1aa; border: 1px solid var(--border); }
    h1 { font-size: 1.15rem; color: var(--text-bright); margin-top: 5px; font-weight: 700; }
    .node-meta { font-size: 10.5px; color: #71717a; margin-top: 4px; display: flex; justify-content: center; gap: 8px; font-family: ui-monospace, monospace; }
    .card { background: var(--card); border: 1px solid var(--border); border-radius: 8px; padding: 12px; margin-bottom: 10px; }
    .card-title { font-size: 12px; font-weight: 600; color: var(--text-bright); margin-bottom: 8px; display: flex; align-items: center; justify-content: space-between; }
    label { display: block; font-size: 10.5px; margin-bottom: 4px; color: #71717a; font-weight: 600; text-transform: uppercase; letter-spacing: 0.3px; }
    
    /* Ações Rápidas de 1 Clique */
    .quick-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 5px; margin-bottom: 10px; }
    .quick-btn { background: rgba(255, 255, 255, 0.02); border: 1px solid var(--border); color: #d4d4d8; padding: 7px 8px; border-radius: 6px; font-size: 11px; font-weight: 500; cursor: pointer; text-align: left; display: flex; align-items: center; gap: 6px; transition: background 0.15s; }
    .quick-btn:hover { background: rgba(255, 255, 255, 0.06); color: #fff; }
    .quick-btn.p0 { border-color: rgba(239, 68, 68, 0.3); color: #f87171; background: rgba(239, 68, 68, 0.05); }
    .quick-btn.p1 { border-color: rgba(249, 115, 22, 0.3); color: #fb923c; background: rgba(249, 115, 22, 0.05); }
    
    .priority-grid { display: grid; grid-template-columns: repeat(5, 1fr); gap: 4px; margin-bottom: 8px; }
    .p-btn { border: 1px solid var(--border); background: #181d27; color: #71717a; padding: 6px 2px; border-radius: 5px; font-size: 11px; font-weight: 600; cursor: pointer; text-align: center; }
    .p-btn.active { border-color: var(--active-color); background: var(--active-color); color: #fff; }
    .p-btn[data-p="0"] { --active-color: var(--p0); }
    .p-btn[data-p="1"] { --active-color: var(--p1); }
    .p-btn[data-p="2"] { --active-color: var(--p2); }
    .p-btn[data-p="3"] { --active-color: var(--p3); }
    .p-btn[data-p="4"] { --active-color: var(--p4); }
    input, textarea { width: 100%; background: #090a0f; border: 1px solid var(--border); border-radius: 5px; padding: 8px 10px; color: var(--text-bright); font-size: 13px; margin-bottom: 8px; outline: none; }
    input:focus, textarea:focus { border-color: #3b82f6; }
    textarea { resize: none; height: 60px; }
    .char-count { font-size: 10.5px; color: #71717a; text-align: right; margin-top: -6px; margin-bottom: 8px; font-family: ui-monospace, monospace; }
    .submit-btn { width: 100%; background: #3b82f6; color: #fff; border: none; padding: 10px; border-radius: 6px; font-weight: 600; font-size: 13.5px; cursor: pointer; }
    .submit-btn.p0 { background: #ef4444; }
    .submit-btn:disabled { opacity: 0.4; cursor: not-allowed; }
    .msg-item { border-left: 3px solid var(--border); padding: 7px 10px; background: #090a0f; border-radius: 0 5px 5px 0; margin-bottom: 6px; font-size: 12px; }
    .msg-item.p0 { border-left-color: var(--p0); }
    .msg-item.p1 { border-left-color: var(--p1); }
    .msg-item.p2 { border-left-color: var(--p2); }
    .msg-item.p3 { border-left-color: var(--p3); }
    .msg-item.p4 { border-left-color: var(--p4); }
    .msg-header { display: flex; justify-content: space-between; font-size: 10.5px; color: #71717a; margin-bottom: 3px; font-family: ui-monospace, monospace; }
    .status-pill { font-size: 9.5px; padding: 1px 5px; border-radius: 4px; font-weight: 600; }
    .status-stored { background: rgba(234, 179, 8, 0.12); color: #eab308; }
    .status-sent { background: rgba(16, 185, 129, 0.12); color: #10b981; }
    .inbox-alert { border: 1px solid rgba(59, 130, 246, 0.25); background: rgba(59, 130, 246, 0.06); border-radius: 6px; padding: 10px; margin-bottom: 10px; }
    .dtn-notice { font-size: 11px; color: #71717a; line-height: 1.35; margin-bottom: 10px; background: rgba(255, 255, 255, 0.02); border: 1px solid var(--border); border-radius: 6px; padding: 8px 10px; }
  </style>
</head>
<body>
  <div class="header">
    <span class="badge badge-dtn">ARES DTN • Resiliência Autônoma</span>
    <h1>Rede de Resgate & Socorro</h1>
    <div class="node-meta">
      <span id="nodeIdDisplay">Nó: ESP32</span>
      <span>•</span>
      <span id="nodeBatteryDisplay">Bateria: 94%</span>
      <span>•</span>
      <span>ESP-NOW</span>
    </div>
  </div>

  <div class="dtn-notice">
    <strong style="color:#d4d4d8;">Conexão Direta de Emergência:</strong> Este nó funciona sem internet celular. As mensagens são salvas na memória local e propagadas por rádio e drones até as equipes de resgate.
  </div>

  <div id="downstreamAlert" class="inbox-alert" style="display:none;">
    <strong style="color:#60a5fa;">Instrução da Defesa Civil:</strong>
    <p id="downstreamText" style="margin-top:3px; font-size:12.5px; color:#fff;"></p>
  </div>

  <div class="card">
    <div class="card-title">
      <span>Transmitir Mensagem de Socorro</span>
      <span id="pNameBadge" style="font-size:11px; color:var(--p0);">P0 (Emergência)</span>
    </div>

    <label>Situações Frequentes:</label>
    <div class="quick-grid">
      <button type="button" class="quick-btn p0" onclick="applyQuick(0, 'Água subindo rapidamente, estamos ilhados no telhado / andar superior.')">
        <span>🚨</span><span>Água Subindo / Ilhados</span>
      </button>
      <button type="button" class="quick-btn p1" onclick="applyQuick(1, 'Pessoa com ferimentos / necessitando atendimento médico urgente.')">
        <span>🩺</span><span>Feridos / Médico</span>
      </button>
      <button type="button" class="quick-btn p1" onclick="applyQuick(1, 'Abrigo com crianças pequenas / idosos com mobilidade reduzida.')">
        <span>👶</span><span>Idoso / Bebê</span>
      </button>
      <button type="button" class="quick-btn" onclick="applyQuick(2, 'Sem água potável ou suprimentos há mais de 24 horas.')">
        <span>💧</span><span>Água e Comida</span>
      </button>
    </div>

    <label>Nível de Criticidade:</label>
    <div class="priority-grid">
      <button type="button" class="p-btn active" data-p="0" onclick="setPriority(0)">P0</button>
      <button type="button" class="p-btn" data-p="1" onclick="setPriority(1)">P1</button>
      <button type="button" class="p-btn" data-p="2" onclick="setPriority(2)">P2</button>
      <button type="button" class="p-btn" data-p="3" onclick="setPriority(3)">P3</button>
      <button type="button" class="p-btn" data-p="4" onclick="setPriority(4)">P4</button>
    </div>

    <label for="callerName">Seu Nome / Ponto de Referência:</label>
    <input type="text" id="callerName" placeholder="Ex: Família Silva - Casa Azul" maxlength="30">

    <label for="msgText">Descrição da Situação:</label>
    <textarea id="msgText" placeholder="Ex: Precisamos de resgate, nível da água subindo..." maxlength="90" oninput="updateCount()"></textarea>
    <div class="char-count"><span id="charCount">0</span>/90 caracteres</div>

    <button type="button" id="sendBtn" class="submit-btn p0" onclick="sendMessage()">Transmitir via Rede Mesh</button>
  </div>

  <div class="card">
    <div class="card-title">
      <span>Mensagens Locais em Custódia</span>
      <span style="font-size:10.5px; color:#71717a;">Buffer do Nó</span>
    </div>
    <div id="messagesList">
      <div style="text-align:center; color:#71717a; padding:8px 0; font-size:11.5px;">Nenhuma mensagem enviada nesta sessão.</div>
    </div>
  </div>

  <script>
    let currentPriority = 0;
    const priorityLabels = {
      0: "P0 (Emergência / Risco à Vida)",
      1: "P1 (Resgate / Feridos Graves)",
      2: "P2 (Água / Mantimentos)",
      3: "P3 (Apoio Operacional)",
      4: "P4 (Contato / Geral)"
    };

    function applyQuick(p, text) {
      setPriority(p);
      document.getElementById('msgText').value = text;
      updateCount();
    }

    function setPriority(p) {
      currentPriority = p;
      document.querySelectorAll('.p-btn').forEach(b => b.classList.toggle('active', parseInt(b.dataset.p) === p));
      const badge = document.getElementById('pNameBadge');
      badge.textContent = priorityLabels[p];
      badge.style.color = `var(--p${p})`;
      const sendBtn = document.getElementById('sendBtn');
      if (p === 0) {
        sendBtn.classList.add('p0');
      } else {
        sendBtn.classList.remove('p0');
      }
    }

    function updateCount() {
      const len = document.getElementById('msgText').value.length;
      document.getElementById('charCount').textContent = len;
    }

    async function sendMessage() {
      const caller = document.getElementById('callerName').value.trim();
      const text = document.getElementById('msgText').value.trim();
      if (!text) {
        alert("Por favor, digite o conteúdo da mensagem.");
        return;
      }

      const fullText = caller ? `${caller}: ${text}`.substring(0, 90) : text;
      const sendBtn = document.getElementById('sendBtn');
      sendBtn.disabled = true;
      sendBtn.textContent = "Gravando na Malha DTN...";

      try {
        const res = await fetch('/api/send', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ priority: currentPriority, text: fullText })
        });
        const data = await res.json();
        addMessageToList(currentPriority, fullText, data.msg_id || 'LOCAL-' + Date.now());
        document.getElementById('msgText').value = '';
        updateCount();
      } catch (e) {
        addMessageToList(currentPriority, fullText, 'DTN-LOCAL-' + Date.now());
      } finally {
        sendBtn.disabled = false;
        sendBtn.textContent = "Transmitir via Rede Mesh";
      }
    }

    function addMessageToList(priority, text, id) {
      const list = document.getElementById('messagesList');
      if (list.children.length === 1 && list.children[0].textContent.includes('Nenhuma')) {
        list.innerHTML = '';
      }
      const item = document.createElement('div');
      item.className = `msg-item p${priority}`;
      item.innerHTML = `
        <div class="msg-header">
          <span>ID: ${id.substring(0, 14)}</span>
          <span class="status-pill status-stored">CUSTÓDIA DTN</span>
        </div>
        <div>${text}</div>
      `;
      list.prepend(item);
    }

    async function checkStatus() {
      try {
        const res = await fetch('/api/status');
        const data = await res.json();
        if (data.nodeId) document.getElementById('nodeIdDisplay').textContent = 'Nó: ' + data.nodeId;
        if (data.battery) document.getElementById('nodeBatteryDisplay').textContent = 'Bateria: ' + data.battery + '%';
        if (data.downstreamMsg) {
          document.getElementById('downstreamAlert').style.display = 'block';
          document.getElementById('downstreamText').textContent = data.downstreamMsg;
        }
      } catch (e) {}
    }
    setInterval(checkStatus, 4000);
    checkStatus();
  </script>
</body>
</html>
)rawliteral";

#endif // ARES_CAPTIVE_HTML_H
