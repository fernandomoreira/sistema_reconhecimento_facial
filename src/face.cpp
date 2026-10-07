// ===========================================================================
// face.cpp - animação e desenho do emoji que segue a pessoa
//
// Olhos, sobrancelhas e boca são pontos numa esfera (a cabeça). Cada ponto é
// dado por dois ângulos: az (para os lados) e el (para cima/baixo, positivo
// = para baixo). A esfera é girada por pitch, yaw e roll e projetada na tela;
// pontos que ficam "atrás" da cabeça não são desenhados.
//
// IMPORTANTE: pc/painel.html tem uma cópia em JavaScript deste desenho.
// ===========================================================================
#include "face.h"

#include <math.h>

#include "display.h"
#include "leds.h"

// ----- Tempos -----
static const uint32_t FRAME_MS  = 33;    // ~30 quadros/s (o I2C a 400 kHz aguenta ~35)
static const uint32_t LOST_MS   = 1500;  // sem LOOK nesse tempo = ninguém na câmera
static const uint32_t SEARCH_MS = 8000;  // procura por 8 s, depois dorme

// ----- Geometria do rosto (ângulos em radianos, tamanhos em fração do raio) -----
static const float EYE_AZ = 0.38f;   // distância dos olhos ao meio do rosto
static const float EYE_EL = -0.18f;  // altura dos olhos (negativo = acima do centro)
static const float EYE_RX = 0.17f;   // meia largura do olho
static const float EYE_RY = 0.22f;   // meia altura do olho
static const float VIS    = 0.08f;   // z mínimo para um ponto estar visível

static const float DEG = PI / 180.0f;

enum class St : uint8_t { DORMINDO, PROCURANDO, SEGUINDO };

struct Pose {
  float x, y;              // centro da cabeça na tela (pixels)
  float yaw, pitch, roll;  // radianos
  float r;                 // raio da cabeça (pixels)
};

static Pose cur = {64, 36, 0, -0.35f, 0.15f, 22};  // o que está na tela
static Pose tgt = cur;                             // para onde está indo

static St       st       = St::DORMINDO;
static uint32_t stSince  = 0;
static uint32_t lastLook = 0;
static uint8_t  mood     = MOOD_NEUTRO;
static char     name[FACE_NAME_MAX + 1] = "";

static uint32_t lastFrame = 0;
static uint32_t nextBlink = 3000;
static uint32_t frames = 0, fpsT0 = 0;
static uint8_t  fps = 0;
static int      ledMask = -1;  // LEDs acesos (bits), -1 = ainda não definido

// ===========================================================================
// Projeção 3D -> tela
// ===========================================================================
static Adafruit_SSD1306* g;
static float pcx, pcy, pr;                     // centro e raio na tela
static float cY, sY, cP, sP, cR, sR;           // senos/cossenos da pose

struct P {
  int16_t x, y;  // pixel na tela
  float   z;     // > 0 = virado para a frente
};

static P proj(float az, float el) {
  // Ponto na esfera de raio 1 (x direita, y baixo, z para fora da tela)
  float x = sinf(az) * cosf(el), y = sinf(el), z = cosf(az) * cosf(el);
  // pitch (eixo x): positivo leva o rosto para cima
  float y1 = y * cP - z * sP, z1 = y * sP + z * cP;
  // yaw (eixo y): positivo leva o rosto para a direita
  float x2 = x * cY + z1 * sY, z2 = -x * sY + z1 * cY;
  // roll (no plano da tela): positivo gira no sentido horário
  float x3 = x2 * cR - y1 * sR, y3 = x2 * sR + y1 * cR;
  return {(int16_t)lroundf(pcx + pr * x3), (int16_t)lroundf(pcy + pr * y3), z2};
}

static void line(P a, P b, bool thick) {
  if (a.z < VIS || b.z < VIS) return;
  g->drawLine(a.x, a.y, b.x, b.y, SSD1306_WHITE);
  if (thick) g->drawLine(a.x, a.y + 1, b.x, b.y + 1, SSD1306_WHITE);
}

