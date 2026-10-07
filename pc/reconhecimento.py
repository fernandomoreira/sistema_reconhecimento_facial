"""
reconhecimento.py - reconhecimento facial no PC + emoji no OLED do ESP32

A webcam fica no PC. Este programa:
  1. encontra os rostos na imagem (modelo YuNet, com 5 pontos: olhos, nariz e
     cantos da boca) e reconhece quem é (modelo SFace + pessoas cadastradas);
  2. calcula para onde a cabeça da pessoa mais próxima está virada e manda
     isso ~20 vezes por segundo para o ESP32 pelo cabo USB;
  3. serve o painel (painel.html) em http://localhost:8000, com a câmera ao
     vivo, a prévia do emoji, o cadastro de pessoas e o estado da placa.

Tudo roda neste PC: as fotos e os dados dos rostos ficam em pc/rostos/.

Uso:
    pip install -r requirements.txt
    python pc/reconhecimento.py                 # detecta a porta e procura as câmeras
    python pc/reconhecimento.py --camera 1      # usa sempre a câmera 1
    python pc/reconhecimento.py --port COM12    # força uma porta
    python pc/reconhecimento.py --http 8080     # muda a porta do site

Rotas da API local:
    GET  /                            painel
    GET  /video                       câmera ao vivo (MJPEG)
    GET  /api/status                  placa + câmera + pessoas (consultado a cada 1 s)
    GET  /api/alvo                    só o rosto seguido (consultado ~10x/s pela prévia)
    GET  /foto/<arquivo>.jpg          foto de uma pessoa cadastrada
    POST /api/cadastrar  {"nome": "..."}   captura o rosto da frente da câmera
    POST /api/cancelar                cancela o cadastro em andamento
    POST /api/remover?nome=...        apaga uma pessoa
    POST /api/humor?id=-1..5          -1 = automático; 0..5 = expressão fixa
    POST /api/calibrar                "de frente para a câmera" = cabeça reta
    POST /api/espelhar?on=0|1         espelha a imagem (o emoji imita como espelho)
    POST /api/camera?index=0..9       troca de câmera
    POST /api/cameras/buscar          procura de novo as câmeras ligadas ao PC
    POST /api/emojis?n=1..8           quantos emojis (um por pessoa) cabem no visor
    GET  /relatorios                  página de relatórios das aparições
    GET  /api/aparicoes?n=5           últimas aparições (quem passou pela câmera e quanto tempo)
    GET  /api/aparicoes/foto/<id>     foto pequena do rosto daquela aparição
    GET  /api/relatorio?de=&ate=&pessoa=&pagina=   totais por pessoa/dia/hora + lista
    GET  /api/relatorio.csv?de=&ate=&pessoa=       todas as aparições do filtro em CSV
                                      (pessoa: nome, __conhecidos ou __desconhecidos)
    POST /api/port?name=COM12|auto    escolhe a porta serial
    POST /api/release | /api/resume   solta / retoma a porta COM (para gravar firmware)
"""

import argparse
import atexit
import json
import os
import re
import threading
import time
import unicodedata
import urllib.request
import webbrowser
from collections import Counter, deque
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from math import atan, atan2, cos, degrees, hypot, sin
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

os.environ.setdefault("OPENCV_LOG_LEVEL", "ERROR")  # esconde avisos internos do OpenCV

try:
    import cv2
    import numpy as np
except ImportError:
    raise SystemExit("Falta o OpenCV. Instale com:  pip install opencv-python numpy")

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    raise SystemExit("Falta o pyserial. Instale com:  pip install pyserial")

from registro import Registro  # banco das aparições (pc/registro.py)

try:  # opcional: nomes das câmeras no Windows ("Integrated Webcam"...)
    from pygrabber.dshow_graph import FilterGraph
except ImportError:
    FilterGraph = None

ROOT = Path(__file__).resolve().parent
HTML_FILE = ROOT / "painel.html"
MODELS_DIR = ROOT / "modelos"
FACES_DIR = ROOT / "rostos"
DB_FILE = FACES_DIR / "pessoas.json"
REG_FILE = ROOT / "dados" / "monitoramento.db"   # aparições (pc/registro.py)
RELATORIO_FILE = ROOT / "relatorios.html"

# Modelos do OpenCV Zoo (baixados sozinhos na primeira vez)
YUNET = "face_detection_yunet_2023mar.onnx"
SFACE = "face_recognition_sface_2021dec.onnx"
MODEL_URLS = {
    YUNET: "https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/" + YUNET,
    SFACE: "https://github.com/opencv/opencv_zoo/raw/main/models/face_recognition_sface/" + SFACE,
}

# ----- Serial -----
BAUD = 115200
POLL_S = 1.0          # pede STATUS à placa a cada 1 s
ANSWER_TIMEOUT_S = 3  # sem resposta nesse tempo = firmware não responde
CMD_TIMEOUT_S = 1.5   # espera máxima pela resposta de um comando
SEND_HZ = 20          # quantas vezes por segundo a pose vai para a placa

# ----- Visão -----
CAM_W, CAM_H = 640, 480
MAX_CAMS = 5            # sem os nomes do Windows, a busca testa as câmeras 0..MAX_CAMS-1
SCAN_TIMEOUT_S = 6      # tempo máximo da busca (câmera que não responde = ocupada)
DARK_MAX = 16           # imagem sem nenhum pixel acima disso = câmera tampada
DETECT_SCORE = 0.75     # confiança mínima do detector
MATCH_THRESHOLD = 0.40  # semelhança mínima (cosseno) para reconhecer; o SFace sugere 0,363
RECOG_EVERY = 5         # reconhece cada rosto a cada N quadros (o resto do tempo só segue)
MAX_RECOG = 4           # no máximo N rostos reconhecidos por quadro
TRACK_KEEP_S = 0.7      # um rosto que sumiu por menos que isso ainda é "o mesmo"
SURPRISE_S = 1.2        # duração da cara de surpresa quando alguém aparece
ABSENT_S = 3.0          # ... se antes ficou esse tempo sem ninguém
SMOOTH = 0.5            # suavização da pose no PC (0 = parado, 1 = sem suavizar)
ENROLL_SAMPLES = 15     # fotos por cadastro
ENROLL_TIMEOUT_S = 20
ENROLL_MIN_W = 0.12     # largura mínima do rosto (fração da imagem) para cadastrar
MAX_SAMPLES = 60        # máximo de amostras guardadas por pessoa
NAME_MAX = 20           # cabe numa linha do OLED
FACE_MAX = 8            # emojis que cabem no OLED (= FACE_MAX de src/face.h)
BOOT_ID = f"{time.time():.3f}"  # identifica esta execução do programa

