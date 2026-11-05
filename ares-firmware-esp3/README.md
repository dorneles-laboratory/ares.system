# ARES Node ESP3 (Router)

Nó intermediário que leva as mensagens do ESP1 (Storage) até o ESP2 (Gateway), passando por um ou mais Routers. Cada Router descobre os vizinhos por beacons, calcula a sua distância até o Gateway, escolhe o próximo salto e refaz a rota sozinho quando um vizinho some. A mensagem é o `AresMessagePacket` do repositório, repassado **inteiro e sem alteração de campos**, exceto o `ttl`, que desconta 1 a cada salto. Sem rota, a mensagem fica guardada até surgir um caminho (store-carry-forward).

```
ESP1 --ESP-NOW--> ESP3 --ESP-NOW--> ESP3 --ESP-NOW--> ESP2 --HTTP--> ingress_handler.py
(Storage)        (Router)          (Router)          (Gateway)
```

Nenhum MAC precisa ser configurado: o Storage e o Gateway também mandam beacons, então o Router acha os dois sozinho.

```text
ares-firmware-esp3/
├── include/
│   ├── ares_config.h               # Canal, intervalos de beacon, tempos de reenvio, tamanhos e debug
│   ├── ares_credentials.h.example  # Modelo (opcional): MAC do ESP2 (Gateway)
│   ├── ares_protocol.h             # Igual nos 3 firmwares
│   ├── ares_control_protocol.h     # Igual nos 3: beacon e ACK de custódia
│   ├── dedup_cache.h               # Memória das mensagens já vistas
│   ├── esp_now_radio.h             # Interface do rádio (igual nos 3)
│   ├── message_store.h             # Fila de mensagens P0-P4
│   ├── neighbor_table.h            # Tabela de vizinhos
│   └── routing.h                   # Distância até o Gateway e escolha do próximo salto
├── src/
│   ├── main.cpp                    # Recepção, beacons, envio com custódia e comandos Serial
│   ├── radio/
│   │   └── esp_now_radio.cpp       # Callbacks, envio e peers do ESP-NOW (igual nos 3)
│   ├── routing/
│   │   ├── neighbor_table.cpp      # Vizinhos, expiração e falhas de enlace
│   │   └── routing.cpp             # Gradiente de saltos até o Gateway
│   └── storage/
│       ├── dedup_cache.cpp         # Buffer circular de IDs vistos
│       └── message_store.cpp       # Fila em RAM com prioridade, espera e validade
├── platformio.ini
└── README.md
```

## Como funciona

### Descoberta de vizinhos
* A cada `BEACON_INTERVAL_MS` (5 s), cada Router manda um beacon em broadcast com: distância até o Gateway, bateria, vagas livres na fila e por qual vizinho ele chega ao Gateway.
* Quem recebe guarda na tabela de vizinhos (até `MAX_NEIGHBORS`), com o RSSI do sinal e a hora em que viu.
* Vizinho sem beacon por `NEIGHBOR_TIMEOUT_MS` (15 s, 3 beacons perdidos) sai da tabela.
* Vizinho que fica `NEIGHBOR_FAIL_ROUNDS` rodadas seguidas sem confirmar envios também sai: é a detecção de enlace perdido.

### Rota até o Gateway (gradiente de saltos)
* O ESP2 manda beacon com `role = ROLE_GATEWAY` e **0 saltos**. Quem ouve o beacon dele está a **1 salto**.
* Se `GATEWAY_MAC` estiver configurado, o Router também testa o enlace com o ESP2 direto: um pacote pequeno a cada `GATEWAY_PROBE_INTERVAL_MS`. Sem ele (padrão), vale só o beacon.
* Beacons de Storage (`ROLE_STORAGE`) entram na tabela de vizinhos (para o ACK de custódia chegar), mas o Storage nunca é escolhido como caminho.
* Quem não alcança o ESP2 fica a **1 + a menor distância** entre os vizinhos vivos. Sem vizinho com rota, fica sem rota.
* Próximo salto: o Gateway direto, se estiver alcançável. Senão, o vizinho mais perto do Gateway, com desempate por melhor RSSI, depois mais bateria e depois mais vagas.
* Proteções contra loop: a mensagem nunca volta para quem a entregou; um vizinho que chega ao Gateway passando por nós é ignorado (split horizon); distância a partir de `MAX_ROUTE_HOPS` conta como sem rota; e o `ttl` de cada mensagem desconta 1 por salto.
* A rota é recalculada a cada volta do `loop()`. Toda mudança aparece no log como `[ROUTE]`.

