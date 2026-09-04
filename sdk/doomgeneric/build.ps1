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

# Sin -HeapMiB: el malloc arranca con la arena de bootstrap en BSS y le pide al
# kernel arenas respaldadas por secciones a medida que las necesita, asi que los
# 6 MiB de zone (DEFAULT_RAM en i_system.c) mas los buffers de present/frame
# previo/audio salen de RAM que solo se ocupa cuando se usa. Antes habia que
# clavar 24 MiB de arena en la BSS -- residentes por instancia aunque Doom no
# los tocara --, y con los 48 MiB genericos una segunda instancia ni entraba.
& $buildUser -Source $scriptDir -Name doomgeneric -NoInstall:$NoInstall

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
    Install-SxfsFilesWithTool $DiskImage $ops
    Write-Host "WAD instalado en: $wadDest"
} else {
    Install-SxfsFilesWithTool $DiskImage $ops
    Write-Host "WAD no encontrado. Copialo en: $WadPath"
    Write-Host "Freedoom (libre): https://freedoom.github.io/download.html"
}