# Expressões (a mesma ordem de src/face.h e pc/painel.html)
MOODS = ["Neutro", "Feliz", "Curioso", "Surpreso", "Bravo", "Triste"]
MOOD_FELIZ, MOOD_CURIOSO, MOOD_SURPRESO = 1, 2, 3

# Chips USB-serial comuns em placas ESP32 (VID -> nome)
KNOWN_VIDS = {
    0x10C4: "Silicon Labs CP210x",
    0x1A86: "WCH CH340/CH9102",
    0x0403: "FTDI",
    0x303A: "Espressif USB nativo",
}


def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def to_oled_text(text, limit=NAME_MAX):
    """Prepara um texto para a fonte do OLED (tabela CP437).

    Ela tem á é í ó ú â ê ô à ç ü ñ, mas não tem ã õ nem maiúsculas acentuadas:
    o que não existe vira a letra sem acento (ã -> a); o resto, '?'.
    Retorna (bytes para a placa, texto como vai aparecer).
    """
    out = bytearray()
    for ch in " ".join(text.split()):
        try:
            b = ch.encode("cp437")
        except UnicodeEncodeError:
            base = unicodedata.normalize("NFKD", ch)
            base = "".join(c for c in base if not unicodedata.combining(c))
            try:
                b = base.encode("cp437") if base else b"?"
            except UnicodeEncodeError:
                b = b"?"
        if not b or b[0] < 0x20 or b in (b'"', b"\\", b"|"):  # '|' separa os nomes
            b = b"?"
        out += b
    out = bytes(out[:limit]).rstrip()
    return out, out.decode("cp437")


def ascii_text(text):
    """Texto sem acentos (o putText do OpenCV não desenha acentos)."""
    t = unicodedata.normalize("NFKD", text)
    return "".join(c for c in t if not unicodedata.combining(c)).encode("ascii", "replace").decode()


def ensure_models():
    MODELS_DIR.mkdir(exist_ok=True)
    for name, url in MODEL_URLS.items():
        path = MODELS_DIR / name
        if path.exists() and path.stat().st_size > 0:
            continue
        print(f"Baixando o modelo {name} (só na primeira vez)...")
        tmp = path.with_suffix(".tmp")
        try:
            urllib.request.urlretrieve(url, tmp)
            tmp.replace(path)
        except Exception as e:
            tmp.unlink(missing_ok=True)
            raise SystemExit(f"Não consegui baixar {name}: {e}\n"
                             f"Baixe manualmente de {url}\ne salve em {path}")


