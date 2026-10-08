// ===========================================================================
// buzzer.cpp - toca os sons do buzzer nota a nota, sem delay()
// ===========================================================================
#include "buzzer.h"

static const uint8_t PIN = 23;

// Buzzer PASSIVO (só um disco, precisa de frequência): false -> usa tone().
// Buzzer ATIVO (tem oscilador dentro, apita só com 3,3 V): troque para true
// se o som sair rouco/fraco; aí a frequência das notas é ignorada.
static const bool BUZZER_ATIVO = false;

struct Nota { uint16_t freq; uint16_t ms; };  // freq 0 = silêncio

static const Nota SOM_PESSOA[] = {{2600, 70}};
static const Nota SOM_TIQUE[]  = {{3200, 25}};
static const Nota SOM_FIM[]    = {{1800, 110}, {0, 40}, {2400, 110}, {0, 40}, {3200, 220}};

struct Som { const Nota* notas; uint8_t n; };
static const Som SONS[BEEP_MAX + 1] = {
    {nullptr, 0},
    {SOM_PESSOA, sizeof(SOM_PESSOA) / sizeof(Nota)},
    {SOM_TIQUE, sizeof(SOM_TIQUE) / sizeof(Nota)},
    {SOM_FIM, sizeof(SOM_FIM) / sizeof(Nota)},
};

static const Som* atual = nullptr;  // som tocando agora (nullptr = calado)
static uint8_t    nota  = 0;        // índice da nota atual
static uint32_t   fimNota = 0;      // millis() em que a nota atual acaba

static void soar(uint16_t freq) {
  if (BUZZER_ATIVO) {
    digitalWrite(PIN, freq ? HIGH : LOW);
  } else if (freq) {
    tone(PIN, freq);
  } else {
    noTone(PIN);
    digitalWrite(PIN, LOW);
  }
}

static void comecarNota() {
  const Nota& nt = atual->notas[nota];
  soar(nt.freq);
  fimNota = millis() + nt.ms;
}

void buzzerBegin() {
  digitalWrite(PIN, LOW);
  pinMode(PIN, OUTPUT);
}

void buzzerPlay(uint8_t tipo) {
  if (tipo == 0 || tipo > BEEP_MAX) return;
  atual = &SONS[tipo];
  nota = 0;
  comecarNota();
}

void buzzerTone(uint16_t freq, uint16_t ms) {
  static Nota avulsa;
  static Som som = {&avulsa, 1};
  avulsa = {freq, ms};
  atual = &som;
  nota = 0;
  comecarNota();
}

void buzzerLoop() {
  if (!atual || (int32_t)(millis() - fimNota) < 0) return;
  if (++nota < atual->n) {
    comecarNota();
  } else {
    soar(0);
    atual = nullptr;
  }
}
