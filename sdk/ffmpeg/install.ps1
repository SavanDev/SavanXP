param(
    [switch]$NoInstall,
    [switch]$WithTestMedia
)

# Estampa los recursos SXE en build/external/mediaplayer.elf e instala el
# resultado en build/disk.img. Lo usan sdk/ffmpeg/build.ps1 y `build.ps1
# ffmpeg-smoke`, que reinstala despues de cada rebuild de la imagen.
#
# El ELF que sale de link.sh no se toca: se estampa una COPIA en build/ffmpeg.
# Estampar dos veces el mismo binario falla (la seccion ya existe), y el smoke
# instala en cada arranque.

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent (Split-Path -Parent $scriptDir)
. (Join-Path $repoRoot "tools/UserAppCommon.ps1")

$linked = Join-Path $repoRoot "build/external/mediaplayer.elf"
if (-not (Test-Path $linked)) {
    throw "Falta build/external/mediaplayer.elf. Se construye con sdk/ffmpeg/build.ps1 (o all.sh); ver sdk/ffmpeg/README.md."
}

$stage = Join-Path $repoRoot "build/ffmpeg"
$resources = Join-Path $stage "sxe"
Ensure-Directory $stage
$stamped = Join-Path $stage "mediaplayer"
Copy-Item -Path $linked -Destination $stamped -Force
Invoke-SxeResourceGenerator -ManifestDirs @(Join-Path $scriptDir "mediaplayer") -OutputDir $resources -ProgramNames @("mediaplayer")
Add-SxeResources "mediaplayer" $stamped $resources

$ops = @(
    @{ Dir = "bin" }
    @{ File = "/disk/bin/mediaplayer"; Source = $stamped }
)

if ($WithTestMedia) {
    # El material de prueba se genera con Python + Pillow y no se versiona.
    $python = Get-PythonExecutable
    $mediaRoot = Join-Path $repoRoot "build/media"
    foreach ($clip in @(
        @{ File = "tono.wav"; Script = "make-tone.py" }
        @{ File = "clip.mjpeg"; Script = "make-clip.py" }
        @{ File = "avsync.avi"; Script = "make-avclip.py" }
    )) {
        $path = Join-Path $mediaRoot $clip.File
        $generator = Join-Path $scriptDir $clip.Script
        if (-not (Test-Path $path) -or (Get-Item $generator).LastWriteTime -gt (Get-Item $path).LastWriteTime) {
            & $python $generator
            if ($LASTEXITCODE -ne 0) {
                throw "Fallo $($clip.Script)."
            }
        }
        $ops += @{ File = "/disk/media/$($clip.File)"; Source = $path }
    }
    $ops = @(@{ Dir = "media" }) + $ops
}

Write-Host "mediaplayer: $((Get-Item $stamped).Length) bytes estampado en $stamped"
if ($NoInstall) {
    return
}
Install-SxfsFilesWithTool $DiskImage $ops
Write-Host "Instalado /disk/bin/mediaplayer en $DiskImage"
