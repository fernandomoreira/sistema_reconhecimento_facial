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
  `leds.*` pinos 5/4/15; `buzzer.*` GPIO 23 (PWM LEDC, sons sem delay, `buzzerLoop()`);
  `pc_link.*` interpreta a serial; `face.*` anima e desenha o emoji.
- `pc/reconhecimento.py` (arquivo único): `Board` (serial, reconexão automática), `FaceDB`
  (`pc/rostos/pessoas.json` + fotos), `Vision` (thread da câmera: detecção, tracking, votos de
  reconhecimento, pose, cadastro, busca de câmeras), `sender()` (20 Hz), `make_handler()` (HTTP).
- `pc/painel.html`: página única, sem build. A **prévia do OLED é uma cópia em JS de
  `src/face.cpp`**.

## Protocolo serial (PC → ESP32, uma linha por comando)

`STATUS` · `M n` + n × `x y yaw pitch roll size mood known` (sem resposta; o PC usa este) ·
`L x y yaw pitch roll size mood` (atalho p/ 1 pessoa) · `IDLE` (sem resposta) ·
`NAMES a|b|c` (nomes cp437 na ordem do M) · `NAME <texto>` (só a 1ª) ·
`BEEP 1|2|3` (sem resposta: pessoa apareceu, tique da captura automática, fim da captura) ·
`VOL 0..100` (volume do buzzer, 0 = mudo; responde o STATUS) ·
`TONE freq ms` (teste, sem resposta; freq 0 = pino ligado direto, como num buzzer ativo).
Respostas: `@` + JSON (`{"fw":"rosto-esp32","ver":3,"count":n,"names":[8 nomes],...,"vol":100}`
ou `{"error":"..."}`); linhas sem `@` são log. Faixas: x,y −100..100; yaw −60..60; pitch, roll
−45..45; size 0..100; mood 0..5.

## Vários emojis (um por pessoa)

- `FACE_MAX` = 8 em `src/face.h`, `reconhecimento.py` e `painel.html` (prévia). O painel escolhe
  o limite (`/api/emojis?n=`, `Vision.max_faces`); o PC manda as `n` pessoas mais perto,
  ordenadas da esquerda para a direita.
- Layout em `cellFor()` (face.cpp e cópia JS): 1 = tela inteira; 2–4 = uma fila com nomes
  embaixo; 5–8 = duas filas sem nomes. Cada slot tem suavização e piscada próprias.
- Linha `M 8` tem ~200 letras: `MAX_LINE` = 400 e `Serial.setRxBufferSize(2048)` no `main.cpp`
  (o buffer padrão de 256 transborda enquanto o OLED é desenhado).
- No PC, cada track tem a sua pose suavizada (`tr["pose"]`) e a sua surpresa (`tr["surprise"]`);
  o reconhecimento faz no máx. `MAX_RECOG` por quadro, os que esperam há mais tempo primeiro.

## Regras que precisam ficar sincronizadas

- **Ordem das expressões** (0 Neutro, 1 Feliz, 2 Curioso, 3 Surpreso, 4 Bravo, 5 Triste) é a
  mesma em `src/face.h`, `MOODS` em `reconhecimento.py` e `MOODS` em `painel.html`.
- Mudou desenho/animação em `src/face.cpp`? Replique na prévia JS de `painel.html`.
- Mudou comando serial? Atualize `pc_link.cpp`, `Board`/`sender` no Python e o README.
- **LEDs**: amarelo G5 = alguém na câmera; verde G4 = alguém cadastrado; vermelho G15 = alguém
  não reconhecido — verde e vermelho podem acender juntos (lógica em `updateLeds()` de
  `face.cpp`, pelo campo `known` de cada pessoa; nomes/cores em `LEDS` do painel, que fica
  embaixo da câmera).
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

## Registro de aparições e relatórios

- `pc/registro.py` (`Registro`): SQLite em `pc/dados/monitoramento.db`, tabela `aparicoes`
  (pessoa '' = desconhecido, inicio/fim em epoch, duracao, ativo, camera, confianca, foto JPEG 96px).
- Ligado em `Vision`: `reg.votar()` a cada reconhecimento, `reg.atualizar()` a cada quadro
  (abre após `MIN_S`, salva a cada `SALVAR_S`), `reg.encerrar()` quando o track some
  (`TRACK_KEEP_S`), quando a câmera para (`_set_no_face`) e no `atexit`.
- Nome da aparição = mais votado com ≥ `NOME_MIN` dos votos; cadastro feito durante a
  aparição zera os votos dela. Cadastrado que volta em ≤ `JUNTAR_S` continua a mesma aparição.
- Painel: quadro "Últimas aparições" (`/api/aparicoes?n=5`, a cada 2 s), com 🗑️ Apagar por linha (também na lista "Todas as aparições" dos relatórios)
  (`/api/aparicoes/apagar?id=` → `Registro.apagar()`; recusa `ativo = 1`, a sessão ainda grava nela). Página
  `pc/relatorios.html` em `/relatorios` (`/api/relatorio`, `/api/relatorio.csv` com `;` + BOM).
