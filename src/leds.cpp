// ===========================================================================
// leds.cpp - controle dos 3 LEDs
// ===========================================================================
#include "leds.h"

// Pinos de cada LED: amarelo (alguém na câmera), verde (conhecido), vermelho (desconhecido).
// ATENÇÃO: GPIO 5 e GPIO 15 são "strapping pins" (lidos pelo chip no
// instante do boot). Com o LED ligado do pino para o GND, via resistor, o
// boot NÃO é afetado: quem define o modo de boot é o GPIO 0 (e o GPIO 2).
// O GPIO 15 apenas controla se as mensagens de boot da ROM aparecem na
// serial. Detalhes no README.
static const uint8_t PINS[LED_COUNT] = {5, 4, 15};

// Estado guardado em memória (é a "fonte da verdade" para o dashboard)
static bool states[LED_COUNT] = {false, false, false};

void ledsBegin() {
  for (uint8_t i = 0; i < LED_COUNT; i++) {
    // Escreve LOW antes de virar saída: o pino já nasce em nível baixo,
    // sem "piscar" o LED. (Durante o boot, antes do nosso código rodar,
    // os GPIO 5 e 15 podem dar um breve pulso - isso é normal.)
    digitalWrite(PINS[i], LOW);
    pinMode(PINS[i], OUTPUT);
    states[i] = false;
  }
}

void ledSet(uint8_t index, bool on) {
  if (index >= LED_COUNT) return;  // índice inválido: ignora
  states[index] = on;
  digitalWrite(PINS[index], on ? HIGH : LOW);
  Serial.printf("[LED] LED %u (G%u) -> %s\n", index + 1, PINS[index], on ? "LIGADO" : "DESLIGADO");
}

bool ledGet(uint8_t index) {
  return index < LED_COUNT ? states[index] : false;
}

uint8_t ledPin(uint8_t index) {
  return index < LED_COUNT ? PINS[index] : 0;
}
