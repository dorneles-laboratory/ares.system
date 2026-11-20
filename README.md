# 🛰️ ARES — Firmware Unificado e Bancada de Teste E2E (6 ESP32s)

Este módulo contém o firmware unificado para a bancada de **6 nós ESP32**, compondo o ecossistema completo de comunicação tolerante a atrasos e desconexões (**DTN / FANET**) do projeto **ARES** (*Adaptive Resilient Emergency System*).


> ✅ **Teste de bancada em hardware real (20/11/2026):** fluxo celular → ROUTER → malha ESP-NOW → GATEWAY → servidor funcionando. Veja [docs/TESTE-BANCADA-2026-11-20.md](docs/TESTE-BANCADA-2026-11-20.md) para resultados, correções aplicadas e o passo a passo atualizado.

---

## 🏗️ Topologia da Bancada Física (Os 6 Nós)

```
[Morador/Vítima] (Celular conectado no Wi-Fi Cativo)
       │
       ▼
 ┌──────────────┐       ESP-NOW        ┌──────────────┐
 │ ESP 1 (Borda)│ ───────────────────► │ ESP 2 (Relay)│
 └──────────────┘                      └──────┬───────┘
  (Ilha Isolada A)                            │
                                              │ (Fora de alcance do Gateway)
                                              ▼
 ┌──────────────┐                      ┌──────────────┐
 │ ESP 4 (FANET)│ ◄─── (Drenagem) ──── │ ESP 3 (Relay)│
 └──────┬───────┘                      └──────────────┘
        │ (Deslocamento físico / Data Mule)
        ▼
 ┌──────────────┐       HTTP / IP      ┌─────────────────────────────┐
 │ESP 6(Gateway)│ ───────────────────► │ PLATAFORMA ARES (PC/Servidor)│
 └──────────────┘                      │  - Backend Ingress + WS/SSE │
                                       │  - Dashboard Topologia/Mapa │
 ┌──────────────┐                      │  - Painel de Resgate        │
 │ ESP 5 (Borda)│ ◄── (Downstream) ─── └─────────────────────────────┘
 └──────────────┘
 (Ilha Isolada B / Equipe de Campo)
```

| Ambiente PlatformIO | Nó | Papel Arquitetural | SSID Wi-Fi / IP | Função Principal |
| :--- | :--- | :--- | :--- | :--- |
| `esp1_edge_sos` | **ESP1** | Borda A (Vítima / Abrigo) | `ARES-EMERGENCIA-01` (`192.168.4.1`) | Sobe Captive Portal offline. Vítimas enviam alertas P0 a P4 direto do smartphone sem internet. |
| `esp2_relay_north` | **ESP2** | Relay Norte (Torre) | Modo Station (ESP-NOW) | Retransmissão ad-hoc com incremento de saltos e telemetria de enlace. |
| `esp3_relay_storage`| **ESP3** | Relay Storage (DTN) | Modo Station (ESP-NOW) | Retenção Store-and-Forward em memória. Guarda mensagens enquanto não há rota até o gateway. |
| `esp4_drone_mule` | **ESP4** | FANET Drone (Data Mule) | Modo Station (ESP-NOW) | Emula o drone móvel: sobrevoa o ESP3, drena a fila de mensagens e viaja até a base. |
| `esp5_edge_field` | **ESP5** | Borda B (Equipe de Campo) | `ARES-RESGATE-02` (`192.168.4.1`) | Captive Portal para socorristas em campo, recebendo ordens de serviço da Base. |
| `esp6_gateway` | **ESP6** | Gateway Ingress Terrestre | Conectado à LAN da Base | Recebe do rádio ESP-NOW, serializa para JSON e despacha via HTTP POST no backend do ARES. |

---

## ⚡ Como Gravar os ESP32s (PlatformIO)

> ⚠️ **Atualizado no teste de 20/11/2026.** Os ambientes `esp1_edge_sos`, `esp6_gateway` etc. e a pasta
> `firmware/ares-mesh-node` não existem mais. O `platformio.ini` (na raiz) tem um só ambiente, `ares_node`:
> **todas as placas recebem o mesmo firmware** e o papel é escolhido depois, pelo monitor serial.
> O que mudou e por quê: [docs/MUDANCAS-2026-11-20.md](docs/MUDANCAS-2026-11-20.md).

```bash
# 1. Gravar (repetir para cada placa; com várias no USB use --upload-port COMx)
pio run -t upload

# 2. Escolher o papel (fica salvo na placa). No Windows, use o terminal PowerShell.
pio device monitor -b 115200 --echo --filter send_on_enter
#   role 1 = GATEWAY | role 2 = ROUTER | role 3 = RELAY (padrão) | role 4 = STORAGE
#   s      = mostra o estado da placa (papel, Wi-Fi, canal)
```

---

## 📱 Como Testar pelo Smartphone (Captive Portal)

1. Ligue o **ESP 1**.
2. No celular, conecte-se à rede Wi-Fi aberta `ARES-EMERGENCIA-01`.
3. O portal de socorro abre automaticamente (ou acesse `http://192.168.4.1` no navegador).
4. Selecione a prioridade (**P0** a **P4**), digite seu nome e o pedido de socorro.
5. Pressione **"Transmitir na Rede Mesh"**.
6. O status de custódia DTN é exibido na tela do smartphone e o pacote salta pelos ESPs até a base central!

---

## 🖥️ Como Visualizar no Dashboard ARES (PC / Base Central)

1. Inicie a stack completa do ARES:
   ```bash
   make dev-upd
   ```
   Ou inicie o backend e o frontend:
   ```bash
   # Terminal 1 (Backend API)
   cd apps/backend && npm run dev

   # Terminal 2 (Frontend React)
   cd apps/frontend && npm run dev
   ```

2. Acesse no navegador:
   - **Monitor de Malha & Topologia em Tempo Real:** `http://localhost:5193/admin/ares/mesh`
   - **Simulador do Portal Cativo Web:** `http://localhost:5193/ares/sos`
   - **Documentação da API (Swagger):** `http://localhost:5026/api-docs`

---

## 🧪 Como Rodar a Simulação E2E sem Placas Conectadas

Você pode validar todo o fluxo dos 6 nós sem ter nenhuma placa plugada na USB utilizando o simulador automatizado:

```bash
npx tsx scripts/simulate-mesh.ts
```

Ou diretamente pelos botões interativos do painel em `http://localhost:5193/admin/ares/mesh`!
