# ARES Node ESP2 (Gateway)

Recebe as mensagens pelo rádio (do Router ou direto do Storage), **grava na flash**, confirma a custódia e envia cada uma ao `ingress_handler.py` por HTTP POST. Só apaga quando o servidor responde 200.

## Em 30 segundos
1. `cp include/ares_credentials.h.example include/ares_credentials.h` e preencha Wi-Fi (2,4 GHz) e o IP do computador.
2. Fixe o canal do Hotspot em **2** (o mesmo `ESPNOW_CHANNEL` do Storage e do Router).
3. Grave e abra o Monitor Serial (115200). Não pode aparecer `[AVISO] O Hotspot está no canal X`.

## O que mudou em relação ao Gateway da POC
| Antes | Agora | Por quê |
|---|---|---|
| Um `pendingPacket`: o que chegava durante o POST era perdido | Até 2.000 mensagens na flash (LittleFS) | O Router precisava esperar 1,5 s entre entregas e mesmo assim podia perder |
| `Serial.printf` dentro do callback do rádio | Callback só copia para a fila | Mesmo tipo de erro do incidente da POC |
| POST no `loop()` travava o rádio por até 3 s | POST numa tarefa do FreeRTOS (`src/uplink/http_uplink.cpp`) | O rádio e os ACKs continuam funcionando durante o POST |
| POST falhou = mensagem perdida | Tenta de novo com pausa crescente (2 s, 4 s ... 30 s) | Servidor ou Wi-Fi fora do ar não perde nada (RF-NG-15) |
| Não se anunciava | Beacon a cada 5 s com 0 saltos e ACK de custódia | Router e Storage acham o Gateway sozinhos |
| IP do servidor fixo no código | Comando `ip 192.168.0.10` (salvo na NVS) | O DHCP do Hotspot troca o IP do notebook |
| Mesma mensagem podia ir duas vezes | Idempotência: lembra as já enviadas (RF-NG-04) | O `ingress_handler.py` também ignora duplicatas |

O JSON mantém os campos antigos e ganhou `destination_id`, `ttl`, `timestamp`, `storage_avail_kb`, `altura`, `gateway_id`, `last_hop_mac` e `gateway_rx_clock_s`.

## Comandos (Monitor Serial, 115200)
| Comando | O que faz |
|---|---|
| `s` | Status: Wi-Fi, canal, servidor, mensagens esperando, contadores |
| `q` | Mensagens esperando o POST |
| `log` | Últimos 20 eventos |
| `ip 192.168.0.10` | Troca o IP do servidor sem regravar |
| `format sim` | **Apaga** todas as mensagens guardadas |

## Problemas comuns (da wiki)
* `[HTTP] Falha no POST ... código -1 (connection refused)`: firewall do Windows na porta 8080 ou rede como "Pública". No PowerShell (admin): `New-NetFirewallRule -DisplayName ARES -Direction Inbound -Protocol TCP -LocalPort 8080 -Action Allow` e mude a rede para "Particular".
* `[AVISO] O Hotspot está no canal X`: o Hotspot mudou de canal. Fixe no canal 2 ou mude `ESPNOW_CHANNEL` no Storage e no Router.
* Não conecta: rede 5 GHz, WPA2-Enterprise ou "AP Isolation" ligados. Use Hotspot 2,4 GHz, WPA2 normal.
* Enquanto o Wi-Fi tenta reconectar, o rádio troca de canal e pode perder pacotes. Os outros nós reenviam porque não recebem o ACK.

## Arduino IDE
`python tools/gerar_sketches_arduino.py` na raiz e abra `arduino-ide/ares_esp2_gateway/ares_esp2_gateway.ino`. Instale a biblioteca **ArduinoJson**. Placa **ESP32 Dev Module**, Partition Scheme **Default 4MB with spiffs**. A placa C8:C9:A3:CB:BC:8C precisa de Flash Mode **DIO** e **40MHz**.
