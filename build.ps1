param(
    [ValidateSet("build", "run", "debug", "smoke", "clean")]
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
$DiskBuildRoot = Join-Path $BuildRoot "diskfs"
$GeneratedRoot = Join-Path $BuildRoot "generated"
$DiskRoot = Join-Path $ProjectRoot "diskfs"
$BusyBoxPortRoot = Join-Path $ProjectRoot "vendor\\busybox-port"
$InitramfsPath = Join-Path $BuildRoot "initramfs.cpio"
$DiskImage = Join-Path $BuildRoot "disk.img"
$ToolRoot = Join-Path $ProjectRoot "tools"
$LimineRoot = Join-Path $ToolRoot "limine"
$LimineBranch = "v10.x-binary"
$KernelElf = Join-Path $BuildRoot "kernel.elf"
$VarsTemplate = Join-Path $BuildRoot "OVMF_VARS.fd"
$DebugConLog = Join-Path $BuildRoot "debugcon.log"
$SmokeSerialLog = Join-Path $BuildRoot "smoke-serial.log"
$SmokeStdoutLog = Join-Path $BuildRoot "smoke-qemu-stdout.log"
$SmokeStderrLog = Join-Path $BuildRoot "smoke-qemu-stderr.log"

. (Join-Path $ToolRoot "UserAppCommon.ps1")

$SvfsSectorSize = 512
$SvfsDirectorySectors = 8
$SvfsMaxFiles = 64
$SvfsTotalSectors = 131072

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
    "kernel/virtio_pci.cpp",
    "kernel/virtio_gpu.cpp",
    "kernel/virtio_input.cpp",
    "kernel/virtio_sound.cpp",
    "kernel/ps2.cpp",
    "kernel/pcspeaker.cpp",
    "kernel/rtc.cpp",
    "kernel/heap.cpp",
    "kernel/net.cpp",
    "kernel/object.cpp",
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
    @{ Name = "sh"; Sources = @("userland/sh.c", "userland/shell_core.c") },
    @{ Name = "shellapp"; Sources = @("userland/shellapp.c", "userland/shell_core.c") },
    @{ Name = "uname"; Source = "userland/uname.c" },
    @{ Name = "df"; Source = "userland/df.c" },
    @{ Name = "ticker"; Source = "userland/ticker.c" },
    @{ Name = "demo"; Source = "userland/demo.c" },
    @{ Name = "ps_legacy"; Source = "userland/ps.c" },
    @{ Name = "fdtest"; Source = "userland/fdtest.c" },
    @{ Name = "waittest"; Source = "userland/waittest.c" },
    @{ Name = "pipestress"; Source = "userland/pipestress.c" },
    @{ Name = "spawnloop"; Source = "userland/spawnloop.c" },
    @{ Name = "badptr"; Source = "userland/badptr.c" },
    @{ Name = "rmdir"; Source = "userland/rmdir.c" },
    @{ Name = "truncate"; Source = "userland/truncate.c" },
    @{ Name = "sync"; Source = "userland/sync.c" },
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
    @{ Name = "audiotest"; Source = "userland/audiotest.c" },
    @{ Name = "desktop"; Source = "userland/desktop.c" },
    @{ Name = "gfxdemo"; Source = "userland/gfxdemo.c" },
    @{ Name = "gputest"; Source = "userland/gputest.c" },
    @{ Name = "keytest"; Source = "userland/keytest.c" },
    @{ Name = "mousetest"; Source = "userland/mousetest.c" },
    @{ Name = "sysinfo"; Source = "userland/sysinfo.c" },
    @{ Name = "forktest"; Source = "userland/forktest.c" },
    @{ Name = "polltest"; Source = "userland/polltest.c" },
    @{ Name = "sigtest"; Source = "userland/sigtest.c" },
    @{ Name = "eventtest"; Source = "userland/eventtest.c" },
    @{ Name = "timertest"; Source = "userland/timertest.c" },
    @{ Name = "sectiontest"; Source = "userland/sectiontest.c" },
    @{ Name = "mmaptest"; Source = "userland/mmaptest.c" },
    @{ Name = "smoke"; Source = "userland/smoke.c" }
)

$BusyBoxApplets = @(
    "busybox",
    "ls",
    "cat",
    "echo",
    "mkdir",
    "rm",
    "mv",
    "cp",
    "ps",
    "true",
    "false",
    "sleep"
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
        "-I", (Join-Path $ProjectRoot "userland"),
        "-I", $GeneratedRoot
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
    Initialize-Svfs2DiskImage -SourceRoot $SourceRoot -OutputPath $OutputPath
}

function Get-ByteArraySha256([byte[]]$Bytes) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace("-", "")
    } finally {
        $sha.Dispose()
    }
}

function Get-SvfsPersistenceSnapshot($Image, [string[]]$Paths) {
    $snapshot = @{}
    foreach ($path in $Paths) {
        $info = Get-Svfs2PathInfo $Image $path
        if (-not $info) {
            continue
        }

        $record = [ordered]@{
            Path = $info.Path
            EntryType = $info.Entry.Type
            InodeType = $info.Inode.Type
            Size = [uint32]$info.Inode.Size
        }
        if ($info.Entry.Type -eq 1 -and $info.Inode.Type -eq 1) {
            $record.Hash = Get-ByteArraySha256 (Read-Svfs2InodeBytes $Image $info.Inode)
        }
        $snapshot[$path] = [pscustomobject]$record
    }
    return $snapshot
}

