# Projeto ARES (Advanced Resilient Emergency System)

Rede de mensagens de emergência com ESP32 comuns: as mensagens viajam por rádio **ESP-NOW** de nó em nó até um **Gateway**, que as envia por **Wi-Fi/HTTP** a um servidor. Nenhuma mensagem some em silêncio: cada nó só apaga a sua cópia quando o próximo confirma que ficou com ela (**custódia**).

---

## 📐 Os 3 ESPs

```
 ESP1 STORAGE              ESP3 ROUTER                ESP2 GATEWAY                 Computador
 cria e GUARDA      ESP-NOW   repassa em direção  ESP-NOW  GUARDA na flash   HTTP    ingress_handler.py
 na flash (LittleFS) ------> ao Gateway (fila     ------> e faz o POST     ------> (porta 8080)
 até ter custódia   <------  P0-P4 na RAM)        <------  ao servidor
                     ACK de custódia               ACK de custódia
```

| ESP | Papel | Pasta | O que ele guarda |
|---|---|---|---|
| ESP1 | **Storage**: cria mensagens (telemetria e texto pelo Serial), aceita mensagens de outros nós e as guarda até entregar | [`ares-firmware-esp1/`](ares-firmware-esp1/README.md) | Até 2.000 mensagens na **flash interna** (LittleFS), sobrevive a queda de energia |
| ESP3 | **Router**: descobre vizinhos, calcula a rota até o Gateway e repassa | [`ares-firmware-esp3/`](ares-firmware-esp3/README.md) | Fila de 24 mensagens na RAM |
| ESP2 | **Gateway**: recebe pelo rádio e envia ao servidor por HTTP | [`ares-firmware-esp2/`](ares-firmware-esp2/README.md) | Até 2.000 mensagens na **flash interna** enquanto o servidor/Wi-Fi está fora |

O Storage também entrega **direto** ao Gateway quando o ouve; o Router só é necessário para aumentar o alcance. Vários Routers formam uma rede de vários saltos.

### Como uma mensagem anda

1. O Storage grava a mensagem na flash e anuncia "estou aqui" (beacon) a cada 5 s. Router e Gateway fazem o mesmo.
2. O Storage escolhe o vizinho mais perto do Gateway (o próprio Gateway vence) e envia.
3. Quem recebe **grava primeiro** e só depois manda o **ACK de custódia**. Só então o Storage apaga a dele.
4. Sem ACK: tenta 3 vezes, espera 3 s e tenta de novo (a rota pode ter mudado).
5. O Gateway envia ao servidor por HTTP. Só apaga quando o servidor responde 200.

### Três confirmações diferentes (a wiki pede para não confundir)

| Confirmação | Quem dá | Significa |
|---|---|---|
| Rádio | Hardware do vizinho, automático | "O sinal chegou". **Não** é custódia. |
| Custódia (ACK) | Firmware do Storage/Router/Gateway | "Gravei, a responsabilidade agora é minha" |
| Entrega | Servidor (HTTP 200) | "Recebi e registrei" |

---

## 📁 Estrutura

```text
ares.system/
├── ares-firmware-esp1/      # ESP1 Storage  (PlatformIO)
├── ares-firmware-esp2/      # ESP2 Gateway  (PlatformIO + ArduinoJson)
├── ares-firmware-esp3/      # ESP3 Router   (PlatformIO)
├── tools/
│   ├── verificar_protocolo.py      # Confere se os arquivos comuns estão iguais nos 3
│   └── gerar_sketches_arduino.py   # Gera as versões para o Arduino IDE
├── ingress_handler.py       # Servidor HTTP que recebe do Gateway
└── tests/                   # Teste isolado de Wi-Fi/HTTP
```

Arquivos **iguais nos três** (mudou em um, copie para os outros e rode `python tools/verificar_protocolo.py`):
`ares_protocol.h` (mensagem de 156 bytes, mesmos campos do repositório oficial), `ares_control_protocol.h` (beacon e ACK), `esp_now_radio.h/.cpp` (rádio) e `dedup_cache.h/.cpp`. O Storage e o Gateway também compartilham `flash_store`, `event_log` e `node_clock`.

---

