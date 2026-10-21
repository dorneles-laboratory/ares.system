# Projeto ARES (Advanced Resilient Emergency System)

Sistema de comunicação e telemetria resiliente baseado em nós ESP32, combinando transmissão de rádio ponto a ponto via **ESP-NOW** com um nó Gateway conectado via **Wi-Fi** a um **Servidor Ingress HTTP**.

---

## 📐 Arquitetura do Sistema

```
+---------------------+                    +---------------------+                    +------------------------+
|  ESP1 (Emissor)     |                    |  ESP2 (Gateway)     |                    | Servidor Ingress       |
|                     |                    |                     |                    | (Python Host)          |
| Coleta Telemetria   |   ESP-NOW (2.4GHz) | Recebe via rádio    |     HTTP POST      |                        |
| e Localização       | -----------------> | Converte para JSON  | -----------------> | Porta 8080             |
| Envia pacote binário|                    | Envia via Wi-Fi STA |                    | Exibe dados no terminal|
+---------------------+                    +---------------------+                    +------------------------+
          |                                           ^
          |  ESP-NOW      +---------------------+     |  ESP-NOW
          +-------------> | ESP3 Router (opc.)  | ----+
                          | Fila P0-P4, dedup,  |
                          | repassa o pacote    |
                          +---------------------+
```

1. **ESP1 (Nó Emissor)**: Gera dados de telemetria (bateria, armazenamento), localização (latitude, longitude, altura) e mensagens em uma estrutura binária (`AresMessagePacket`) e despacha periodicamente via rádio ESP-NOW.
2. **ESP2 (Nó Gateway)**: Escuta pacotes ESP-NOW na placa de rede, armazena em buffer seguro, converte em JSON estruturado e despacha via requisição HTTP POST para o servidor Ingress.
3. **ESP3 (Nó Router, opcional)**: Fica entre o ESP1 e o ESP2 para aumentar o alcance; vários ESP3 formam uma rede de vários saltos. Descobre os vizinhos por beacons, calcula a rota até o ESP2 e a refaz quando um vizinho some, descarta duplicatas, guarda numa fila por prioridade (P0 primeiro) e repassa o `AresMessagePacket` sem alteração, guardando-o enquanto o próximo salto não confirmar. Detalhes em [`ares-firmware-esp3/README.md`](ares-firmware-esp3/README.md).
4. **Ingress Handler (Python)**: Servidor HTTP rodando na máquina host/servidor que consome e exibe os pacotes JSON recebidos do Gateway.

---

## 📁 Estrutura de Arquivos

```text
ares/
├── ares-firmware-esp1/         # Firmware do ESP32 Emissor (ESP-NOW TX)
│   ├── include/
│   │   ├── ares_config.h       # Constantes de intervalo, pinos e fallback
│   │   ├── ares_credentials.h  # MAC Address do ESP2 de destino (segredos)
│   │   └── ares_protocol.h     # Definição das structs de pacote e cabeçalhos
│   ├── src/
│   │   ├── main.cpp            # Setup e loop de geração de dados
│   │   └── radio/
│   │       └── esp_now_link.cpp# Inicialização e envio ESP-NOW
│   └── platformio.ini          # Configuração do PlatformIO (esp32dev)
│
├── ares-firmware-esp2/         # Firmware do ESP32 Gateway (ESP-NOW RX -> HTTP)
│   ├── include/
│   │   ├── ares_config.h       # Constantes do gateway e servidor ingress
│   │   ├── ares_credentials.h  # Credenciais Wi-Fi e IP do Ingress
│   │   └── ares_protocol.h     # Definição das structs de pacote
│   ├── src/
│   │   ├── main.cpp            # Recepção, serialização JSON e envio HTTP POST
│   │   └── radio/
│   │       └── esp_now_rx.cpp  # Callback de recepção ESP-NOW
│   └── platformio.ini          # Configuração do PlatformIO + ArduinoJson
│
├── ares-firmware-esp3/         # Firmware do ESP32 Router (ESP-NOW RX -> fila -> ESP-NOW TX)
│   ├── include/
│   │   ├── ares_config.h       # Canal, reenvio, tamanho da fila e debug
│   │   ├── ares_credentials.h  # MAC Address do ESP2 (próximo salto)
│   │   ├── ares_protocol.h     # Definição das structs de pacote
│   │   ├── ares_router_protocol.h # Beacon e ACK de custódia entre Routers
│   │   ├── dedup_cache.h       # Mensagens já vistas
│   │   ├── esp_now_router.h    # Interface do rádio
│   │   ├── message_store.h     # Fila P0-P4
│   │   ├── neighbor_table.h    # Tabela de vizinhos
│   │   └── routing.h           # Distância até o Gateway e próximo salto
│   ├── src/
│   │   ├── main.cpp            # Recepção, beacons, envio com custódia e comandos
│   │   ├── radio/
│   │   │   └── esp_now_router.cpp # Callbacks e envio ESP-NOW
│   │   ├── routing/
│   │   │   ├── neighbor_table.cpp # Vizinhos e expiração
│   │   │   └── routing.cpp     # Gradiente de saltos até o Gateway
│   │   └── storage/
│   │       ├── dedup_cache.cpp # Buffer circular de IDs
│   │       └── message_store.cpp # Fila em RAM por prioridade
│   └── platformio.ini          # Configuração do PlatformIO (esp32dev)
│
├── ingress_handler.py          # Servidor HTTP Ingress principal em Python
│
└── tests/
    ├── test_server.py          # Servidor HTTP simples para teste de bancada
    └── test-server/            # Firmware ESP32 de teste isolado para envio HTTP
```