# ===========================================================================
# Placa (porta serial)
# ===========================================================================
class Board:
    """Mantém a conexão serial com o ESP32 numa thread própria."""

    def __init__(self, forced_port=None):
        self.lock = threading.Lock()
        self.answer = threading.Condition(self.lock)
        self.ser = None
        self.port_info = None       # dados da porta aberta
        self.wanted = forced_port   # None = automático
        self.released = False       # usuário soltou a porta
        self.last_error = ""
        self.status = None          # último JSON de status da placa
        self.last_answer_at = 0.0
        self.answer_seq = 0
        self.last_reply = None
        self.connected_at = None
        self.log = deque(maxlen=60)
        self.tx_lock = threading.Lock()  # um comando com resposta por vez
        self.w_lock = threading.Lock()   # uma linha escrita por vez
        threading.Thread(target=self._run, daemon=True).start()

    # ----- descoberta de portas -----
    @staticmethod
    def list_ports():
        out = []
        for p in sorted(list_ports.comports(), key=lambda p: p.device):
            out.append({
                "name": p.device,
                "description": p.description,
                "vidpid": f"{p.vid:04X}:{p.pid:04X}" if p.vid is not None else "",
                "chip": KNOWN_VIDS.get(p.vid, ""),
                "serial": p.serial_number or "",
                "likelyEsp32": p.vid in KNOWN_VIDS,
            })
        return out

    def _pick_port(self):
        ports = self.list_ports()
        if self.wanted:
            return next((p for p in ports if p["name"].upper() == self.wanted.upper()), None)
        return next((p for p in ports if p["likelyEsp32"]), None)

    # ----- abrir/fechar -----
    def _open(self, info):
        s = serial.Serial()
        s.port = info["name"]
        s.baudrate = BAUD
        s.timeout = 0.1
        s.write_timeout = 1
        # DTR/RTS desligados antes de abrir: assim a placa não reinicia
        s.dtr = False
        s.rts = False
        s.open()
        with self.lock:
            self.ser = s
            self.port_info = info
            self.connected_at = time.time()
            self.last_error = ""
            self.status = None
            self.last_answer_at = 0.0
        self._add_log(f"[PC] Porta {info['name']} aberta ({info['description']})")

    def _close(self, reason=""):
        with self.lock:
            s, self.ser = self.ser, None
            name = self.port_info["name"] if self.port_info else "?"
            self.port_info = None
            self.status = None
            self.connected_at = None
            self.answer.notify_all()
        if s:
            try:
                s.close()
            except Exception:
                pass
            self._add_log(f"[PC] Porta {name} fechada" + (f": {reason}" if reason else ""))

    def _add_log(self, line):
        self.log.append({"t": time.time(), "line": line})

    # ----- laço principal -----
    def _run(self):
        buf = b""
        last_poll = 0.0
        while True:
            if self.released:
                if self.ser:
                    self._close("liberada pelo usuário")
                time.sleep(0.3)
                continue

            if not self.ser:
                info = self._pick_port()
                if not info:
                    with self.lock:
                        self.last_error = (f"Porta {self.wanted} não encontrada" if self.wanted
                                           else "Nenhuma placa ESP32 encontrada nas portas USB")
                    time.sleep(1)
                    continue
                try:
                    self._open(info)
                    buf = b""
                    last_poll = 0.0
                except Exception as e:
                    msg = str(e)
                    if "PermissionError" in msg or "Acesso negado" in msg or "Access is denied" in msg:
                        msg = f"{info['name']} está ocupada por outro programa (Monitor Serial, upload...)"
                    with self.lock:
                        self.last_error = msg
                    time.sleep(1.5)
                    continue

            # Troca de porta pedida pelo usuário
            if self.wanted and self.port_info and self.port_info["name"].upper() != self.wanted.upper():
                self._close("troca de porta")
                continue

            try:
                if time.time() - last_poll >= POLL_S:
                    last_poll = time.time()
                    # Se um comando estiver em andamento, pula este STATUS
                    if self.tx_lock.acquire(blocking=False):
                        try:
                            self._write("STATUS")
                        finally:
                            self.tx_lock.release()
                chunk = self.ser.read(256)
            except Exception as e:
                self._close(f"placa desconectada ({e.__class__.__name__})")
                with self.lock:
                    self.last_error = "A placa foi desconectada do USB"
                continue

            if chunk:
                buf += chunk
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    try:
                        line = raw.decode("utf-8")
                    except UnicodeDecodeError:
                        line = raw.decode("cp437")  # nomes acentuados vêm em CP437
                    self._handle_line(line.strip())

    def _handle_line(self, line):
        if not line:
            return
        if line.startswith("@"):
            try:
                data = json.loads(line[1:])
            except ValueError:
                self._add_log("[?] " + line)
                return
            with self.lock:
                self.last_answer_at = time.time()
                if "error" not in data:
                    self.status = data
                self.last_reply = data
                self.answer_seq += 1
                self.answer.notify_all()
            if "error" in data:
                self._add_log("[ERRO] " + data["error"])
        else:
            self._add_log(line)

    def _write(self, cmd):
        s = self.ser
        if not s:
            raise IOError("porta fechada")
        data = cmd if isinstance(cmd, bytes) else cmd.encode()
        with self.w_lock:
            s.write(data + b"\n")

    # ----- usado pelo resto do programa -----
    def answering(self):
        with self.lock:
            return self.ser is not None and time.time() - self.last_answer_at < ANSWER_TIMEOUT_S

    def send(self, cmd):
        """Manda um comando sem esperar resposta (L e IDLE, várias vezes por segundo)."""
        try:
            self._write(cmd)
            return True
        except Exception:
            return False  # o laço principal percebe a desconexão

    def command(self, cmd, done):
        """Envia um comando e espera uma resposta '@...' em que done(resposta)
        seja verdadeiro (ou um erro). Respostas de STATUS automáticos que
        chegarem no meio são ignoradas."""
        with self.tx_lock:
            with self.lock:
                if not self.ser:
                    return None, "Placa não conectada"
                seq = self.answer_seq
            try:
                self._write(cmd)
            except Exception as e:
                return None, f"Falha ao enviar: {e}"
            shown = cmd.decode("cp437") if isinstance(cmd, bytes) else cmd
            self._add_log(f"[PC] >> {shown}")
            deadline = time.time() + CMD_TIMEOUT_S
            with self.lock:
                while self.ser and time.time() < deadline:
                    if self.answer_seq != seq:
                        seq = self.answer_seq
                        reply = self.last_reply
                        if "error" in reply:
                            return None, "Placa: " + reply["error"]
                        if done(reply):
                            return reply, None
                    self.answer.wait(max(0.0, deadline - time.time()))
            return None, "A placa não respondeu"

    def snapshot(self):
        with self.lock:
            now = time.time()
            port_open = self.ser is not None
            answering = port_open and (now - self.last_answer_at) < ANSWER_TIMEOUT_S
            return {
                "portOpen": port_open,
                "boardAnswering": answering,
                "port": self.port_info,
                "baud": BAUD,
                "mode": "manual" if self.wanted else "auto",
                "wantedPort": self.wanted,
                "released": self.released,
                "openSince": self.connected_at,
                "lastAnswerAgo": round(now - self.last_answer_at, 1) if self.last_answer_at else None,
                "error": "" if answering else self.last_error,
                "board": self.status if answering else None,
                "ports": self.list_ports(),
                "log": list(self.log)[-30:],
                "serverTime": now,
            }


# ===========================================================================
# Pessoas cadastradas
# ===========================================================================
class FaceDB:
    """Guarda as "impressões" (vetores de 128 números do SFace) de cada pessoa
    em pc/rostos/pessoas.json, mais uma foto pequena para o painel."""

    def __init__(self):
        self.lock = threading.Lock()
        self.people = {}  # nome -> {"emb": array (N, 128), "foto": "arquivo.jpg", "criado": ts}
        if DB_FILE.exists():
            data = json.loads(DB_FILE.read_text(encoding="utf-8"))
            for name, p in data.get("pessoas", {}).items():
                emb = np.array(p["embeddings"], dtype=np.float32).reshape(-1, 128)
                self.people[name] = {"emb": emb, "foto": p.get("foto", ""), "criado": p.get("criado", 0)}
        self._rebuild()

    def _rebuild(self):
        rows, labels = [], []
        for name, p in self.people.items():
            rows.append(p["emb"])
            labels += [name] * len(p["emb"])
        self.matrix = np.vstack(rows) if rows else np.zeros((0, 128), np.float32)
        self.labels = labels

    def _save(self):
        FACES_DIR.mkdir(exist_ok=True)
        data = {"pessoas": {
            name: {"embeddings": np.round(p["emb"], 5).tolist(), "foto": p["foto"], "criado": p["criado"]}
            for name, p in self.people.items()}}
        tmp = DB_FILE.with_suffix(".tmp")
        tmp.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
        tmp.replace(DB_FILE)

    def add(self, name, embs, photo_jpg):
        with self.lock:
            embs = np.asarray(embs, dtype=np.float32)
            old = self.people.get(name)
            if old:  # já existe: junta as amostras (melhora o reconhecimento)
                embs = np.vstack([old["emb"], embs])[-MAX_SAMPLES:]
                foto = old["foto"]
            else:
                slug = re.sub(r"[^a-z0-9]+", "_", ascii_text(name).lower()).strip("_") or "pessoa"
                foto = f"{slug}_{int(time.time())}.jpg"
            FACES_DIR.mkdir(exist_ok=True)
            if photo_jpg is not None:
                (FACES_DIR / foto).write_bytes(photo_jpg)
            self.people[name] = {"emb": embs, "foto": foto,
                                 "criado": old["criado"] if old else time.time()}
            self._save()
            self._rebuild()

    def remove(self, name):
        with self.lock:
            p = self.people.pop(name, None)
            if not p:
                return False
            if p["foto"]:
                (FACES_DIR / p["foto"]).unlink(missing_ok=True)
            self._save()
            self._rebuild()
            return True

    def match(self, feat):
        """Retorna (nome, semelhança) da pessoa mais parecida, ou ("", semelhança)."""
        with self.lock:
            if not len(self.matrix):
                return "", 0.0
            sims = self.matrix @ feat
            i = int(np.argmax(sims))
            best = float(sims[i])
            return (self.labels[i] if best >= MATCH_THRESHOLD else ""), best

    def list(self):
        with self.lock:
            return [{"nome": n, "amostras": len(p["emb"]), "foto": p["foto"], "criado": p["criado"]}
                    for n, p in sorted(self.people.items(), key=lambda kv: kv[0].lower())]

    def photo_path(self, filename):
        with self.lock:
            if any(p["foto"] == filename for p in self.people.values()):
                return FACES_DIR / filename
        return None


