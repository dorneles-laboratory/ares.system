# ARES Node ESP3 (Router)

Nó intermediário que fica entre o ESP1 (Emissor) e o ESP2 (Gateway). Recebe o `AresMessagePacket` via ESP-NOW, guarda numa fila de prioridades (P0 primeiro) e repassa o pacote **inteiro e sem alteração de campos**, exceto o `ttl`, que desconta 1 a cada salto. Se o próximo salto não confirmar o recebimento, a mensagem continua guardada e é tentada de novo mais tarde (store-carry-forward).

```
ESP1 (Emissor) --ESP-NOW--> Router --ESP-NOW--> ESP2 (Gateway) --HTTP--> ingress_handler.py
```

O ESP1 e o ESP2 não mudam: o ESP1 só passa a ter o MAC do Router no seu `GATEWAY_MAC`.

```text
ares-firmware-esp3/
├── include/
│   ├── ares_config.h               # Canal, tempos de reenvio, tamanhos da fila e debug
│   ├── ares_credentials.h.example  # Modelo: MAC do ESP2 (próximo salto)
│   ├── ares_protocol.h             # Cópia idêntica à do ESP1/ESP2
│   ├── dedup_cache.h               # Memória das mensagens já vistas
│   ├── esp_now_router.h            # Interface do rádio
│   └── message_store.h             # Fila de mensagens P0-P4
├── src/
│   ├── main.cpp                    # Recepção, fila, envio com confirmação e comandos Serial
│   ├── radio/
│   │   └── esp_now_router.cpp      # Inicialização, callbacks e envio ESP-NOW
│   └── storage/
│       ├── dedup_cache.cpp         # Buffer circular de IDs vistos
│       └── message_store.cpp       # Fila em RAM com prioridade e espera entre tentativas
├── platformio.ini
└── README.md
```

## Como funciona

1. **Recepção**: o callback do ESP-NOW só aceita pacotes com exatamente `sizeof(AresMessagePacket)` bytes (a mesma regra do ESP2) e copia para uma fila do FreeRTOS. Nada de `Serial` dentro do callback; o processamento acontece no `loop()`.
2. **Duplicatas**: a chave é (`source_id`, `msg_id`, `timestamp`). O `timestamp` entra porque o ESP1 recomeça o contador do `msg_id` quando reinicia.
3. **TTL**: o `header.ttl` conta saltos. Pacote que chega com `ttl == 0` é descartado; os outros seguem com `ttl - 1`.
4. **Fila**: até `STORE_SLOTS` mensagens somando todas as prioridades. Sai primeiro a P0 e, na mesma prioridade, a mais antiga. Com a fila cheia, uma mensagem mais urgente toma a vaga da mais antiga de prioridade menor.
5. **Envio**: unicast para `GATEWAY_MAC` no canal `ESPNOW_CHANNEL`. A vaga só é liberada quando o callback de envio informa sucesso, que é a confirmação automática do rádio do ESP2 (não existe ACK do ARES ainda). Sem confirmação após `MAX_TX_ATTEMPTS`, a mensagem espera `RETRY_BACKOFF_MS` e volta para a fila.

## Configuração

1. Duplique o modelo e coloque o MAC do ESP2 (o que aparece em `--- DESCOBRINDO ENDEREÇO MAC ---` no monitor dele):
   ```bash
   cp ares-firmware-esp3/include/ares_credentials.h.example ares-firmware-esp3/include/ares_credentials.h
   ```
   ```c
   #define GATEWAY_MAC {0xC8, 0xC9, 0xA3, 0xCB, 0xBC, 0x8C}
   ```
2. Grave o Router:
   ```bash
   pio run -d ares-firmware-esp3 -t upload
   pio device monitor -d ares-firmware-esp3 -b 115200
   ```
3. Anote o MAC que o Router mostra no boot e coloque esse MAC no `GATEWAY_MAC` do `ares_credentials.h` do **ESP1**. Grave o ESP1 de novo.
4. O canal precisa ser o mesmo nos três: o ESP1 e o Router usam o canal 2 fixo, e o ESP2 segue o canal do Wi-Fi/Hotspot. Configure o Hotspot no canal 2 ou mude `ESPNOW_CHANNEL` no `ares_config.h` do Router (e o canal no ESP1) para o canal do Hotspot.

## Verificando

No Monitor Serial do Router:
```text
[RX] msg-uuid-1 | Origem: ESP1-EMISSO | P4 | ttl=64 | Vizinho: 24:0A:C4:..:..:..
[QUEUE] Guardada. Fila: 1/24
[TX] msg-uuid-1 | P4 | enviando ao próximo salto
[ACK] msg-uuid-1 entregue ao próximo salto (tentativa 1/3, rodada 1). Vaga liberada.
```
O ESP2 faz o POST e o `ingress_handler.py` imprime o JSON como no README da raiz.

Comandos no Monitor Serial:

| Comando | O que faz |
|---|---|
| `s` | Status: MAC, próximo salto e contadores |
| `q` | Lista a fila por prioridade |
| `p0 texto` ... `p4 texto` | Injeta uma mensagem com essa prioridade, como se tivesse chegado pelo rádio |
| `r` | Injeta de novo a última mensagem, igual, para testar o descarte de duplicatas |

## Teste com uma placa só

Sem ESP1 e sem ESP2 dá para ver a fila, a prioridade e o reenvio funcionando:

1. No `ares_credentials.h`, coloque no `GATEWAY_MAC` um MAC que não está ligado, por exemplo `{0x02, 0x00, 0x00, 0x00, 0x00, 0x01}`. Não use `FF:FF:...`: broadcast sempre "dá certo" e nada ficaria guardado.
2. Grave e abra o Monitor Serial em 115200.
3. Digite `p4 primeira`, `p4 segunda` e `p0 socorro`. Cada uma mostra `[RX]` e `[QUEUE]`.
4. Esperado: cada mensagem tenta 3 vezes por rodada e mostra uma linha `[TX] ... rodada N: 3 tentativas sem confirmação`, fica guardada e volta 3 s depois. A `p0` passa na frente das P4 que ainda estão esperando.
5. `q` mostra as três na fila; `r` mostra `[DUP]`; `s` mostra `Repassadas: 0`.

## Debug

Igual ao ESP1: `FORCE_SOFTWARE_DEBUG true` no `ares_config.h` ou o pino `GPIO 4` em 3.3V ativa os logs.

## Limitações conhecidas

* Próximo salto fixo (`GATEWAY_MAC`): ainda não há descoberta de vizinhos nem escolha de rota.
* A confirmação é a do rádio do próximo salto. Ela garante que o ESP2 recebeu, não que o POST HTTP deu certo.
* A fila fica na RAM e se perde se o Router reiniciar.
