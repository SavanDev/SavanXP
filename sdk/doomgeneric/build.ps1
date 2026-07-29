param(
    [string]$WadPath,
    [switch]$NoInstall
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent (Split-Path -Parent $scriptDir)
$buildUser = Join-Path $repoRoot "tools/build-user.ps1"
$commonTools = Join-Path $repoRoot "tools/UserAppCommon.ps1"

if (-not $WadPath) {
    # Freedoom (IWAD libre) es el contenido por defecto del LiveCD: jugable sin
    # depender de WADs propietarios/shareware de id. El motor lo reconoce en su
    # tabla de IWADs (d_iwad.c) y lo busca en FILES_DIR="/disk/games/doom".
    $WadPath = Join-Path $scriptDir "wad/freedoom1.wad"
}

# La arena de malloc es BSS, y el kernel mapea la BSS entera al exec: son MiB
# de RAM fisica residentes por instancia. Doom pide 6 MiB de zone (DEFAULT_RAM
# en i_system.c) mas los buffers de present/frame previo/audio, que no llegan a
# 6 MiB mas; 24 MiB deja margen de sobra. Con los 48 MiB genericos, una segunda
# instancia no entraba en RAM y el exec fallaba por falta de memoria.
& $buildUser -Source $scriptDir -Name doomgeneric -HeapMiB 24 -NoInstall:$NoInstall

if ($NoInstall) {
    return
}

. $commonTools

$ops = @(
    @{ Dir = "games" }
    @{ Dir = "games/doom" }
)

if (Test-Path -LiteralPath $WadPath) {
    $wadResolved = (Resolve-Path $WadPath).Path
    $wadName = Split-Path -Leaf $WadPath
    $wadDest = "/disk/games/doom/$wadName"
    $ops += @{ File = $wadDest; Source = $wadResolved }
    Install-SvfsFilesWithTool $DiskImage $ops
    Write-Host "WAD instalado en: $wadDest"
} else {
    Install-SvfsFilesWithTool $DiskImage $ops
    Write-Host "WAD no encontrado. Copialo en: $WadPath"
    Write-Host "Freedoom (libre): https://freedoom.github.io/download.html"
}
