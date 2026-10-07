@echo off
REM Abre o reconhecimento facial + painel do ESP32 no navegador
cd /d "%~dp0"
python -c "import cv2, serial, numpy, pygrabber" 2>NUL || pip install -r requirements.txt
python pc\reconhecimento.py %*
pause
