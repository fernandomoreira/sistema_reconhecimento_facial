// ===========================================================================
// display.cpp - inicialização do OLED SSD1306 128x64 (I2C)
// ===========================================================================
#include "display.h"

#include <Wire.h>

// ----- Configuração do hardware (igual ao projeto Display_OLED_Faces) -----
static const uint8_t PIN_SDA   = 21;
static const uint8_t PIN_SCL   = 22;
static const uint8_t OLED_ADDR = 0x3C;

// -1 = o display não tem pino de reset próprio (usa o reset do ESP32)
static Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);

static bool ok = false;  // display encontrado?

bool displayBegin() {
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);  // I2C rápido (400 kHz): ~30 quadros por segundo

  ok = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (!ok) {
    Serial.println("[OLED] Display nao encontrado! Verifique fios e endereco 0x3C.");
    return false;
  }
  oled.cp437(true);  // tabela CP437 de verdade (letras acentuadas no nome)
  oled.clearDisplay();
  oled.display();
  Serial.println("[OLED] Display inicializado.");
  return true;
}

bool displayIsOk() {
  return ok;
}

Adafruit_SSD1306& displayOled() {
  return oled;
}
