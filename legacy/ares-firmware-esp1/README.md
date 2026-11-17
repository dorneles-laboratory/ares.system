# ARES Node ESP1 (Storage)

Cria mensagens, **guarda na flash do próprio ESP32** e só apaga cada uma quando o próximo nó (Router ou Gateway) confirma que ficou com ela. Se a energia cair, as mensagens continuam lá no próximo boot.

## Em 30 segundos
* **Grave e pronto.** Não precisa configurar MAC: o Storage acha o Router/Gateway pelos beacons deles.
* A cada 30 s ele cria uma telemetria P4. Para mandar texto, digite no Monitor Serial: `p0 Socorro, 3 pessoas no telhado`.
* `q` mostra o que está guardado; `log` mostra o que aconteceu com cada mensagem.

## Arquivos
```text
ares-firmware-esp1/
├── include/
│   ├── ares_config.h            # Canal, intervalo da telemetria, capacidade, posição fixa...
│   ├── ares_credentials.h.example # Opcional: forçar um próximo salto (bancada)
│   ├── ares_protocol.h          # Mensagem de 156 bytes (igual nos 3 ESPs)
│   ├── ares_control_protocol.h  # Beacon e ACK de custódia (igual nos 3 ESPs)
│   ├── esp_now_radio.h          # Rádio (igual nos 3 ESPs)
│   ├── flash_store.h            # Gavetas de mensagens na flash
│   ├── event_log.h              # Registro de eventos
│   ├── node_clock.h             # Relógio e numeração que sobrevivem ao reinício
│   ├── neighbor_table.h / next_hop.h / dedup_cache.h
├── src/
│   ├── main.cpp                 # Cria, recebe, entrega com custódia, comandos Serial
│   ├── radio/esp_now_radio.cpp
│   ├── routing/neighbor_table.cpp, next_hop.cpp
│   └── storage/flash_store.cpp, event_log.cpp, node_clock.cpp, dedup_cache.cpp
└── platformio.ini
```

## Como a flash é usada (sem microSD)
| Onde | O quê | Por quê |
|---|---|---|
| Partição `spiffs` com **LittleFS**, arquivos `/ares/s000.bin` ... `s124.bin` | 2.000 "gavetas" de 180 bytes, 16 por arquivo (~352 KB) | Criadas no 1º boot: nunca falta espaço no meio. Arquivos pequenos porque o LittleFS copia o arquivo a partir do ponto alterado: com um arquivo único, cada gravação levava 6-8 s; com 16 gavetas por arquivo, só um bloco de 4 KB é regravado. |
| Cada gaveta | Pacote + CRC32 | Se a energia cair no meio da gravação, o CRC não bate e a gaveta é ignorada. Como o ACK só sai **depois** de gravar, quem mandou ainda tem a cópia. |
| RAM | Índice de ~20 bytes por gaveta (~40 KB) | Escolher a próxima (P0 primeiro) sem ler a flash toda hora. Reconstruído a cada boot. |
| Arquivo `/events.log` | `CRIADA`, `CUSTODIA_ACEITA`, `ENTREGUE`, `EXPIRADA`, `DESCARTADA`... | RS-04 da wiki: nada some em silêncio. Vira `events.old` ao passar de 32 KB. |
| NVS (`Preferences`) | Relógio do nó e contador de boots | `millis()` zera a cada reinício; a validade (7 dias) e a sequência dos IDs não. |

**Memória cheia:** a mensagem nova toma a vaga da mais antiga de prioridade **menor** (P4 primeiro). P0 nunca perde a vaga. Se tudo for tão urgente quanto a nova, ela é recusada (e quem a mandou continua com ela).

**ID das mensagens:** `msg_id = A1B2C3-65537` (3 últimos bytes do MAC + sequência) e `source_id = STG-A1B2C3`. A sequência é `boots × 65536 + n`: nunca repete, mesmo depois de reiniciar.

## Comandos (Monitor Serial, 115200)
| Comando | O que faz |
|---|---|
| `p0 texto` ... `p4 texto` | Cria uma mensagem (P0 = emergência) |
| `t` | Cria uma telemetria agora |
| `s` | Status: MAC, memória, vizinhos, contadores, RAM livre |
| `n` | Vizinhos (Router/Gateway/Storage, saltos, RSSI) |
| `q` | Mensagens guardadas, por prioridade |
| `log` | Últimos 20 eventos |
| `format sim` | **Apaga** todas as mensagens e o registro |

## O que aparece no Serial
```text
[STORE] LittleFS montado: 8 KB usados de 1472 KB.
[STORE] Preparando a flash: 2000 gavetas x 180 bytes = 351 KB ... pronto.   (só no 1º boot)
[STORE] 0 mensagem(ns) recuperada(s) da flash.
[NBR] Novo vizinho: XX:XX:... (Gateway) | 0 salto(s) até o Gateway
[ROUTE] Entregando para XX:XX:... (Gateway)
[NEW] A1B2C3-65537 | P4 | "Telemetria STG-A1B2C3" | estado: PENDENTE
[TX] A1B2C3-65537 | P4 | enviando para XX:XX:... (Gateway)
[ACK] A1B2C3-65537 | estado: CUSTÓDIA ACEITA pelo Gateway XX:XX:... Apagada da flash.
```

## Arduino IDE
`python tools/gerar_sketches_arduino.py` na raiz e abra `arduino-ide/ares_esp1_storage/ares_esp1_storage.ino`. Placa **ESP32 Dev Module**, Partition Scheme **Default 4MB with spiffs**.

## Problemas comuns
* `LittleFS não montou` / `Partição de arquivos pequena demais`: Partition Scheme sem área de arquivos. Use "Default 4MB with spiffs".
* `[ROUTE] Ninguém para entregar`: Router/Gateway desligado ou em outro canal. As mensagens ficam guardadas.
* Reinicia sozinho logo que o rádio liga (brownout): troque o cabo USB; se precisar, `RADIO_LOW_TX_POWER true`.