// Curva sobre a esfera: u vai de -1 a 1, az = az0 + u*halfW,
// el = el0 + bend*(1-u²) + tilt*u  (bend > 0 = meio mais baixo, sorriso)
static void curve(float az0, float halfW, float el0, float bend, float tilt, bool thick) {
  const int N = 7;
  P prev = proj(az0 - halfW, el0 - tilt);
  for (int i = 1; i < N; i++) {
    float u = -1.0f + 2.0f * i / (N - 1);
    P p = proj(az0 + u * halfW, el0 + bend * (1 - u * u) + tilt * u);
    line(prev, p, thick);
    prev = p;
  }
}

static void fillEllipse(int16_t x0, int16_t y0, float rx, float ry, uint16_t color) {
  if (rx < 0.5f) rx = 0.5f;
  if (ry < 0.5f) ry = 0.5f;
  int h = (int)ceilf(ry);
  for (int dy = -h; dy <= h; dy++) {
    float t = 1.0f - (dy * dy) / (ry * ry);
    if (t < 0) continue;
    int w = (int)(rx * sqrtf(t) + 0.5f);
    g->drawFastHLine(x0 - w, y0 + dy, 2 * w + 1, color);
  }
}

// ===========================================================================
// Partes do rosto
// ===========================================================================
static void drawEye(float az, float open, uint8_t em) {
  if (st == St::DORMINDO || open < 0.25f) {   // fechado: um tracinho
    curve(az, 0.13f, EYE_EL + 0.02f, 0.03f, 0, true);
    return;
  }
  if (em == MOOD_FELIZ) {                      // feliz: olhos ^ ^
    curve(az, 0.14f, EYE_EL + 0.03f, -0.09f, 0, true);
    return;
  }
  P c = proj(az, EYE_EL);
  if (c.z < VIS) return;
  float s      = em == MOOD_SURPRESO ? 1.3f : 1.0f;
  float squash = constrain(c.z, 0.3f, 1.0f);   // olho de lado fica mais fino
  float rx = pr * EYE_RX * s * squash;
  float ry = pr * EYE_RY * s * open;
  if (em == MOOD_BRAVO) ry *= 0.75f;
  fillEllipse(c.x, c.y, rx, ry, SSD1306_WHITE);
  if (rx >= 2.5f && ry >= 3.0f)                // brilho no olho
    g->fillRect(c.x - (int)(rx * 0.45f), c.y - (int)(ry * 0.5f), 2, 2, SSD1306_BLACK);
}

static void drawBrows(uint8_t em) {
  for (int side = -1; side <= 1; side += 2) {
    float az = side * EYE_AZ;
    switch (em) {
      case MOOD_BRAVO:     // ponta de dentro mais baixa
        curve(az, 0.14f, EYE_EL - 0.30f, 0, -side * 0.07f, true);
        break;
      case MOOD_TRISTE:    // ponta de dentro mais alta
        curve(az, 0.14f, EYE_EL - 0.30f, 0, side * 0.07f, true);
        break;
      case MOOD_SURPRESO:  // as duas lá em cima
        curve(az, 0.14f, EYE_EL - 0.42f, -0.04f, 0, false);
        break;
      case MOOD_CURIOSO:   // só a da direita levantada
        if (side > 0) curve(az, 0.14f, EYE_EL - 0.38f, -0.05f, 0, false);
        else          curve(az, 0.13f, EYE_EL - 0.27f, 0, 0, false);
        break;
      default:
        break;
    }
  }
}

static void drawRing(float az, float el, float rx, float ry) {
  P c = proj(az, el);
  if (c.z < VIS) return;
  float squash = constrain(c.z, 0.3f, 1.0f);
  fillEllipse(c.x, c.y, pr * rx * squash, pr * ry, SSD1306_WHITE);
  fillEllipse(c.x, c.y, pr * rx * squash - 1.5f, pr * ry - 1.5f, SSD1306_BLACK);
}

