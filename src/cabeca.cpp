// ===========================================================================
// cabeca.cpp - controle dos 2 motores 28BYJ-48 (meio passo, com aceleração)
//
// O 28BYJ-48 tem redução de ~64:1: em meio passo são 4096 passos por volta
// (~11,4 passos por grau). Cada motor tem um "alvo" (em passos); a tarefa
// dos motores acelera até a velocidade máxima, freia perto do alvo e, depois
// de um tempo parado, desliga as bobinas (o motor esfria e para de gastar
// energia; a caixa de redução segura a cabeça no lugar).
// ===========================================================================
#include "cabeca.h"

#include <Preferences.h>
#include <math.h>

#include "face.h"

static const uint8_t  PINOS[2][4]  = {{32, 33, 25, 26},   // motor X (IN1..IN4)
                                      {16, 17, 18, 19}};  // motor Y
static const float    PASSOS_VOLTA = 4096.0f;   // meios-passos por volta
static const float    PASSOS_GRAU  = PASSOS_VOLTA / 360.0f;
static const float    ACEL         = 2500.0f;   // meios-passos/s²
static const uint32_t SOLTAR_MS    = 1500;      // parado esse tempo: desliga as bobinas
static const int32_t  ZONA_MORTA   = 4;         // ignora mudanças de alvo menores (~0,35°)
static const uint32_t ALVO_MS      = 20;        // atualiza o alvo a cada 20 ms

// Faixas do emoji (as mesmas do protocolo): yaw ±60°, pitch ±45°
static const float EMOJI_YAW = 60.0f, EMOJI_PITCH = 45.0f;

// Meio passo: 8 combinações das 4 bobinas
static const uint8_t SEQ[8] = {0b0001, 0b0011, 0b0010, 0b0110, 0b0100, 0b1100, 0b1000, 0b1001};

struct Motor {
  volatile int32_t alvo = 0;      // passos (escrito no núcleo 1)
  volatile int32_t pos  = 0;      // passos (escrito pela tarefa)
  volatile bool    andando = false;
  float    vel = 0;               // passos/s, com sinal
  float    frac = 0;              // fração de passo acumulada
  uint8_t  fase = 0;              // índice em SEQ
  bool     ligado = false;        // bobinas energizadas?
  uint32_t paradoDesde = 0;
};

static Motor     motores[2];
static CabecaCfg cfg = {false, 60, 30, false, false, 600};
static Preferences prefs;
static uint32_t  ultimoAlvo = 0;

// ---------------------------------------------------------------------------
// Bobinas
// ---------------------------------------------------------------------------
static void escrever(uint8_t m, uint8_t bits) {
  for (uint8_t i = 0; i < 4; i++) digitalWrite(PINOS[m][i], (bits >> i) & 1);
}

static void passo(uint8_t m, int8_t dir) {
  Motor& mt = motores[m];
  bool inv = m == 0 ? cfg.invX : cfg.invY;
  mt.fase = (mt.fase + ((dir > 0) != inv ? 1 : 7)) & 7;
  escrever(m, SEQ[mt.fase]);
  mt.ligado = true;
  mt.pos = mt.pos + dir;
}

// Um ciclo do motor: dt em segundos
static void atualizar(uint8_t m, float dt, uint32_t agora) {
  Motor& mt = motores[m];
  int32_t dist = mt.alvo - mt.pos;

  if (dist == 0 && fabsf(mt.vel) < 50.0f) {   // chegou
    mt.vel = 0;
    mt.frac = 0;
    if (mt.andando) { mt.andando = false; mt.paradoDesde = agora; }
    if (mt.ligado && agora - mt.paradoDesde > SOLTAR_MS) {
      escrever(m, 0);
      mt.ligado = false;
    }
    return;
  }
  if (!mt.ligado) escrever(m, SEQ[mt.fase]);   // reenergiza na fase em que parou
  mt.andando = true;

  // Velocidade que ainda dá para frear até o alvo: v² = 2·a·d
  float vmax = cfg.vel;
  float quer = dist == 0 ? 0 : copysignf(fminf(vmax, sqrtf(2.0f * ACEL * fabsf((float)dist))), (float)dist);
  float dv = ACEL * dt;
  if (mt.vel < quer) mt.vel = fminf(mt.vel + dv, quer);
  else               mt.vel = fmaxf(mt.vel - dv, quer);

  mt.frac += mt.vel * dt;
  for (uint8_t n = 0; n < 3 && fabsf(mt.frac) >= 1.0f; n++) {   // no máx. 3 passos por ciclo
    int8_t dir = mt.frac > 0 ? 1 : -1;
    passo(m, dir);
    mt.frac -= dir;
  }
}

static void tarefa(void*) {
  uint32_t antes = micros();
  for (;;) {
    vTaskDelay(1);   // 1 ms (o FreeRTOS do ESP32 roda a 1000 Hz)
    uint32_t agora = micros();
    float dt = (agora - antes) / 1e6f;
    antes = agora;
    if (dt > 0.01f) dt = 0.01f;
    uint32_t ms = millis();
    atualizar(0, dt, ms);
    atualizar(1, dt, ms);
  }
}

