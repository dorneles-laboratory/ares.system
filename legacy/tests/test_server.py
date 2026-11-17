from http.server import BaseHTTPRequestHandler, HTTPServer
import json

HOST = "0.0.0.0"
PORT = 8080

class TestHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        content_length = int(self.headers.get('Content-Length', 0))
        post_data = self.rfile.read(content_length)
        
        print(f"\n[SUCESSO] Requisição POST recebida de {self.client_address[0]}!")
        print(f"Conteúdo recebido: {post_data.decode('utf-8')}")
        
        self.send_response(200)
        self.send_header('Content-type', 'application/json')
        self.end_headers()
        self.wfile.write(b'{"status":"ok_conexao_estabelecida"}')

    def do_GET(self):
        print(f"\n[INFO] Requisição GET recebida de {self.client_address[0]}")
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b'Server is alive!')

def run():
    server = HTTPServer((HOST, PORT), TestHandler)
    print(f"--- Servidor de Teste Rodando em {HOST}:{PORT} ---")
    print("Aguardando o ESP32...")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nServidor encerrado.")
        server.server_close()

if __name__ == '__main__':
    run()