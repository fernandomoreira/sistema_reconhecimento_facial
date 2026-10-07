// ===========================================================================
// face.h - os rostos animados (emojis) que imitam as pessoas na câmera do PC
//
// O PC (pc/reconhecimento.py) encontra os rostos na câmera e manda, várias
// vezes por segundo, onde cada um está e para onde a cabeça está virada.
// Este módulo suaviza esses valores e desenha no OLED (~30 quadros/s) um
// emoji para cada pessoa (até FACE_MAX):
//   - 1 pessoa: a cabeça anda pela tela inteira acompanhando a pessoa
//   - 2 a 4 pessoas: uma fila de cabeças, com o nome embaixo de cada uma
//   - 5 a 8 pessoas: duas filas de cabeças menores (sem nomes)
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

// Tamanho máximo do nome de cada pessoa (21 letras cabem numa linha)
constexpr size_t FACE_NAME_MAX = 20;

// Quantos emojis cabem no visor ao mesmo tempo (2 filas de 4)
constexpr uint8_t FACE_MAX = 8;

// Uma pessoa na câmera. Todos os valores já vêm limitados pelo PC:
//   x, y   posição do rosto na imagem, -100..100 (0 = centro)
//   yaw    virar a cabeça, -60..60 graus (positivo = para a direita da tela)
//   pitch  -45..45 graus (positivo = olhando para cima)
//   roll   -45..45 graus (positivo = tombada no sentido horário)
//   size   0..100 (quanto maior, mais perto da câmera)
//   mood   expressão (Mood)
//   known  true = pessoa cadastrada (acende o LED verde), false = LED vermelho
struct FaceIn {
  int     x, y, yaw, pitch, roll, size;
  uint8_t mood;
  bool    known;
};

// Desenha o primeiro quadro (o emoji começa dormindo)
void faceBegin();

// Há n pessoas na câmera (1..FACE_MAX), da esquerda para a direita
void faceLookMany(uint8_t n, const FaceIn* faces);

// Atalho para uma pessoa só (comando L; "conhecida" = tem nome)
void faceLook(int x, int y, int yaw, int pitch, int roll, int size, uint8_t mood);

// Ninguém na câmera: procura por alguns segundos e depois dorme
void faceIdle();

// Nomes das pessoas, separados por '|' (bytes CP437; "" = sem nome).
// Ex.: "Ana||Bruno" = 1ª Ana, 2ª desconhecida, 3ª Bruno
void faceSetNames(const char* list);

// Nome só da 1ª pessoa (apaga os outros)
void faceSetName(const char* name);

// Deve ser chamada sempre no loop(): anima e redesenha quando é hora
void faceLoop();

// Estado para o painel: "seguindo", "procurando" ou "dormindo"
const char* faceState();
uint8_t     faceCount();             // emojis na tela agora
uint8_t     faceMood();              // expressão do 1º emoji
const char* faceName(uint8_t i = 0); // nome da pessoa i
uint8_t     faceFps();
