"""
acesso.py - acesso ao painel pela internet (fora de casa), com senha

O servidor do painel só escuta em 127.0.0.1. Para acessar de fora, o programa
abre um túnel do Cloudflare (cloudflared, "quick tunnel"): ele cria um endereço
https://<palavras>.trycloudflare.com que leva até este PC, sem abrir portas no
roteador. O endereço muda a cada vez que o túnel liga.

Regras:
  - Quem acessa pelo próprio PC (localhost) entra direto, como sempre.
  - Quem chega pelo túnel (ou por qualquer endereço que não seja localhost)
    precisa da senha. Sem senha definida, o túnel nem liga.
  - Senha, ligar/desligar o túnel e encerrar o programa só pelo próprio PC.
  - Errou a senha FALHAS_MAX vezes: aquele IP espera BLOQUEIO_S.

Arquivos (ficam só neste PC, pc/dados/ está no .gitignore):
  pc/dados/acesso.json     senha (PBKDF2 com sal) e se o túnel liga sozinho
  pc/bin/cloudflared.exe   baixado sozinho na primeira vez que o túnel liga
"""

import atexit
import hashlib
import hmac
import json
import os
import re
import secrets
import shutil
import subprocess
import threading
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent
CONFIG_FILE = ROOT / "dados" / "acesso.json"
BIN_DIR = ROOT / "bin"
CLOUDFLARED = BIN_DIR / "cloudflared.exe"
CLOUDFLARED_URL = ("https://github.com/cloudflare/cloudflared/releases/latest/download/"
                   "cloudflared-windows-amd64.exe")

SENHA_MIN = 8               # letras mínimas da senha
PBKDF2_ITER = 300_000
SESSAO_S = 7 * 86400        # login vale 7 dias (some se o programa reiniciar)
COOKIE = "sessao"
FALHAS_MAX = 5              # senhas erradas seguidas por IP ...
BLOQUEIO_S = 10 * 60        # ... e o tempo de espera depois disso
FALHAS_GERAL_MAX = 30       # erradas de todos os IPs juntos em BLOQUEIO_S: bloqueia todo mundo
RELIGAR_S = 5               # o túnel caiu sozinho: tenta de novo depois disso
URL_RE = re.compile(r"https://[a-z0-9-]+\.trycloudflare\.com")
LOCAL_HOSTS = ("localhost", "127.0.0.1", "[::1]", "::1")


def host_local(host):
    """True se o cabeçalho Host é deste PC (sem a porta)."""
    host = (host or "").strip().lower()
    if host.startswith("["):
        host = host.split("]")[0] + "]"
    else:
        host = host.rsplit(":", 1)[0]
    return host in LOCAL_HOSTS


def _hash(senha, sal, it=PBKDF2_ITER):
    return hashlib.pbkdf2_hmac("sha256", senha.encode(), bytes.fromhex(sal), it).hex()


