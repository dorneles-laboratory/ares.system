#include <Arduino.h>
#include <Preferences.h>
#include "../../include/ares_config.h"
#include "../../include/node_clock.h"

// Preferences = biblioteca do ESP32 que grava pares chave/valor na partição NVS.
// A NVS já espalha as gravações pela flash (wear leveling), mas mesmo assim só
// salvamos o relógio a cada CLOCK_SAVE_INTERVAL_MS.
static Preferences prefs;
static uint32_t clockAtBoot = 0;     // Segundos de vida salvos antes deste boot
static uint32_t bootCount = 0;
static uint32_t sequenceInBoot = 0;  // Mensagens criadas neste boot
static unsigned long lastSaveAt = 0;

void nodeClockBegin() {
    prefs.begin("ares", false);       // "ares" = nome do espaço de chaves; false = leitura e escrita
    // O relógio salvo pode estar atrasado até CLOCK_SAVE_INTERVAL_MS (o tempo que ficou
    // ligado depois do último salvamento). Somamos esse intervalo inteiro para o relógio
    // NUNCA voltar para trás depois de um reinício (no teste, o BOOT aparecia no log
    // "antes" de eventos que aconteceram antes dele). O preço é adiantar no máximo 1 min.
    clockAtBoot = prefs.getUInt("clock", 0) + CLOCK_SAVE_INTERVAL_MS / 1000;
    bootCount = prefs.getUInt("boots", 0) + 1;
    prefs.putUInt("boots", bootCount);
    prefs.putUInt("clock", clockAtBoot);
    lastSaveAt = millis();
}

uint32_t nodeClockNow() {
    return clockAtBoot + (uint32_t) (millis() / 1000UL);
}

void nodeClockMaybeSave() {
    if (millis() - lastSaveAt < CLOCK_SAVE_INTERVAL_MS) return;
    lastSaveAt = millis();
    prefs.putUInt("clock", nodeClockNow());
}

// Sequência = boots x 65536 + contador do boot. Não precisa gravar na NVS a cada mensagem
// e continua crescendo entre reinícios (RF-NG-05: ordenar por sequência do nó, não por relógio).
uint32_t nodeNextSequence() {
    if (sequenceInBoot >= 65535) {
        // Mais de 65 mil mensagens num boot só: "gasta" um boot para não repetir números
        bootCount++;
        prefs.putUInt("boots", bootCount);
        sequenceInBoot = 0;
    }
    sequenceInBoot++;
    return (bootCount << 16) | sequenceInBoot;
}

uint32_t nodeBootCount() {
    return bootCount;
}