## 🛠️ Pré-requisitos

* 3 placas **ESP32 comuns** (ESP32-WROOM-32 / "ESP32 Dev Module"). Não precisa de S3, PSRAM, microSD nem LoRa.
* Cabos USB **de dados** e bons (um cabo ruim causou reinício por *brownout* nos testes).
* Wi-Fi/Hotspot **2,4 GHz** com o canal **fixo** (veja "Canal" abaixo).
* Python 3.8+ para o `ingress_handler.py`.
* **PlatformIO** (VS Code) **ou** **Arduino IDE 2** (veja a seção do Arduino IDE).

---

## 🚀 Passo a passo (3 placas)

**1. Servidor no computador**
```bash
python3 ingress_handler.py
```
Anote o IP do computador (`ipconfig` no Windows, `ip a` no Linux).

**2. Fixe o canal do Hotspot em 2** (ou mude `ESPNOW_CHANNEL` nos três `ares_config.h` para o canal do Hotspot).

**3. ESP2 Gateway**: copie `ares-firmware-esp2/include/ares_credentials.h.example` para `ares_credentials.h`, preencha Wi-Fi e IP, grave e abra o Monitor Serial (115200). Esperado: `[WiFi] Conectado ... Canal: 2` sem `[AVISO]` de canal.

**4. ESP3 Router**: grave sem configurar nada. Em até 5 s: `[NBR] Novo vizinho: ... (Gateway)` e `[ROUTE] Rota: 1 salto(s)`.

**5. ESP1 Storage**: grave sem configurar nada. No primeiro boot ele prepara a flash (alguns segundos). Depois: `[NBR] Novo vizinho`, `[ROUTE] Entregando para ...`, e a cada 30 s uma telemetria. Digite `p0 Socorro, 2 pessoas` para mandar uma mensagem urgente.

**6. Confira**: o JSON aparece no terminal do `ingress_handler.py`.

Comandos úteis no Monitor Serial de qualquer ESP: `s` (status), `q` (mensagens guardadas), `log` (eventos, Storage/Gateway), `n` (vizinhos, Storage/Router).

### Testes que valem a pena fazer
| Teste | Como | Esperado |
|---|---|---|
| Queda de energia | Desligue o Gateway, crie mensagens no Storage, desligue e religue o Storage | `[STORE] N mensagem(ns) recuperada(s) da flash` e todas chegam quando o Gateway volta |
| Servidor fora | Pare o `ingress_handler.py` | Gateway: `[HTTP] Falha no POST` com pausas crescentes; nada se perde. Ao religar, tudo chega |
| Prioridade | Com o Gateway desligado, crie `p4 a`, `p4 b`, `p0 c` | Ao religar, `c` chega primeiro |
| Rota alternativa | Storage longe do Gateway, Router no meio | `[ROUTE] Entregando para ... (Router)` |

### Resultado do teste em placas reais (05/11/2026)
Storage + Gateway em ESP32 comuns, Hotspot de iPhone (2,4 GHz, canal 6), Arduino IDE 2:

| Teste | Resultado |
|---|---|
| Gravar uma mensagem na flash | ~0,15 s |
| `p0` no Storage até a custódia no Gateway | 0,58 s |
| Storage → Gateway → Wi-Fi → servidor | JSON chegou, HTTP 200 |
| Servidor desligado e religado | Nada se perdeu, nada duplicou, P1 chegou antes das P4 |

Falta testar: Router (ESP3) em placa real, queda de energia com fila e flash quase cheia.

---

## 🧰 Usando o Arduino IDE

