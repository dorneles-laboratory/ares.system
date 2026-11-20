# Teste de bancada — 20/11/2026

Este documento registra o teste em hardware real do firmware unificado ARES. O fluxo de ponta a ponta funcionou:

> **Celular (sem internet) → Wi-Fi do ROUTER → malha ESP-NOW → GATEWAY → Wi-Fi da base → servidor HTTP**

Mensagens de SOS digitadas no celular, conectado só na rede aberta de uma ESP32, chegaram ao servidor na base. Chegaram com origem, gateway, prioridade, número de saltos e texto.

---

## 1. Montagem usada

| Placa (ID) | Papel | Onde estava ligada | Função no teste |
| :--- | :--- | :--- | :--- |
| `ESP32_755B34` | **GATEWAY (1)** | USB do notebook da base (cabo bom) | Recebe da malha via ESP-NOW e envia via HTTP ao servidor pela rede `ARES_BASE_WIFI` |
| `ESP32_599644` | **ROUTER (2)** | USB do notebook | Sobe a rede Wi-Fi aberta `ARES-...` e o portal `http://192.168.4.1`, onde o celular envia o SOS |
| `ESP32_CBBC8C` | **STORAGE (4)** | USB do notebook | Nó de retenção DTN; participa da malha e repassa telemetria |
| `ESP32_756580` | RELAY (3) | USB do notebook (parte do teste) | Repetidor; foi usado e depois desligado para liberar a porta |
| `ESP32_A00370` | RELAY (3) | Outro notebook | Apareceu na malha no início do teste |

Base:
- Notebook com o **Hotspot Móvel do Windows** ligado: SSID `ARES_BASE_WIFI`, banda 2,4 GHz, IP da base `192.168.137.1`.
- Servidor de teste `tools/servidor_teste.py` ouvindo na porta **5026**, no endpoint que o gateway já usa por padrão (`http://192.168.137.1:5026/api/ares/ingress`).

---

## 2. Resultado (log real do servidor)

Heartbeat do gateway listando os nós vistos na malha:

```
[18:58:42] heartbeat de ESP32_755B34 | canal Wi-Fi 11 | nos: ESP32_599644=ROUTER(1 saltos), ESP32_CBBC8C=STORAGE(1 saltos)
```

SOS enviados pelo celular através do portal do ROUTER:

```
[19:00:18] MENSAGEM SOS RECEBIDA
   de: ESP32_599644  via gateway: ESP32_755B34
   prioridade: P1  saltos: 1
   texto: 'julinha: Pessoa com ferimentos / necessitando atendimento médico urgente.'

[19:00:29] MENSAGEM SOS RECEBIDA
   de: ESP32_599644  via gateway: ESP32_755B34
   prioridade: P0  saltos: 1
   texto: 'julinha: Água subindo rapidamente, estamos ilhados no telhado / andar superior.'
```

O que o teste comprovou:
- **Captive portal offline:** o celular conectou na rede aberta do ROUTER e enviou SOS de P0 a P4, sem internet.
- **Malha ESP-NOW:** o ROUTER entregou o pacote ao GATEWAY pelo rádio. Os nós aparecem nos heartbeats com papel e número de saltos.
- **Ingress HTTP:** o GATEWAY, conectado ao hotspot da base, enviou cada SOS ao servidor (`/api/ares/ingress`) e um heartbeat a cada ~10 s (`/api/ares/heartbeat`).
- **Papéis dinâmicos:** os papéis foram trocados em tempo real pelo monitor serial (`role 1..4`). Eles ficam salvos na NVS e sobrevivem a reinícios e à troca de fonte.
- **Redundância:** num momento havia **dois gateways** ativos por engano. O mesmo SOS chegou duas vezes, uma por gateway, o que mostra que a malha entrega por caminhos paralelos. Depois que um deles voltou a ser RELAY, cada SOS passou a chegar uma vez só.
- Todos os pacotes chegaram com `saltos: 1` porque as placas estavam lado a lado. Para demonstrar 2 saltos e o store-and-forward, é preciso afastar o ROUTER do GATEWAY e deixar o STORAGE/RELAY no meio.

---

## 3. Problemas encontrados e correções aplicadas

As mudanças no código estão marcadas com `[CORRECAO USB]` e `[CORRECAO CANAL]`.

### 3.1 Brownout pela USB do notebook
**Sintoma:** `E BOD: Brownout detector was triggered` em loop. A placa reinicia sempre na hora em que liga o rádio.

**Causa:** a porta USB do notebook não segura o pico de corrente do Wi-Fi/ESP-NOW. O papel ROUTER, que roda softAP, ESP-NOW e servidor web juntos, é o que mais consome.

**Correções** (`src/main.cpp`):
- Potência de transmissão reduzida de 15 dBm para **8,5 dBm** em todos os papéis (`WiFi.setTxPower(WIFI_POWER_8_5dBm)`).
- Troca de frequência de CPU por papel (80/160 MHz) **desativada**: a CPU fica na frequência padrão.
- Detector de brownout **mantido desligado** no `setup()`, como no código original. Num teste intermediário ele foi religado, e o ROUTER ficou preso em loop de reset. Desligado de novo, o ROUTER ficou estável.

