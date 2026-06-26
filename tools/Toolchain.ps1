Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# Resolucion central del toolchain de SavanXP.
#
# Este modulo es el unico lugar que sabe donde viven las herramientas de build.
# No contiene ninguna ruta absoluta de una maquina concreta: resuelve por
#   1. override explicito por variable de entorno
#   2. toolchain horneado por tools/bootstrap.ps1 (toolchain/toolchain.json)
#   3. PATH
# y deja que el llamador (Require-Executable) emita el error si nada resuelve.

$Script:ToolchainRoot = Join-Path (Split-Path -Parent $PSScriptRoot) "toolchain"
$Script:ToolchainManifestPath = Join-Path $Script:ToolchainRoot "toolchain.json"
$Script:ToolchainManifest = $null
$Script:ToolchainManifestLoaded = $false

# name logico -> variable de entorno de override + clave en el manifiesto generado
$Script:ToolchainTools = @{
    "clang"              = @{ Env = "SAVANXP_CLANG";   Manifest = "clang" }
    "clang++"            = @{ Env = "SAVANXP_CLANGXX"; Manifest = "clangxx" }
    "ld.lld"             = @{ Env = "SAVANXP_LD";      Manifest = "lld" }
    "qemu-system-x86_64" = @{ Env = "SAVANXP_QEMU";    Manifest = "qemu" }
    "haxe"               = @{ Env = "SAVANXP_HAXE";    Manifest = "haxe" }
    "haxelib"            = @{ Env = "SAVANXP_HAXELIB"; Manifest = "haxelib" }
}

function Get-ToolchainManifest {
    if (-not $Script:ToolchainManifestLoaded) {
        $Script:ToolchainManifestLoaded = $true
        if (Test-Path $Script:ToolchainManifestPath) {
            try {
                $Script:ToolchainManifest = Get-Content -Raw -Path $Script:ToolchainManifestPath | ConvertFrom-Json
            } catch {
                Write-Warning "No se pudo leer el manifiesto del toolchain ($Script:ToolchainManifestPath): $_"
                $Script:ToolchainManifest = $null
            }
        }
    }

    return $Script:ToolchainManifest
}

function Get-ManifestValue([string]$Key) {
    $manifest = Get-ToolchainManifest
    if (-not $manifest) {
        return $null
    }

    $prop = $manifest.PSObject.Properties[$Key]
    if ($prop -and $prop.Value) {
        return $prop.Value
    }

    return $null
}

# Devuelve la lista de candidatos para una herramienta, en orden de preferencia.
# El resultado se pasa tal cual a Require-Executable, manteniendo el contrato previo.
function Get-ToolchainCandidates([string]$Name) {
    $candidates = @()
    $info = $Script:ToolchainTools[$Name]

    if ($info) {
        $envValue = [Environment]::GetEnvironmentVariable($info.Env)
        if ($envValue) {
            $candidates += $envValue
        }

        $manifestValue = Get-ManifestValue $info.Manifest
        if ($manifestValue) {
            $candidates += $manifestValue
        }
    }

    # Fallback final: el nombre pelado, que Get-Command resuelve contra PATH.
    $candidates += $Name
    return $candidates
}

function Resolve-OvmfPair {
    $envCode = $env:OVMF_CODE
    $envVars = $env:OVMF_VARS
    if ($envCode -and $envVars -and (Test-Path $envCode) -and (Test-Path $envVars)) {
        return @{ Code = $envCode; Vars = $envVars }
    }

    $code = Get-ManifestValue "ovmfCode"
    $vars = Get-ManifestValue "ovmfVars"
    if ($code -and $vars -and (Test-Path $code) -and (Test-Path $vars)) {
        return @{ Code = $code; Vars = $vars }
    }

    throw "No se encontro OVMF. Ejecuta 'tools/bootstrap.ps1' o defini OVMF_CODE y OVMF_VARS."
}
