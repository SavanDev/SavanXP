param(
    [string]$WadPath,
    [switch]$NoInstall
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent (Split-Path -Parent $scriptDir)
$buildUser = Join-Path $repoRoot "tools\build-user.ps1"
$commonTools = Join-Path $repoRoot "tools\UserAppCommon.ps1"

if (-not $WadPath) {
    # Freedoom (IWAD libre) es el contenido por defecto del LiveCD: jugable sin
    # depender de WADs propietarios/shareware de id. El motor lo reconoce en su
    # tabla de IWADs (d_iwad.c) y lo busca en FILES_DIR="/disk/games/doom".
    $WadPath = Join-Path $scriptDir "wad\freedoom1.wad"
}

& $buildUser -Source $scriptDir -Name doomgeneric -NoInstall:$NoInstall

if ($NoInstall) {
    return
}

. $commonTools

$image = Open-SvfsImage $DiskImage
Ensure-SvfsDirectory $image "games"
Ensure-SvfsDirectory $image "games/doom"

if (Test-Path -LiteralPath $WadPath) {
    $wadBytes = [System.IO.File]::ReadAllBytes((Resolve-Path $WadPath).Path)
    $wadName = Split-Path -Leaf $WadPath
    $wadDest = "/disk/games/doom/$wadName"
    Install-SvfsFile -Image $image -DestinationPath $wadDest -Data $wadBytes
    Write-Host "WAD instalado en: $wadDest"
} else {
    Write-Host "WAD no encontrado. Copialo en: $WadPath"
    Write-Host "Freedoom (libre): https://freedoom.github.io/download.html"
}

Save-SvfsImage $image
