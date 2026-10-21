#ifndef ROUTING_H
#define ROUTING_H

#include <stdint.h>

// Roteamento por gradiente de saltos até o Gateway:
// - quem alcança o ESP2 direto está a 1 salto;
// - os outros ficam a 1 + o menor valor entre os vizinhos vivos;
// - o próximo salto é o vizinho mais perto do Gateway (desempate: RSSI, bateria, vagas).

void routingInit(const uint8_t *gatewayMac, const uint8_t *selfMac);
const uint8_t *routingGatewayMac();
// O rádio do ESP2 confirmou um envio (dados ou teste de enlace)
void routingGatewayConfirmed(uint32_t now);
// Uma rodada inteira de envios ao ESP2 ficou sem confirmação
void routingGatewayFailed();
bool routingGatewayAlive(uint32_t now);
uint32_t routingGatewayLastConfirmed();
// Distância deste Router até o Gateway (HOPS_UNREACHABLE se não houver rota)
uint8_t routingMyHops(uint32_t now);
// Escolhe o próximo salto, nunca devolvendo para excludeMac (de quem a mensagem veio).
// Retorna false se não houver rota agora.
bool routingChooseNextHop(uint32_t now, const uint8_t *excludeMac, uint8_t outMac[6], bool *isGateway);

#endif
