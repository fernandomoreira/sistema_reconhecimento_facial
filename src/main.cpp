// ===========================================================================
// main.cpp - ponto de entrada do projeto ESP32 Reconhecimento Facial
//
// A câmera fica no PC: o programa pc/reconhecimento.py encontra e reconhece
// o rosto e manda pela USB para onde a pessoa está e para onde ela olha.
// O ESP32 desenha no OLED um emoji que imita esses movimentos:
//   leds     -> indicam se tem alguém e se a pessoa é conhecida
//   display  -> liga o OLED
//   face     -> anima e desenha o emoji
//   pc_link  -> recebe os comandos do PC pela serial
// ===========================================================================
#include <Arduino.h>

#include "display.h"
#include "face.h"
#include "leds.h"
#include "pc_link.h"

void setup() {
  // Buffer maior: uma linha M com 8 pessoas tem ~300 letras e chega enquanto
  // o OLED está sendo desenhado (o buffer padrão de 256 transbordaria)
  Serial.setRxBufferSize(2048);
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=== ESP32 Reconhecimento Facial (camera no PC, emoji no OLED) ===");

  // LEDs primeiro: garante que todos ficam apagados o quanto antes
  ledsBegin();

  displayBegin();
  faceBegin();  // começa dormindo até aparecer alguém na câmera
}

void loop() {
  pcLinkLoop();
  faceLoop();
  delay(1);  // pequena pausa: deixa o processador "respirar"
}
