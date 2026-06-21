[CmdletBinding()]
param(
    # Re-descarga y re-extrae aunque ya exista el toolchain.
    [switch]$Force,
    # Omite LLVM/QEMU si ya los tenes resueltos por otra via.
    [switch]$SkipLlvm,
    [switch]$SkipQemu
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$ToolsRoot = $PSScriptRoot
$ProjectRoot = Split-Path -Parent $ToolsRoot
$ToolchainRoot = Join-Path $ProjectRoot "toolchain"
$CacheRoot = Join-Path $ToolchainRoot "_cache"
$LockPath = Join-Path $ToolsRoot "toolchain.lock.json"
$ManifestPath = Join-Path $ToolchainRoot "toolchain.json"

function Write-Step([string]$Message) {
    Write-Host "==> $Message"
}

function Ensure-Directory([string]$Path) {
    if (-not (Test-Path $Path)) {
        New-Item -ItemType Directory -Path $Path -Force | Out-Null
    }
}

function Get-RemoteFile([string]$Url, [string]$Destination) {
    Write-Step "Descargando $Url"
    $previous = $ProgressPreference
    $ProgressPreference = "SilentlyContinue"
    try {
        Invoke-WebRequest -Uri $Url -OutFile $Destination -UseBasicParsing
    } catch {
        throw "Fallo la descarga de '$Url'. Revisa la URL/version en toolchain.lock.json. Detalle: $_"
    } finally {
        $ProgressPreference = $previous
    }
}

# Verifica el sha256. Si el esperado esta vacio, lo calcula, lo imprime y avisa.
function Confirm-FileHash([string]$Path, [string]$Expected, [string]$Label) {
    $actual = (Get-FileHash -Path $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if (-not $Expected) {
        Write-Warning "$Label sin sha256 fijado. Verificacion OMITIDA."
        Write-Host "    sha256 calculado: $actual"
        Write-Host "    Pega ese valor en tools/toolchain.lock.json para fijarlo."
        return
    }

    if ($actual -ne $Expected.ToLowerInvariant()) {
        throw "${Label}: sha256 no coincide. esperado=$Expected obtenido=$actual"
    }

    Write-Step "${Label}: sha256 verificado"
}

function Expand-TarArchive([string]$Archive, [string]$Destination) {
    Ensure-Directory $Destination
    # bsdtar (incluido en Windows 10/11) maneja .tar.xz directamente.
    & tar -xf $Archive -C $Destination
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo al extraer '$Archive' con tar."
    }
}

function Install-Llvm($Spec) {
    $target = Join-Path $ToolchainRoot "llvm"
    $clangxx = Join-Path $target "$($Spec.bin)\clang++.exe"
    if ((Test-Path $clangxx) -and -not $Force) {
        Write-Step "LLVM ya presente en $target (usa -Force para re-instalar)"
        return $target
    }

    if (Test-Path $target) {
        Remove-Item -Recurse -Force $target
    }

    Ensure-Directory $CacheRoot
    $archive = Join-Path $CacheRoot ("llvm-" + $Spec.version + "." + $Spec.archive)
    if ($Force -or -not (Test-Path $archive)) {
        Get-RemoteFile $Spec.url $archive
    }
    Confirm-FileHash $archive $Spec.sha256 "LLVM $($Spec.version)"

    Write-Step "Extrayendo LLVM..."
    $staging = Join-Path $CacheRoot "llvm-staging"
    if (Test-Path $staging) {
        Remove-Item -Recurse -Force $staging
    }
    Expand-TarArchive $archive $staging

    # El tar.xz expande a una carpeta clang+llvm-<ver>-.../ ; la normalizamos a toolchain/llvm.
    $inner = Get-ChildItem -Path $staging -Directory | Select-Object -First 1
    if (-not $inner) {
        throw "El archivo de LLVM no contenia la carpeta esperada."
    }
    Move-Item -Path $inner.FullName -Destination $target
    Remove-Item -Recurse -Force $staging

    if (-not (Test-Path $clangxx)) {
        throw "Tras instalar LLVM no se encontro clang++ en $clangxx"
    }
    return $target
}

# weilnetz solo conserva los builds recientes en /w64/ (los viejos se archivan a
# /w64/<anio>/). Si la URL pineada caduca, descubrimos el build mas nuevo del indice.
function Resolve-QemuUrl($Spec) {
    try {
        $head = Invoke-WebRequest -Uri $Spec.url -Method Head -UseBasicParsing -ErrorAction Stop
        if ($head.StatusCode -eq 200) {
            return $Spec.url
        }
    } catch {
        Write-Warning "La URL pineada de QEMU no responde; buscando build disponible en el indice..."
    }

    $index = $Spec.PSObject.Properties["index"].Value
    if (-not $index) {
        $index = "https://qemu.weilnetz.de/w64/"
    }

    $page = Invoke-WebRequest -Uri $index -UseBasicParsing
    $latest = $page.Links |
        Where-Object { $_.href -match 'qemu-w64-setup-\d+\.exe$' } |
        ForEach-Object { $_.href } | Sort-Object -Unique | Select-Object -Last 1
    if (-not $latest) {
        throw "No se pudo descubrir un build de QEMU en $index. Actualiza la URL en toolchain.lock.json."
    }

    $resolved = ([Uri]::new([Uri]$index, $latest)).AbsoluteUri
    Write-Warning "Usando QEMU descubierto: $resolved (actualiza toolchain.lock.json para fijarlo)"
    return $resolved
}

function Install-Qemu($Spec) {
    $target = Join-Path $ToolchainRoot "qemu"
    $qemuExe = Join-Path $target "qemu-system-x86_64.exe"
    if ((Test-Path $qemuExe) -and -not $Force) {
        Write-Step "QEMU ya presente en $target (usa -Force para re-instalar)"
        return $target
    }

    if (Test-Path $target) {
        Remove-Item -Recurse -Force $target
    }

    Ensure-Directory $CacheRoot
    $installer = Join-Path $CacheRoot ("qemu-" + $Spec.version + "-setup.exe")
    if ($Force -or -not (Test-Path $installer)) {
        Get-RemoteFile (Resolve-QemuUrl $Spec) $installer
    }
    Confirm-FileHash $installer $Spec.sha256 "QEMU $($Spec.version)"

    # El instalador NSIS de weilnetz soporta extraccion silenciosa.
    # /D debe ir ultimo, sin comillas; por eso toolchain/ no debe tener espacios.
    Write-Step "Extrayendo QEMU (silencioso) a $target ..."
    Ensure-Directory $target
    $proc = Start-Process -FilePath $installer -ArgumentList "/S", "/D=$target" -Wait -PassThru
    if ($proc.ExitCode -ne 0) {
        throw "El instalador de QEMU devolvio codigo $($proc.ExitCode)."
    }

    if (-not (Test-Path $qemuExe)) {
        throw "Tras instalar QEMU no se encontro $qemuExe. Si tu ruta tiene espacios, mueve el repo o instala QEMU manualmente y usa -SkipQemu."
    }
    return $target
}

function Resolve-OvmfFromQemu([string]$QemuRoot) {
    $share = Join-Path $QemuRoot "share"
    $candidates = @(
        @{ Code = Join-Path $share "edk2-x86_64-code.fd"; Vars = Join-Path $share "edk2-x86_64-vars.fd" },
        @{ Code = Join-Path $share "edk2-x86_64-code.fd"; Vars = Join-Path $share "edk2-i386-vars.fd" },
        @{ Code = Join-Path $share "OVMF_CODE.fd";        Vars = Join-Path $share "OVMF_VARS.fd" }
    )
    foreach ($pair in $candidates) {
        if ((Test-Path $pair.Code) -and (Test-Path $pair.Vars)) {
            return $pair
        }
    }
    return $null
}

# --- main -----------------------------------------------------------------

if (-not (Test-Path $LockPath)) {
    throw "No se encontro $LockPath"
}
$lock = Get-Content -Raw -Path $LockPath | ConvertFrom-Json

Ensure-Directory $ToolchainRoot

$manifest = [ordered]@{}

if (-not $SkipLlvm) {
    $llvmRoot = Install-Llvm $lock.llvm
    $llvmBin = Join-Path $llvmRoot $lock.llvm.bin
    $manifest["clang"]   = Join-Path $llvmBin "clang.exe"
    $manifest["clangxx"] = Join-Path $llvmBin "clang++.exe"
    $manifest["lld"]     = Join-Path $llvmBin "ld.lld.exe"
}

if (-not $SkipQemu) {
    $qemuRoot = Install-Qemu $lock.qemu
    $manifest["qemu"] = Join-Path $qemuRoot "qemu-system-x86_64.exe"

    $ovmf = Resolve-OvmfFromQemu $qemuRoot
    if ($ovmf) {
        $manifest["ovmfCode"] = $ovmf.Code
        $manifest["ovmfVars"] = $ovmf.Vars
    } else {
        Write-Warning "No se encontro OVMF dentro de QEMU. Tendras que definir OVMF_CODE/OVMF_VARS."
    }
}

# Fusiona con el manifiesto previo para no perder claves de un run parcial.
if ((Test-Path $ManifestPath) -and ($SkipLlvm -or $SkipQemu)) {
    $existing = Get-Content -Raw -Path $ManifestPath | ConvertFrom-Json
    foreach ($prop in $existing.PSObject.Properties) {
        if (-not $manifest.Contains($prop.Name)) {
            $manifest[$prop.Name] = $prop.Value
        }
    }
}

$manifest | ConvertTo-Json | Set-Content -Path $ManifestPath -Encoding UTF8
Write-Step "Manifiesto escrito en $ManifestPath"
Write-Host ""
Write-Host "Toolchain listo. Ahora podes correr: .\build.ps1 build"