O repositório é PlatformIO. Para o Arduino IDE, gere os sketches:
```bash
python tools/gerar_sketches_arduino.py
```
Abra `arduino-ide/ares_esp1_storage/ares_esp1_storage.ino` (idem para `ares_esp2_gateway` e `ares_esp3_router`).
* Placa: **ESP32 Dev Module**. Partition Scheme: **Default 4MB with spiffs** (o Storage e o Gateway usam essa área para a flash).
* ESP2: instale a biblioteca **ArduinoJson** (Ferramentas > Gerenciar Bibliotecas).
* Placa que fica reiniciando com `partition table error` (a C8:C9:A3:CB:BC:8C fez isso): **Flash Mode DIO** e **Flash Frequency 40MHz**.
* `Failed to connect to ESP32: No serial data received` na gravação: segure **BOOT**, aperte e solte **EN**, solte o BOOT e grave de novo (ou Upload Speed 115200). Se continuar mesmo com outro cabo e outra porta, o defeito é da placa: troque-a.
* Duas janelas do `ingress_handler.py` abertas ao mesmo tempo dividem a porta 8080 no Windows e o JSON aparece na janela "errada". Feche todas (`taskkill /IM python.exe /F`) e abra uma só.
* O código compila nos núcleos 2.x (PlatformIO) e 3.x (Arduino IDE atual): os callbacks do ESP-NOW que mudaram têm `#if` para os dois.

---

## 💡 Erros conhecidos (da wiki e dos testes) e o que o código faz

| Erro | Origem | Tratamento |
|---|---|---|
| HTTP dentro do callback do rádio derrubava a pilha TCP/IP | Post-mortem da POC | Callbacks só copiam para fila do FreeRTOS. O POST roda numa tarefa própria no Gateway. Nada de `Serial` no callback (o ESP2 antigo imprimia). |
| Gateway guardava 1 pacote e perdia o resto durante o POST | Gateway da POC | Gateway guarda até 2.000 na flash, com ACK de custódia. O Router não precisa mais esperar 1,5 s. |
| `POST -1` / `connection refused` | Post-mortem | Firewall do Windows na porta 8080 (`New-NetFirewallRule -DisplayName ARES -Direction Inbound -Protocol TCP -LocalPort 8080 -Action Allow`), rede como **Particular**, servidor em `0.0.0.0`. O Gateway tenta de novo sozinho. |
| IP do notebook muda (DHCP do Hotspot) | Contingência | No Gateway: `ip 192.168.x.y` no Serial. Fica salvo, sem regravar. |
| Hotspot troca de canal → nós param de se ouvir | Contingência | Fixe o canal. O Gateway avisa `[AVISO] O Hotspot está no canal X` a cada 10 s. |
| Wi-Fi 5 GHz, WPA2-Enterprise, AP Isolation | Contingência | Não suportados pelo ESP32: use Hotspot 2,4 GHz, WPA2 normal, sem isolamento. |
| `msg_id` repetia a cada reinício do ESP1 | Repositório | Storage usa `MAC curto + sequência` que nunca repete (contador de boots na NVS). |
| `source_id` "ESP1-EMISSOR" (12 letras) era cortado | Repositório | `source_id` agora é `STG-A1B2C3` (10 letras). |
| "Peer channel is not equal to the home channel" | Testes do Router | Peers com canal 0 (= canal atual do rádio). |
| Reinício por brownout na USB | Testes do Router | Troque o cabo. Se precisar, `RADIO_LOW_TX_POWER true`. |
| Mensagem de ~260 B não cabe no ESP-NOW v1 (250 B) | Wiki (Storage) | O `AresMessagePacket` tem 156 B. Um `static_assert` impede passar de 250. |
| microSD / PSRAM / ESP32-S3 | Wiki | Não usados: flash interna com LittleFS; índice de 2.000 mensagens ocupa ~40 KB de RAM. |

## ⚠️ Limitações que continuam
* A fila do **Router** fica na RAM (24 mensagens): se ele reiniciar com mensagens, elas se perdem. Storage e Gateway não têm esse problema.
* A flash do ESP32 aguenta ~100 mil gravações por bloco; o LittleFS distribui as gravações, e com telemetria a cada 30 s isso dá muitos anos. Não diminua muito o `TELEMETRY_INTERVAL_MS`.
* O relógio do Storage/Gateway é o "tempo de vida" do nó (sem GNSS/internet): a validade de 7 dias conta o tempo **ligado**.
* Enquanto o Gateway tenta reconectar no Wi-Fi, o rádio dele troca de canal e ele pode perder pacotes. Os nós reenviam porque não recebem o ACK.
* Sem criptografia/autenticação das mensagens, sem LoRa e sem troca automática de papéis (fora do escopo por enquanto).
