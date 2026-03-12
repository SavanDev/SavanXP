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
$RootfsBuild = Join-Path $BuildRoot "rootfs"
$DiskRoot = Join-Path $ProjectRoot "diskfs"
$InitramfsPath = Join-Path $BuildRoot "initramfs.cpio"
$DiskImage = Join-Path $BuildRoot "disk.img"
$ToolRoot = Join-Path $ProjectRoot "tools"
$LimineRoot = Join-Path $ToolRoot "limine"
$LimineBranch = "v10.x-binary"
$KernelElf = Join-Path $BuildRoot "kernel.elf"
$VarsTemplate = Join-Path $BuildRoot "OVMF_VARS.fd"
$DebugConLog = Join-Path $BuildRoot "debugcon.log"

$SvfsSectorSize = 512
$SvfsDirectorySectors = 8
$SvfsMaxFiles = 64
$SvfsTotalSectors = 32768

$KernelSources = @(
    "arch/x86_64/context.S",
    "arch/x86_64/entry.cpp",
    "arch/x86_64/cpu_init.cpp",
    "arch/x86_64/timer.cpp",
    "kernel/kernel_main.cpp",
    "kernel/console.cpp",
    "kernel/device.cpp",
    "kernel/pci.cpp",
    "kernel/input.cpp",
    "kernel/tty.cpp",
    "kernel/ui.cpp",
    "kernel/ps2.cpp",
    "kernel/pcspeaker.cpp",
    "kernel/heap.cpp",
    "kernel/net.cpp",
    "kernel/physical_memory.cpp",
    "kernel/vmm.cpp",
    "kernel/vfs.cpp",
    "kernel/block.cpp",
    "kernel/svfs.cpp",
    "kernel/elf.cpp",
    "kernel/process.cpp",
    "kernel/panic.cpp",
    "kernel/runtime.cpp"
)

$UserPrograms = @(
    @{ Name = "init"; Source = "userland/init.c" },
    @{ Name = "sh"; Source = "userland/sh.c" },
    @{ Name = "echo"; Source = "userland/echo.c" },
    @{ Name = "uname"; Source = "userland/uname.c" },
    @{ Name = "ls"; Source = "userland/ls.c" },
    @{ Name = "cat"; Source = "userland/cat.c" },
    @{ Name = "sleep"; Source = "userland/sleep.c" },
    @{ Name = "ticker"; Source = "userland/ticker.c" },
    @{ Name = "demo"; Source = "userland/demo.c" },
    @{ Name = "true"; Source = "userland/true.c" },
    @{ Name = "false"; Source = "userland/false.c" },
    @{ Name = "ps"; Source = "userland/ps.c" },
    @{ Name = "fdtest"; Source = "userland/fdtest.c" },
    @{ Name = "waittest"; Source = "userland/waittest.c" },
    @{ Name = "pipestress"; Source = "userland/pipestress.c" },
    @{ Name = "spawnloop"; Source = "userland/spawnloop.c" },
    @{ Name = "badptr"; Source = "userland/badptr.c" },
    @{ Name = "mv"; Source = "userland/mv.c" },
    @{ Name = "rm"; Source = "userland/rm.c" },
    @{ Name = "rmdir"; Source = "userland/rmdir.c" },
    @{ Name = "truncate"; Source = "userland/truncate.c" },
    @{ Name = "seektest"; Source = "userland/seektest.c" },
    @{ Name = "renametest"; Source = "userland/renametest.c" },
    @{ Name = "truncatetest"; Source = "userland/truncatetest.c" },
    @{ Name = "errtest"; Source = "userland/errtest.c" },
    @{ Name = "netinfo"; Source = "userland/netinfo.c" },
    @{ Name = "ping"; Source = "userland/ping.c" },
    @{ Name = "udpsend"; Source = "userland/udpsend.c" },
    @{ Name = "udprecv"; Source = "userland/udprecv.c" },
    @{ Name = "udptest"; Source = "userland/udptest.c" },
    @{ Name = "tcpget"; Source = "userland/tcpget.c" },
    @{ Name = "beep"; Source = "userland/beep.c" },
    @{ Name = "gfxdemo"; Source = "userland/gfxdemo.c" },
    @{ Name = "keytest"; Source = "userland/keytest.c" },
    @{ Name = "sysinfo"; Source = "userland/sysinfo.c" }
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

function Get-UserFlags {
    return @(
        "-target", "x86_64-unknown-none-elf",
        "-ffreestanding",
        "-fno-stack-protector",
        "-fno-pic",
        "-fno-pie",
        "-mno-red-zone",
        "-mcmodel=small",
        "-mno-mmx",
        "-mno-sse",
        "-mno-sse2",
        "-mgeneral-regs-only",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Wno-language-extension-token",
        "-Wno-c23-extensions",
        "-I", (Join-Path $ProjectRoot "include"),
        "-I", (Join-Path $ProjectRoot "userland")
    )
}

function Compile-Object([string]$Compiler, [string]$SourcePath, [string]$ObjectPath, [string[]]$Flags, [string]$Language = "") {
    $languageArgs = @()
    if ($Language) {
        $languageArgs = @("-x", $Language)
    }

    $compileArgs = @(
        "-c"
    ) + $languageArgs + @(
        $SourcePath,
        "-o", $ObjectPath
    ) + $Flags

    & $Compiler @compileArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo la compilacion de $SourcePath."
    }
}

