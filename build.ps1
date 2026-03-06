param(
    [ValidateSet("build", "run", "debug", "clean")]
    [string]$Command = "build"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildRoot = Join-Path $ProjectRoot "build"
$ObjRoot = Join-Path $BuildRoot "obj"
$ImageRoot = Join-Path $BuildRoot "image"
$BootRoot = Join-Path $ImageRoot "boot"
$EfiBootRoot = Join-Path $ImageRoot "EFI\\BOOT"
$ToolRoot = Join-Path $ProjectRoot "tools"
$LimineRoot = Join-Path $ToolRoot "limine"
$LimineBranch = "v10.x-binary"
$KernelElf = Join-Path $BuildRoot "kernel.elf"
$VarsTemplate = Join-Path $BuildRoot "OVMF_VARS.fd"
$DebugConLog = Join-Path $BuildRoot "debugcon.log"

$CxxSources = @(
    "arch/x86_64/entry.cpp",
    "arch/x86_64/cpu_init.cpp",
    "arch/x86_64/timer.cpp",
    "kernel/kernel_main.cpp",
    "kernel/console.cpp",
    "kernel/heap.cpp",
    "kernel/physical_memory.cpp",
    "kernel/panic.cpp",
    "kernel/runtime.cpp"
)

function New-Directory([string]$Path) {
    if (-not (Test-Path $Path)) {
        New-Item -ItemType Directory -Path $Path | Out-Null
    }
}

function Resolve-Executable([string[]]$Candidates) {
    foreach ($candidate in $Candidates) {
        if (-not $candidate) {
            continue
        }

        $command = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($command) {
            return $command.Source
        }

        if (Test-Path $candidate) {
            return $candidate
        }
    }

    return $null
}

function Require-Executable([string]$DisplayName, [string[]]$Candidates) {
    $resolved = Resolve-Executable $Candidates
    if (-not $resolved) {
        throw "No se encontro '$DisplayName'. Revise PATH o instala la herramienta."
    }

    return $resolved
}

function Ensure-Limine {
    if (Test-Path (Join-Path $LimineRoot "BOOTX64.EFI")) {
        return
    }

    New-Directory $ToolRoot

    if (Test-Path $LimineRoot) {
        Remove-Item -Recurse -Force $LimineRoot
    }

    Write-Host "Descargando Limine ($LimineBranch)..."
    & git clone --depth 1 --branch $LimineBranch https://github.com/limine-bootloader/limine.git $LimineRoot
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo al clonar Limine."
    }
}

function Resolve-OvmfPair {
    $envCode = $env:OVMF_CODE
    $envVars = $env:OVMF_VARS
    if ($envCode -and $envVars -and (Test-Path $envCode) -and (Test-Path $envVars)) {
        return @{ Code = $envCode; Vars = $envVars }
    }

    $pairs = @(
        @{ Code = "C:\\Program Files\\qemu\\share\\edk2-x86_64-code.fd"; Vars = "C:\\Program Files\\qemu\\share\\edk2-x86_64-vars.fd" },
        @{ Code = "C:\\Program Files\\qemu\\share\\edk2-x86_64-code.fd"; Vars = "C:\\Program Files\\qemu\\share\\edk2-i386-vars.fd" },
        @{ Code = "C:\\Program Files\\qemu\\share\\OVMF_CODE.fd"; Vars = "C:\\Program Files\\qemu\\share\\OVMF_VARS.fd" },
        @{ Code = "C:\\msys64\\mingw64\\share\\edk2-x86_64-code.fd"; Vars = "C:\\msys64\\mingw64\\share\\edk2-x86_64-vars.fd" },
        @{ Code = "C:\\msys64\\usr\\share\\edk2-ovmf\\x64\\OVMF_CODE.fd"; Vars = "C:\\msys64\\usr\\share\\edk2-ovmf\\x64\\OVMF_VARS.fd" }
    )

    foreach ($pair in $pairs) {
        if ((Test-Path $pair.Code) -and (Test-Path $pair.Vars)) {
            return $pair
        }
    }

    throw "No se encontro OVMF. Defini OVMF_CODE y OVMF_VARS o instala QEMU/OVMF en una ruta conocida."
}

