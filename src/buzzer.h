// ===========================================================================
// buzzer.h - beeps no GPIO 23, sem travar o loop (o emoji continua animando)
//
// Os sons vêm do PC (comando BEEP da serial):
//   BEEP_PESSOA   1 beep curto: alguém apareceu na câmera
//   BEEP_TIQUE    beep bem curtinho, repetido enquanto a captura automática tira as fotos
//   BEEP_FIM      sinal de 3 notas subindo: captura automática terminou e salvou a pessoa
// ===========================================================================
#pragma once
#include <Arduino.h>

enum BeepTipo : uint8_t { BEEP_PESSOA = 1, BEEP_TIQUE = 2, BEEP_FIM = 3, BEEP_MAX = 3 };

// Configura o pino e garante o buzzer calado
void buzzerBegin();

// Começa a tocar um som (interrompe o que estiver tocando)
void buzzerPlay(uint8_t tipo);

// Toca um tom avulso (freq em Hz, duração em ms): para testar o buzzer.
// freq 0 = liga o pino direto (3,3 V), o jeito certo de tocar um buzzer ativo
void buzzerTone(uint16_t freq, uint16_t ms);

// Volume 0..100 (0 = mudo: os sons são ignorados)
void buzzerSetVolume(uint8_t v);
uint8_t buzzerVolume();

// Deve ser chamada sempre no loop(): avança as notas do som em andamento
void buzzerLoop();