function Write-CpioEntry([System.IO.BinaryWriter]$Writer, [string]$RelativePath, [byte[]]$Data, [uint32]$Mode) {
    $pathBytes = [System.Text.Encoding]::ASCII.GetBytes($RelativePath)
    $nameSize = $pathBytes.Length + 1
    $fileSize = if ($Data) { $Data.Length } else { 0 }
    $fields = @(
        "070701",
        ([Convert]::ToString(0, 16).PadLeft(8, '0')),
        ([Convert]::ToString($Mode, 16).PadLeft(8, '0')),
        ([Convert]::ToString(0, 16).PadLeft(8, '0')),
        ([Convert]::ToString(0, 16).PadLeft(8, '0')),
        ([Convert]::ToString(1, 16).PadLeft(8, '0')),
        ([Convert]::ToString(0, 16).PadLeft(8, '0')),
        ([Convert]::ToString($fileSize, 16).PadLeft(8, '0')),
        ([Convert]::ToString(0, 16).PadLeft(8, '0')),
        ([Convert]::ToString(0, 16).PadLeft(8, '0')),
        ([Convert]::ToString(0, 16).PadLeft(8, '0')),
        ([Convert]::ToString(0, 16).PadLeft(8, '0')),
        ([Convert]::ToString($nameSize, 16).PadLeft(8, '0')),
        ([Convert]::ToString(0, 16).PadLeft(8, '0'))
    )
    $headerBytes = [System.Text.Encoding]::ASCII.GetBytes(($fields -join ""))
    $Writer.Write($headerBytes)
    $Writer.Write($pathBytes)
    $Writer.Write([byte]0)
    while (($Writer.BaseStream.Position % 4) -ne 0) {
        $Writer.Write([byte]0)
    }
    if ($fileSize -gt 0) {
        $Writer.Write($Data)
    }
    while (($Writer.BaseStream.Position % 4) -ne 0) {
        $Writer.Write([byte]0)
    }
}

function Set-AsciiField([byte[]]$Buffer, [int]$Offset, [string]$Text, [int]$Capacity) {
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($Text)
    $count = [Math]::Min($bytes.Length, $Capacity - 1)
    [Array]::Copy($bytes, 0, $Buffer, $Offset, $count)
    $Buffer[$Offset + $count] = 0
}

function Get-AsciiField([byte[]]$Buffer, [int]$Offset, [int]$Capacity) {
    $length = 0
    while ($length -lt ($Capacity - 1) -and $Buffer[$Offset + $length] -ne 0) {
        $length += 1
    }
    return [System.Text.Encoding]::ASCII.GetString($Buffer, $Offset, $length)
}

function Get-UInt32Le([byte[]]$Buffer, [int]$Offset) {
    return [System.BitConverter]::ToUInt32($Buffer, $Offset)
}

function Set-UInt32Le([byte[]]$Buffer, [int]$Offset, [uint32]$Value) {
    $bytes = [System.BitConverter]::GetBytes($Value)
    [Array]::Copy($bytes, 0, $Buffer, $Offset, 4)
}

