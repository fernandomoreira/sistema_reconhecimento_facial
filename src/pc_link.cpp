// ===========================================================================
// pc_link.cpp - interpreta os comandos que chegam do PC pela serial
// ===========================================================================
#include "pc_link.h"

#include "buzzer.h"
#include "display.h"
#include "face.h"
#include "leds.h"

static const uint8_t FW_VERSION = 3;
// M com 8 pessoas: "M 8" + 8 x 8 números (~300 letras); NAMES: 8 nomes de 20
static const size_t  MAX_LINE   = 400;

static char     line[MAX_LINE];
static size_t   lineLen  = 0;
static bool     overflow = false;  // linha maior que o buffer: descarta
static uint32_t looks    = 0;      // conta os comandos L/M recebidos

static void sendStatus() {
  Serial.printf("@{\"fw\":\"rosto-esp32\",\"ver\":%u,\"uptime\":%lu,\"oled\":%s,"
                "\"state\":\"%s\",\"count\":%u,\"max\":%u,\"mood\":%u,\"name\":\"%s\",\"names\":[",
                FW_VERSION, (unsigned long)(millis() / 1000), displayIsOk() ? "true" : "false",
                faceState(), faceCount(), FACE_MAX, faceMood(), faceName(0));
  for (uint8_t i = 0; i < FACE_MAX; i++) {
    Serial.printf("%s\"%s\"", i ? "," : "", faceName(i));
  }
  Serial.printf("],\"fps\":%u,\"looks\":%lu,\"leds\":[", faceFps(), (unsigned long)looks);
  for (uint8_t i = 0; i < LED_COUNT; i++) {
    Serial.print(i ? "," : "");
    Serial.print(ledGet(i) ? 1 : 0);
  }
  Serial.println("]}");
}

static void sendError(const char* msg) {
  Serial.printf("@{\"error\":\"%s\"}\n", msg);
}

// Lê um inteiro (pode ser negativo) de *p e avança o ponteiro.
// Retorna false se não houver número.
static bool readInt(const char*& p, long& out) {
  while (*p == ' ') p++;
  bool neg = false;
  if (*p == '-' || *p == '+') neg = (*p++ == '-');
  if (!isDigit(*p)) return false;
  long v = 0;
  while (isDigit(*p) && v < 100000) v = v * 10 + (*p++ - '0');
  out = neg ? -v : v;
  return true;
}

// Aspas e barras quebrariam o JSON do STATUS: troca por '?'
static void sanitize(char* s) {
  for (char* c = s; *c; c++) {
    if (*c == '"' || *c == '\\') *c = '?';
  }
}

static void runCommand(char* cmd) {
  // Converte o nome do comando para maiúsculas (aceita "status" também)
  char* p = cmd;
  while (*p && *p != ' ') { *p = toupper(*p); p++; }
  const char* args = p;

  if (strcmp(cmd, "STATUS") == 0) {
    sendStatus();
  } else if (strncmp(cmd, "M ", 2) == 0) {
    // M n  e depois n grupos de: x y yaw pitch roll size mood known
    long n;
    if (!readInt(args, n) || n < 0 || n > FACE_MAX) { sendError("use M n (0..8) + 8 numeros por pessoa"); return; }
    FaceIn faces[FACE_MAX];
    for (long i = 0; i < n; i++) {
      long v[8];
      for (int k = 0; k < 8; k++) {
        if (!readInt(args, v[k])) { sendError("use M n + x y yaw pitch roll size mood known"); return; }
      }
      faces[i] = {(int)v[0], (int)v[1], (int)v[2], (int)v[3], (int)v[4], (int)v[5],
                  (uint8_t)(v[6] < 0 ? 0 : v[6]), v[7] != 0};
    }
    faceLookMany((uint8_t)n, faces);
    looks++;
  } else if (strncmp(cmd, "L ", 2) == 0) {
    long v[7];
    for (int i = 0; i < 7; i++) {
      if (!readInt(args, v[i])) { sendError("use L x y yaw pitch roll size mood"); return; }
    }
    faceLook(v[0], v[1], v[2], v[3], v[4], v[5], v[6] < 0 ? 0 : (uint8_t)v[6]);
    looks++;
  } else if (strncmp(cmd, "BEEP ", 5) == 0) {
    long t;
    if (!readInt(args, t) || t < 1 || t > BEEP_MAX) { sendError("use BEEP 1..3"); return; }
    buzzerPlay((uint8_t)t);
  } else if (strncmp(cmd, "TONE ", 5) == 0) {
    long f, ms;
    if (!readInt(args, f) || !readInt(args, ms) || f < 100 || f > 10000 || ms < 1 || ms > 3000) {
      sendError("use TONE freq(100..10000) ms(1..3000)");
      return;
    }
    buzzerTone((uint16_t)f, (uint16_t)ms);
  } else if (strcmp(cmd, "IDLE") == 0) {
    faceIdle();
  } else if (strcmp(cmd, "NAMES") == 0 || strncmp(cmd, "NAMES ", 6) == 0) {
    char* n = cmd[5] ? cmd + 6 : cmd + 5;
    sanitize(n);
    faceSetNames(n);
    Serial.printf("[PC] Nomes: %s\n", n[0] ? n : "(nenhum)");
    sendStatus();
  } else if (strcmp(cmd, "NAME") == 0 || strncmp(cmd, "NAME ", 5) == 0) {
    char* n = cmd[4] ? cmd + 5 : cmd + 4;
    sanitize(n);
    faceSetName(n);
    Serial.printf("[PC] Nome: %s\n", n[0] ? n : "(nenhum)");
    sendStatus();
  } else {
    sendError("comando desconhecido");
  }
}

void pcLinkLoop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line[lineLen] = 0;
      if (overflow)         sendError("linha muito longa");
      else if (lineLen > 0) runCommand(line);
      lineLen  = 0;
      overflow = false;
    } else if (lineLen < MAX_LINE - 1) {
      line[lineLen++] = c;
    } else {
      overflow = true;
    }
  }
}
