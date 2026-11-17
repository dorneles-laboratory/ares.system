#ifndef NODE_CLOCK_H
#define NODE_CLOCK_H

#include <stdint.h>

// ============================================================================
// Relógio e numeração do nó, guardados na NVS (memória de configuração da flash).
//
// Por que existe: o ESP32 não tem relógio com bateria e o millis() volta a zero a cada
// reinício. Sem GNSS nem internet, o Storage conta o próprio tempo de vida em segundos e
// salva na NVS de tempos em tempos. Assim:
//  - a validade de 7 dias continua contando depois de um reinício (a wiki manda contar
//    a partir da recepção no nó, não pelo relógio de quem enviou);
//  - o número de sequência das mensagens nunca repete (o antigo ESP1 voltava a
//    "msg-uuid-1" a cada reinício e o Router achava que era duplicata).
// ============================================================================

// Carrega da NVS. Chamar uma vez no setup().
void nodeClockBegin();
// Segundos de vida do nó, somando todos os boots (nunca volta para trás)
uint32_t nodeClockNow();
// Salva o relógio na NVS se já passou CLOCK_SAVE_INTERVAL_MS (chamar no loop)
void nodeClockMaybeSave();
// Próximo número de sequência de mensagem: sempre cresce, mesmo depois de reiniciar
uint32_t nodeNextSequence();
uint32_t nodeBootCount();

#endif