function Ensure-SvfsDisk([string]$SourceRoot, [string]$OutputPath) {
    if (Test-Path $OutputPath) {
        $bytes = [System.IO.File]::ReadAllBytes($OutputPath)
        if ($bytes.Length -ge $SvfsSectorSize) {
            $magic = Get-AsciiField $bytes 0 8
            if ($magic.StartsWith("SVFS1")) {
                $currentTotalSectors = Get-UInt32Le $bytes 12
                if ($currentTotalSectors -lt $SvfsTotalSectors) {
                    $newBytes = New-Object byte[] ($SvfsTotalSectors * $SvfsSectorSize)
                    [Array]::Copy($bytes, 0, $newBytes, 0, $bytes.Length)
                    Set-UInt32Le -Buffer $newBytes -Offset 12 -Value $SvfsTotalSectors
                    [System.IO.File]::WriteAllBytes($OutputPath, $newBytes)
                }
            }
        }
        return
    }

    New-Directory (Split-Path -Parent $OutputPath)

    $totalBytes = $SvfsTotalSectors * $SvfsSectorSize
    $image = New-Object byte[] $totalBytes
    $directoryLba = 1
    $dataLba = $directoryLba + $SvfsDirectorySectors
    $nextFreeLba = $dataLba

    Set-AsciiField -Buffer $image -Offset 0 -Text "SVFS1" -Capacity 8
    Set-UInt32Le -Buffer $image -Offset 8 -Value 1
    Set-UInt32Le -Buffer $image -Offset 12 -Value $SvfsTotalSectors
    Set-UInt32Le -Buffer $image -Offset 16 -Value $directoryLba
    Set-UInt32Le -Buffer $image -Offset 20 -Value $SvfsDirectorySectors
    Set-UInt32Le -Buffer $image -Offset 24 -Value $dataLba
    Set-UInt32Le -Buffer $image -Offset 28 -Value $SvfsMaxFiles

    $items = @()
    if (Test-Path $SourceRoot) {
        $items = Get-ChildItem -Path $SourceRoot -File | Sort-Object Name
    }

    $entryIndex = 0
    foreach ($item in $items) {
        if ($entryIndex -ge $SvfsMaxFiles) {
            throw "SVFS: demasiados archivos semilla."
        }

        $name = $item.Name
        if ($name.Length -ge 48) {
            throw "SVFS: nombre demasiado largo '$name'."
        }

        $bytes = [System.IO.File]::ReadAllBytes($item.FullName)
        $sectors = [Math]::Max([uint32]1, [uint32][Math]::Ceiling($bytes.Length / [double]$SvfsSectorSize))
        if (($nextFreeLba + $sectors) -gt $SvfsTotalSectors) {
            throw "SVFS: la imagen no tiene espacio suficiente para '$name'."
        }

        [Array]::Copy($bytes, 0, $image, $nextFreeLba * $SvfsSectorSize, $bytes.Length)

        $entryOffset = ($directoryLba * $SvfsSectorSize) + ($entryIndex * 64)
        $image[$entryOffset] = 1
        Set-AsciiField -Buffer $image -Offset ($entryOffset + 4) -Text $name -Capacity 48
        Set-UInt32Le -Buffer $image -Offset ($entryOffset + 52) -Value $nextFreeLba
        Set-UInt32Le -Buffer $image -Offset ($entryOffset + 56) -Value $sectors
        Set-UInt32Le -Buffer $image -Offset ($entryOffset + 60) -Value ([uint32]$bytes.Length)

        $nextFreeLba += $sectors
        $entryIndex += 1
    }

    Set-UInt32Le -Buffer $image -Offset 32 -Value $nextFreeLba
    [System.IO.File]::WriteAllBytes($OutputPath, $image)
}

