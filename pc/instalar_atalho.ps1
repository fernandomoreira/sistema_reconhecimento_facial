# instalar_atalho.ps1 - cria o atalho "Monitoramento Interativo" na Área de Trabalho
#
# O atalho roda pc\iniciar.pyw com pythonw.exe (sem janela preta): abre o painel
# se o programa já estiver rodando, ou inicia o programa escondido.
#
#   instalar_atalho.bat                 só o atalho da Área de Trabalho
#   instalar_atalho.bat -Inicializar    também liga o programa junto com o Windows
#   instalar_atalho.bat -Remover        apaga os dois atalhos

param([switch]$Inicializar, [switch]$Remover)

$ErrorActionPreference = "Stop"
$pc = $PSScriptRoot
$nome = "Monitoramento Interativo.lnk"
$desktop = Join-Path ([Environment]::GetFolderPath("Desktop")) $nome
$startup = Join-Path ([Environment]::GetFolderPath("Startup")) $nome

if ($Remover) {
    foreach ($f in $desktop, $startup) {
        if (Test-Path $f) { Remove-Item $f -Confirm:$false; Write-Host "Removido: $f" }
    }
    exit 0
}

# pythonw.exe: o do PATH ou o que fica ao lado do python.exe
$pyw = (Get-Command pythonw.exe -ErrorAction SilentlyContinue).Source
if (-not $pyw) {
    $py = (Get-Command python.exe -ErrorAction SilentlyContinue).Source
    if ($py) { $pyw = Join-Path (Split-Path $py) "pythonw.exe" }
}
if (-not $pyw -or -not (Test-Path $pyw) -or $pyw -like "*WindowsApps*") {
    Write-Host "Não encontrei o pythonw.exe. Instale o Python de python.org (marque 'Add to PATH')."
    exit 1
}

function Novo-Atalho($arquivo, $argumentos) {
    $sh = (New-Object -ComObject WScript.Shell).CreateShortcut($arquivo)
    $sh.TargetPath = $pyw
    $sh.Arguments = "`"$pc\iniciar.pyw`" $argumentos".Trim()
    $sh.WorkingDirectory = Split-Path $pc
    $sh.IconLocation = "$pc\icone.ico"
    $sh.Description = "Programa de Monitoramento Interativo (câmera + emoji no ESP32)"
    $sh.Save()
    Write-Host "Atalho criado: $arquivo"
}

Novo-Atalho $desktop ""
if ($Inicializar) {
    Novo-Atalho $startup "--no-browser"   # ao ligar o PC: sobe escondido, sem abrir o navegador
} elseif (Test-Path $startup) {
    Write-Host "(O programa continua ligando junto com o Windows. Para tirar: instalar_atalho.bat -Remover)"
}