---

## 🛠️ Pré-requisitos

### Hardware
* 2x Placas de desenvolvimento **ESP32** (ex: ESP32-WROOM-32 / ESP32 Dev Module), ou 3x com o ESP3 (Router).
* 2x Cabos de dados Micro-USB ou USB-C (3x com o ESP3).
* Ponto de acesso Wi-Fi de 2.4 GHz (pode ser o roteador local ou o Hotspot/Ponto de Acesso do celular).

### Software
* **Python 3.8+** (utiliza bibliotecas nativas `http.server` e `json`, sem necessidade de `pip install`).
* **VS Code** com a extensão oficial **PlatformIO IDE** (ou a CLI do PlatformIO `pio`).

---

## 🚀 Como Executar o Projeto (Passo a Passo)

### 1. Iniciar o Servidor Ingress no Computador

O servidor precisa estar rodando para receber as requisições HTTP do ESP2 Gateway.

1. Descubra o IP local do seu computador na mesma rede Wi-Fi que o ESP32 usará:
   * **Linux**: `ip a` ou `hostname -I`
   * **macOS**: `ipconfig getifaddr en0`
   * **Windows**: `ipconfig`
2. No diretório raiz do projeto, execute o script:
   ```bash
   python3 ingress_handler.py
   ```
   O terminal indicará:
   ```text
   --- ARES Ingress Handler Rodando em 0.0.0.0:8080 ---
   Aguardando conexões HTTP POST do Gateway ESP2...
   ```

---

### 2. Configurar e Gravar o ESP2 (Gateway)

O Gateway precisa das credenciais do seu Wi-Fi e do IP do seu computador (onde o `ingress_handler.py` está rodando).

1. Abra a pasta `ares-firmware-esp2/include/`.
2. Se ainda não existir o arquivo `ares_credentials.h`, duplique o modelo:
   ```bash
   cp ares-firmware-esp2/include/ares_credentials.h.example ares-firmware-esp2/include/ares_credentials.h
   ```
3. Edite `ares_credentials.h` com os dados da sua rede e o IP da sua máquina:
   ```c
   #ifndef ARES_CREDENTIALS_H
   #define ARES_CREDENTIALS_H

   #define WIFI_SSID "NOME_DO_SEU_WIFI"
   #define WIFI_PASSWORD "SENHA_DO_SEU_WIFI"
   #define INGRESS_SERVER_IP "192.168.1.X" // IP descoberto no passo 1

   #endif
   ```
4. Conecte o ESP2 ao computador via USB.
5. Compile e faça o upload:
   * **Pelo VS Code**: Abra o projeto, clique no ícone do PlatformIO na barra lateral e selecione `ares-firmware-esp2` -> **Upload and Monitor**.
   * **Pela CLI**:
     ```bash
     pio run -d ares-firmware-esp2 -t upload
     pio device monitor -d ares-firmware-esp2 -b 115200
     ```
6. **Importante**: Observe no Monitor Serial a seguinte linha de saída:
   ```text
   --- DESCOBRINDO ENDEREÇO MAC ---
   XX:XX:XX:XX:XX:XX
   ```
   **Anote esse MAC Address**, pois ele será usado na configuração do ESP1.

---

### 3. Configurar e Gravar o ESP1 (Emissor)

O Emissor precisa saber o endereço MAC do ESP2 para enviar mensagens via ESP-NOW.