function New-Initramfs([string]$SourceRoot, [string]$OutputPath) {
    $stream = [System.IO.File]::Open($OutputPath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
    try {
        $writer = New-Object System.IO.BinaryWriter($stream)
        $items = Get-ChildItem -Path $SourceRoot -Recurse | Sort-Object FullName
        foreach ($item in $items) {
            $relative = $item.FullName.Substring($SourceRoot.Length).TrimStart('\').Replace('\', '/')
            if ([string]::IsNullOrWhiteSpace($relative)) {
                continue
            }

            if ($item.PSIsContainer) {
                Write-CpioEntry -Writer $writer -RelativePath $relative -Data $null -Mode 16877
            } else {
                $mode = if ($relative.StartsWith("bin/")) { 33261 } else { 33188 }
                Write-CpioEntry -Writer $writer -RelativePath $relative -Data ([System.IO.File]::ReadAllBytes($item.FullName)) -Mode $mode
            }
        }
        Write-CpioEntry -Writer $writer -RelativePath "TRAILER!!!" -Data $null -Mode 0
        $writer.Flush()
    } finally {
        $stream.Dispose()
    }
}

function Build-Userland([string]$Compiler, [string]$Linker) {
    $userFlags = Get-UserFlags
    $userObjRoot = Join-Path $ObjRoot "user"
    $binRoot = Join-Path $RootfsBuild "bin"
    New-Directory $userObjRoot
    New-Directory $RootfsBuild
    New-Directory $binRoot

    Copy-Item (Join-Path $ProjectRoot "rootfs\\README") (Join-Path $RootfsBuild "README") -Force

    foreach ($program in $UserPrograms) {
        $objectFiles = @()
        foreach ($source in @("userland/crt0.S", "userland/libc.c", "userland/gfx.c", $program.Source)) {
            $sourcePath = Join-Path $ProjectRoot $source
            $objectName = "$($program.Name)_$([IO.Path]::GetFileNameWithoutExtension($source)).o"
            $objectPath = Join-Path $userObjRoot $objectName
            $objectFiles += $objectPath
            $language = ""
            if ($source.EndsWith(".c")) {
                $language = "c"
            } elseif ($source.EndsWith(".S")) {
                $language = "assembler-with-cpp"
            }
            Compile-Object -Compiler $Compiler -SourcePath $sourcePath -ObjectPath $objectPath -Flags $userFlags -Language $language
        }

        $outputPath = Join-Path $binRoot $program.Name
        $linkArgs = @(
            "-m", "elf_x86_64",
            "-T", (Join-Path $ProjectRoot "userland\\linker.ld"),
            "-z", "max-page-size=0x1000",
            "--build-id=none",
            "-o", $outputPath
        ) + $objectFiles

        & $Linker @linkArgs
        if ($LASTEXITCODE -ne 0) {
            throw "Fallo el link de userland para $($program.Name)."
        }
    }

    New-Initramfs -SourceRoot $RootfsBuild -OutputPath $InitramfsPath
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
    if (Test-Path $RootfsBuild) {
        Remove-Item -Recurse -Force $RootfsBuild
    }

    $commonFlags = Get-CommonFlags
    $objectFiles = @()

    foreach ($source in $KernelSources) {
        $sourcePath = Join-Path $ProjectRoot $source
        $objectPath = Join-Path $ObjRoot (([IO.Path]::GetFileNameWithoutExtension($source)) + ".o")
        $objectFiles += $objectPath
        Compile-Object -Compiler $clang -SourcePath $sourcePath -ObjectPath $objectPath -Flags $commonFlags
    }

    Build-Userland -Compiler $clang -Linker $ld
    Ensure-SvfsDisk -SourceRoot $DiskRoot -OutputPath $DiskImage

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
    Copy-Item $InitramfsPath (Join-Path $BootRoot "initramfs.cpio") -Force
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
        "-machine", "q35,pcspk-audiodev=audio0",
        "-m", "256M",
        "-cpu", "max",
        "-audiodev", "sdl,id=audio0",
        "-drive", "if=pflash,format=raw,readonly=on,file=$($ovmf.Code)",
        "-drive", "if=pflash,format=raw,file=$VarsTemplate",
        "-drive", "file=fat:rw:build/image,format=raw",
        "-netdev", "user,id=net0",
        "-device", "rtl8139,netdev=net0",
        "-device", "isa-ide,id=svide",
        "-drive", "if=none,id=svdisk,media=disk,format=raw,file=$DiskImage",
        "-device", "ide-hd,drive=svdisk,bus=svide.0",
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