class Tunel:
    """Processo do cloudflared. estado: desligado, baixando, conectando, no_ar, erro."""

    def __init__(self, porta):
        self.porta = porta
        self.quer = False
        self.estado = "desligado"
        self.url = None
        self.erro = None
        self.desde = None
        self._proc = None
        self._lock = threading.Lock()
        atexit.register(self.desligar)

    def snapshot(self):
        return {"on": self.quer, "estado": self.estado, "url": self.url,
                "erro": self.erro, "desde": self.desde}

    def ligar(self):
        with self._lock:
            if self.quer:
                return
            self.quer = True
            self.erro = None
        threading.Thread(target=self._run, daemon=True).start()

    def desligar(self):
        with self._lock:
            self.quer = False
            proc, self._proc = self._proc, None
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()
        self.estado, self.url, self.desde = "desligado", None, None

    def _exe(self):
        achado = shutil.which("cloudflared")
        if achado:
            return achado
        if not CLOUDFLARED.exists():
            self.estado = "baixando"
            print("Baixando o cloudflared (só na primeira vez)...")
            BIN_DIR.mkdir(parents=True, exist_ok=True)
            tmp = CLOUDFLARED.with_suffix(".part")
            urllib.request.urlretrieve(CLOUDFLARED_URL, tmp)
            tmp.replace(CLOUDFLARED)
        return str(CLOUDFLARED)

    def _run(self):
        while self.quer:
            try:
                exe = self._exe()
                self.estado, self.url = "conectando", None
                proc = subprocess.Popen(
                    [exe, "tunnel", "--no-autoupdate", "--url", f"http://127.0.0.1:{self.porta}"],
                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, stdin=subprocess.DEVNULL,
                    text=True, encoding="utf-8", errors="replace",
                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
                with self._lock:
                    if not self.quer:          # desligaram enquanto baixava
                        proc.terminate()
                        break
                    self._proc = proc
                ultimas = []
                for linha in proc.stderr:      # o cloudflared escreve tudo no stderr
                    ultimas = (ultimas + [linha.strip()])[-5:]
                    m = URL_RE.search(linha)
                    if m and not self.url:
                        self.url, self.estado, self.desde, self.erro = m.group(0), "no_ar", time.time(), None
                        print(f"Acesso pela internet: {self.url}")
                proc.wait()
                if not self.quer:
                    break
                self.erro = "O túnel caiu; tentando de novo. " + (ultimas[-1] if ultimas else "")
            except Exception as e:  # sem internet, download falhou...
                if not self.quer:
                    break
                self.erro = f"Não consegui ligar o túnel: {e}"
            self.estado, self.url = "erro", None
            print(self.erro)
            time.sleep(RELIGAR_S)


class Acesso:
    def __init__(self, porta):
        self.cfg = {}
        try:
            self.cfg = json.loads(CONFIG_FILE.read_text("utf-8"))
        except (OSError, ValueError):
            pass
        self.sessoes = {}         # token -> validade (epoch)
        self.falhas = {}          # ip -> [erradas seguidas, bloqueado até]
        self.falhas_geral = []    # horários das senhas erradas (todos os IPs)
        self._lock = threading.Lock()
        self.tunel = Tunel(porta)
        if self.cfg.get("tunel") and self.tem_senha():
            self.tunel.ligar()    # estava ligado da última vez

    # ----- configuração -----
    def _salvar(self):
        CONFIG_FILE.parent.mkdir(parents=True, exist_ok=True)
        tmp = CONFIG_FILE.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.cfg, indent=2), "utf-8")
        tmp.replace(CONFIG_FILE)

    def tem_senha(self):
        return bool(self.cfg.get("senha"))

    def definir_senha(self, senha):
        """Troca a senha e derruba quem estava logado. ValueError se for curta."""
        if len(senha) < SENHA_MIN:
            raise ValueError(f"A senha precisa ter pelo menos {SENHA_MIN} letras")
        sal = secrets.token_hex(16)
        self.cfg["senha"] = {"sal": sal, "iter": PBKDF2_ITER, "hash": _hash(senha, sal)}
        with self._lock:
            self.sessoes.clear()
        self._salvar()

    def set_tunel(self, on):
        if on and not self.tem_senha():
            raise ValueError("Defina uma senha antes de ligar o acesso pela internet")
        self.cfg["tunel"] = bool(on)
        self._salvar()
        self.tunel.ligar() if on else self.tunel.desligar()

    def snapshot(self):
        with self._lock:
            agora = time.time()
            logados = sum(v > agora for v in self.sessoes.values())
        return {"senha": self.tem_senha(), "tunel": self.tunel.snapshot(), "logados": logados}

    # ----- login -----
    def login(self, ip, senha):
        """Token da sessão nova, ou (None, mensagem de erro)."""
        agora = time.time()
        with self._lock:
            self.falhas_geral = [t for t in self.falhas_geral if agora - t < BLOQUEIO_S]
            erradas, ate = self.falhas.get(ip, [0, 0])
            if ate > agora or len(self.falhas_geral) >= FALHAS_GERAL_MAX:
                espera = max(ate - agora, 60)
                return None, f"Muitas tentativas erradas. Espere {int(espera // 60) + 1} min."
        s = self.cfg.get("senha")
        ok = bool(s) and hmac.compare_digest(_hash(senha, s["sal"], s.get("iter", PBKDF2_ITER)), s["hash"])
        with self._lock:
            if not ok:
                erradas += 1
                self.falhas[ip] = [0, agora + BLOQUEIO_S] if erradas >= FALHAS_MAX else [erradas, 0]
                self.falhas_geral.append(agora)
                return None, "Senha errada"
            self.falhas.pop(ip, None)
            # limpa sessões vencidas de vez em quando
            self.sessoes = {k: v for k, v in self.sessoes.items() if v > agora}
            token = secrets.token_urlsafe(32)
            self.sessoes[token] = agora + SESSAO_S
            return token, None

    def sair(self, token):
        with self._lock:
            self.sessoes.pop(token, None)

    def sessao_ok(self, token):
        with self._lock:
            return bool(token) and self.sessoes.get(token, 0) > time.time()
