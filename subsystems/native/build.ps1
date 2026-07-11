# SavanXP - build del subsistema nativo (Haxe), Fase 0.
#
# Cadena: Haxe -> reflaxe.CPP -> C++17 -> clang++ freestanding -> ELF nativo.
#
# Es un build APARTE (patron sdk/doomgeneric/build.ps1): no lo invoca el build
# principal y, por defecto, NO toca build/disk.img. Con -Install instala el ELF
# en /disk/bin/<Name>.
#
# Las libs reflaxe/reflaxe.CPP se clonan pineadas (tools/toolchain.lock.json)
# bajo toolchain/haxe-libs/ (ignorado por git). El compilador Haxe se resuelve
# por tools/Toolchain.ps1 (env SAVANXP_HAXE > toolchain horneado > PATH).
param(
    [string]$Name = "nativehello",
    # Directorio (relativo a subsystems/native/) con el codigo Haxe de la app.
    # Cada app define su propia clase Main.
    [string]$Source = "haxe",
    [switch]$Install
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent (Split-Path -Parent $scriptDir)
. (Join-Path $repoRoot "tools\UserAppCommon.ps1")

$toolchainRoot = Join-Path $repoRoot "toolchain"
$haxeLibsRoot = Join-Path $toolchainRoot "haxe-libs"
$lockPath = Join-Path $repoRoot "tools\toolchain.lock.json"
$posixSdk = Join-Path $repoRoot "subsystems\posix\sdk\v1"
$nativeSdk = Join-Path $scriptDir "sdk"
$haxeSrc = Join-Path $scriptDir $Source
if (-not (Test-Path $haxeSrc)) { throw "No existe el directorio de fuentes Haxe '$Source'." }
$outRoot = Join-Path $repoRoot "build\native"
# gen/obj por app para poder construir varias sin pisarse.
$genDir = Join-Path $outRoot "gen-$Name"

function Write-Step([string]$Message) { Write-Host "==> $Message" }

# --- 1. Resolver toolchain ---------------------------------------------------
$haxe = Require-Executable "haxe" (Get-ToolchainCandidates "haxe")
$clang = Require-Executable "clang" (Get-ToolchainCandidates "clang")
$clangxx = Require-Executable "clang++" (Get-ToolchainCandidates "clang++")
$lld = Require-Executable "ld.lld" (Get-ToolchainCandidates "ld.lld")

# --- 2. Asegurar las libs reflaxe pineadas -----------------------------------
$lock = Get-Content -Raw -Path $lockPath | ConvertFrom-Json
Ensure-Directory $haxeLibsRoot
# git escribe progreso a stderr; en PS 5.1 con ErrorActionPreference=Stop eso se
# convierte en error terminante. Lo corremos con Continue y validamos por exit code.
function Invoke-Git([string[]]$GitArgs) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & git @GitArgs 2>&1 | ForEach-Object { Write-Host $_ }
    } finally {
        $ErrorActionPreference = $previous
    }
    return $LASTEXITCODE
}
function Ensure-HaxeLib([string]$Dir, [string]$Url, [string]$Commit) {
    $path = Join-Path $haxeLibsRoot $Dir
    if (-not (Test-Path (Join-Path $path ".git"))) {
        Write-Step "Clonando $Url"
        if ((Invoke-Git @("clone", $Url, $path)) -ne 0) { throw "Fallo el clone de $Url" }
    }
    $head = (& git -C $path rev-parse HEAD).Trim()
    if ($head -ne $Commit) {
        Write-Step "Fijando $Dir a $Commit"
        Invoke-Git @("-C", $path, "fetch", "--depth", "1", "origin", $Commit) | Out-Null
        if ((Invoke-Git @("-C", $path, "checkout", "--quiet", $Commit)) -ne 0) {
            throw "No se pudo fijar $Dir a $Commit"
        }
    }
    return $path
}
$reflaxe = Ensure-HaxeLib "reflaxe" $lock.haxelibs.reflaxe.url $lock.haxelibs.reflaxe.commit
$reflaxeCpp = Ensure-HaxeLib "reflaxe.CPP" $lock.haxelibs.'reflaxe.cpp'.url $lock.haxelibs.'reflaxe.cpp'.commit

