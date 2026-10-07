# CLAUDE.md — memória do projeto

Sistema de reconhecimento facial: a **webcam do PC** reconhece rostos e calcula a pose da
cabeça; um **ESP32** com **OLED SSD1306 128x64** desenha um emoji 3D que imita a pessoa;
um **painel web** em `http://localhost:8000` controla tudo. Irmão do projeto
`../Display_OLED_Faces` (mesma placa, mesmos pinos, mesmo estilo de painel).

Tudo (código, comentários, painel, README, mensagens) é em **português do Brasil**.

## Comandos

```bash
python -m platformio run                 # compila o firmware (pio não está no PATH)
python -m platformio run -t upload       # grava (antes: "Liberar porta" no painel ou pare o programa)
python pc/reconhecimento.py              # programa do PC + painel (abre o navegador)
python pc/reconhecimento.py --no-browser --camera 1 --port COM12 --http 8000
abrir_painel.bat                         # atalho para o usuário (instala dependências se faltar)
```

Placa do usuário: ESP32 DevKit no **COM12** (CP210x, VID 10C4). Câmeras do PC: 0 = Lenovo FHD
Webcam (externa), 1 = Integrated Webcam (costuma dar imagem preta: obturador/tampa fechados).

## Arquitetura

```
Webcam ─► pc/reconhecimento.py ──USB serial 115200──► ESP32 ──I2C──► OLED
           (YuNet + SFace, pose)                      src/face.cpp (emoji 3D, ~31 fps)
           └─ HTTP: pc/painel.html, /video (MJPEG), /api/*
```

- `src/main.cpp` junta os módulos; `display.*` liga o OLED (SDA 21, SCL 22, 0x3C, cp437);
  `leds.*` pinos 5/4/15; `pc_link.*` interpreta a serial; `face.*` anima e desenha o emoji.
- `pc/reconhecimento.py` (arquivo único): `Board` (serial, reconexão automática), `FaceDB`
  (`pc/rostos/pessoas.json` + fotos), `Vision` (thread da câmera: detecção, tracking, votos de
  reconhecimento, pose, cadastro, busca de câmeras), `sender()` (20 Hz), `make_handler()` (HTTP).
- `pc/painel.html`: página única, sem build. A **prévia do OLED é uma cópia em JS de
  `src/face.cpp`**.

## Protocolo serial (PC → ESP32, uma linha por comando)

`STATUS` · `L x y yaw pitch roll size mood` (sem resposta) · `IDLE` (sem resposta) ·
`NAME <texto cp437>`. Respostas: `@` + JSON (`{"fw":"rosto-esp32","ver":1,...}` ou
`{"error":"..."}`); linhas sem `@` são log. Faixas: x,y −100..100; yaw −60..60; pitch, roll
−45..45; size 0..100; mood 0..5.

## Regras que precisam ficar sincronizadas

- **Ordem das expressões** (0 Neutro, 1 Feliz, 2 Curioso, 3 Surpreso, 4 Bravo, 5 Triste) é a
  mesma em `src/face.h`, `MOODS` em `reconhecimento.py` e `MOODS` em `painel.html`.
- Mudou desenho/animação em `src/face.cpp`? Replique na prévia JS de `painel.html`.
- Mudou comando serial? Atualize `pc_link.cpp`, `Board`/`sender` no Python e o README.
- **LEDs**: amarelo G5 = alguém na câmera; verde G4 = pessoa cadastrada; vermelho G15 = não
  reconhecida (lógica em `updateLeds()` de `face.cpp`; nomes/cores em `LEDS` do painel).
- `NAME_MAX`/`FACE_NAME_MAX` = 20 (cabe numa linha do OLED). Acentos sem glifo CP437 (ã, õ...)
  são removidos por `to_oled_text()`.

## Armadilhas já encontradas

- **OpenCV 5 (pip) aqui não captura por DirectShow** (`CAP_DSHOW` falha): `_open_camera()` cai
  no backend padrão (MSMF). A ordem do MSMF bate com os nomes do `pygrabber` (DirectShow).
- **Câmera ocupada** (app Câmera do Windows, Teams) trava o `VideoCapture` por ~33 s: a busca
  testa as câmeras em threads paralelas com `SCAN_TIMEOUT_S`. A busca roda na thread da câmera.
- `json.dumps` quebra com tipos numpy (`np.bool_`, `np.float32`): converta com `bool()`/`float()`
  tudo que vai para `/api/status`.
- `do_POST` precisa **ler o corpo antes de responder**, senão o Windows derruba a conexão
  (WinError 10053).
- Só um programa por vez usa a porta COM: para gravar firmware, "Liberar porta" no painel.
- Ao escrever arquivos com barras invertidas (`'\\'`, `.bat`), use as ferramentas Write/Edit:
  heredoc/printf no bash corrompem `\\`, `\r` e caminhos.
- Saída do Python no console do Windows sai em cp1252 ("C�mera") — é só o terminal.

## Dados pessoais

`pc/rostos/` (fotos e vetores dos rostos) fica **só no PC** e está no `.gitignore`. Nunca
versionar. Os modelos `.onnx` em `pc/modelos/` são baixados sozinhos se faltarem
(`ensure_models()`), mas estão versionados para funcionar offline.

## Testes

Não há testes automatizados. Validação manual: `python -m platformio run` compila; com a placa
no USB, `curl localhost:8000/api/status` deve mostrar `boardAnswering: true`; a câmera pode ser
simulada com uma imagem em movimento (foi assim que o cadastro/reconhecimento foi testado).