# ===========================================================================
# Câmera e reconhecimento
# ===========================================================================
def camera_names():
    """Nomes das câmeras na ordem do DirectShow (= o número usado pelo OpenCV).
    Precisa do pygrabber; sem ele a lista vem vazia e as câmeras viram "Câmera N"."""
    if FilterGraph is None:
        return []
    try:
        import comtypes
        comtypes.CoInitialize()  # esta função roda fora da thread principal
        return list(FilterGraph().get_input_devices())
    except Exception:
        return []


def iou(a, b):
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    ix = max(0.0, min(ax + aw, bx + bw) - max(ax, bx))
    iy = max(0.0, min(ay + ah, by + bh) - max(ay, by))
    inter = ix * iy
    union = aw * ah + bw * bh - inter
    return inter / union if union > 0 else 0.0


def head_pose(f):
    """Estima a pose da cabeça a partir dos 5 pontos do YuNet.

    Retorna (r, t, roll):
      r     deslocamento do nariz para o lado / distância entre os olhos
            (0 de frente; vira yaw com atan)
      t     altura do nariz entre a linha dos olhos (0) e a da boca (1)
            (menor = olhando para cima)
      roll  inclinação da linha dos olhos, em graus (positivo = sentido horário)
    """
    (ax, ay), (bx, by) = sorted([(f[4], f[5]), (f[6], f[7])])  # olho da esquerda e da direita
    roll = atan2(by - ay, bx - ax)
    mx, my = (ax + bx) / 2, (ay + by) / 2
    d = max(hypot(bx - ax, by - ay), 1.0)
    c, s = cos(-roll), sin(-roll)

    def unroll(px, py):  # tira a inclinação: gira em volta do meio dos olhos
        dx, dy = px - mx, py - my
        return dx * c - dy * s, dx * s + dy * c

    nx, ny = unroll(f[8], f[9])
    _, mouth_y = unroll((f[10] + f[12]) / 2, (f[11] + f[13]) / 2)
    t = ny / mouth_y if mouth_y > 1 else 0.5
    return nx / d, t, degrees(roll)


