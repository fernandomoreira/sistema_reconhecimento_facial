@echo off
REM Cria o atalho "Monitoramento Interativo" na Area de Trabalho (sem janela preta)
REM   instalar_atalho.bat                 so o atalho
REM   instalar_atalho.bat -Inicializar    tambem liga o programa junto com o Windows
REM   instalar_atalho.bat -Remover        apaga os atalhos
cd /d "%~dp0"
python -c "import cv2, serial, numpy, pygrabber" 2>NUL || pip install -r requirements.txt
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0pc\instalar_atalho.ps1" %*
pause
