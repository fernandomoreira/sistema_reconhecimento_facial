// ===========================================================================
// pc_link.cpp - interpreta os comandos que chegam do PC pela serial
// ===========================================================================
#include "pc_link.h"

#include "display.h"
#include "face.h"
#include "leds.h"

static const uint8_t FW_VERSION = 1;
static const size_t  MAX_LINE   = 64;

static char     line[MAX_LINE];
static size_t   lineLen  = 0;
static bool     overflow = false;  // linha maior que o buffer: descarta
static uint32_t looks    = 0;      // conta os comandos L recebidos

static void sendStatus() {
  Serial.printf("@{\"fw\":\"rosto-esp32\",\"ver\":%u,\"uptime\":%lu,\"oled\":%s,"
                "\"state\":\"%s\",\"mood\":%u,\"name\":\"%s\",\"fps\":%u,\"looks\":%lu,\"leds\":[",
                FW_VERSION, (unsigned long)(millis() / 1000), displayIsOk() ? "true" : "false",
                faceState(), faceMood(), faceName(), faceFps(), (unsigned long)looks);
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

static void runCommand(char* cmd) {
  // Converte o nome do comando para maiúsculas (aceita "status" também)
  char* p = cmd;
  while (*p && *p != ' ') { *p = toupper(*p); p++; }
  const char* args = p;

  if (strcmp(cmd, "STATUS") == 0) {
    sendStatus();
  } else if (strncmp(cmd, "L ", 2) == 0) {
    long v[7];
    for (int i = 0; i < 7; i++) {
      if (!readInt(args, v[i])) { sendError("use L x y yaw pitch roll size mood"); return; }
    }
    faceLook(v[0], v[1], v[2], v[3], v[4], v[5], v[6] < 0 ? 0 : (uint8_t)v[6]);
    looks++;
  } else if (strcmp(cmd, "IDLE") == 0) {
    faceIdle();
  } else if (strcmp(cmd, "NAME") == 0 || strncmp(cmd, "NAME ", 5) == 0) {
    // Aspas e barras quebrariam o JSON do STATUS: troca por '?'
    char* n = cmd[4] ? cmd + 5 : cmd + 4;
    for (char* c = n; *c; c++) {
      if (*c == '"' || *c == '\\') *c = '?';
    }
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
