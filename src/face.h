// ===========================================================================
// face.h - o rosto animado (emoji) que imita a pessoa na câmera do PC
//
// O PC (pc/reconhecimento.py) encontra o rosto na câmera e manda, várias
// vezes por segundo, onde ele está e para onde a cabeça está virada.
// Este módulo suaviza esses valores e desenha o emoji no OLED (~30 quadros/s):
//   - a cabeça anda pela tela acompanhando a pessoa (esquerda/direita, cima/baixo)
//   - olhos e boca giram numa "esfera" 3D: virar (yaw), inclinar para cima/baixo
//     (pitch) e tombar a cabeça para o lado (roll)
//   - quanto mais perto da câmera, maior a cabeça
//   - pisca sozinho; sem ninguém na câmera ele procura e depois dorme
//
// IMPORTANTE: pc/painel.html tem uma cópia em JavaScript deste desenho (a
// prévia do visor). Se mudar o desenho aqui, mude lá também.
// ===========================================================================
#pragma once
#include <Arduino.h>

// Expressões (a mesma ordem em pc/reconhecimento.py e pc/painel.html)
enum Mood : uint8_t {
  MOOD_NEUTRO = 0,
  MOOD_FELIZ,     // pessoa conhecida
  MOOD_CURIOSO,   // pessoa desconhecida
  MOOD_SURPRESO,  // alguém acabou de aparecer
  MOOD_BRAVO,
  MOOD_TRISTE,
  MOOD_COUNT
};

// Tamanho máximo do nome mostrado no visor (21 letras cabem numa linha)
constexpr size_t FACE_NAME_MAX = 20;

// Desenha o primeiro quadro (o emoji começa dormindo)
void faceBegin();

// Há uma pessoa na câmera. Todos os valores já vêm limitados pelo PC:
//   x, y   posição do rosto na imagem, -100..100 (0 = centro)
//   yaw    virar a cabeça, -60..60 graus (positivo = para a direita da tela)
//   pitch  -45..45 graus (positivo = olhando para cima)
//   roll   -45..45 graus (positivo = tombada no sentido horário)
//   size   0..100 (quanto maior, mais perto da câmera)
void faceLook(int x, int y, int yaw, int pitch, int roll, int size, uint8_t mood);

// Ninguém na câmera: procura por alguns segundos e depois dorme
void faceIdle();

// Nome da pessoa reconhecida (bytes CP437; "" = nenhum)
void faceSetName(const char* name);

// Deve ser chamada sempre no loop(): anima e redesenha quando é hora
void faceLoop();

// Estado para o painel: "seguindo", "procurando" ou "dormindo"
const char* faceState();
uint8_t     faceMood();
const char* faceName();
uint8_t     faceFps();
