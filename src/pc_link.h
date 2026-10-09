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
//
// Respostas começam com '@' seguido de JSON:
//   @{"fw":"rosto-esp32","ver":3,"uptime":12,"oled":true,"state":"seguindo",
//     "count":2,"max":8,"mood":1,"name":"Ana","names":["Ana","",...],
//     "fps":29,"looks":350,"leds":[1,1,1],"vol":100}
//   (vol = volume do buzzer; state = seguindo | procurando | dormindo; count = emojis na tela;
//    looks conta os comandos L/M)
//   @{"error":"mensagem"}
// Linhas sem '@' são apenas mensagens de log.
// ===========================================================================
#pragma once
#include <Arduino.h>

// Deve ser chamada sempre no loop(): lê e executa os comandos recebidos
void pcLinkLoop();