1. Abra a pasta `ares-firmware-esp1/include/`.
2. Se ainda não existir `ares_credentials.h`, duplique o modelo:
   ```bash
   cp ares-firmware-esp1/include/ares_credentials.h.example ares-firmware-esp1/include/ares_credentials.h
   ```
3. Edite `ares_credentials.h` inserindo os bytes do MAC Address obtido no ESP2:
   ```c
   #ifndef ARES_CREDENTIALS_H
   #define ARES_CREDENTIALS_H

   // Exemplo: se o MAC for C8:C9:A3:CB:BC:8C
   #define GATEWAY_MAC {0xC8, 0xC9, 0xA3, 0xCB, 0xBC, 0x8C}

   #endif
   ```
4. Conecte o ESP1 ao computador via USB.
5. Compile e faça o upload:
   * **Pelo VS Code**: Selecione `ares-firmware-esp1` -> **Upload and Monitor**.
   * **Pela CLI**:
     ```bash
     pio run -d ares-firmware-esp1 -t upload
     pio device monitor -d ares-firmware-esp1 -b 115200
     ```

---

### 4. (Opcional) Colocar o ESP3 (Router) no meio

Com o ESP1 e o ESP2 funcionando direto, dá para colocar um terceiro ESP32, o ESP3, entre eles. Siga o [`ares-firmware-esp3/README.md`](ares-firmware-esp3/README.md): o Router recebe o MAC do ESP2 no `GATEWAY_MAC` dele, e o ESP1 passa a ter o MAC do Router no `GATEWAY_MAC`. Nenhum código do ESP1 ou do ESP2 muda.

---

## 🔍 Verificando o Funcionamento

Com os dois ESPs ligados e o script Python em execução:

1. **No Monitor Serial do ESP1 (Emissor)**:
   ```text
   [LOOP] Pacote #1 despachado com sucesso via ESP-NOW.
   [LOOP] Pacote #2 despachado com sucesso via ESP-NOW.
   ```
2. **No Monitor Serial do ESP2 (Gateway)**:
   ```text
   [ESP-NOW RX] Pacote recebido de MAC: ... | Tamanho: ... bytes
   --- [ARES GATEWAY] Pacote Processado na Thread Principal ---
   ...
   [HTTP] POST enviado com sucesso! Código de resposta: 200
   [HTTP] Resposta do Servidor: {"status":"success"}
   ```
3. **No Terminal do Python (`ingress_handler.py`)**:
   ```text
   [ARES INGRESS] Pacote recebido com sucesso do Gateway!
   {
       "msg_id": "msg-uuid-1",
       "source_id": "ESP1-EMISSOR",
       "priority": 4,
       "battery_level": 98.5,
       "lat": "-29.6842",
       "lon": "-53.8069",
       "text_message": "ARES #1"
   }
   ```

---

## 🧪 Teste Isolado de Conectividade Wi-Fi/HTTP (`tests/`)

Caso queira validar a conexão Wi-Fi e comunicação HTTP do ESP32 com o computador antes de testar o ESP-NOW:

1. Inicie o servidor de teste:
   ```bash
   python3 tests/test_server.py
   ```
2. Configure `tests/test-server/include/ares_credentials.h` com seu SSID, senha e IP local.
3. Grave o firmware de teste em um ESP32:
   ```bash
   pio run -d tests/test-server -t upload
   ```
4. O ESP32 enviará um JSON `{"teste":"ola_do_esp32"}` a cada 5 segundos na rota raiz do servidor.

---

## 💡 Dicas e Soluções de Problemas

* **Rede Wi-Fi 2.4 GHz**: Os módulos ESP32 padrão só operam em redes de 2.4 GHz. Certifique-se de que a rede Wi-Fi ou o Hotspot do celular esteja operando nessa frequência.
* **Canal do ESP-NOW**: O protocolo ESP-NOW compartilha o canal de rádio com a interface Wi-Fi. Certifique-se de que ambos os dispositivos estejam no mesmo canal (o ESP2 sincroniza com o canal do AP conectado).
* **Firewall do Computador**: Se o ESP2 se conectar ao Wi-Fi mas falhar no envio HTTP (`Falha no POST`), verifique se o firewall do sistema operacional (como `ufw`, `iptables` ou Windows Defender) não está bloqueando conexões de entrada na porta `8080`.
* **Modo Debug no ESP1**: No arquivo `ares-firmware-esp1/include/ares_config.h`, a diretiva `FORCE_SOFTWARE_DEBUG true` ativa logs detalhados via serial. Alternativamente, colocar o pino `GPIO 4` em nível lógico alto (3.3V) ativa o debug físico.