class Vision:
    def __init__(self, cam_index, db, reg):
        self.db = db
        self.reg = reg               # banco das aparições
        self.det = cv2.FaceDetectorYN.create(str(MODELS_DIR / YUNET), "", (320, 320), DETECT_SCORE, 0.3, 50)
        self.rec = cv2.FaceRecognizerSF.create(str(MODELS_DIR / SFACE), "")
        self.lock = threading.Lock()
        self.frame_cond = threading.Condition()
        self.jpeg = None
        self.jpeg_seq = 0

        self.want_cam = cam_index
        self.cam_fixed = cam_index is not None  # --camera: não troca sozinho
        if cam_index is None:
            self.want_cam = 0
        self.cam_index = None
        self.cams = []               # resultado da última busca de câmeras
        self.dark = False            # a câmera aberta só manda imagem preta?
        self.scanning = True         # a 1ª busca roda quando o programa abre
        self.scan_req = True
        self.scan_time = 0.0
        self.cam_ok = False
        self.cam_error = ""
        self.frame_size = (0, 0)
        self.fps = 0.0
        self.mirror = True
        self.mood_override = -1
        self.max_faces = FACE_MAX    # quantos emojis no visor (1..FACE_MAX)
        # Calibração: valores de r e t com a pessoa olhando reto para a câmera
        self.calib = {"r": 0.0, "t": 0.55}

        self.tracks = []
        self.visible = []
        self.next_id = 1
        self.last_seen = 0.0         # última vez em que havia um rosto
        self.raw = None              # (r, t) sem calibração, para o botão Calibrar
        self.target = None           # pessoa principal (a mais perto): medidores e Calibrar
        self.targets = []            # o que vai para a placa: 1 dict por emoji, da esquerda p/ direita
        self.enroll = None
        threading.Thread(target=self._run, daemon=True).start()

    # ----- câmera -----
    def _open_camera(self, idx):
        cap = cv2.VideoCapture(idx, cv2.CAP_DSHOW) if os.name == "nt" else cv2.VideoCapture(idx)
        if not cap.isOpened():
            cap.release()
            cap = cv2.VideoCapture(idx)
        if not cap.isOpened():
            cap.release()
            return None
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, CAM_W)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, CAM_H)
        return cap

    def request_scan(self):
        self.scanning = True
        self.scan_req = True

    def _probe(self, i, out):
        """Abre a câmera i, lê um quadro e fecha. Guarda (largura, altura) em out[i]."""
        c = self._open_camera(i)
        if c is None:
            return
        size, dark = None, True
        for _ in range(8):  # os primeiros quadros podem vir pretos enquanto a câmera liga
            ok, f = c.read()
            if ok and f is not None:
                size = (f.shape[1], f.shape[0])
                if f.max() > DARK_MAX:
                    dark = False
                    break
        c.release()
        if size:
            out[i] = (size, dark)

    def _scan_cameras(self, cap):
        """Testa as câmeras (a que já está aberta não é reaberta). Todas ao mesmo
        tempo e com limite de tempo: uma câmera ocupada por outro programa trava
        o OpenCV por ~30 s, e a busca não pode ficar esperando por ela."""
        names = camera_names()
        indices = range(len(names)) if names else range(MAX_CAMS)
        sizes, threads = {}, []
        for i in indices:
            if i == self.cam_index and cap is not None:
                sizes[i] = (self.frame_size, self.dark)
                continue
            th = threading.Thread(target=self._probe, args=(i, sizes), daemon=True)
            th.start()
            threads.append(th)
        limit = time.time() + SCAN_TIMEOUT_S
        for th in threads:
            th.join(max(0.0, limit - time.time()))

        found = []
        for i in indices:
            name = names[i] if i < len(names) else f"Câmera {i}"
            if i in sizes:
                size, dark = sizes[i]
                found.append({"index": i, "name": name, "size": [int(v) for v in size],
                              "busy": False, "dark": bool(dark)})
            elif names:  # o Windows diz que existe, mas não abriu: está em uso
                found.append({"index": i, "name": name, "size": None, "busy": True, "dark": False})
        self.cams = found
        self.scan_time = time.time()
        ok = [c["index"] for c in found if not c["busy"]]
        lit = [c["index"] for c in found if not c["busy"] and not c["dark"]]
        estado = lambda c: " (ocupada)" if c["busy"] else " (imagem preta)" if c["dark"] else ""
        lista = ", ".join("%d = %s%s" % (c["index"], c["name"], estado(c)) for c in found)
        print(f"Câmeras encontradas: {lista or 'nenhuma'}")
        # Se a câmera escolhida não existe ou só manda preto, usa a primeira com imagem
        if not self.cam_fixed:
            if lit and self.want_cam not in lit:
                self.want_cam = lit[0]
            elif ok and self.want_cam not in ok:
                self.want_cam = ok[0]
        self.scanning = False

    def camera_name(self):
        c = next((c for c in self.cams if c["index"] == self.cam_index), None)
        return f"{self.cam_index} · {c['name']}" if c else f"Câmera {self.cam_index}"

    def _set_no_face(self):
        # Sem imagem: ninguém pode ser visto, então fecha as aparições abertas
        self.reg.encerrar(self.tracks)
        self.tracks = []
        with self.lock:
            self.visible = []
            self.target = None
            self.targets = []

    def _run(self):
        cap = None
        n, t0 = 0, time.time()
        while True:
            # A busca roda nesta mesma thread: assim nunca disputa a câmera aberta
            if self.scan_req:
                self.scan_req = False
                try:
                    self._scan_cameras(cap)
                except Exception as e:
                    print(f"Falha ao buscar câmeras: {e}")
                    self.scanning = False
            if cap is None or self.want_cam != self.cam_index:
                if cap is not None:
                    cap.release()
                self.cam_index = self.want_cam
                cap = self._open_camera(self.cam_index)
                if cap is None:
                    self.cam_ok = False
                    self.cam_error = (f"Câmera {self.cam_index} não encontrada ou ocupada por outro "
                                      f"programa (Teams, Zoom, Câmera do Windows...)")
                    self._set_no_face()
                    time.sleep(2)
                    continue
                self.cam_ok, self.cam_error, self.dark = True, "", False
                print(f"Câmera {self.cam_index} aberta.")

            ok, frame = cap.read()
            if not ok or frame is None:
                self.cam_ok = False
                self.cam_error = f"A câmera {self.cam_index} parou de mandar imagens"
                self._set_no_face()
                cap.release()
                cap = None
                time.sleep(1)
                continue
            self.dark = bool(frame[::8, ::8].max() <= DARK_MAX)

            if self.mirror:
                frame = cv2.flip(frame, 1)
            try:
                self._process(frame)
            except Exception as e:  # nunca deixa a thread da câmera morrer
                self.cam_error = f"Erro no processamento: {e}"
                time.sleep(0.2)

            n += 1
            if time.time() - t0 >= 1.0:
                self.fps = n / (time.time() - t0)
                n, t0 = 0, time.time()

    # ----- reconhecimento -----
    def _feature(self, frame, face):
        crop = self.rec.alignCrop(frame, face)
        feat = self.rec.feature(crop).flatten().astype(np.float32)
        feat /= (np.linalg.norm(feat) or 1.0)
        return feat, crop

    def _update_tracks(self, faces, now):
        visible, free = [], list(self.tracks)
        for f in sorted(faces, key=lambda f: -f[2] * f[3]):  # maiores primeiro
            box = tuple(float(v) for v in f[:4])
            best = max(free, key=lambda t: iou(t["box"], box), default=None)
            if best is not None and iou(best["box"], box) > 0.25:
                free.remove(best)
                best.update(box=box, face=f, last=now)
                visible.append(best)
            else:
                # Surpresa: alguém apareceu depois de um tempo sem ninguém, ou
                # chegou uma pessoa nova enquanto outras já estavam na câmera
                surprise = now - self.last_seen > ABSENT_S or bool(visible)
                visible.append({"id": self.next_id, "box": box, "face": f, "first": now, "last": now,
                                "votes": deque(maxlen=5), "name": "", "score": 0.0, "since": RECOG_EVERY,
                                "pose": None, "surprise": now + SURPRISE_S if surprise else 0.0})
                self.next_id += 1
        # Os que sumiram há pouco continuam guardados (o detector às vezes pisca)
        self.tracks = visible + [t for t in free if now - t["last"] < TRACK_KEEP_S]
        # Sumiu de vez: fecha a aparição no banco
        self.reg.encerrar([t for t in free if now - t["last"] >= TRACK_KEEP_S])
        return visible

    def _process(self, frame):
        h, w = frame.shape[:2]
        now = time.time()
        self.det.setInputSize((w, h))
        _, faces = self.det.detect(frame)
        faces = [] if faces is None else list(faces)
        visible = self._update_tracks(faces, now)

        # Reconhece no máximo MAX_RECOG rostos por quadro (os que esperam há mais
        # tempo primeiro): com muita gente, todos são reconhecidos, só que mais devagar
        for tr in visible:
            tr["since"] += 1
        due = sorted((t for t in visible if t["since"] >= RECOG_EVERY), key=lambda t: -t["since"])
        for tr in due[:MAX_RECOG]:
            feat, _ = self._feature(frame, tr["face"])
            name, score = self.db.match(feat)
            tr["votes"].append(name)
            self.reg.votar(tr, name, score)
            tr["score"] = score
            tr["since"] = 0
            tr["name"] = Counter(tr["votes"]).most_common(1)[0][0]

        target = visible[0] if visible else None  # o maior rosto = o mais perto
        if target is not None:
            self.last_seen = now
        self.reg.atualizar(visible, self.camera_name(), frame, now)
        self._update_targets(visible, w, h, now)
        self._enroll_step(frame, visible, now)
        self._publish(frame, visible, target)

    def _update_targets(self, visible, w, h, now):
        """Um alvo por emoji: as max_faces pessoas mais perto, da esquerda para a direita."""
        if not visible:
            with self.lock:
                self.visible = []
                self.target = None
                self.targets = []
            return
        if visible[0]["face"] is not None:
            r, t, _ = head_pose(visible[0]["face"])
            self.raw = (r, t)
        chosen = visible[:self.max_faces]            # já vêm do maior para o menor
        main = self._target_for(chosen[0], w, h, now)
        targets = [main] + [self._target_for(tr, w, h, now) for tr in chosen[1:]]
        targets.sort(key=lambda t: t["x"])
        with self.lock:
            self.target = main
            self.targets = targets

    def _target_for(self, tr, w, h, now):
        x, y, fw, fh = tr["box"]
        r, t, roll = head_pose(tr["face"])
        raw = {
            "x": ((x + fw / 2) / w * 2 - 1) * 100,
            "y": ((y + fh / 2) / h * 2 - 1) * 100,
            # O nariz fica ~0,45 "distância entre olhos" à frente do rosto
            "yaw": degrees(atan((r - self.calib["r"]) / 0.45)),
            "pitch": (self.calib["t"] - t) * 120,
            "roll": roll,
            "size": (fw / w - 0.10) / (0.40 - 0.10) * 100,
        }
        p = tr["pose"]   # cada pessoa tem a sua suavização
        if p is None:
            p = raw
        else:
            p = {k: p[k] + (raw[k] - p[k]) * SMOOTH for k in raw}
        tr["pose"] = p

        if self.mood_override >= 0:
            mood = self.mood_override
        elif now < tr["surprise"]:
            mood = MOOD_SURPRESO
        elif tr["name"]:
            mood = MOOD_FELIZ
        else:
            mood = MOOD_CURIOSO

        return {
            "id": tr["id"],
            "x": int(round(clamp(p["x"], -100, 100))),
            "y": int(round(clamp(p["y"], -100, 100))),
            "yaw": int(round(clamp(p["yaw"], -60, 60))),
            "pitch": int(round(clamp(p["pitch"], -45, 45))),
            "roll": int(round(clamp(p["roll"], -45, 45))),
            "size": int(round(clamp(p["size"], 0, 100))),
            "mood": mood,
            "name": tr["name"],
            "known": bool(tr["name"]),
            "score": round(tr["score"], 3),
        }

    # ----- cadastro -----
    def start_enroll(self, name):
        with self.lock:
            self.enroll = {"name": name, "state": "capturando", "samples": [], "photo": None,
                           "next": 0.0, "deadline": time.time() + ENROLL_TIMEOUT_S,
                           "msg": "Olhe para a câmera..."}

    def cancel_enroll(self):
        with self.lock:
            if self.enroll and self.enroll["state"] == "capturando":
                self.enroll.update(state="cancelado", msg="Cadastro cancelado")

    def _enroll_step(self, frame, visible, now):
        en = self.enroll
        if not en or en["state"] != "capturando":
            return
        if now > en["deadline"]:
            en.update(state="erro", msg="Tempo esgotado. Fique de frente para a câmera, "
                                        "com o rosto bem iluminado, e tente de novo.")
            return
        if not visible:
            en["msg"] = "Nenhum rosto na câmera"
            return
        if len(visible) > 1:
            en["msg"] = "Tem mais de um rosto: fique só você na frente da câmera"
            return
        if visible[0]["box"][2] < ENROLL_MIN_W * frame.shape[1]:
            en["msg"] = "Chegue mais perto da câmera"
            return
        if now < en["next"]:
            return
        feat, crop = self._feature(frame, visible[0]["face"])
        en["samples"].append(feat)
        if en["photo"] is None:
            en["photo"] = cv2.imencode(".jpg", crop, [cv2.IMWRITE_JPEG_QUALITY, 90])[1].tobytes()
        en["next"] = now + 0.15
        en["msg"] = "Capturando... mexa a cabeça devagar (um pouco para os lados, cima e baixo)"
        if len(en["samples"]) >= ENROLL_SAMPLES:
            self.db.add(en["name"], en["samples"], en["photo"])
            visible[0]["votes"].clear()       # reconhece de novo já com o cadastro novo
            if "sess" in visible[0]:          # e a aparição atual passa a ser dessa pessoa
                visible[0]["sess"]["votos"].clear()
            visible[0]["since"] = RECOG_EVERY
            en.update(state="ok", msg=f"{en['name']} cadastrado(a)!")
            print(f"Pessoa cadastrada: {en['name']}")

    # ----- imagem para o painel -----
    def _publish(self, frame, visible, target):
        img = frame  # desenha por cima (o quadro não é mais usado)
        with self.lock:
            on_oled = {t["id"] for t in self.targets}   # rostos que viram emoji: borda grossa
        for tr in visible:
            x, y, fw, fh = (int(v) for v in tr["box"])
            known = bool(tr["name"])
            color = (94, 197, 34) if known else (11, 158, 245)  # verde / laranja (BGR)
            thick = 3 if tr["id"] in on_oled else 1
            cv2.rectangle(img, (x, y), (x + fw, y + fh), color, thick)
            label = ascii_text(tr["name"]) if known else "Desconhecido"
            label += f"  {tr['score'] * 100:.0f}%"
            (tw, th), _ = cv2.getTextSize(label, cv2.FONT_HERSHEY_SIMPLEX, 0.55, 1)
            cv2.rectangle(img, (x, y - th - 10), (x + tw + 10, y), color, -1)
            cv2.putText(img, label, (x + 5, y - 6), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (15, 17, 21), 1,
                        cv2.LINE_AA)
            if tr is target:  # os 5 pontos usados para calcular a pose
                f = tr["face"]
                for i in range(5):
                    cv2.circle(img, (int(f[4 + 2 * i]), int(f[5 + 2 * i])), 3, (246, 130, 59), -1,
                               cv2.LINE_AA)
        ok, jpg = cv2.imencode(".jpg", img, [cv2.IMWRITE_JPEG_QUALITY, 75])
        if not ok:
            return
        with self.lock:
            self.visible = [{
                "id": tr["id"], "name": tr["name"], "score": round(tr["score"], 3),
                "box": [round(v / s, 4) for v, s in zip(tr["box"], (img.shape[1], img.shape[0]) * 2)],
            } for tr in visible]
            self.frame_size = (img.shape[1], img.shape[0])
        with self.frame_cond:
            self.jpeg = jpg.tobytes()
            self.jpeg_seq += 1
            self.frame_cond.notify_all()

    def wait_frame(self, last_seq, timeout=2.0):
        with self.frame_cond:
            if self.jpeg_seq == last_seq:
                self.frame_cond.wait(timeout)
            if self.jpeg_seq == last_seq:
                return None, last_seq
            return self.jpeg, self.jpeg_seq

    # ----- comandos do painel -----
    def calibrate(self):
        if self.raw is None:
            return False
        self.calib = {"r": float(self.raw[0]), "t": float(self.raw[1])}
        return True

    def get_targets(self):
        with self.lock:
            return [dict(t) for t in self.targets]

    def light(self):
        with self.lock:
            en = self.enroll
            return {
                "target": dict(self.target) if self.target else None,
                "targets": [dict(t) for t in self.targets],
                "maxFaces": self.max_faces,
                "faces": list(self.visible),
                "enroll": None if not en else {
                    "name": en["name"], "state": en["state"], "msg": en["msg"],
                    "count": len(en["samples"]), "total": ENROLL_SAMPLES,
                },
                "t": time.time(),
            }

    def snapshot(self):
        s = self.light()
        s.update({
            "camOk": self.cam_ok,
            "camIndex": self.want_cam,
            "cams": self.cams,
            "camDark": bool(self.dark),
            "scanning": self.scanning,
            "camError": self.cam_error,
            "fps": round(self.fps, 1),
            "frameSize": list(self.frame_size),
            "mirror": self.mirror,
            "moodOverride": self.mood_override,
            "moods": MOODS,
            "faceMax": FACE_MAX,
            "calib": {k: round(v, 3) for k, v in self.calib.items()},
            "threshold": MATCH_THRESHOLD,
        })
        return s


