#!/usr/bin/env python3
"""Servidor de teste para o Gateway do firmware unificado ARES (repo do colega).
Recebe POST /api/ares/ingress (SOS) e /api/ares/heartbeat. Porta 5026. Só biblioteca padrão."""
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORTA = 5026


class Handler(BaseHTTPRequestHandler):
    def _responder(self, corpo):
        dados = json.dumps(corpo).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(dados)))
        self.end_headers()
        self.wfile.write(dados)

    def do_POST(self):
        tamanho = int(self.headers.get("Content-Length", 0))
        bruto = self.rfile.read(tamanho)
        try:
            msg = json.loads(bruto or b"{}")
        except json.JSONDecodeError:
            msg = {"_invalido": bruto.decode(errors="replace")}

        hora = time.strftime("%H:%M:%S")
        if self.path.endswith("/ingress"):
            print(f"\n[{hora}] MENSAGEM SOS RECEBIDA")
            print(f"   de: {msg.get('source_id')}  via gateway: {msg.get('gateway_id')}")
            print(f"   prioridade: P{msg.get('priority')}  saltos: {msg.get('hop_count')}")
            print(f"   texto: {msg.get('text_message')!r}")
        elif self.path.endswith("/heartbeat"):
            nos = msg.get("mesh_nodes", [])
            resumo = ", ".join(f"{n.get('node_id')}={n.get('role')}({n.get('hops')} saltos)" for n in nos)
            print(f"[{hora}] heartbeat de {msg.get('gateway_id')} | canal Wi-Fi {msg.get('wifi_channel')} | nos: {resumo or '-'}")
        else:
            print(f"[{hora}] POST {self.path}: {msg}")

        self._responder({"status": "ok", "server_timestamp": int(time.time())})

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    print(f"Servidor de teste ARES ouvindo em 0.0.0.0:{PORTA}  (Ctrl+C para sair)")
    ThreadingHTTPServer(("0.0.0.0", PORTA), Handler).serve_forever()
