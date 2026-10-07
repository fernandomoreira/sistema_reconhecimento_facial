// ===========================================================================
// leds.h - os 3 LEDs: amarelo = alguém na câmera, verde = conhecido, vermelho = desconhecido
// Os LEDs são identificados pelo índice 0, 1 e 2 (na API: id 1, 2 e 3).
// ===========================================================================
#pragma once
#include <Arduino.h>

constexpr uint8_t LED_COUNT = 3;

// Configura os pinos e garante que todos os LEDs começam apagados
void ledsBegin();

// Liga (true) ou desliga (false) o LED de índice 0..2
void ledSet(uint8_t index, bool on);

// Retorna o estado atual do LED de índice 0..2
bool ledGet(uint8_t index);

// Retorna o número do GPIO do LED (ex.: 5, 4, 15)
uint8_t ledPin(uint8_t index);
