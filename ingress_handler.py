from http.server import BaseHTTPRequestHandler, HTTPServer
import json

HOST = "0.0.0.0"  # Escuta em todas as interfaces
PORT = 8080      

# Mensagens já recebidas: (source_id, msg_id, timestamp). Se a resposta 200 se perder no
# caminho, o Gateway manda a mesma mensagem de novo; aqui ela é reconhecida e não é
# mostrada duas vezes (idempotência, RF-NG-04). Fica só na memória enquanto o servidor roda.
mensagens_vistas = set()


class AresIngressHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        # Responde a acessos de navegador/testes na raiz
        self.send_response(200)
        self.send_header('Content-type', 'text/html')
        self.end_headers()
        self.wfile.write(b"<h3>ARES Ingress Server Ativo e Operando!</h3>")

    def do_POST(self):
        try:
            content_length = int(self.headers.get('Content-Length', 0))
            post_data = self.rfile.read(content_length)
            
            payload = json.loads(post_data.decode('utf-8'))

            chave = (payload.get("source_id"), payload.get("msg_id"), payload.get("timestamp"))
            if chave in mensagens_vistas:
                # Responde 200 mesmo assim: o Gateway precisa saber que pode apagar a dele
                print(f"\n[ARES INGRESS] Duplicada ignorada: {payload.get('msg_id')} de {payload.get('source_id')}")
            else:
                mensagens_vistas.add(chave)
                print("\n[ARES INGRESS] Pacote recebido com sucesso do Gateway!")
                print(json.dumps(payload, indent=4, ensure_ascii=False))
            
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.end_headers()
            self.wfile.write(b'{"status":"success"}')
            
        except Exception as e:
            print(f"[ERRO] Falha ao processar o pacote JSON: {e}")
            self.send_response(400)
            self.end_headers()

def run():
    server_address = (HOST, PORT)
    httpd = HTTPServer(server_address, AresIngressHandler)
    print(f"--- ARES Ingress Handler Rodando em {HOST}:{PORT} ---")
    print("Aguardando conexões HTTP POST do Gateway ESP2...")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[INFO] Servidor Ingress encerrado pelo usuário.")
        httpd.server_close()

if __name__ == '__main__':
    run()