"""
iniciar.pyw - abre o Programa de Monitoramento Interativo sem janela preta

É o que o atalho da Área de Trabalho executa (com pythonw.exe):
  - se o programa já está rodando, só abre o painel no navegador;
  - senão, inicia o reconhecimento.py escondido (ele mesmo abre o navegador)
    e guarda o que ele escreve em pc/dados/programa.log;
  - se o programa fechar logo no começo, mostra o erro numa janela.

Para fechar o programa: botão "⏻ Encerrar" no topo do painel.
Uso:  pythonw pc/iniciar.pyw [--no-browser]   (--no-browser: ao ligar o Windows)
"""

import ctypes
import os
import subprocess
import sys
import time
import urllib.request
import webbrowser
from pathlib import Path

ROOT = Path(__file__).resolve().parent
LOG = ROOT / "dados" / "programa.log"
URL = "http://localhost:8000"
ESPERA_S = 60   # tempo máximo para o painel responder (a 1ª vez baixa os modelos)


def rodando():
    try:
        urllib.request.urlopen(URL + "/api/alvo", timeout=2).read()
        return True
    except Exception:
        return False


def aviso(texto):
    ctypes.windll.user32.MessageBoxW(None, texto, "Monitoramento Interativo", 0x10)  # ícone de erro


def main():
    sem_navegador = "--no-browser" in sys.argv
    if rodando():
        if not sem_navegador:
            webbrowser.open(URL)
        return

    # python.exe (não o pythonw) para a saída ir para o log; CREATE_NO_WINDOW esconde o console
    exe = Path(sys.executable)
    if exe.name.lower() == "pythonw.exe" and exe.with_name("python.exe").exists():
        exe = exe.with_name("python.exe")
    LOG.parent.mkdir(parents=True, exist_ok=True)
    log = open(LOG, "w", encoding="utf-8")
    args = [str(exe), str(ROOT / "reconhecimento.py")] + (["--no-browser"] if sem_navegador else [])
    proc = subprocess.Popen(args, cwd=ROOT.parent, stdout=log, stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL,
                            env=dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONUNBUFFERED="1"),
                            creationflags=subprocess.CREATE_NO_WINDOW)

    fim = time.time() + ESPERA_S
    while time.time() < fim:
        if proc.poll() is not None:     # fechou sozinho: mostra o motivo
            log.close()
            ultimas = LOG.read_text("utf-8", errors="replace").strip().splitlines()[-12:]
            aviso("O programa fechou ao abrir.\n\n" + "\n".join(ultimas) +
                  f"\n\nLog completo: {LOG}\nSe faltar alguma biblioteca, rode abrir_painel.bat uma vez.")
            return
        if rodando():
            return
        time.sleep(0.5)
    aviso(f"O painel não respondeu em {ESPERA_S} s. Veja o log: {LOG}")


if __name__ == "__main__":
    main()
