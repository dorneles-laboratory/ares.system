#!/usr/bin/env python3
"""Confere se os arquivos que precisam ser IGUAIS nos três firmwares estão iguais.

Uso (na raiz do repositório):  python tools/verificar_protocolo.py

Por que existe: o ESP1 (Storage), o ESP2 (Gateway) e o ESP3 (Router) são projetos
separados, cada um com a sua cópia do protocolo. Se alguém muda um campo em um só,
os pacotes deixam de bater (tamanho diferente = descartado) e ninguém entende o porquê.
"""
import hashlib
import pathlib
import sys

RAIZ = pathlib.Path(__file__).resolve().parent.parent
FIRMWARES = ["ares-firmware-esp1", "ares-firmware-esp2", "ares-firmware-esp3"]

# Arquivo -> em quais firmwares ele precisa estar idêntico
COMPARTILHADOS = {
    "include/ares_protocol.h": FIRMWARES,
    "include/ares_control_protocol.h": FIRMWARES,
    "include/esp_now_radio.h": FIRMWARES,
    "src/radio/esp_now_radio.cpp": FIRMWARES,
    "include/dedup_cache.h": FIRMWARES,
    "src/storage/dedup_cache.cpp": FIRMWARES,
    "include/neighbor_table.h": ["ares-firmware-esp1", "ares-firmware-esp3"],
    "src/routing/neighbor_table.cpp": ["ares-firmware-esp1", "ares-firmware-esp3"],
    "include/flash_store.h": ["ares-firmware-esp1", "ares-firmware-esp2"],
    "src/storage/flash_store.cpp": ["ares-firmware-esp1", "ares-firmware-esp2"],
    "include/event_log.h": ["ares-firmware-esp1", "ares-firmware-esp2"],
    "src/storage/event_log.cpp": ["ares-firmware-esp1", "ares-firmware-esp2"],
    "include/node_clock.h": ["ares-firmware-esp1", "ares-firmware-esp2"],
    "src/storage/node_clock.cpp": ["ares-firmware-esp1", "ares-firmware-esp2"],
}


def resumo(caminho: pathlib.Path) -> str:
    # Ignora a diferença de final de linha (Windows CRLF x Linux LF)
    return hashlib.sha256(caminho.read_bytes().replace(b"\r\n", b"\n")).hexdigest()[:12]


def main() -> int:
    erros = 0
    for arquivo, firmwares in COMPARTILHADOS.items():
        resumos = {}
        for fw in firmwares:
            caminho = RAIZ / fw / arquivo
            if not caminho.exists():
                print(f"[FALTA]   {fw}/{arquivo}")
                erros += 1
                continue
            resumos[fw] = resumo(caminho)
        if len(set(resumos.values())) > 1:
            erros += 1
            print(f"[DIFERENTE] {arquivo}")
            for fw, h in resumos.items():
                print(f"            {h}  {fw}")
        elif resumos:
            print(f"[OK]      {arquivo}")
    if erros:
        print(f"\n{erros} problema(s). Copie a versão certa para os outros firmwares.")
        return 1
    print("\nTudo igual nos firmwares.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
