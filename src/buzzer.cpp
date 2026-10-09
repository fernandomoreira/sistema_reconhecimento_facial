// ===========================================================================
// buzzer.cpp - toca os sons do buzzer nota a nota, sem delay()
//
// O som sai por PWM (LEDC). O volume é a largura do pulso (duty): 50% é o mais
// alto que um buzzer passivo consegue; pulsos mais estreitos soam mais baixo.
// ===========================================================================
#include "buzzer.h"

static const uint8_t PIN = 23;
static const uint8_t CANAL = 0;     // canal LEDC (os LEDs e o OLED não usam LEDC)
static const uint8_t BITS = 10;     // resolução do duty: 0..1023
static const uint32_t FREQ_ATIVO = 20000;  // buzzer ativo: PWM rápido só para dosar o volume

// Buzzer PASSIVO (só um disco, precisa de frequência): false -> toca as notas.
// Buzzer ATIVO (tem oscilador dentro, apita sozinho com 3,3 V): true -> liga o
// pino direto (com volume); a frequência das notas é ignorada.
// O buzzer da placa do usuário é PASSIVO (as notas mudam de tom).
static const bool BUZZER_ATIVO = false;

struct Nota { uint16_t freq; uint16_t ms; };  // freq 0 = silêncio

// Notas entre 1500 e 2700 Hz: nessa faixa o buzzer testado soou mais alto
static const Nota SOM_PESSOA[] = {{2000, 90}};
static const Nota SOM_TIQUE[]  = {{2300, 30}};
static const Nota SOM_FIM[]    = {{1500, 120}, {0, 40}, {2000, 120}, {0, 40}, {2700, 250}};

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
static uint8_t    volume = 100;     // 0..100 (0 = mudo)
static bool       direto = false;   // nota de teste "TONE 0": pino ligado direto

static void calar() {
  ledcWrite(CANAL, 0);
}

// Liga o som numa frequência (0 = silêncio)
static void soar(uint16_t freq) {
  if (direto) {                      // teste: 3,3 V contínuo (o que um buzzer ativo espera)
    ledcWriteTone(CANAL, FREQ_ATIVO);
    ledcWrite(CANAL, (1 << BITS));
    return;
  }
  if (!freq || !volume) { calar(); return; }
  uint32_t duty;
  if (BUZZER_ATIVO) {
    ledcWriteTone(CANAL, FREQ_ATIVO);
    duty = (uint32_t)(1 << BITS) * volume / 100;
  } else {
    ledcWriteTone(CANAL, freq);
    // O ouvido percebe o volume mais ou menos pelo quadrado do duty
    duty = (uint32_t)(1 << (BITS - 1)) * volume * volume / 10000;
  }
  ledcWrite(CANAL, duty ? duty : 1);
}

static void comecarNota() {
  const Nota& nt = atual->notas[nota];
  soar(nt.freq);
  fimNota = millis() + nt.ms;
}

void buzzerBegin() {
  digitalWrite(PIN, LOW);
  pinMode(PIN, OUTPUT);
  ledcSetup(CANAL, 2000, BITS);
  ledcAttachPin(PIN, CANAL);
  calar();
}

void buzzerPlay(uint8_t tipo) {
  if (tipo == 0 || tipo > BEEP_MAX || !volume) return;
  direto = false;
  atual = &SONS[tipo];
  nota = 0;
  comecarNota();
}

void buzzerTone(uint16_t freq, uint16_t ms) {
  static Nota avulsa;
  static Som som = {&avulsa, 1};
  direto = freq == 0;
  avulsa = {freq, ms};
  atual = &som;
  nota = 0;
  comecarNota();
}

void buzzerSetVolume(uint8_t v) {
  volume = v > 100 ? 100 : v;
  if (!volume) {   // mudo: corta o que estiver tocando
    atual = nullptr;
    direto = false;
    calar();
  }
}

uint8_t buzzerVolume() {
  return volume;
}

void buzzerLoop() {
  if (!atual || (int32_t)(millis() - fimNota) < 0) return;
  if (++nota < atual->n) {
    comecarNota();
  } else {
    direto = false;
    calar();
    atual = nullptr;
  }
}
