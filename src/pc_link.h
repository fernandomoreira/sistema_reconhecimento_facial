// ===========================================================================
// pc_link.h - comunicação com o PC pela porta serial (USB)
//
// O PC manda uma linha de texto por comando (terminada em '\n'):
//   STATUS                                  -> só pede o estado
//   M <n> { <x> <y> <yaw> <pitch> <roll> <size> <mood> <known> } x n
//                                           -> n pessoas na câmera (1..8), da
//                                              esquerda para a direita (até ~20x/s).
//                                              Sem resposta, para não encher a serial
//   L <x> <y> <yaw> <pitch> <roll> <size> <mood>
//                                           -> atalho para uma pessoa só (sem resposta)
//   IDLE                                    -> ninguém na câmera (sem resposta)
//   NAMES <nome1>|<nome2>|...               -> nomes das pessoas, na ordem do M
//                                              (bytes CP437, até 20 cada; vazio = desconhecida)
//   NAME <texto>                            -> nome só da 1ª pessoa
//   BEEP <tipo>                             -> toca um som no buzzer (sem resposta):
//                                              1 = pessoa apareceu, 2 = tique da captura
//                                              automática, 3 = captura terminada
//   TONE <freq> <ms>                        -> tom avulso (100..10000 Hz, até 3000 ms), para
//                                              teste; freq 0 = pino ligado direto (buzzer ativo)
//   VOL <0..100>                            -> volume do buzzer (0 = mudo); responde o STATUS
//   CAB ON | CAB OFF                        -> cabeça física segue o emoji / volta ao centro
//   CAB ZERO                                -> posição atual dos motores = centro (com OFF)
//   CAB MOVER <dx> <dy>                     -> move à mão, em graus (-360..360, com OFF)
//   CAB CFG <limX> <limY> <invX> <invY> <vel>
//                                           -> giro máximo (X 5..180, Y 5..90 graus), inverter
//                                              (0/1) e velocidade (100..1000 meios-passos/s),
//                                              salvos na flash. Todos os CAB respondem o STATUS
//
// Respostas começam com '@' seguido de JSON:
//   @{"fw":"rosto-esp32","ver":4,"uptime":12,"oled":true,"state":"seguindo",
//     "count":2,"max":8,"mood":1,"name":"Ana","names":["Ana","",...],
//     "fps":29,"looks":350,"leds":[1,1,1],"vol":100,
//     "cab":{"on":true,"x":12.3,"y":-4.0,"ax":12.5,"ay":-4.0,"mov":true,
//            "limX":60,"limY":30,"invX":false,"invY":false,"vel":600,"n":3}}
//   (vol = volume do buzzer; state = seguindo | procurando | dormindo; count = emojis na tela;
//    looks conta os comandos L/M; cab = cabeça física: x/y = ângulo atual dos motores,
//    ax/ay = alvo, mov = algum motor andando, n = quantos comandos CAB já foram executados)
//   @{"error":"mensagem"}
// Linhas sem '@' são apenas mensagens de log.
// ===========================================================================
#pragma once
#include <Arduino.h>

// Deve ser chamada sempre no loop(): lê e executa os comandos recebidos
void pcLinkLoop();
