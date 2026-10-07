// ===========================================================================
// display.h - o OLED SSD1306 128x64 (I2C) onde o rosto é desenhado
//
// Este módulo só liga o display. Quem desenha o rosto é o face.cpp, usando
// o objeto devolvido por displayOled().
// ===========================================================================
#pragma once
#include <Arduino.h>
#include <Adafruit_SSD1306.h>

constexpr int SCREEN_W = 128;
constexpr int SCREEN_H = 64;

// Inicializa o I2C (SDA=21, SCL=22) e o display no endereço 0x3C.
// Retorna false se o display não for encontrado (o resto do projeto segue).
bool displayBegin();

// true se o display foi encontrado no barramento I2C
bool displayIsOk();

// O display em si (para desenhar com a Adafruit GFX)
Adafruit_SSD1306& displayOled();
