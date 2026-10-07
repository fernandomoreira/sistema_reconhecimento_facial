// ===========================================================================
// face.cpp - animação e desenho dos emojis que seguem as pessoas
//
// Olhos, sobrancelhas e boca são pontos numa esfera (a cabeça). Cada ponto é
// dado por dois ângulos: az (para os lados) e el (para cima/baixo, positivo
// = para baixo). A esfera é girada por pitch, yaw e roll e projetada na tela;
// pontos que ficam "atrás" da cabeça não são desenhados.
//
// Cada pessoa tem o seu "slot" (posição na tela, suavização e piscada
// próprias). Com mais de uma pessoa a tela é dividida em células: uma fila
// para 2 a 4 pessoas, duas filas para 5 a 8.
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

struct Slot {
  Pose     cur, tgt;   // o que está na tela / para onde está indo
  uint8_t  mood;
  bool     known;
  uint32_t nextBlink;
};

// Célula da tela onde fica cada emoji quando há mais de um
struct Cell {
  float cx, cy;   // centro
  float w;        // largura (para caber o nome)
  float rmax;     // maior raio de cabeça que cabe
  bool  label;    // tem espaço para o nome embaixo?
};

static Slot     slots[FACE_MAX];
static uint8_t  count = 1;  // emojis na tela (dormindo/procurando = 1)
static char     names[FACE_MAX][FACE_NAME_MAX + 1];

static St       st       = St::DORMINDO;
static uint32_t stSince  = 0;
static uint32_t lastLook = 0;

static uint32_t lastFrame = 0;
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
// Layout: onde fica o emoji i quando há n na tela (n >= 2)
// ===========================================================================
static Cell cellFor(uint8_t n, uint8_t i) {
  uint8_t rows  = n <= 4 ? 1 : 2;
  uint8_t cols  = rows == 1 ? n : (n + 1) / 2;
  uint8_t row   = i / cols, col = i % cols;
  uint8_t inRow = row == 0 ? min(n, cols) : n - cols;  // a 2ª fila pode ter menos
  float   cw    = (float)SCREEN_W / cols;
  float   ch    = (float)SCREEN_H / rows;
  float   x0    = (SCREEN_W - inRow * cw) / 2;         // centraliza a fila
  Cell c;
  c.cx = x0 + cw * (col + 0.5f);
  c.w  = cw;
  if (rows == 1) {   // uma fila: cabeça um pouco para cima, nome embaixo
    c.cy    = 28;
    c.rmax  = min(cw / 2 - 1, 22.0f);
    c.label = true;
  } else {
    c.cy    = ch * (row + 0.5f);
    c.rmax  = min(cw, ch) / 2 - 1;
    c.label = false;
  }
  return c;
}

// Converte o que o PC mandou em posição/tamanho na tela
static Pose targetFor(const FaceIn& f, uint8_t n, uint8_t i) {
  Pose t;
  t.yaw   = f.yaw * DEG;
  t.pitch = f.pitch * DEG;
  t.roll  = f.roll * DEG;
  if (n == 1) {      // sozinho: anda pela tela inteira
    t.r = 18 + f.size * 0.10f;                         // raio de 18 a 28 pixels
    t.x = SCREEN_W / 2 + f.x / 100.0f * (SCREEN_W / 2 - t.r - 2);
    t.y = SCREEN_H / 2 + f.y / 100.0f * 8;
  } else {           // vários: cada um na sua célula
    Cell c = cellFor(n, i);
    t.r = c.rmax * (0.75f + 0.25f * f.size / 100.0f);
    t.x = c.cx;
    t.y = c.cy;
  }
  return t;
}

// ===========================================================================
// Animação
// ===========================================================================
static float eyeOpen(Slot& s, uint32_t now) {
  if (now >= s.nextBlink) {
    uint32_t d = now - s.nextBlink;
    if (d < 160) return fabsf((float)d - 80.0f) / 80.0f;  // fecha e abre em 160 ms
    s.nextBlink = now + random(2000, 5500);
  }
  return 1.0f;
}

static uint8_t effectiveMood(const Slot& s) {
  if (st == St::SEGUINDO)   return s.mood;
  if (st == St::PROCURANDO) return MOOD_CURIOSO;
  return MOOD_NEUTRO;
}

static void lose(uint32_t now) {
  st      = St::PROCURANDO;
  stSince = now;
  count   = 1;   // procura/dorme com um emoji só
  Serial.println("[ROSTO] Ninguem na camera: procurando...");
}