function Assert-SvfsPersistenceRetained($Image, $BeforeSnapshot) {
    foreach ($path in $BeforeSnapshot.Keys) {
        $before = $BeforeSnapshot[$path]
        $after = Get-Svfs2PathInfo $Image $path
        if (-not $after) {
            throw "La build perdio el path persistente '$path'."
        }
        if ($after.Entry.Type -ne $before.EntryType -or $after.Inode.Type -ne $before.InodeType) {
            throw "La build cambio el tipo persistente de '$path' (antes entry=$($before.EntryType)/inode=$($before.InodeType), ahora entry=$($after.Entry.Type)/inode=$($after.Inode.Type))."
        }
        if ($before.PSObject.Properties.Match("Hash").Count -ne 0) {
            $afterHash = Get-ByteArraySha256 (Read-Svfs2InodeBytes $Image $after.Inode)
            if ($afterHash -ne $before.Hash) {
                throw "La build modifico el contenido persistente de '$path'."
            }
        }
    }
}

function Repair-SvfsReachableMetadataAndReport($Image, [string]$Phase) {
    $repair = Repair-Svfs2ReachableMetadata $Image
    if ($repair.InodeBitsFixed -eq 0 -and $repair.BlockBitsFixed -eq 0) {
        return
    }

    Write-Host ("SVFS2: metadata repaired before {0} (inode bits={1}, block bits={2}, reachable inodes={3})" -f `
        $Phase, $repair.InodeBitsFixed, $repair.BlockBitsFixed, $repair.ReachableInodes)
}

function Sync-SvfsDiskTree($Image, [string]$SourceRoot, [string]$DestinationRoot = "/disk") {
    if (-not (Test-Path $SourceRoot)) {
        return
    }

    $items = Get-ChildItem -Path $SourceRoot -Recurse | Sort-Object FullName
    foreach ($item in $items) {
        $relative = $item.FullName.Substring($SourceRoot.Length).TrimStart('\').Replace('\', '/')
        if ([string]::IsNullOrWhiteSpace($relative)) {
            continue
        }

        if ($item.PSIsContainer) {
            Ensure-SvfsDirectory $Image $relative
        } else {
            Install-SvfsFile -Image $Image -DestinationPath ("$DestinationRoot/$relative") -Data ([System.IO.File]::ReadAllBytes($item.FullName))
        }
    }
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

function Generate-CursorAsset {
    New-Directory $GeneratedRoot

    $scriptPath = Join-Path $ToolRoot "GenerateCursorAsset.ps1"
    $sourcePath = Join-Path $ProjectRoot "assets\\cursor.png"
    $outputPath = Join-Path $GeneratedRoot "cursor_asset.h"

    & powershell -ExecutionPolicy Bypass -File $scriptPath -SourcePath $sourcePath -OutputPath $outputPath
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo la generacion del asset del cursor."
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
        $programSources = if ($program.ContainsKey("Sources")) { $program.Sources } else { @($program.Source) }
        foreach ($source in @("userland/crt0.S", "userland/libc.c", "userland/gfx.c") + $programSources) {
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

function Install-BusyBox {
    New-Directory $RootfsBuild
    New-Directory (Join-Path $RootfsBuild "bin")

    $busyboxOutput = Join-Path $RootfsBuild "bin\\busybox"
    $builtBusyBox = Build-ExternalUserProgram -SourcePath $BusyBoxPortRoot -ProgramName "busybox" -OutputPath $busyboxOutput
    foreach ($applet in $BusyBoxApplets) {
        if ($applet -eq "busybox") {
            continue
        }
        Copy-Item $builtBusyBox (Join-Path $RootfsBuild "bin\\$applet") -Force
    }
}

function Build-Kernel([switch]$SmokeMode) {
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
    if (Test-Path $DiskBuildRoot) {
        Remove-Item -Recurse -Force $DiskBuildRoot
    }
    New-Directory $GeneratedRoot

    $commonFlags = Get-CommonFlags
    $objectFiles = @()

    foreach ($source in $KernelSources) {
        $sourcePath = Join-Path $ProjectRoot $source
        $objectPath = Join-Path $ObjRoot (([IO.Path]::GetFileNameWithoutExtension($source)) + ".o")
        $objectFiles += $objectPath
        Compile-Object -Compiler $clang -SourcePath $sourcePath -ObjectPath $objectPath -Flags $commonFlags
    }

    Generate-CursorAsset
    Build-Userland -Compiler $clang -Linker $ld
    Install-BusyBox
    if ($SmokeMode) {
        Set-Content -Path (Join-Path $RootfsBuild "SMOKE") -Value "smoke" -NoNewline
    }
    New-Initramfs -SourceRoot $RootfsBuild -OutputPath $InitramfsPath
    Copy-Item $DiskRoot $DiskBuildRoot -Recurse -Force
    New-Directory (Join-Path $DiskBuildRoot "bin")
    Copy-Item (Join-Path $RootfsBuild "bin\\*") (Join-Path $DiskBuildRoot "bin") -Force
    Ensure-SvfsDisk -SourceRoot $DiskBuildRoot -OutputPath $DiskImage
    $diskImage = Open-SvfsImage $DiskImage
    Repair-SvfsReachableMetadataAndReport $diskImage "pre-sync"
    Assert-Svfs2Consistency $diskImage $DiskImage
    $persistentSnapshot = Get-SvfsPersistenceSnapshot $diskImage @(
        "/disk/bin/doomgeneric",
        "/disk/games/doom/doom1.wad"
    )
    Sync-SvfsDiskTree -Image $diskImage -SourceRoot $DiskBuildRoot
    Repair-SvfsReachableMetadataAndReport $diskImage "post-sync"
    Assert-Svfs2Consistency $diskImage $DiskImage
    Assert-SvfsPersistenceRetained $diskImage $persistentSnapshot
    Save-SvfsImage $diskImage

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
        "-audiodev", "sdl,id=audio1",
        "-display", "gtk,grab-on-hover=on,show-cursor=off,window-close=on",
        "-rtc", "base=localtime",
        "-drive", "if=pflash,format=raw,readonly=on,file=$($ovmf.Code)",
        "-drive", "if=pflash,format=raw,file=$VarsTemplate",
        "-drive", "file=fat:rw:build/image,format=raw",
        "-netdev", "user,id=net0",
        "-device", "rtl8139,netdev=net0",
        "-device", "virtio-vga,xres=1280,yres=800",
        "-device", "virtio-sound-pci,audiodev=audio1,streams=1",
        "-device", "virtio-tablet-pci",
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

function Run-SmokeQemu {
    $qemu = Require-Executable "qemu-system-x86_64" @(
        "qemu-system-x86_64",
        "C:\\Program Files\\qemu\\qemu-system-x86_64.exe"
    )
    Build-Kernel -SmokeMode

    $ovmf = Resolve-OvmfPair
    Copy-Item $ovmf.Vars $VarsTemplate -Force
    if (Test-Path $DebugConLog) {
        Remove-Item $DebugConLog -Force
    }
    if (Test-Path $SmokeSerialLog) {
        Remove-Item $SmokeSerialLog -Force
    }
    if (Test-Path $SmokeStdoutLog) {
        Remove-Item $SmokeStdoutLog -Force
    }
    if (Test-Path $SmokeStderrLog) {
        Remove-Item $SmokeStderrLog -Force
    }
    $args = @(
        "-machine", "q35,pcspk-audiodev=audio0",
        "-m", "256M",
        "-cpu", "max",
        "-audiodev", "none,id=audio0",
        "-audiodev", "none,id=audio1",
        "-display", "none",
        "-rtc", "base=localtime",
        "-drive", "if=pflash,format=raw,readonly=on,file=""$($ovmf.Code)""",
        "-drive", "if=pflash,format=raw,file=""$VarsTemplate""",
        "-drive", "file=fat:rw:build/image,format=raw",
        "-netdev", "user,id=net0",
        "-device", "rtl8139,netdev=net0",
        "-device", "virtio-vga,xres=1280,yres=800",
        "-device", "virtio-sound-pci,audiodev=audio1,streams=1",
        "-device", "virtio-tablet-pci",
        "-device", "isa-ide,id=svide",
        "-drive", "if=none,id=svdisk,media=disk,format=raw,file=""$DiskImage""",
        "-device", "ide-hd,drive=svdisk,bus=svide.0",
        "-serial", "file:$SmokeSerialLog",
        "-debugcon", "file:$DebugConLog",
        "-global", "isa-debugcon.iobase=0xe9",
        "-no-reboot",
        "-no-shutdown"
    )

    $process = Start-Process -FilePath $qemu -ArgumentList $args -PassThru -RedirectStandardOutput $SmokeStdoutLog -RedirectStandardError $SmokeStderrLog
    try {
        $deadline = (Get-Date).AddMinutes(2)
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 1000
            if (-not (Test-Path $SmokeSerialLog)) {
                if ($process.HasExited) {
                    break
                }
                continue
            }

            $content = Get-Content $SmokeSerialLog -Raw
            if ($content -match "SMOKE PASS") {
                Start-Sleep -Milliseconds 2000
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
                Write-Host "Smoke PASS"
                return
            }
            if ($content -match "SMOKE FAIL") {
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
                throw "Smoke FAIL. Revisar $SmokeSerialLog"
            }
            if ($process.HasExited) {
                break
            }
        }
    } finally {
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
    }

    if ($process.HasExited) {
        throw "Smoke aborted. Revisar $SmokeSerialLog y $SmokeStderrLog"
    }
    throw "Smoke timeout. Revisar $SmokeSerialLog y $SmokeStderrLog"
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
    "smoke" {
        Run-SmokeQemu
    }
    "clean" {
        if (Test-Path $BuildRoot) {
            Remove-Item -Recurse -Force $BuildRoot
        }
    }
}