- Relatórios também têm **Consumo do sistema** (`/api/consumo`: `consumo()`/`pasta_info()` no
  reconhecimento.py + `Registro.consumo()`) e **Liberar espaço** (`/api/limpar` →
  `Registro.limpar(antes, so_fotos, previa)`: nunca apaga `ativo = 1`; depois `VACUUM` +
  `wal_checkpoint(TRUNCATE)`). O painel mostra a prévia antes e pede 2 cliques para apagar.
- O `-wal` do SQLite crescia (4 MB para um banco de 0,4 MB): `PRAGMA journal_size_limit` = 1 MB.
  Teste limpeza sempre numa **cópia** do banco (`pc/dados/monitoramento.db*`), nunca no real.
- Título do painel: **Programa de Monitoramento Interativo**.

## Captura automática e edição de nomes

- Painel: card “Captura automática” (botão deslizante + modo `qualquer`/`especifico`), `/api/auto`.
  Estado em `Vision.auto_on`, `auto_mode`, `auto` (captura em andamento, só a thread da câmera
  mexe) e `auto_info` (o que vai em `/api/alvo` → `auto`). Lógica em `Vision._auto_step()`.
- Só captura quem os últimos `AUTO_VOTOS` reconhecimentos deram desconhecido; cada track tenta
  uma vez (`tr["auto_feito"]`). Específico: `tr["fix"]` = desde quando olha fixo (`AUTO_FIXAR_S`,
  `AUTO_OLHAR_GRAUS`); desviar por mais de `AUTO_DESVIO_S` interrompe. Durante a captura o nome
  no OLED vira `OLED_OLHANDO` e o vídeo mostra `MSG_OLHANDO`.
- `FaceDB.add(..., auto=True)` escolhe "Desconhecido N" (`contadorDesconhecidos` em
  `pessoas.json`, números não se repetem); a pessoa fica com `"auto": true` até ganhar nome.
- `/api/renomear`: `FaceDB.rename()` (KeyError = nome já existe → 409; `juntar` mistura),
  `Registro.renomear()` (aparições) e `Vision.renames` (votos dos rostos na câmera).

## Buzzer (GPIO 23)

- Buzzer do usuário: **passivo** (as notas mudam de tom), ligado direto no GPIO 23 → som
  **baixo** com 3,3 V. Soou mais alto entre 1500 e 2700 Hz (varredura com `TONE`), por isso os
  sons de `src/buzzer.cpp` ficam nessa faixa. Mais volume só com hardware (NPN + 5 V).
- `buzzer.cpp`: notas `{freq, ms}` (`SOM_PESSOA`, `SOM_TIQUE`, `SOM_FIM`), PWM no canal LEDC 0
  com 10 bits. Volume = duty: passivo `512 × (vol/100)²` (50% = máximo); `BUZZER_ATIVO = true`
  usa PWM de 20 kHz com duty linear. Core Arduino-ESP32 **2.0.17**: API `ledcSetup`/
  `ledcAttachPin`/`ledcWriteTone` (`ledcWriteTone` refaz o setup em 10 bits).
- PC: `Vision.beep()` enfileira em `Vision.beeps` se `Vision.volume > 0`; `sender()` manda
  `BEEP n` e, quando `status.vol` da placa ≠ `Vision.volume`, manda `VOL n` (como o `NAMES`).
  Pessoa nova = track novo (máx. 1 a cada `BEEP_PESSOA_S`); tique a cada `BEEP_TIQUE_S`
  enquanto a captura automática está `capturando`; fim em `_auto_finish()` quando salva.
- Painel: botão de som ao lado de “Espelhar” passa por `VOLUMES` = mudo/baixo/médio/alto
  (0/20/50/100) → `/api/som?vol=`. O volume começa em 100 a cada execução (não é salvo).
- **Testar sons direto na placa**: `POST /api/release`, abrir a COM12 com pyserial com
  `dtr = rts = False` **antes** do `open()` (senão a placa reinicia), mandar `BEEP`/`TONE`/`VOL`,
  fechar e `POST /api/resume`.

## Layout do painel

Câmera e emoji (largo) → Cadastrar · Captura automática · Expressão (lado a lado) → Pessoas
cadastradas (largo, cartões na horizontal) → Últimas aparições (largo) → Informações técnicas.

## Dados pessoais

`pc/rostos/` (fotos e vetores dos rostos) e `pc/dados/` (aparições) ficam **só no PC** e estão
no `.gitignore`. Nunca versionar. Os modelos `.onnx` em `pc/modelos/` são baixados sozinhos se faltarem
(`ensure_models()`), mas estão versionados para funcionar offline.

## Testes

Não há testes automatizados. Validação manual: `python -m platformio run` compila; com a placa
no USB, `curl localhost:8000/api/status` deve mostrar `boardAnswering: true`; a câmera pode ser
simulada com uma imagem em movimento (foi assim que o cadastro/reconhecimento foi testado).