# --- 3a. Exponer el _std de reflaxe.CPP como overrides .cross.hx ---------------
# El _std (String/Array/Map...) como .hx plano envenena el contexto macro/eval
# de Haxe (los shadows con untyped __cpp__ se tipan dentro del compilador).
# El mecanismo correcto es el que usa `haxelib run reflaxe build`: renombrarlos
# a *.cross.hx, que Haxe solo aplica al target de generacion ("cross") y no al
# contexto macro. Ver subsystems/native/README.md.
$stdCrossDir = Join-Path $outRoot "std-cross"
if (Test-Path $stdCrossDir) { Remove-Item -Recurse -Force $stdCrossDir }
$stdSourceDir = Join-Path $reflaxeCpp "std\cxx\_std"
Get-ChildItem $stdSourceDir -Recurse -Filter *.hx | ForEach-Object {
    $relative = $_.FullName.Substring($stdSourceDir.Length + 1)
    $target = Join-Path $stdCrossDir ($relative -replace '\.hx$', '.cross.hx')
    Ensure-Directory (Split-Path -Parent $target)
    Copy-Item $_.FullName $target
}
# Shadows locales del _std: se copian como *.cross.hx encima de los de la lib.
# Cada uno arregla un choque con el runtime nativo freestanding:
#  - Math.hx: overload isFinite ambiguo en Haxe 4.
#  - Std.hx: try/catch (rompe con -fno-exceptions) y std::stof en parseFloat
#    (reintroduce Float). Ver subsystems/native/haxe-std-fixes/.
Get-ChildItem (Join-Path $scriptDir "haxe-std-fixes") -Filter *.hx | ForEach-Object {
    $crossName = ($_.Name -replace '\.hx$', '.cross.hx')
    Copy-Item $_.FullName (Join-Path $stdCrossDir $crossName) -Force
}

# --- 3b. Generar C++ desde Haxe -----------------------------------------------
if (Test-Path $genDir) { Remove-Item -Recurse -Force $genDir }
Ensure-Directory $genDir
# Escribimos un .hxml y dejamos que haxe lo parsee. Pasar args con comillas
# embebidas (nullSafety("reflaxe")) directo al exe se rompe en PowerShell 5.1.
$hxmlPath = Join-Path $outRoot "generated-$Name.hxml"
$hxmlLines = @(
    # Sin el analizador de Haxe: const-foldea condiciones que dependen de
    # untyped __cpp__ (las trata como constantes) y elimina los if. Es el
    # mismo motivo por el que el CI de reflaxe.CPP compila con --no-opt.
    "--no-opt",
    "-cp $haxeSrc",
    "-cp $(Join-Path $scriptDir 'haxe-support')",
    "-cp $(Join-Path $reflaxe 'src')",
    "-cp $(Join-Path $reflaxeCpp 'src')",
    "-cp $(Join-Path $reflaxeCpp 'std')",
    "-cp $stdCrossDir",
    "-D cxx",
    "-D reflaxe.cpp",
    "-D retain-untyped-meta",
    "-D cpp-output=$genDir",
    '--macro nullSafety("reflaxe")',
    "--macro reflaxe.ReflectCompiler.Start()",
    '--macro nullSafety("cxxcompiler")',
    "--macro SxnCompilerInit.Start()",
    "-main Main"
)
Set-Content -Path $hxmlPath -Value $hxmlLines -Encoding ASCII
Write-Step "Generando C++ con reflaxe.CPP"
& $haxe $hxmlPath
if ($LASTEXITCODE -ne 0) { throw "Fallo la generacion de Haxe." }
$mainCpp = Join-Path $genDir "src\Main.cpp"
if (-not (Test-Path $mainCpp)) { throw "reflaxe.CPP no genero Main.cpp en $genDir." }

# --- 4. Compilar y linkear freestanding --------------------------------------
$objDir = Join-Path $outRoot "obj-$Name"
if (Test-Path $objDir) { Remove-Item -Recurse -Force $objDir }
Ensure-Directory $objDir
$nativeHeader = Join-Path $nativeSdk "include\savanxp_native.h"