**Na prática:** use cabo USB curto e bom, de preferência direto no notebook (sem hub), com o notebook na tomada. Se ainda houver resets, alimente a placa com um carregador de 5 V / 1 A ou com um power bank.

### 3.2 Canal de rádio diferente entre gateway e nós
**Sintoma:** o gateway, ao conectar no hotspot, passa a operar no canal do hotspot (no teste, 11). Os outros nós continuavam no canal 1 do ESP-NOW e não se enxergavam.

**Correções:**
- `include/ares_config.h`: `ARES_ESP_NOW_CHANNEL` de `1` para **`11`**, o mesmo canal do hotspot `ARES_BASE_WIFI`.
- `src/main.cpp`: RELAY e STORAGE fixam explicitamente o canal com `esp_wifi_set_channel(ARES_ESP_NOW_CHANNEL, WIFI_SECOND_CHAN_NONE)`.

> ⚠️ O canal do hotspot do Windows depende do Wi-Fi ao qual o notebook está conectado. Se o hotspot subir em outro canal, ajuste `ARES_ESP_NOW_CHANNEL` para o mesmo valor, ou desconecte o notebook de outras redes. O canal atual aparece no comando `s` do gateway ("Canal de Rádio").

### 3.3 Comando `role` virando RELAY
**Sintoma:** `role router` resultava em `RELAY (3)`.

**Causa:** `aresStringToRole()` (`include/ares_protocol.h`) devolve RELAY para qualquer texto que não reconhece. O monitor serial envia o backspace como um byte, então uma correção feita durante a digitação estraga o comando.

**Como evitar:** prefira os números, `role 1` (GATEWAY), `role 2` (ROUTER), `role 3` (RELAY) ou `role 4` (STORAGE), sem usar backspace. Confira o papel com `s`.

### 3.4 Não dava para digitar no monitor serial
No terminal **Git Bash** do VS Code, o `pio device monitor` não recebe o teclado. Use um terminal **PowerShell** com:

```powershell
pio device monitor -b 115200 --echo --filter send_on_enter --eol LF
```

### 3.5 Comandos do README antigo
Os comandos `pio run -d firmware/ares-mesh-node -e esp1_edge_sos ...` não funcionam mais: o firmware foi promovido para a raiz e há um único ambiente, `ares_node`. Todas as placas recebem o **mesmo** firmware, e o papel é escolhido depois.

---

## 4. Como reproduzir o teste

### 4.1 Preparar a base (notebook)
1. Ligue o **Hotspot Móvel do Windows**: nome `ARES_BASE_WIFI`, banda **2,4 GHz** (Configurações > Rede e Internet > Hotspot Móvel). Confira qual senha o firmware espera ou configure a rede no gateway com `wifi <SSID> <SENHA>`.
2. Rode o servidor de teste, que usa só a biblioteca padrão do Python:
   ```powershell
   python tools/servidor_teste.py
   ```
   Se o Firewall do Windows perguntar, permita o acesso. A janela mostra os heartbeats e os SOS.

### 4.2 Gravar as placas
Todas as placas recebem o mesmo firmware. Na raiz do repositório:
```powershell
pio device list
pio run -t upload --upload-port COMx
```

### 4.3 Definir os papéis
Abra o monitor de cada placa (`pio device monitor -p COMx -b 115200 --echo --filter send_on_enter --eol LF`) e digite:

| Papel | Comando |
| :--- | :--- |
| GATEWAY | `role 1` (ou segurar o BOOT por 1 s) |
| ROUTER (portal do celular) | `role 2` |
| RELAY | `role 3` |
| STORAGE | `role 4` |

Use **um único GATEWAY**. Com dois, cada SOS chega duplicado. Confira tudo com `s`: papel, canal 11 e, no gateway, "Conectado ao Wi-Fi: SIM".

### 4.4 Enviar um SOS
1. No celular, conecte na rede aberta **`ARES-...`** criada pelo ROUTER e desligue os dados móveis.
2. O portal abre sozinho; se não abrir, acesse `http://192.168.4.1`.
3. Escolha a prioridade (P0 a P4), preencha nome e mensagem e toque em **Transmitir na Rede Mesh**.
4. Em poucos segundos aparece `MENSAGEM SOS RECEBIDA` na janela do `servidor_teste.py`.

---

## 5. Comandos seriais úteis

| Comando | O que faz |
| :--- | :--- |
| `s` | Status do nó: ID, papel, canal, Wi-Fi, IP, backend, fila |
| `role <1-4>` | Troca o papel e salva na NVS |
| `wifi <SSID> <SENHA>` | (Gateway) configura a rede da base |
| `backend <URL>` | (Gateway) muda o endpoint de ingress |
| `hb` | (Gateway) envia um heartbeat imediato |
| `scan` | Lista as redes Wi-Fi visíveis |