static void drawMouth(uint8_t em) {
  if (st == St::DORMINDO) {          // roncando: boquinha "o"
    drawRing(0, 0.45f, 0.07f, 0.08f);
    return;
  }
  switch (em) {
    case MOOD_FELIZ: {               // sorriso aberto (preenchido)
      const int N = 7;
      P top[N], bot[N];
      for (int i = 0; i < N; i++) {
        float u = -1.0f + 2.0f * i / (N - 1);
        top[i] = proj(u * 0.32f, 0.32f);
        bot[i] = proj(u * 0.32f, 0.32f + 0.22f * (1 - u * u));
      }
      for (int i = 0; i < N - 1; i++) {
        if (top[i].z < VIS || top[i + 1].z < VIS) continue;
        g->fillTriangle(top[i].x, top[i].y, top[i + 1].x, top[i + 1].y, bot[i].x, bot[i].y, SSD1306_WHITE);
        g->fillTriangle(top[i + 1].x, top[i + 1].y, bot[i + 1].x, bot[i + 1].y, bot[i].x, bot[i].y, SSD1306_WHITE);
      }
      break;
    }
    case MOOD_SURPRESO: drawRing(0, 0.46f, 0.11f, 0.14f);              break;
    case MOOD_CURIOSO:  curve(0.06f, 0.15f, 0.44f, 0, -0.03f, true);   break;
    case MOOD_BRAVO:    curve(0, 0.22f, 0.48f, -0.07f, 0, true);       break;
    case MOOD_TRISTE:   curve(0, 0.24f, 0.50f, -0.10f, 0, true);       break;
    default:            curve(0, 0.26f, 0.42f, 0.05f, 0, true);        break;
  }
}

// ===========================================================================
// Animação
// ===========================================================================
static float eyeOpen(uint32_t now) {
  if (now >= nextBlink) {
    uint32_t d = now - nextBlink;
    if (d < 160) return fabsf((float)d - 80.0f) / 80.0f;  // fecha e abre em 160 ms
    nextBlink = now + random(2000, 5500);
  }
  return 1.0f;
}

static uint8_t effectiveMood() {
  if (st == St::SEGUINDO)   return mood;
  if (st == St::PROCURANDO) return MOOD_CURIOSO;
  return MOOD_NEUTRO;
}

static void updateTargets(uint32_t now) {
  if (st == St::SEGUINDO && now - lastLook > LOST_MS) {
    st = St::PROCURANDO;
    stSince = now;
    Serial.println("[ROSTO] Pessoa sumiu: procurando...");
  }
  if (st == St::PROCURANDO && now - stSince > SEARCH_MS) {
    st = St::DORMINDO;
    stSince = now;
    Serial.println("[ROSTO] Ninguem por perto: dormindo");
  }

  float t = now / 1000.0f;
  if (st == St::PROCURANDO) {        // olha para os lados
    tgt = {64, 32, 0.75f * sinf(t * 1.6f), 0.12f * sinf(t * 0.9f), 0, 22};
  } else if (st == St::DORMINDO) {   // cabeça baixa, "respirando"
    tgt = {64, 36, 0, -0.35f + 0.05f * sinf(t * 1.4f), 0.15f, 22};
  }
}

static void smooth(float dt) {
  float k = 1.0f - expf(-dt * (st == St::DORMINDO ? 2.5f : 12.0f));
  cur.x     += (tgt.x - cur.x) * k;
  cur.y     += (tgt.y - cur.y) * k;
  cur.yaw   += (tgt.yaw - cur.yaw) * k;
  cur.pitch += (tgt.pitch - cur.pitch) * k;
  cur.roll  += (tgt.roll - cur.roll) * k;
  cur.r     += (tgt.r - cur.r) * k;
}

// Amarelo (G5) = tem alguém na câmera, verde (G4) = pessoa cadastrada,
// vermelho (G15) = pessoa não reconhecida
static void updateLeds() {
  bool seen = st == St::SEGUINDO;
  int mask = (seen ? 1 : 0) | (seen && name[0] ? 2 : 0) | (seen && !name[0] ? 4 : 0);
  if (mask == ledMask) return;
  for (uint8_t i = 0; i < LED_COUNT; i++) {
    if (ledMask < 0 || ((mask ^ ledMask) & (1 << i))) ledSet(i, mask & (1 << i));
  }
  ledMask = mask;
}

