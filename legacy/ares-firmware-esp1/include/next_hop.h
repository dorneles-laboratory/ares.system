#ifndef NEXT_HOP_H
#define NEXT_HOP_H

#include <stdint.h>

// ============================================================================
// Escolha do próximo salto do Storage.
// O Storage não repassa mensagens de outros em direção ao Gateway (isso é papel do
// Router); ele só precisa saber para QUEM entregar o que guardou:
//  - o Gateway, se ele estiver ouvindo (beacon com 0 saltos), ou
//  - o Router mais perto do Gateway (menos saltos), com desempate por sinal (RSSI)
//    e depois por bateria.
// Se nenhum vizinho foi descoberto e o ares_credentials.h tiver FIXED_NEXT_HOP_MAC,
// usa esse MAC fixo (útil para teste de bancada).
// ============================================================================

// excludeMac: não devolve a mensagem para quem a entregou (pode ser nullptr).
// Retorna false se não há ninguém para entregar agora.
bool nextHopChoose(const uint8_t *excludeMac, uint8_t outMac[6], bool *isGateway);

#endif