### Recepção, duplicatas e fila
* O callback do ESP-NOW só copia o pacote para uma fila do FreeRTOS. Nada de `Serial` dentro do callback (lição do incidente da POC).
* Duplicatas: a chave é (`source_id`, `msg_id`, `timestamp`).
* Fila: até `STORE_SLOTS` mensagens somando P0-P4. Sai primeiro a P0 e, na mesma prioridade, a mais antiga. Com a fila cheia, uma mensagem mais urgente toma a vaga da mais antiga de prioridade menor.
* Validade: mensagem guardada há mais de `MESSAGE_MAX_AGE_MS` (7 dias, RF-NR-05) é descartada.
* Nenhum descarte é silencioso (RS-04): duplicata, `ttl` esgotado, fila cheia e expiração aparecem no log e nos contadores do `s`.

### Envio e as três confirmações
O relatório do Router separa três confirmações, e o ESP3 também:

| Confirmação | De quem | No ESP3 |
|---|---|---|
| Sucesso do rádio | Hardware do próximo salto | Callback de envio do ESP-NOW (unicast) |
| Custódia | Outro Router ou o Gateway | `CTRL_CUSTODY_ACK`, enviado depois que a mensagem entrou na fila/flash dele |
| Entrega ao destino | Servidor | O ESP2 só apaga a dele quando o `ingress_handler.py` responde 200 |

* A vaga só é liberada com o ACK de custódia (espera de até `CUSTODY_TIMEOUT_MS`), tanto para outro Router quanto para o Gateway. Se o ACK se perder e a mensagem chegar de novo, quem já a tem descarta a duplicata e manda o ACK outra vez.
* O Router também manda ACK de custódia para quem lhe entregou (Storage ou outro Router).
* A antiga espera de 1,5 s entre entregas ao ESP2 saiu: o Gateway novo guarda na flash e confirma cada uma.
* Sem confirmação depois de `MAX_TX_ATTEMPTS` tentativas, a mensagem espera `RETRY_BACKOFF_MS` e volta para a fila; enquanto isso, a rota pode mudar.

### Pacotes de controle
Beacon (18 bytes) e ACK de custódia (40 bytes) estão em `ares_control_protocol.h`, igual nos três firmwares. Eles têm tamanho diferente do `AresMessagePacket` (156 bytes, garantido por `static_assert`): é o tamanho que diz se um pacote é dado ou controle.

## Configuração

1. Nada é obrigatório: grave e pronto. O Router acha o Gateway e o Storage pelos beacons deles.
   ```bash
   pio run -d ares-firmware-esp3 -t upload
   pio device monitor -d ares-firmware-esp3 -b 115200
   ```
   (Arduino IDE: `python tools/gerar_sketches_arduino.py` e abra `arduino-ide/ares_esp3_router/`.)
2. Opcional: `GATEWAY_MAC` no `ares_credentials.h` (copie do `.example`) liga o teste de enlace direto com o ESP2. Use o MAC que o ESP2 mostra em `--- DESCOBRINDO ENDEREÇO MAC ---`.
3. O canal precisa ser o mesmo em todos: Storage e Routers usam `ESPNOW_CHANNEL` (2), e o ESP2 segue o canal do Wi-Fi/Hotspot. Fixe o Hotspot no canal 2 ou mude `ESPNOW_CHANNEL` nos três `ares_config.h`.
4. Bateria (opcional): ligue a bateria por um divisor de tensão a um pino ADC e coloque o pino em `BATTERY_ADC_PIN`. Sem isso, o beacon anuncia bateria desconhecida.

## Comandos no Monitor Serial