// ---------------------------------------------------------------------------
// Alvos
// ---------------------------------------------------------------------------
static int32_t graus(float g, uint8_t lim) {
  g = constrain(g, -(float)lim, (float)lim);
  return lroundf(g * PASSOS_GRAU);
}

static void mirar(uint8_t m, int32_t alvo) {
  if (abs(alvo - motores[m].alvo) >= ZONA_MORTA || alvo == 0) motores[m].alvo = alvo;
}

static void salvar() {
  prefs.putBool("on", cfg.on);
  prefs.putUChar("limX", cfg.limX);
  prefs.putUChar("limY", cfg.limY);
  prefs.putBool("invX", cfg.invX);
  prefs.putBool("invY", cfg.invY);
  prefs.putUShort("vel", cfg.vel);
}

// ===========================================================================
// API
// ===========================================================================
void cabecaBegin() {
  for (uint8_t m = 0; m < 2; m++) {
    for (uint8_t i = 0; i < 4; i++) {
      digitalWrite(PINOS[m][i], LOW);
      pinMode(PINOS[m][i], OUTPUT);
    }
  }
  prefs.begin("cabeca", false);
  cfg.on   = prefs.getBool("on", false);
  cfg.limX = constrain(prefs.getUChar("limX", 60), 5, 180);
  cfg.limY = constrain(prefs.getUChar("limY", 30), 5, 90);
  cfg.invX = prefs.getBool("invX", false);
  cfg.invY = prefs.getBool("invY", false);
  cfg.vel  = constrain(prefs.getUShort("vel", 600), 100, 1000);
  Serial.printf("[CABECA] Seguir: %s, X +-%u graus, Y +-%u graus, %u passos/s\n",
                cfg.on ? "sim" : "nao", cfg.limX, cfg.limY, cfg.vel);

  // Núcleo 0: o loop() (OLED, serial) fica no núcleo 1
  xTaskCreatePinnedToCore(tarefa, "cabeca", 3072, nullptr, 2, nullptr, 0);
}

void cabecaLoop() {
  uint32_t agora = millis();
  if (!cfg.on || agora - ultimoAlvo < ALVO_MS) return;
  ultimoAlvo = agora;
  float yaw, pitch;
  faceHeadPose(yaw, pitch);
  mirar(0, graus(yaw / EMOJI_YAW * cfg.limX, cfg.limX));
  mirar(1, graus(pitch / EMOJI_PITCH * cfg.limY, cfg.limY));
}

void cabecaLigar(bool on) {
  if (on == cfg.on) return;
  cfg.on = on;
  if (!on) {   // volta ao centro (depois as bobinas são soltas sozinhas)
    motores[0].alvo = 0;
    motores[1].alvo = 0;
  }
  prefs.putBool("on", on);
  Serial.printf("[CABECA] Seguir o emoji: %s\n", on ? "sim" : "nao");
}

bool cabecaMover(float dx, float dy) {
  if (cfg.on) return false;
  motores[0].alvo = motores[0].alvo + lroundf(dx * PASSOS_GRAU);
  motores[1].alvo = motores[1].alvo + lroundf(dy * PASSOS_GRAU);
  return true;
}

void cabecaZerar() {
  // Para onde está (o alvo vira a posição) e chama isso de centro. A tarefa
  // só mexe em pos dando passos; trocar os dois juntos com o motor parado é seguro
  for (uint8_t m = 0; m < 2; m++) {
    Motor& mt = motores[m];
    int32_t aqui = mt.pos;
    mt.alvo = aqui;
    while (mt.andando) delay(1);   // espera frear
    mt.alvo = 0;
    mt.pos  = 0;
  }
  Serial.println("[CABECA] Posicao atual agora e o centro");
}

void cabecaConfigurar(const CabecaCfg& c) {
  cfg.limX = constrain(c.limX, 5, 180);
  cfg.limY = constrain(c.limY, 5, 90);
  cfg.invX = c.invX;
  cfg.invY = c.invY;
  cfg.vel  = constrain(c.vel, 100, 1000);
  salvar();
  Serial.printf("[CABECA] Config: X +-%u, Y +-%u, inverter %d/%d, %u passos/s\n",
                cfg.limX, cfg.limY, cfg.invX, cfg.invY, cfg.vel);
}

const CabecaCfg& cabecaCfg() {
  return cfg;
}

float cabecaAngulo(uint8_t eixo) {
  return eixo < 2 ? motores[eixo].pos / PASSOS_GRAU : 0;
}

float cabecaAlvo(uint8_t eixo) {
  return eixo < 2 ? motores[eixo].alvo / PASSOS_GRAU : 0;
}

bool cabecaAndando() {
  return motores[0].andando || motores[1].andando;
}
