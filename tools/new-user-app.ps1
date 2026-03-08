param(
    [Parameter(Mandatory = $true)]
    [string]$Name,
    [string]$DestinationRoot = "sdk"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$projectRoot = Split-Path -Parent $PSScriptRoot
$templateRoot = Join-Path $projectRoot "sdk\\template"
$destinationBase = [System.IO.Path]::GetFullPath((Join-Path $projectRoot $DestinationRoot))
$destinationRoot = Join-Path $destinationBase $Name

if (-not (Test-Path $templateRoot)) {
    throw "No se encontro el template publico del SDK en '$templateRoot'."
}

if (Test-Path $destinationRoot) {
    throw "La carpeta destino '$destinationRoot' ya existe."
}

New-Item -ItemType Directory -Path $destinationRoot | Out-Null
Copy-Item (Join-Path $templateRoot "main.c") (Join-Path $destinationRoot "main.c")

Write-Host "App creada en: $destinationRoot"
Write-Host "Siguiente paso:"
Write-Host "  .\\tools\\build-user.ps1 -Source $destinationRoot -Name $Name"