| Comando | O que faz |
|---|---|
| `s` | Status: MAC, Gateway, rota, vizinhos e contadores |
| `n` | Lista os vizinhos: distância, RSSI, bateria, vagas, há quanto tempo foi visto |
| `q` | Lista a fila por prioridade |
| `p0 texto` ... `p4 texto` | Injeta uma mensagem com essa prioridade, como se tivesse chegado pelo rádio |
| `r` | Injeta de novo a última mensagem, igual, para testar o descarte de duplicatas |
| `v MAC saltos [rssi]` | Simula o beacon de um vizinho (ex.: `v 02:00:00:00:00:AA 1 -50`) |

## Testes

### Com uma placa só (modo simulação)
1. No `ares_credentials.h`, coloque no `GATEWAY_MAC` um MAC que não está ligado, por exemplo `{0x02, 0x00, 0x00, 0x00, 0x00, 0x01}`.
2. Grave e abra o Monitor Serial em 115200. Esperado: `[ROUTE] Sem rota até o Gateway`.
3. **Descoberta e escolha de rota:** `v 02:00:00:00:00:AA 2 -40` e `v 02:00:00:00:00:BB 1 -80`. Aparecem dois `[NBR] Novo vizinho` e `[ROUTE] Rota: 2 salto(s) ... próximo salto 02:00:00:00:00:BB`: o BB vence por estar mais perto do Gateway, mesmo com sinal pior. `n` lista os dois.
4. **Vizinho que some:** espere 15 s sem digitar. Aparecem `[NBR] Vizinho ... sumiu` para os dois e `[ROUTE] Sem rota`.
5. **Guardar sem rota:** `p4 teste`. Aparecem `[RX]` e `[QUEUE]`, e a mensagem fica guardada sem tentar enviar.
6. **Enlace perdido e rota alternativa:** repita os dois `v` do passo 3. A mensagem sai para o BB; como ele é de mentira, ninguém confirma. Depois de 2 rodadas aparece `[NBR] Enlace com 02:00:00:00:00:BB perdido` e a rota muda sozinha para o AA (`[ROUTE] Rota: 3 salto(s)`). O AA também não responde e sai do mesmo jeito, e a rota volta a `Sem rota`. A mensagem continua na fila (`q`).
7. `r` mostra `[DUP]`; `s` mostra a rota e os contadores.

### Com 3 placas (ESP1 → ESP3 → ESP2)
No Router: `[NBR] Novo vizinho: ... (Gateway)`, `[NBR] Novo vizinho: ... (Storage)` e `[ROUTE] Rota: 1 salto(s) ... (o próprio Gateway)`, depois `[RX]`, `[QUEUE]`, `[ACK] Custódia ... confirmada para` (o Storage), `[TX] ... enviando para ... (Gateway)` e `[ACK] ... aceita (custódia) pelo Gateway`. O JSON aparece no `ingress_handler.py`.

Para obrigar a mensagem a passar pelo Router numa mesa só, afaste o Storage do Gateway (ou diminua a potência com `RADIO_LOW_TX_POWER true` no Storage): se o Storage ouvir o Gateway, ele entrega direto, que é o caminho mais curto.

### Com 4 placas (rota de dois Routers e rota alternativa)
Dois Routers. Afaste um deles do ESP2 (ou cubra a antena) até ele mostrar `Rota: 2 salto(s) ... (Router vizinho)`. As mensagens passam pelos dois Routers, com `[ACK] ... aceita (custódia) pelo Router`. Desligue o Router do meio: em até 15 s o outro mostra `Sem rota` e guarda as mensagens. Religue: a rota volta e a fila é drenada com P0 primeiro.

## Debug

Igual ao ESP1: `FORCE_SOFTWARE_DEBUG true` no `ares_config.h` ou o pino `GPIO 4` em 3.3V ativa os logs.

## Limitações conhecidas

* A fila do Router fica na RAM (24 mensagens) e se perde se ele reiniciar. Quem guarda na flash são o Storage (ESP1) e o Gateway (ESP2). Se precisar, o Router pode usar o mesmo `flash_store` do ESP1 no lugar do `message_store` (a interface é parecida).
* No núcleo 2.x (o do PlatformIO), o RSSI vem do modo promíscuo do Wi-Fi, porque o callback do ESP-NOW não traz o sinal. No núcleo 3.x ele vem do próprio callback.
* Fora do escopo por enquanto: autenticação das mensagens, LoRa, troca automática de papéis e drones.