# SSE2 HABILITADO (a diferencia del kernel, que va -mno-sse): el kernel gestiona
# el estado FPU/SSE por proceso (FXSAVE/FXRSTOR en el context switch), asi que el
# codigo nativo puede usar float/double por hardware. Esto es lo que hace andar
# el Float de Haxe (division, Math, etc.) sin soft-float. El stack lo alinea a 16
# el crt0. -mno-red-zone se mantiene por consistencia con el resto del userland.
$cFlags = @(
    "-target", "x86_64-unknown-none-elf",
    "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fno-pie",
    "-mno-red-zone", "-mcmodel=small",
    "-I", (Join-Path $nativeSdk "include"),
    "-I", (Join-Path $genDir "include")
)
$cxxFlags = $cFlags + @(
    "-std=c++17", "-fno-exceptions", "-fno-rtti",
    "-fno-threadsafe-statics", "-fno-use-cxa-atexit", "-nostdinc++",
    # Mini std C++ freestanding del SDK nativo (<memory> sobre sxn_alloc).
    "-isystem", (Join-Path $nativeSdk "include\cxxstd"),
    "-include", $nativeHeader
)

function Invoke-Compile([string]$Tool, [string[]]$Pre, [string]$Src, [string]$Obj, [string[]]$Flags) {
    & $Tool -c @Pre $Src -o $Obj @Flags
    if ($LASTEXITCODE -ne 0) { throw "Fallo la compilacion de $Src." }
}

$crt0Obj = Join-Path $objDir "crt0.o"
$shimObj = Join-Path $objDir "sx_native.o"
$guiObj = Join-Path $objDir "sx_gui.o"
$cxxGlueObj = Join-Path $objDir "sx_cxx.o"
$entryObj = Join-Path $objDir "sx_entry.o"
$objects = @($crt0Obj, $shimObj, $guiObj, $cxxGlueObj, $entryObj)

Write-Step "Compilando runtime nativo"
Invoke-Compile $clang @() (Join-Path $posixSdk "runtime\crt0.S") $crt0Obj $cFlags
Invoke-Compile $clang @("-x", "c") (Join-Path $nativeSdk "runtime\sx_native.c") $shimObj $cFlags
Invoke-Compile $clang @("-x", "c") (Join-Path $nativeSdk "runtime\sx_gui.c") $guiObj $cFlags
Invoke-Compile $clangxx @() (Join-Path $nativeSdk "runtime\sx_cxx.cpp") $cxxGlueObj $cxxFlags
Invoke-Compile $clangxx @() (Join-Path $nativeSdk "runtime\sx_entry.cpp") $entryObj $cxxFlags

# Compilar todo el C++ generado salvo el _main_.cpp de reflaxe (incluye <memory>;
# proveemos nuestra propia entrada en sx_entry.cpp).
Write-Step "Compilando C++ generado"
foreach ($cpp in Get-ChildItem (Join-Path $genDir "src") -Filter *.cpp) {
    if ($cpp.Name -eq "_main_.cpp") { continue }
    $obj = Join-Path $objDir ([System.IO.Path]::ChangeExtension($cpp.Name, ".o"))
    Invoke-Compile $clangxx @() $cpp.FullName $obj $cxxFlags
    $objects += $obj
}

$elfPath = Join-Path $outRoot "$Name.elf"
Write-Step "Linkeando $Name.elf"
& $lld -nostdlib -static -T (Join-Path $posixSdk "linker.ld") -o $elfPath @objects
if ($LASTEXITCODE -ne 0) { throw "Fallo el link de $Name." }

# Estampar e_ident[EI_OSABI] (byte 7) para que el kernel corra este binario con
# identidad de subsistema nativo. Debe coincidir con elf::kOsAbiNative y con
# SXN_ELF_OSABI_NATIVE (sdk/include/savanxp_native.h).
$elfOsAbiNative = 0x53
$bytes = [System.IO.File]::ReadAllBytes($elfPath)
$bytes[7] = $elfOsAbiNative
[System.IO.File]::WriteAllBytes($elfPath, $bytes)
Write-Host "ELF generado: $elfPath (EI_OSABI=0x$('{0:x}' -f $elfOsAbiNative), nativo)"

# --- 5. Instalar (opcional) --------------------------------------------------
if ($Install) {
    $image = Open-SvfsImage $DiskImage
    Ensure-SvfsDirectory $image "bin"
    Install-SvfsFile -Image $image -DestinationPath "/disk/bin/$Name" -Data ([System.IO.File]::ReadAllBytes($elfPath))
    Save-SvfsImage $image
    Write-Host "Instalado en: /disk/bin/$Name"
}
