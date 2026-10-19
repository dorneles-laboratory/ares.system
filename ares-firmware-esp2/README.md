ares-firmware-esp2/
├── include/                 # Arquivos de cabeçalho globais (.h)
│   ├── ares_config.h        # Configurações de rede, pinos e constantes
│   └── ares_protocol.h      # Definição das structs do Payload e Header
├── src/                     # Código-fonte principal em C++
│   ├── main.cpp             # Ponto de entrada (setup e loop)
│   ├── radio/               # Camada de rádio e ESP-NOW
│   │   └── esp_now_rx.cpp   # Funções de inicialização e recepção ESP-NOW
│   └── utils/               # Utilitários (geração de ID, timestamp, etc)
├── platformio.ini           # Arquivo de configuração e dependências do PlatformIO
└── README.md                # Documentação rápida do nó gateway receptor