function Get-CommonFlags {
    return @(
        "-std=c++20",
        "-target", "x86_64-unknown-none-elf",
        "-ffreestanding",
        "-fno-exceptions",
        "-fno-rtti",
        "-fno-stack-protector",
        "-fno-pic",
        "-fno-pie",
        "-mno-red-zone",
        "-mcmodel=kernel",
        "-mno-mmx",
        "-mno-sse",
        "-mno-sse2",
        "-mgeneral-regs-only",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-I", (Join-Path $ProjectRoot "include"),
        "-I", (Join-Path $ProjectRoot "vendor")
    )
}

function Build-Kernel {
    $clang = Require-Executable "clang++" @(
        "clang++",
        "C:\\Program Files\\LLVM\\bin\\clang++.exe"
    )
    $ld = Require-Executable "ld.lld" @(
        "ld.lld",
        "C:\\Program Files\\LLVM\\bin\\ld.lld.exe"
    )
    $git = Require-Executable "git" @("git")

    Ensure-Limine
    New-Directory $BuildRoot
    New-Directory $ObjRoot
    New-Directory $BootRoot
    New-Directory $EfiBootRoot

    $commonFlags = Get-CommonFlags
    $objectFiles = @()

    foreach ($source in $CxxSources) {
        $sourcePath = Join-Path $ProjectRoot $source
        $objectPath = Join-Path $ObjRoot (([IO.Path]::GetFileNameWithoutExtension($source)) + ".o")
        $objectFiles += $objectPath

        $compileArgs = @(
            "-c",
            $sourcePath,
            "-o", $objectPath
        ) + $commonFlags

        & $clang @compileArgs
        if ($LASTEXITCODE -ne 0) {
            throw "Fallo la compilacion de $source."
        }
    }

    $linkArgs = @(
        "-m", "elf_x86_64",
        "-T", (Join-Path $ProjectRoot "linker.ld"),
        "-z", "max-page-size=0x1000",
        "--build-id=none",
        "-o", $KernelElf
    ) + $objectFiles

    & $ld @linkArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo el link del kernel."
    }

    Copy-Item (Join-Path $ProjectRoot "boot\\limine.conf") (Join-Path $ImageRoot "limine.conf") -Force
    New-Directory (Join-Path $BootRoot "limine")
    Copy-Item $KernelElf (Join-Path $BootRoot "kernel.elf") -Force
    Copy-Item (Join-Path $ProjectRoot "boot\\limine.conf") (Join-Path $BootRoot "limine\\limine.conf") -Force
    Copy-Item (Join-Path $LimineRoot "BOOTX64.EFI") (Join-Path $EfiBootRoot "BOOTX64.EFI") -Force
    Set-Content -Path (Join-Path $ImageRoot "startup.nsh") -Value "fs0:\EFI\BOOT\BOOTX64.EFI" -NoNewline
}

function Run-Qemu([switch]$WaitForDebugger) {
    $qemu = Require-Executable "qemu-system-x86_64" @(
        "qemu-system-x86_64",
        "C:\\Program Files\\qemu\\qemu-system-x86_64.exe"
    )
    Build-Kernel

    $ovmf = Resolve-OvmfPair
    Copy-Item $ovmf.Vars $VarsTemplate -Force
    if (Test-Path $DebugConLog) {
        Remove-Item $DebugConLog -Force
    }

    $args = @(
        "-machine", "q35",
        "-m", "256M",
        "-cpu", "max",
        "-drive", "if=pflash,format=raw,readonly=on,file=$($ovmf.Code)",
        "-drive", "if=pflash,format=raw,file=$VarsTemplate",
        "-drive", "file=fat:rw:build/image,format=raw",
        "-serial", "stdio",
        "-debugcon", "file:$DebugConLog",
        "-global", "isa-debugcon.iobase=0xe9",
        "-no-reboot",
        "-no-shutdown"
    )

    if ($WaitForDebugger) {
        $args += @("-s", "-S")
    }

    & $qemu @args
}

switch ($Command) {
    "build" {
        Build-Kernel
    }
    "run" {
        Run-Qemu
    }
    "debug" {
        Run-Qemu -WaitForDebugger
    }
    "clean" {
        if (Test-Path $BuildRoot) {
            Remove-Item -Recurse -Force $BuildRoot
        }
    }
}
