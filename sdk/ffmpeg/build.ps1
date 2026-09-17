param(
    # Compila y estampa, pero no toca build/disk.img.
    [switch]$NoInstall,
    # No reconstruye el port: solo estampa e instala el ELF ya linkeado.
    [switch]$SkipPort,
    # Instala tambien el material de prueba en /disk/media.
    [switch]$WithTestMedia,
    # Directorio de trabajo de FFmpeg (fuente, objetos). Sin espacios; por
    # default $HOME/savanxp-ffmpeg, o su forma corta 8.3 si tiene espacios.
    [string]$Work
)

# Construye el port de FFmpeg y el reproductor, e instala /disk/bin/mediaplayer.
#
# El trabajo lo hacen los scripts .sh de esta carpeta, que corren igual en Linux:
# este script solo resuelve con tools/Toolchain.ps1 el bash, el LLVM horneado y
# el GNU make fijado, y se los pasa por el entorno. Ver README.md.

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent (Split-Path -Parent $scriptDir)
. (Join-Path $repoRoot "tools/UserAppCommon.ps1")

if (-not $SkipPort) {
    $bash = Resolve-PosixShell
    $clang = Require-Executable "clang" (Get-ToolchainCandidates "clang")
    $make = Require-Executable "make" (Get-ToolchainCandidates "make")

    $separator = [IO.Path]::PathSeparator
    $env:PATH = (Split-Path -Parent $clang) + $separator + $env:PATH
    # Con barras normales: bash la ejecuta tal cual, y una ruta con '\' no.
    $env:MAKE = $make -replace '\\', '/'
    if ($Work) {
        $env:WORK = $Work -replace '\\', '/'
    }

    # Con "Stop", Windows PowerShell 5.1 convierte en error terminante la primera
    # linea que un comando nativo escribe en stderr si la salida esta
    # redirigida -- un warning de clang cortaba el build a la mitad --. El
    # resultado lo decide el codigo de salida de bash, no stderr.
    $ErrorActionPreference = "Continue"
    & $bash (Join-Path $scriptDir "all.sh")
    $ErrorActionPreference = "Stop"
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo el build del port de FFmpeg (ver la salida de arriba)."
    }
}

& (Join-Path $scriptDir "install.ps1") -NoInstall:$NoInstall -WithTestMedia:$WithTestMedia