# ===========================================================================
# Envio para a placa
# ===========================================================================
def sender(board, vision):
    """Manda as poses ~20x/s (M n ...) ou IDLE quando não há ninguém; e o
    NAMES sempre que os nomes na placa forem diferentes dos desejados."""
    last_idle = 0.0
    last_name_try = 0.0
    while True:
        time.sleep(1 / SEND_HZ)
        if not board.answering():
            continue
        now = time.time()
        tgts = vision.get_targets()

        # Nomes na mesma ordem dos emojis; os que sobram ficam vazios
        texts = [to_oled_text(t["name"]) for t in tgts] + [(b"", "")] * (FACE_MAX - len(tgts))
        want = [shown for _, shown in texts]
        with board.lock:
            on_board = board.status.get("names") if board.status else None
        if on_board is not None and list(on_board) != want and now - last_name_try > 1.0:
            last_name_try = now
            data = b"|".join(d for d, _ in texts[:len(tgts)])
            board.command(b"NAMES " + data if data else b"NAMES", lambda r: "names" in r)
            continue

        if tgts:
            parts = [f"M {len(tgts)}"] + [
                f"{t['x']} {t['y']} {t['yaw']} {t['pitch']} {t['roll']} {t['size']} {t['mood']} "
                f"{int(t['known'])}" for t in tgts]
            board.send(" ".join(parts))
        elif now - last_idle > 1.0:
            last_idle = now
            board.send("IDLE")


