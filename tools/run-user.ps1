param(
    [Parameter(Mandatory = $true)]
    [string]$Source,
    [string]$Name,
    [string]$Destination
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$buildUser = Join-Path $PSScriptRoot "build-user.ps1"
$projectRoot = Split-Path -Parent $PSScriptRoot

$args = @{
    Source = $Source
}
if ($Name) {
    $args.Name = $Name
}
if ($Destination) {
    $args.Destination = $Destination
}

& $buildUser @args
if ($LASTEXITCODE -ne 0) {
    throw "Fallo la instalacion de la app externa."
}

& (Join-Path $projectRoot "build.ps1") run
