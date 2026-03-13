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
    $WadPath = Join-Path $scriptDir "wad\doom1.wad"
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
    Install-SvfsFile -Image $image -DestinationPath "/disk/games/doom/doom1.wad" -Data $wadBytes
    Write-Host "WAD instalado en: /disk/games/doom/doom1.wad"
} else {
    Write-Host "WAD no encontrado. Copialo en: $WadPath"
}

Save-SvfsImage $image
