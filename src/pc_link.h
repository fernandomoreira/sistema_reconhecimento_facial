// ===========================================================================
// pc_link.h - comunicação com o PC pela porta serial (USB)
//
// O PC manda uma linha de texto por comando (terminada em '\n'):
//   STATUS                                  -> só pede o estado
//   L <x> <y> <yaw> <pitch> <roll> <size> <mood>
//                                           -> tem alguém na câmera (até ~20x/s).
//                                              Sem resposta, para não encher a serial
//   IDLE                                    -> ninguém na câmera (sem resposta)
//   NAME <texto>                            -> nome da pessoa reconhecida
//                                              (bytes CP437, até 20; vazio = apaga)
//
// Respostas começam com '@' seguido de JSON:
//   @{"fw":"rosto-esp32","ver":1,"uptime":12,"oled":true,"state":"seguindo",
//     "mood":1,"name":"Ana","fps":29,"looks":350,"leds":[1,1,0]}
//   (state = seguindo | procurando | dormindo; looks conta os comandos L)
//   @{"error":"mensagem"}
// Linhas sem '@' são apenas mensagens de log.
// ===========================================================================
#pragma once
#include <Arduino.h>

// Deve ser chamada sempre no loop(): lê e executa os comandos recebidos
void pcLinkLoop();