static void draw(uint32_t now) {
  Adafruit_SSD1306& o = displayOled();
  g = &o;
  o.clearDisplay();

  pcx = cur.x;
  pcy = cur.y;
  pr  = cur.r;
  cY = cosf(cur.yaw);   sY = sinf(cur.yaw);
  cP = cosf(cur.pitch); sP = sinf(cur.pitch);
  cR = cosf(cur.roll);  sR = sinf(cur.roll);

  // Contorno da cabeça (anel de 2 pixels)
  int16_t cx = lroundf(cur.x), cy = lroundf(cur.y), r = lroundf(cur.r);
  o.fillCircle(cx, cy, r, SSD1306_WHITE);
  o.fillCircle(cx, cy, r - 2, SSD1306_BLACK);

  uint8_t em   = effectiveMood();
  float   open = eyeOpen(now);
  drawBrows(em);
  drawEye(-EYE_AZ, open, em);
  drawEye(EYE_AZ, open, em);
  drawMouth(em);

  o.setTextColor(SSD1306_WHITE);
  if (st == St::DORMINDO) {          // "z Z" subindo
    float p1 = (now % 2400) / 2400.0f;
    float p2 = fmodf(p1 + 0.5f, 1.0f);
    o.setTextSize(1);
    o.setCursor(cx + r * 0.7f + p1 * 6, cy - r * 0.6f - p1 * 10);
    o.print('z');
    o.setTextSize(2);
    o.setCursor(cx + r * 0.9f + p2 * 6, cy - r - p2 * 10);
    o.print('Z');
  } else if (st == St::PROCURANDO && (now / 600) % 2 == 0) {
    o.setTextSize(2);
    o.setCursor(cx + r + 4, cy - r);
    o.print('?');
  } else if (st == St::SEGUINDO && name[0]) {
    // Nome no canto de baixo, do lado oposto ao da cabeça
    int16_t w = strlen(name) * 6 - 1;
    int16_t x = cx >= SCREEN_W / 2 ? 1 : SCREEN_W - 1 - w;
    o.fillRect(x - 1, SCREEN_H - 9, w + 2, 9, SSD1306_BLACK);
    o.setTextSize(1);
    o.setCursor(x, SCREEN_H - 8);
    o.print(name);
  }
  o.display();
}

// ===========================================================================
// API
// ===========================================================================
void faceBegin() {
  randomSeed(esp_random());
  lastFrame = millis();
  updateLeds();
  if (displayIsOk()) draw(lastFrame);
}

void faceLook(int x, int y, int yaw, int pitch, int roll, int size, uint8_t m) {
  x     = constrain(x, -100, 100);
  y     = constrain(y, -100, 100);
  yaw   = constrain(yaw, -60, 60);
  pitch = constrain(pitch, -45, 45);
  roll  = constrain(roll, -45, 45);
  size  = constrain(size, 0, 100);

  tgt.r     = 18 + size * 0.10f;                        // raio de 18 a 28 pixels
  tgt.x     = SCREEN_W / 2 + x / 100.0f * (SCREEN_W / 2 - tgt.r - 2);
  tgt.y     = SCREEN_H / 2 + y / 100.0f * 8;
  tgt.yaw   = yaw * DEG;
  tgt.pitch = pitch * DEG;
  tgt.roll  = roll * DEG;
  mood      = m < MOOD_COUNT ? m : MOOD_NEUTRO;

  lastLook = millis();
  if (st != St::SEGUINDO) {
    st = St::SEGUINDO;
    stSince = lastLook;
    Serial.println("[ROSTO] Pessoa na camera: seguindo");
  }
}

void faceIdle() {
  if (st == St::SEGUINDO) {
    st = St::PROCURANDO;
    stSince = millis();
    Serial.println("[ROSTO] Pessoa sumiu: procurando...");
  }
}

void faceSetName(const char* n) {
  strncpy(name, n, FACE_NAME_MAX);
  name[FACE_NAME_MAX] = 0;
}

void faceLoop() {
  uint32_t now = millis();
  if (now - lastFrame < FRAME_MS) return;
  float dt = (now - lastFrame) / 1000.0f;
  if (dt > 0.1f) dt = 0.1f;
  lastFrame = now;

  updateTargets(now);
  smooth(dt);
  updateLeds();

  frames++;
  if (now - fpsT0 >= 1000) {
    fps    = frames;
    frames = 0;
    fpsT0  = now;
  }
  if (displayIsOk()) draw(now);
}

const char* faceState() {
  switch (st) {
    case St::SEGUINDO:   return "seguindo";
    case St::PROCURANDO: return "procurando";
    default:             return "dormindo";
  }
}

uint8_t faceMood() {
  return mood;
}

const char* faceName() {
  return name;
}

uint8_t faceFps() {
  return fps;
}
