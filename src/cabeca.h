// ===========================================================================
// cabeca.h - cabeça física com 2 motores de passo 28BYJ-48 (drivers ULN2003)
//
// A cabeça imita o emoji principal do OLED (a pessoa mais perto da câmera):
//   motor X (horizontal) -> yaw   do emoji: virar para os lados
//   motor Y (vertical)   -> pitch do emoji: olhar para cima/baixo
// Sem ninguém na câmera ela também procura (olha para os lados) e "dorme"
// (cabeça baixa), igual ao emoji.
//
// Ligações (cada ULN2003 tem as entradas IN1..IN4):
//   motor X: IN1 = GPIO 32, IN2 = GPIO 33, IN3 = GPIO 25, IN4 = GPIO 26
//   motor Y: IN1 = GPIO 16, IN2 = GPIO 17, IN3 = GPIO 18, IN4 = GPIO 19
//   ULN2003 "+" no 5 V de uma FONTE EXTERNA (cada motor puxa ~250 mA) e o
//   "-" no GND da fonte E no GND do ESP32 (terra comum).
//
// Os motores andam numa tarefa própria no núcleo 0, um passo por milissegundo
// no máximo: o desenho do OLED (núcleo 1) não atrapalha o movimento.
//
// Não há chave de fim de curso: a posição em que a cabeça está ao ligar a
// placa é o CENTRO. Se ela ficar torta, desligue o "seguir", acerte com
// cabecaMover() e chame cabecaZerar().
// ===========================================================================
#pragma once
#include <Arduino.h>

// Configuração (guardada na memória flash da placa, sobrevive ao desligar)
struct CabecaCfg {
  bool     on;          // true = segue o emoji; false = fica parada no centro
  uint8_t  limX;        // giro máximo do motor X para cada lado (graus, 5..180)
  uint8_t  limY;        // giro máximo do motor Y para cima/baixo (graus, 5..90)
  bool     invX, invY;  // inverte o sentido do motor (se girar ao contrário)
  uint16_t vel;         // velocidade máxima em meios-passos/s (100..1000)
};

// Configura os pinos, lê a configuração salva e começa a tarefa dos motores
void cabecaBegin();

// Deve ser chamada sempre no loop(): manda os motores para a pose do emoji
void cabecaLoop();

// Liga/desliga o "seguir" (desligado: volta ao centro e solta as bobinas)
void cabecaLigar(bool on);

// Movimento manual em graus (só com o "seguir" desligado). false = recusado
bool cabecaMover(float dx, float dy);

// A posição atual passa a ser o centro
void cabecaZerar();

// Muda a configuração (o campo "on" é ignorado; use cabecaLigar)
void cabecaConfigurar(const CabecaCfg& c);

const CabecaCfg& cabecaCfg();

// Para o STATUS: ângulo atual e alvo de cada motor (graus) e se está andando
float cabecaAngulo(uint8_t eixo);   // 0 = X, 1 = Y
float cabecaAlvo(uint8_t eixo);
bool  cabecaAndando();