static void updateTargets(uint32_t now) {
  if (st == St::SEGUINDO && now - lastLook > LOST_MS) lose(now);
  if (st == St::PROCURANDO && now - stSince > SEARCH_MS) {
    st = St::DORMINDO;
    stSince = now;
    Serial.println("[ROSTO] Ninguem por perto: dormindo");
  }

  float t = now / 1000.0f;
  if (st == St::PROCURANDO) {        // olha para os lados
    slots[0].tgt = {64, 32, 0.75f * sinf(t * 1.6f), 0.12f * sinf(t * 0.9f), 0, 22};
  } else if (st == St::DORMINDO) {   // cabeça baixa, "respirando"
    slots[0].tgt = {64, 36, 0, -0.35f + 0.05f * sinf(t * 1.4f), 0.15f, 22};
  }
}

static void smooth(Slot& s, float dt) {
  float k = 1.0f - expf(-dt * (st == St::DORMINDO ? 2.5f : 12.0f));
  s.cur.x     += (s.tgt.x - s.cur.x) * k;
  s.cur.y     += (s.tgt.y - s.cur.y) * k;
  s.cur.yaw   += (s.tgt.yaw - s.cur.yaw) * k;
  s.cur.pitch += (s.tgt.pitch - s.cur.pitch) * k;
  s.cur.roll  += (s.tgt.roll - s.cur.roll) * k;
  s.cur.r     += (s.tgt.r - s.cur.r) * k;
}

// Amarelo (G5) = tem alguém na câmera, verde (G4) = alguma pessoa cadastrada,
// vermelho (G15) = alguma pessoa não reconhecida
static void updateLeds() {
  bool seen = st == St::SEGUINDO, known = false, unknown = false;
  for (uint8_t i = 0; seen && i < count; i++) {
    if (slots[i].known) known = true;
    else                unknown = true;
  }
  int mask = (seen ? 1 : 0) | (known ? 2 : 0) | (unknown ? 4 : 0);
  if (mask == ledMask) return;
  for (uint8_t i = 0; i < LED_COUNT; i++) {
    if (ledMask < 0 || ((mask ^ ledMask) & (1 << i))) ledSet(i, mask & (1 << i));
  }
  ledMask = mask;
}

static void drawHead(Slot& s, uint32_t now) {
  pcx = s.cur.x;
  pcy = s.cur.y;
  pr  = s.cur.r;
  cY = cosf(s.cur.yaw);   sY = sinf(s.cur.yaw);
  cP = cosf(s.cur.pitch); sP = sinf(s.cur.pitch);
  cR = cosf(s.cur.roll);  sR = sinf(s.cur.roll);

  // Contorno da cabeça (anel de 2 pixels)
  int16_t cx = lroundf(s.cur.x), cy = lroundf(s.cur.y), r = lroundf(s.cur.r);
  if (r < 3) return;   // ainda "nascendo"
  g->fillCircle(cx, cy, r, SSD1306_WHITE);
  g->fillCircle(cx, cy, r - 2, SSD1306_BLACK);

  uint8_t em   = effectiveMood(s);
  float   open = eyeOpen(s, now);
  drawBrows(em);
  drawEye(-EYE_AZ, open, em);
  drawEye(EYE_AZ, open, em);
  drawMouth(em);
}

static void draw(uint32_t now) {
  Adafruit_SSD1306& o = displayOled();
  g = &o;
  o.clearDisplay();
  o.setTextColor(SSD1306_WHITE);
  o.setTextSize(1);

  for (uint8_t i = 0; i < count; i++) drawHead(slots[i], now);

  const Pose& c0 = slots[0].cur;
  int16_t cx = lroundf(c0.x), cy = lroundf(c0.y), r = lroundf(c0.r);
  if (st == St::DORMINDO) {          // "z Z" subindo
    float p1 = (now % 2400) / 2400.0f;
    float p2 = fmodf(p1 + 0.5f, 1.0f);
    o.setCursor(cx + r * 0.7f + p1 * 6, cy - r * 0.6f - p1 * 10);
    o.print('z');
    o.setTextSize(2);
    o.setCursor(cx + r * 0.9f + p2 * 6, cy - r - p2 * 10);
    o.print('Z');
  } else if (st == St::PROCURANDO && (now / 600) % 2 == 0) {
    o.setTextSize(2);
    o.setCursor(cx + r + 4, cy - r);
    o.print('?');
  } else if (st == St::SEGUINDO && count == 1 && names[0][0]) {
    // Sozinho: nome no canto de baixo, do lado oposto ao da cabeça
    int16_t w = strlen(names[0]) * 6 - 1;
    int16_t x = cx >= SCREEN_W / 2 ? 1 : SCREEN_W - 1 - w;
    o.fillRect(x - 1, SCREEN_H - 9, w + 2, 9, SSD1306_BLACK);
    o.setCursor(x, SCREEN_H - 8);
    o.print(names[0]);
  } else if (st == St::SEGUINDO && count > 1) {
    // Uma fila: nome (cortado se precisar) embaixo de cada cabeça
    for (uint8_t i = 0; i < count; i++) {
      Cell c = cellFor(count, i);
      if (!c.label || !names[i][0]) continue;
      size_t maxCh = (size_t)((c.w - 2) / 6);
      size_t len   = min(strlen(names[i]), maxCh);
      int16_t w = len * 6 - 1;
      int16_t x = lroundf(c.cx - w / 2.0f);
      o.fillRect(x - 1, SCREEN_H - 9, w + 2, 9, SSD1306_BLACK);
      o.setCursor(x, SCREEN_H - 8);
      o.write((const uint8_t*)names[i], len);
    }
  }
  o.display();
}

