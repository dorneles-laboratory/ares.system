#!/usr/bin/env python3
"""Gera as versões para o Arduino IDE dos três firmwares (que são projetos PlatformIO).

Uso (na raiz do repositório):  python tools/gerar_sketches_arduino.py
Resultado: pasta "arduino-ide/" com um sketch por ESP. Abra o .ino no Arduino IDE.

Por que precisa: o Arduino IDE exige que o sketch seja uma pasta com um .ino do mesmo
nome e não enxerga a pasta include/ do PlatformIO. O script copia tudo para uma pasta
só, transforma o main.cpp no .ino e corrige os #include "../../include/x.h" -> "x.h".
A pasta arduino-ide/ é gerada: edite sempre os arquivos do PlatformIO e gere de novo.
"""
import pathlib
import re
import shutil

RAIZ = pathlib.Path(__file__).resolve().parent.parent
SAIDA = RAIZ / "arduino-ide"
SKETCHES = {
    "ares-firmware-esp1": "ares_esp1_storage",
    "ares-firmware-esp2": "ares_esp2_gateway",
    "ares-firmware-esp3": "ares_esp3_router",
}
INCLUDE_RELATIVO = re.compile(r'#include\s+"(?:\.\./)+include/([^"]+)"')


def gerar(projeto: str, nome: str) -> None:
    origem = RAIZ / projeto
    destino = SAIDA / nome
    if destino.exists():
        shutil.rmtree(destino)
    destino.mkdir(parents=True)

    for h in sorted((origem / "include").glob("*.h")):
        shutil.copy(h, destino / h.name)
    # Credenciais: usa o seu arquivo se existir; senão, o modelo (preencha antes de gravar)
    credenciais = origem / "include" / "ares_credentials.h"
    modelo = origem / "include" / "ares_credentials.h.example"
    if not credenciais.exists() and modelo.exists():
        shutil.copy(modelo, destino / "ares_credentials.h")

    for cpp in sorted((origem / "src").rglob("*.cpp")):
        texto = INCLUDE_RELATIVO.sub(r'#include "\1"', cpp.read_text(encoding="utf-8"))
        alvo = destino / (f"{nome}.ino" if cpp.name == "main.cpp" and cpp.parent.name == "src" else cpp.name)
        if alvo.exists():
            raise SystemExit(f"Dois arquivos com o mesmo nome em {projeto}: {cpp.name}")
        alvo.write_text(texto, encoding="utf-8")
    print(f"[OK] {destino.relative_to(RAIZ)}")


def main() -> None:
    for projeto, nome in SKETCHES.items():
        gerar(projeto, nome)
    print("\nNo Arduino IDE: Placa 'ESP32 Dev Module', Partition Scheme 'Default 4MB with spiffs'.")
    print("Se a placa reiniciar com 'partition table error': Flash Mode DIO e Flash Frequency 40MHz.")
    print("O ESP2 precisa da biblioteca ArduinoJson (Ferramentas > Gerenciar Bibliotecas).")


if __name__ == "__main__":
    main()