# ===========================================================================
# Servidor HTTP do painel
# ===========================================================================
def make_handler(board, vision, db, reg):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass  # silencia o log de cada requisição

        def _send(self, code, body, ctype="application/json; charset=utf-8", download=None, cache=False):
            data = body if isinstance(body, bytes) else json.dumps(body, ensure_ascii=False).encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Cache-Control", "max-age=86400" if cache else "no-store")
            if download:
                self.send_header("Content-Disposition", f'attachment; filename="{download}"')
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def _status(self):
            s = board.snapshot()
            s["vision"] = vision.snapshot()
            s["people"] = db.list()
            s["boot"] = BOOT_ID  # muda quando o programa reinicia: o painel reabre o vídeo
            return s

        def _stream(self):
            self.send_response(200)
            self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=quadro")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            seq = -1
            try:
                while True:
                    jpg, seq = vision.wait_frame(seq)
                    if jpg is None:
                        continue
                    self.wfile.write(b"--quadro\r\nContent-Type: image/jpeg\r\nContent-Length: "
                                     + str(len(jpg)).encode() + b"\r\n\r\n" + jpg + b"\r\n")
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, OSError):
                pass  # a página foi fechada

        def _filtro(self, q):
            """de/ate (AAAA-MM-DD) e pessoa da query string; ValueError se a data for inválida."""
            de, ate = q.get("de") or None, q.get("ate") or None
            for d in (de, ate):
                if d:
                    datetime.strptime(d, "%Y-%m-%d")
            return de, ate, q.get("pessoa") or None

        def do_GET(self):
            url = urlparse(self.path)
            path = url.path
            q = {k: v[0] for k, v in parse_qs(url.query).items()}
            if path in ("/", "/index.html"):
                self._send(200, HTML_FILE.read_bytes(), "text/html; charset=utf-8")
            elif path == "/relatorios":
                self._send(200, RELATORIO_FILE.read_bytes(), "text/html; charset=utf-8")
            elif path == "/api/aparicoes":
                n = q.get("n", "5")
                self._send(200, reg.recentes(min(int(n), 50) if n.isdigit() else 5))
            elif path.startswith("/api/aparicoes/foto/"):
                ident = path.rsplit("/", 1)[1]
                foto = reg.foto(int(ident)) if ident.isdigit() else None
                if foto:
                    self._send(200, foto, "image/jpeg", cache=True)  # a foto nunca muda
                else:
                    self._send(404, {"error": "foto não encontrada"})
            elif path in ("/api/relatorio", "/api/relatorio.csv"):
                try:
                    de, ate, pessoa = self._filtro(q)
                except ValueError:
                    return self._send(400, {"error": "datas no formato AAAA-MM-DD"})
                if path.endswith(".csv"):
                    nome = f"aparicoes_{de or 'inicio'}_a_{ate or 'hoje'}.csv"
                    self._send(200, reg.csv(de, ate, pessoa), "text/csv; charset=utf-8", download=nome)
                else:
                    pag = q.get("pagina", "1")
                    self._send(200, reg.relatorio(de, ate, pessoa, int(pag) if pag.isdigit() else 1))
            elif path == "/video":
                self._stream()
            elif path == "/api/status":
                self._send(200, self._status())
            elif path == "/api/alvo":
                self._send(200, vision.light())
            elif path.startswith("/foto/"):
                f = db.photo_path(unquote(path[6:]))
                if f and f.exists():
                    self._send(200, f.read_bytes(), "image/jpeg")
                else:
                    self._send(404, {"error": "foto não encontrada"})
            else:
                self._send(404, {"error": "rota não encontrada"})

        def do_POST(self):
            url = urlparse(self.path)
            q = {k: v[0] for k, v in parse_qs(url.query).items()}
            path = url.path
            # Lê o corpo sempre: responder sem ler faz o Windows derrubar a conexão
            length = int(self.headers.get("Content-Length") or 0)
            raw = self.rfile.read(length) if 0 < length <= 4096 else b""

            if path == "/api/cadastrar":
                try:
                    name = " ".join(str(json.loads(raw or b"{}").get("nome", "")).split())
                except (ValueError, AttributeError):
                    return self._send(400, {"error": 'envie JSON {"nome": "..."}'})
                if not name:
                    return self._send(400, {"error": "Digite o nome da pessoa"})
                if len(name) > NAME_MAX:
                    return self._send(400, {"error": f"Use no máximo {NAME_MAX} letras (é o que cabe no visor)"})
                if not vision.cam_ok:
                    return self._send(503, {"error": "A câmera não está funcionando"})
                vision.start_enroll(name)
            elif path == "/api/cancelar":
                vision.cancel_enroll()
            elif path == "/api/remover":
                if not db.remove(q.get("nome", "")):
                    return self._send(404, {"error": "Pessoa não encontrada"})
            elif path == "/api/humor":
                try:
                    m = int(q.get("id", "-1"))
                except ValueError:
                    m = -2
                if not -1 <= m < len(MOODS):
                    return self._send(400, {"error": f"id deve ser de -1 a {len(MOODS) - 1}"})
                vision.mood_override = m
            elif path == "/api/calibrar":
                if not vision.calibrate():
                    return self._send(409, {"error": "Nenhum rosto na câmera para calibrar"})
            elif path == "/api/espelhar":
                vision.mirror = q.get("on", "1") == "1"
            elif path == "/api/camera":
                i = q.get("index", "")
                if not i.isdigit() or int(i) > 9:
                    return self._send(400, {"error": "index deve ser de 0 a 9"})
                vision.want_cam = int(i)
            elif path == "/api/cameras/buscar":
                vision.request_scan()
            elif path == "/api/emojis":
                n = q.get("n", "")
                if not n.isdigit() or not 1 <= int(n) <= FACE_MAX:
                    return self._send(400, {"error": f"n deve ser de 1 a {FACE_MAX}"})
                vision.max_faces = int(n)
            elif path == "/api/port":
                name = q.get("name", "auto")
                board.wanted = None if name.lower() == "auto" else name
                board.released = False
            elif path == "/api/release":
                board.released = True
            elif path == "/api/resume":
                board.released = False
            else:
                return self._send(404, {"error": "rota não encontrada"})
            self._send(200, self._status())

    return Handler


def main():
    ap = argparse.ArgumentParser(description="Reconhecimento facial + emoji no OLED do ESP32")
    ap.add_argument("--port", help="porta serial (ex.: COM12). Padrão: automático")
    ap.add_argument("--camera", type=int, help="número da câmera (padrão: a primeira encontrada)")
    ap.add_argument("--http", type=int, default=8000, help="porta do site (padrão 8000)")
    ap.add_argument("--no-browser", action="store_true", help="não abre o navegador")
    args = ap.parse_args()

    ensure_models()
    db = FaceDB()
    reg = Registro(REG_FILE)
    vision = Vision(args.camera, db, reg)
    # Fechou o programa com gente na câmera: grava o fim dessas aparições
    atexit.register(lambda: reg.encerrar(vision.tracks))
    board = Board(args.port)
    threading.Thread(target=sender, args=(board, vision), daemon=True).start()

    server = ThreadingHTTPServer(("127.0.0.1", args.http), make_handler(board, vision, db, reg))
    url = f"http://localhost:{args.http}"
    print(f"Painel rodando em {url}  (Ctrl+C para sair)")
    if not args.no_browser:
        threading.Timer(0.8, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nEncerrando...")


if __name__ == "__main__":
    main()