// ===========================================================================
// API
// ===========================================================================
void faceBegin() {
  randomSeed(esp_random());
  lastFrame = millis();
  for (uint8_t i = 0; i < FACE_MAX; i++) {
    slots[i].cur = slots[i].tgt = {64, 36, 0, -0.35f, 0.15f, 22};
    slots[i].mood = MOOD_NEUTRO;
    slots[i].known = false;
    slots[i].nextBlink = 3000 + i * 700;   // cada um pisca numa hora
    names[i][0] = 0;
  }
  updateLeds();
  if (displayIsOk()) draw(lastFrame);
}

void faceLookMany(uint8_t n, const FaceIn* faces) {
  if (n == 0) { faceIdle(); return; }
  if (n > FACE_MAX) n = FACE_MAX;
  for (uint8_t i = 0; i < n; i++) {
    FaceIn f = faces[i];
    f.x     = constrain(f.x, -100, 100);
    f.y     = constrain(f.y, -100, 100);
    f.yaw   = constrain(f.yaw, -60, 60);
    f.pitch = constrain(f.pitch, -45, 45);
    f.roll  = constrain(f.roll, -45, 45);
    f.size  = constrain(f.size, 0, 100);

    Slot& s = slots[i];
    s.tgt   = targetFor(f, n, i);
    s.mood  = f.mood < MOOD_COUNT ? f.mood : MOOD_NEUTRO;
    s.known = f.known;
    if (i >= count) {        // emoji novo: nasce pequeno no lugar dele e cresce
      s.cur   = s.tgt;
      s.cur.r = 0;
    }
  }
  if (n != count) Serial.printf("[ROSTO] %u emoji(s) na tela\n", n);
  count = n;

  lastLook = millis();
  if (st != St::SEGUINDO) {
    st = St::SEGUINDO;
    stSince = lastLook;
    Serial.println("[ROSTO] Pessoa na camera: seguindo");
  }
}

void faceLook(int x, int y, int yaw, int pitch, int roll, int size, uint8_t m) {
  FaceIn f = {x, y, yaw, pitch, roll, size, m, names[0][0] != 0};
  faceLookMany(1, &f);
}

void faceIdle() {
  if (st == St::SEGUINDO) lose(millis());
}

void faceSetNames(const char* list) {
  const char* p = list;
  for (uint8_t i = 0; i < FACE_MAX; i++) {
    size_t n = 0;
    while (*p && *p != '|') {
      if (n < FACE_NAME_MAX) names[i][n++] = *p;
      p++;
    }
    names[i][n] = 0;
    if (*p == '|') p++;
  }
}

void faceSetName(const char* n) {
  for (uint8_t i = 1; i < FACE_MAX; i++) names[i][0] = 0;
  strncpy(names[0], n, FACE_NAME_MAX);
  names[0][FACE_NAME_MAX] = 0;
}

void faceLoop() {
  uint32_t now = millis();
  if (now - lastFrame < FRAME_MS) return;
  float dt = (now - lastFrame) / 1000.0f;
  if (dt > 0.1f) dt = 0.1f;
  lastFrame = now;

  updateTargets(now);
  for (uint8_t i = 0; i < count; i++) smooth(slots[i], dt);
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

uint8_t faceCount() {
  return count;
}

uint8_t faceMood() {
  return slots[0].mood;
}

const char* faceName(uint8_t i) {
  return i < FACE_MAX ? names[i] : "";
}

uint8_t faceFps() {
  return fps;
}
