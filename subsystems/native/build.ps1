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
$haxeSrc = Join-Path $scriptDir "haxe"
$outRoot = Join-Path $repoRoot "build\native"
$genDir = Join-Path $outRoot "gen"

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

# --- 3. Generar C++ desde Haxe -----------------------------------------------
if (Test-Path $genDir) { Remove-Item -Recurse -Force $genDir }
Ensure-Directory $genDir
# Escribimos un .hxml y dejamos que haxe lo parsee. Pasar args con comillas
# embebidas (nullSafety("reflaxe")) directo al exe se rompe en PowerShell 5.1.
$hxmlPath = Join-Path $outRoot "generated.hxml"
$hxmlLines = @(
    "-cp $haxeSrc",
    "-cp $(Join-Path $reflaxe 'src')",
    "-cp $(Join-Path $reflaxeCpp 'src')",
    "-cp $(Join-Path $reflaxeCpp 'std')",
    "-D cxx",
    "-D reflaxe.cpp",
    "-D retain-untyped-meta",
    "-D cpp-output=$genDir",
    '--macro nullSafety("reflaxe")',
    "--macro reflaxe.ReflectCompiler.Start()",
    '--macro nullSafety("cxxcompiler")',
    "--macro cxxcompiler.CompilerInit.Start()",
    "-main Main"
)
Set-Content -Path $hxmlPath -Value $hxmlLines -Encoding ASCII
Write-Step "Generando C++ con reflaxe.CPP"
& $haxe $hxmlPath
if ($LASTEXITCODE -ne 0) { throw "Fallo la generacion de Haxe." }
$mainCpp = Join-Path $genDir "src\Main.cpp"
if (-not (Test-Path $mainCpp)) { throw "reflaxe.CPP no genero Main.cpp en $genDir." }

# --- 4. Compilar y linkear freestanding --------------------------------------
$objDir = Join-Path $outRoot "obj"
if (Test-Path $objDir) { Remove-Item -Recurse -Force $objDir }
Ensure-Directory $objDir
$nativeHeader = Join-Path $nativeSdk "include\savanxp_native.h"

$cFlags = @(
    "-target", "x86_64-unknown-none-elf",
    "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fno-pie",
    "-mno-red-zone", "-mcmodel=small", "-mno-mmx", "-mno-sse", "-mno-sse2",
    "-mgeneral-regs-only",
    "-I", (Join-Path $nativeSdk "include"),
    "-I", (Join-Path $genDir "include")
)
$cxxFlags = $cFlags + @(
    "-std=c++17", "-fno-exceptions", "-fno-rtti",
    "-fno-threadsafe-statics", "-fno-use-cxa-atexit", "-nostdinc++",
    "-include", $nativeHeader
)

function Invoke-Compile([string]$Tool, [string[]]$Pre, [string]$Src, [string]$Obj, [string[]]$Flags) {
    & $Tool -c @Pre $Src -o $Obj @Flags
    if ($LASTEXITCODE -ne 0) { throw "Fallo la compilacion de $Src." }
}

$crt0Obj = Join-Path $objDir "crt0.o"
$shimObj = Join-Path $objDir "sx_native.o"
$entryObj = Join-Path $objDir "sx_entry.o"
$objects = @($crt0Obj, $shimObj, $entryObj)

Write-Step "Compilando runtime nativo"
Invoke-Compile $clang @() (Join-Path $posixSdk "runtime\crt0.S") $crt0Obj $cFlags
Invoke-Compile $clang @("-x", "c") (Join-Path $nativeSdk "runtime\sx_native.c") $shimObj $cFlags
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
Write-Host "ELF generado: $elfPath"

# --- 5. Instalar (opcional) --------------------------------------------------
if ($Install) {
    $image = Open-SvfsImage $DiskImage
    Ensure-SvfsDirectory $image "bin"
    Install-SvfsFile -Image $image -DestinationPath "/disk/bin/$Name" -Data ([System.IO.File]::ReadAllBytes($elfPath))
    Save-SvfsImage $image
    Write-Host "Instalado en: /disk/bin/$Name"
}
