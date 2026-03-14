param(
    [Parameter(Mandatory = $true)]
    [string]$Source,
    [string]$Name,
    [string]$Destination,
    [string]$OutputPath,
    [switch]$NoInstall
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot "UserAppCommon.ps1")

$sourceResolved = Resolve-Path $Source
$sourceFull = $sourceResolved.Path
if (-not $Name) {
    $sourceItem = Get-Item $sourceFull
    if ($sourceItem.PSIsContainer) {
        $Name = $sourceItem.Name
    } else {
        $Name = [System.IO.Path]::GetFileNameWithoutExtension($sourceFull)
    }
}
if (-not $Destination) {
    $Destination = "/disk/bin/$Name"
}

$outputRoot = Join-Path $BuildRoot "external"
Ensure-Directory $outputRoot
$elfOutputPath = if ($OutputPath) { [System.IO.Path]::GetFullPath($OutputPath) } else { Join-Path $outputRoot ("{0}.elf" -f $Name) }

$elfPath = Build-ExternalUserProgram -SourcePath $sourceFull -ProgramName $Name -OutputPath $elfOutputPath

if (-not $NoInstall) {
    $image = Open-SvfsImage $DiskImage
    Ensure-SvfsDirectory $image "bin"
    Ensure-SvfsDirectory $image "tmp"
    Install-SvfsFile -Image $image -DestinationPath $Destination -Data ([System.IO.File]::ReadAllBytes($elfPath))
    Save-SvfsImage $image
    Write-Host "Instalado en: $Destination"
}

Write-Host "ELF generado: $elfPath"
