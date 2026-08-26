param(
    [ValidateSet("build", "iso", "run", "debug", "smoke", "ac97-stream", "ac97-count", "virtio-count", "virtio-stream", "windowd-smoke", "progman-smoke", "sxe-smoke", "filesapp-smoke", "net-smoke", "cursor-repro", "gpu-soak", "native-guihost", "native-hello", "native-sxgui", "clean")]
    [string]$Command = "build",

    [ValidateRange(1, 4096)]
    [int]$GpuSoakIterations = 96,

    # whpx usa el Windows Hypervisor Platform (Hyper-V) en vez de TCG. Con
    # "-cpu max"/"-cpu host" bajo whpx, OVMF crashea con #GP en PlatformPei
    # apenas arranca: WHPX no puede respaldar features de CPU muy nuevas
    # (APX y afines) que esos modelos exponen al guest. "qemu64" evita el
    # problema y ya viene confirmado bootenado end-to-end bajo whpx.
    [ValidateSet("tcg", "whpx")]
    [string]$Accel = "tcg",

    # Levanta QEMU con hardware paravirtualizado (virtio-vga + virtio-tablet +
    # virtio-sound). Por defecto NO se usa: la maquina se arma con el hardware
    # "base" (VGA estandar, mouse PS/2, AC'97), que es el mismo que emula
    # VirtualBox, para que el kernel ejercite los backends de fallback
    # (fb_gpu / ps2 / ac97) sin salir de QEMU.
    [switch]$Virtio,

    # Excluye del build las apps de testeo/diagnostico (marcadas Test en
    # $UserPrograms): no se compilan, no entran al rootfs y el menu del
    # escritorio se compila sin sus entradas. Los comandos de automatizacion
    # (smoke, windowd-smoke, etc.) las fuerzan siempre porque sus harnesses
    # dependen de ellas.
    [switch]$NoTestApps
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildRoot = Join-Path $ProjectRoot "build"
$ObjRoot = Join-Path $BuildRoot "obj"
$ImageRoot = Join-Path $BuildRoot "image"
$BootRoot = Join-Path $ImageRoot "boot"
$EfiBootRoot = Join-Path $ImageRoot "EFI/BOOT"
$RootfsBuild = Join-Path $BuildRoot "rootfs"
$DiskBuildRoot = Join-Path $BuildRoot "diskfs"
$GeneratedRoot = Join-Path $BuildRoot "generated"
# Blobs .sxmeta/.sxicon que se estampan en los binarios (docs/SXE_FORMAT.md).
$SxeResourceRoot = Join-Path $GeneratedRoot "sxe"
$DiskRoot = Join-Path $ProjectRoot "diskfs"
$BusyBoxPortRoot = Join-Path $ProjectRoot "vendor/busybox-port"
$InitramfsPath = Join-Path $BuildRoot "initramfs.cpio"
$DiskImage = Join-Path $BuildRoot "disk.img"
$IsoImage = Join-Path $BuildRoot "SavanXP.iso"
$ToolRoot = Join-Path $ProjectRoot "tools"
$SubsystemRoot = Join-Path $ProjectRoot "subsystems"
$PosixRoot = Join-Path $SubsystemRoot "posix"
$PosixKernelRoot = Join-Path $PosixRoot "kernel"
$PosixSdkRoot = Join-Path $PosixRoot "sdk/v1"
$PosixUserlandRoot = Join-Path $PosixRoot "userland"
$LimineRoot = Join-Path $ToolRoot "limine"
$LimineBranch = "v10.x-binary"
$KernelElf = Join-Path $BuildRoot "kernel.elf"
$VarsTemplate = Join-Path $BuildRoot "OVMF_VARS.fd"
$DebugConLog = Join-Path $BuildRoot "debugcon.log"
$SmokeSerialLog = Join-Path $BuildRoot "smoke-serial.log"
$SmokeStdoutLog = Join-Path $BuildRoot "smoke-qemu-stdout.log"
$SmokeStderrLog = Join-Path $BuildRoot "smoke-qemu-stderr.log"

. (Join-Path $ToolRoot "UserAppCommon.ps1")
. (Join-Path $ToolRoot "Toolchain.ps1")
. (Join-Path $ToolRoot "Ninja.ps1")

$SvfsSectorSize = 512
$SvfsDirectorySectors = 8
$SvfsMaxFiles = 64
$SvfsTotalSectors = 131072

$PosixKernelSources = @(
    Get-ChildItem -Path $PosixKernelRoot -Filter "*.cpp" -File |
        Sort-Object FullName |
        ForEach-Object { $_.FullName.Substring($ProjectRoot.Length + 1).Replace("\", "/") }
)

$KernelSources = @(
    "arch/x86_64/context.S",
    "arch/x86_64/entry.cpp",
    "arch/x86_64/cpu_init.cpp",
    "arch/x86_64/timer.cpp",
    "kernel/kernel_main.cpp",
    "kernel/boot_screen.cpp",
    "kernel/console.cpp",
    "kernel/device.cpp",
    "kernel/display.cpp",
    "kernel/ata.cpp",
    "kernel/ramdisk.cpp",
    "kernel/nic.cpp",
    "kernel/rtl8139.cpp",
    "kernel/pci.cpp",
    "kernel/input.cpp",
    "kernel/tty.cpp",
    "kernel/ui.cpp",
    "kernel/virtio_pci.cpp",
    "kernel/virtio_gpu.cpp",
    "kernel/fb_gpu.cpp",
    "kernel/gpu_device.cpp",
    "kernel/virtio_input.cpp",
    "kernel/virtio_sound.cpp",
    "kernel/ac97.cpp",
    "kernel/audio.cpp",
    "kernel/audio_device.cpp",
    "kernel/ps2.cpp",
    "kernel/pcspeaker.cpp",
    "kernel/power.cpp",
    "kernel/acpi.cpp",
    "kernel/ioapic.cpp",
    "kernel/uacpi_glue.cpp",
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
    "kernel/subsystem.cpp",
    "kernel/panic.cpp",
    "kernel/runtime.cpp"
) + $PosixKernelSources

$UserPrograms = @(
    @{ Name = "init"; Source = "subsystems/posix/userland/init.c" },
    @{ Name = "sh"; Sources = @("subsystems/posix/userland/sh.c", "subsystems/posix/userland/shell_core.c") },
    @{ Name = "shellapp"; Sources = @("subsystems/posix/userland/shellapp.c", "subsystems/posix/userland/shell_core.c") },
    @{ Name = "uname"; Source = "subsystems/posix/userland/uname.c" },
    @{ Name = "df"; Source = "subsystems/posix/userland/df.c" },
    @{ Name = "ticker"; Source = "subsystems/posix/userland/ticker.c"; Test = $true },
    @{ Name = "demo"; Source = "subsystems/posix/userland/demo.c"; Test = $true },
    @{ Name = "fdtest"; Source = "subsystems/posix/userland/fdtest.c"; Test = $true },
    @{ Name = "waittest"; Source = "subsystems/posix/userland/waittest.c"; Test = $true },
    @{ Name = "pipestress"; Source = "subsystems/posix/userland/pipestress.c"; Test = $true },
    @{ Name = "spawnloop"; Source = "subsystems/posix/userland/spawnloop.c"; Test = $true },
    @{ Name = "badptr"; Source = "subsystems/posix/userland/badptr.c"; Test = $true },
    @{ Name = "rmdir"; Source = "subsystems/posix/userland/rmdir.c" },
    @{ Name = "truncate"; Source = "subsystems/posix/userland/truncate.c" },
    @{ Name = "sync"; Source = "subsystems/posix/userland/sync.c" },
    @{ Name = "seektest"; Source = "subsystems/posix/userland/seektest.c"; Test = $true },
    @{ Name = "renametest"; Source = "subsystems/posix/userland/renametest.c"; Test = $true },
    @{ Name = "truncatetest"; Source = "subsystems/posix/userland/truncatetest.c"; Test = $true },
    @{ Name = "errtest"; Source = "subsystems/posix/userland/errtest.c"; Test = $true },
    @{ Name = "netinfo"; Source = "subsystems/posix/userland/netinfo.c" },
    @{ Name = "ping"; Source = "subsystems/posix/userland/ping.c" },
    @{ Name = "udpsend"; Source = "subsystems/posix/userland/udpsend.c" },
    @{ Name = "udprecv"; Source = "subsystems/posix/userland/udprecv.c" },
    @{ Name = "udptest"; Source = "subsystems/posix/userland/udptest.c"; Test = $true },
    @{ Name = "nettest"; Source = "subsystems/posix/userland/nettest.c"; Test = $true },
    @{ Name = "tcpget"; Source = "subsystems/posix/userland/tcpget.c" },
    @{ Name = "beep"; Source = "subsystems/posix/userland/beep.c" },
    @{ Name = "audiotest"; Source = "subsystems/posix/userland/audiotest.c"; Test = $true },
    @{ Name = "compositord"; Source = "subsystems/posix/userland/compositord.c" },
    # windowd lee los recursos SXE del binario que lanza (fase 4).
    @{ Name = "windowd"; Sources = @(
        "subsystems/posix/sdk/v1/runtime/sxe.c",
        "subsystems/posix/userland/windowd.c",
        "subsystems/posix/userland/windowd_compositor_client.c",
        "subsystems/posix/userland/desktop_icons.c",
        "subsystems/posix/userland/windowd_appinfo.c",
        "subsystems/posix/userland/desktop_wallpaper.c",
        "subsystems/posix/userland/windowd_layout.c",
        "subsystems/posix/userland/windowd_render.c"
    ) },
    @{ Name = "shellui"; Sources = @(
        "subsystems/posix/userland/shellui.c",
        "subsystems/posix/userland/desktop_wallpaper.c"
    ) },
    # progman lee los recursos SXE de los binarios que lista (fase 3).
    @{ Name = "progman"; Sources = @(
        "subsystems/posix/sdk/v1/runtime/sxe.c",
        "subsystems/posix/userland/progman.c",
        "subsystems/posix/userland/progman_registry.c",
        "subsystems/posix/userland/desktop_icons.c",
        "subsystems/posix/userland/desktop_wallpaper.c",
        "subsystems/posix/sdk/v1/runtime/sxgui.c",
        "subsystems/posix/sdk/v1/runtime/sxgui_app.c",
        "subsystems/posix/sdk/v1/runtime/posix.c"
    ) },
    @{ Name = "aboutapp"; Sources = @(
        "subsystems/posix/userland/aboutapp.c",
        "subsystems/posix/sdk/v1/runtime/sxgui.c",
        "subsystems/posix/sdk/v1/runtime/sxgui_app.c",
        "subsystems/posix/sdk/v1/runtime/posix.c"
    ) },
    # filesapp resuelve asociaciones leyendo el .sxmeta de los binarios (fase 5).
    @{ Name = "filesapp"; Sources = @(
        "subsystems/posix/userland/filesapp.c",
        "subsystems/posix/userland/file_assoc.c",
        "subsystems/posix/sdk/v1/runtime/sxe.c",
        "subsystems/posix/sdk/v1/runtime/sxgui.c",
        "subsystems/posix/sdk/v1/runtime/sxgui_app.c",
        "subsystems/posix/sdk/v1/runtime/posix.c"
    ) },
    @{ Name = "notepad"; Sources = @(
        "subsystems/posix/userland/notepad.c",
        "subsystems/posix/sdk/v1/runtime/sxgui.c",
        "subsystems/posix/sdk/v1/runtime/sxgui_app.c",
        "subsystems/posix/sdk/v1/runtime/posix.c"
    ) },
    @{ Name = "widgetsdemo"; Sources = @(
        "subsystems/posix/userland/widgetsdemo.c",
        "subsystems/posix/sdk/v1/runtime/sxgui.c",
        "subsystems/posix/sdk/v1/runtime/sxgui_app.c",
        "subsystems/posix/sdk/v1/runtime/posix.c"
    ); Test = $true },
    @{ Name = "gfxdemo"; Source = "subsystems/posix/userland/gfxdemo.c"; Test = $true },
    @{ Name = "gputest"; Source = "subsystems/posix/userland/gputest.c"; Test = $true },
    @{ Name = "keytest"; Source = "subsystems/posix/userland/keytest.c"; Test = $true },
    @{ Name = "mousetest"; Source = "subsystems/posix/userland/mousetest.c"; Test = $true },
    @{ Name = "sysinfo"; Source = "subsystems/posix/userland/sysinfo.c" },
    @{ Name = "forktest"; Source = "subsystems/posix/userland/forktest.c"; Test = $true },
    @{ Name = "polltest"; Source = "subsystems/posix/userland/polltest.c"; Test = $true },
    @{ Name = "sigtest"; Source = "subsystems/posix/userland/sigtest.c"; Test = $true },
    @{ Name = "eventtest"; Source = "subsystems/posix/userland/eventtest.c"; Test = $true },
    @{ Name = "timertest"; Source = "subsystems/posix/userland/timertest.c"; Test = $true },
    @{ Name = "sectiontest"; Source = "subsystems/posix/userland/sectiontest.c"; Test = $true },
    @{ Name = "semaphoretest"; Source = "subsystems/posix/userland/semaphoretest.c"; Test = $true },
    @{ Name = "mmaptest"; Source = "subsystems/posix/userland/mmaptest.c"; Test = $true },
    @{ Name = "smoke"; Source = "subsystems/posix/userland/smoke.c"; Test = $true },
    @{ Name = "sxetest"; Sources = @(
        "subsystems/posix/userland/sxetest.c",
        "subsystems/posix/sdk/v1/runtime/sxe.c"
    ); Test = $true }
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

# Resolve-OvmfPair vive ahora en tools/Toolchain.ps1.

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
        "-I", (Join-Path $PosixSdkRoot "include"),
        "-I", (Join-Path $ProjectRoot "vendor"),
        "-I", (Join-Path $ProjectRoot "vendor/uacpi/include")
    )
}

# Flags para compilar uACPI (vendorizado, C11). Misma ABI freestanding que el
# kernel (mismo code model / sin SSE / red-zone) para que linkee, pero como C y
# con warnings relajados (codigo de terceros). UACPI_USE_BUILTIN_STRING hace que
# uACPI use __builtin_mem* en vez de depender de simbolos libc extra.
function Get-UacpiFlags {
    return @(
        "-std=gnu11",
        "-target", "x86_64-unknown-none-elf",
        "-ffreestanding",
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
        "-DUACPI_USE_BUILTIN_STRING",
        "-I", (Join-Path $ProjectRoot "vendor/uacpi/include")
    )
}

function Get-UacpiCompileEdges {
    $uacpiObjRoot = Join-Path $ObjRoot "uacpi"
    New-Directory $uacpiObjRoot

    $edges = @()
    $objectFiles = @()
    $sources = Get-ChildItem -Path (Join-Path $ProjectRoot "vendor/uacpi/source") -Filter "*.c" -File | Sort-Object Name
    foreach ($source in $sources) {
        $objectPath = Join-Path $uacpiObjRoot ($source.BaseName + ".o")
        $objectFiles += $objectPath
        $edges += [pscustomobject]@{
            SourcePath = $source.FullName
            ObjectPath = $objectPath
            FlagsVar = "uacpiflags"
            LangFlag = "-x c"
        }
    }

    return [pscustomobject]@{ Edges = $edges; ObjectFiles = $objectFiles }
}

function Get-UserFlags([bool]$IncludeTestApps = $true) {
    return @(
        "-DDESKTOP_INCLUDE_TEST_APPS=$(if ($IncludeTestApps) { 1 } else { 0 })",
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
        "-I", (Join-Path $PosixSdkRoot "include"),
        "-I", $PosixUserlandRoot,
        "-I", $GeneratedRoot
    )
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

# Rutas persistentes que una rebuild NO debe recrear ni modificar: viven en el
# disco, no en el arbol de build. Se snapshotean antes del sync y se verifican
# despues.
$Script:SvfsPersistentPaths = @(
    "/disk/bin/doomgeneric",
    "/disk/games/doom/freedoom1.wad"
)

# Construye/actualiza build/disk.img desde el arbol $SourceRoot con el tool
# nativo libsvfs (svfs-cli): el tool hace TODAS las escrituras (create +
# sync/populate + flush). PowerShell queda como pura validacion: snapshot de
# persistencia antes del sync y, tras el sync, chequeo de consistencia y de que
# lo persistente sobrevivio. No hay byte-poking PS en el camino de escritura.
function Build-SvfsDiskImage([string]$SourceRoot, [string]$OutputPath) {
    $exe = Build-SvfsCli

    # Crea la imagen si falta o si la existente no es un SVFS2 valido. Una imagen
    # valida se preserva (persistencia); el sync de abajo reconcilia el arbol.
    if (Test-Path $OutputPath) {
        try {
            Open-SvfsImage $OutputPath | Out-Null
        } catch {
            Remove-Item $OutputPath -Force
        }
    }
    if (-not (Test-Path $OutputPath)) {
        New-Directory (Split-Path -Parent $OutputPath)
        & $exe create $OutputPath $Script:Svfs2TotalSectors
        if ($LASTEXITCODE -ne 0) {
            throw "svfs-cli create fallo para '$OutputPath'."
        }
    }

    # Snapshot de persistencia ANTES del sync (imagen tal cual quedo de la build
    # anterior, o recien creada vacia).
    $before = Open-SvfsImage $OutputPath
    $snapshot = Get-SvfsPersistenceSnapshot $before $Script:SvfsPersistentPaths

    # Sync: mkdir + install de todo el arbol en una sola pasada del tool. Es
    # aditivo (no borra): reconcilia el arbol preservando lo persistente. Si la
    # imagen preservada se fragmento hasta no tener corrida contigua libre,
    # Invoke-SvfsApply la compacta (preservando lo que no produce esta build) y
    # reintenta, avisando en el output.
    $manifest = New-SvfsManifest -SourceRoot $SourceRoot
    Invoke-SvfsApply -Exe $exe -ImagePath $OutputPath -ManifestPath $manifest `
        -FailureMessage "svfs-cli apply fallo para '$OutputPath'."

    # Validacion de solo lectura: el tool ya persistio, PS no reescribe.
    $after = Open-SvfsImage $OutputPath
    Assert-Svfs2Consistency $after $OutputPath
    Assert-SvfsPersistenceRetained $after $snapshot
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

    $python = Get-PythonExecutable
    $scriptPath = Join-Path $ToolRoot "gen_cursor_asset.py"
    $outputPath = Join-Path $GeneratedRoot "cursor_asset.h"

    & $python $scriptPath --project-root $ProjectRoot --output $outputPath
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo la generacion de cursor_asset.h."
    }
}

function Generate-DesktopIconAssets {
    New-Directory $GeneratedRoot

    $python = Get-PythonExecutable
    $sourceArtScript = Join-Path $ToolRoot "gen_desktop_source_art.py"
    $scriptPath = Join-Path $ToolRoot "gen_desktop_icon_assets.py"
    $outputPath = Join-Path $GeneratedRoot "desktop_icon_assets.h"

    & $python $sourceArtScript --project-root $ProjectRoot
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo la generacion del arte fuente del desktop."
    }
    & $python $scriptPath --project-root $ProjectRoot --output $outputPath
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo la generacion de desktop_icon_assets.h."
    }
}

# Los blobs .sxmeta/.sxicon de todos los programas in-tree, en una sola corrida
# de python: el costo es el arranque del interprete, no el trabajo. El
# estampado en si lo hace Add-SxeResources (tools/UserAppCommon.ps1), compartido
# con el camino de apps externas.
function Generate-SxeResources {
    Invoke-SxeResourceGenerator -ManifestDirs @($PosixUserlandRoot) -OutputDir $SxeResourceRoot
}

function Get-KernelCompileEdges([string[]]$Sources) {
    $edges = @()
    $objectFiles = @()

    foreach ($source in $Sources) {
        $sourcePath = Join-Path $ProjectRoot $source
        $objectPath = Join-Path $ObjRoot (([IO.Path]::GetFileNameWithoutExtension($source)) + ".o")
        $objectFiles += $objectPath
        $edges += [pscustomobject]@{
            SourcePath = $sourcePath
            ObjectPath = $objectPath
            FlagsVar = "kernelflags"
            LangFlag = ""
        }
    }

    return [pscustomobject]@{ Edges = $edges; ObjectFiles = $objectFiles }
}

function Get-UserlandCompileEdges([bool]$IncludeTestApps = $true) {
    $userObjRoot = Join-Path $ObjRoot "user"
    New-Directory $userObjRoot

    $edges = @()
    $programs = @()

    foreach ($program in $UserPrograms) {
        if (-not $IncludeTestApps -and $program.ContainsKey("Test") -and $program.Test) {
            continue
        }
        $objectFiles = @()
        $programSources = if ($program.ContainsKey("Sources")) { $program.Sources } else { @($program.Source) }
        foreach ($source in @(
            "subsystems/posix/sdk/v1/runtime/crt0.S",
            "subsystems/posix/sdk/v1/runtime/libc.c",
            "subsystems/posix/sdk/v1/runtime/gfx.c",
            "subsystems/posix/sdk/v1/runtime/gfx2d.c"
        ) + $programSources) {
            $sourcePath = Join-Path $ProjectRoot $source
            $objectName = "$($program.Name)_$([IO.Path]::GetFileNameWithoutExtension($source)).o"
            $objectPath = Join-Path $userObjRoot $objectName
            $objectFiles += $objectPath

            $langFlag = ""
            if ($source.EndsWith(".c")) {
                $langFlag = "-x c"
            } elseif ($source.EndsWith(".S")) {
                $langFlag = "-x assembler-with-cpp"
            }

            $edges += [pscustomobject]@{
                SourcePath = $sourcePath
                ObjectPath = $objectPath
                FlagsVar = "userflags"
                LangFlag = $langFlag
            }
        }

        $programs += [pscustomobject]@{ Name = $program.Name; ObjectFiles = $objectFiles }
    }

    return [pscustomobject]@{ Edges = $edges; Programs = $programs }
}

function Build-Userland([string]$Linker, [object[]]$Programs) {
    $binRoot = Join-Path $RootfsBuild "bin"
    New-Directory $RootfsBuild
    New-Directory $binRoot

    Copy-Item (Join-Path $ProjectRoot "rootfs/README") (Join-Path $RootfsBuild "README") -Force

    # Se resuelven una sola vez para todo el loop: Resolve-Executable consulta
    # el disco en cada llamada y aca hay decenas de programas.
    $objcopy = Require-Executable "llvm-objcopy" (Get-LlvmToolCandidates "llvm-objcopy")
    $readelf = Require-Executable "llvm-readelf" (Get-LlvmToolCandidates "llvm-readelf")

    foreach ($program in $Programs) {
        $outputPath = Join-Path $binRoot $program.Name
        $linkArgs = @(
            "-m", "elf_x86_64",
            "-T", (Join-Path $PosixSdkRoot "linker.ld"),
            "-z", "max-page-size=0x1000",
            "--build-id=none",
            "-o", $outputPath
        ) + $program.ObjectFiles

        & $Linker @linkArgs
        if ($LASTEXITCODE -ne 0) {
            throw "Fallo el link de userland para $($program.Name)."
        }

        Add-SxeResources -Name $program.Name -BinaryPath $outputPath -ResourceDir $SxeResourceRoot -Objcopy $objcopy -Readelf $readelf
    }

    New-Initramfs -SourceRoot $RootfsBuild -OutputPath $InitramfsPath
}

function Install-BusyBox {
    New-Directory $RootfsBuild
    New-Directory (Join-Path $RootfsBuild "bin")

    $busyboxOutput = Join-Path $RootfsBuild "bin/busybox"
    $builtBusyBox = Build-ExternalUserProgram -SourcePath $BusyBoxPortRoot -ProgramName "busybox" -OutputPath $busyboxOutput
    foreach ($applet in $BusyBoxApplets) {
        if ($applet -eq "busybox") {
            continue
        }
        Copy-Item $builtBusyBox (Join-Path $RootfsBuild "bin/$applet") -Force
    }
}

function ConvertTo-CygwinPath([string]$WindowsPath) {
    $full = [System.IO.Path]::GetFullPath($WindowsPath)
    if ($full -notmatch '^([A-Za-z]):[\\/](.*)$') {
        throw "No se pudo convertir '$WindowsPath' a una ruta de Cygwin."
    }
    $drive = $Matches[1].ToLowerInvariant()
    $rest = $Matches[2] -replace '\\', '/'
    return "/cygdrive/$drive/$rest"
}

# $IsWindows no existe en Windows PowerShell 5.1 (solo en pwsh 6+): ahi,
# como esa build solo corre en Windows, asumimos $true directamente.
function Install-LimineImageFiles {
    New-Directory (Join-Path $BootRoot "limine")
    New-Directory $EfiBootRoot

    Copy-Item (Join-Path $LimineRoot "BOOTX64.EFI") (Join-Path $EfiBootRoot "BOOTX64.EFI") -Force

    foreach ($file in @("limine-bios-cd.bin", "limine-bios.sys", "limine-uefi-cd.bin")) {
        $source = Join-Path $LimineRoot $file
        if (-not (Test-Path $source)) {
            throw "No se encontro $file en tools/limine. Vuelve a ejecutar el build para regenerar Limine."
        }
        Copy-Item $source (Join-Path $BootRoot "limine/$file") -Force
    }
}

function Build-Kernel([string]$AutomationCommand = "", [bool]$IncludeTestApps = (-not $NoTestApps)) {
    $clang = Require-Executable "clang++" (Get-ToolchainCandidates "clang++")
    $ld = Require-Executable "ld.lld" (Get-ToolchainCandidates "ld.lld")
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
    $userFlags = Get-UserFlags -IncludeTestApps $IncludeTestApps
    $uacpiFlags = Get-UacpiFlags

    Generate-CursorAsset
    Generate-DesktopIconAssets
    Generate-SxeResources

    $kernelPlan = Get-KernelCompileEdges $KernelSources
    $uacpiPlan = Get-UacpiCompileEdges
    $userlandPlan = Get-UserlandCompileEdges -IncludeTestApps $IncludeTestApps
    $objectFiles = $kernelPlan.ObjectFiles + $uacpiPlan.ObjectFiles

    Invoke-NinjaCompile -BuildRoot $BuildRoot -Clangxx $clang -KernelFlags $commonFlags -UserFlags $userFlags -UacpiFlags $uacpiFlags -Edges ($kernelPlan.Edges + $uacpiPlan.Edges + $userlandPlan.Edges)

    Build-Userland -Linker $ld -Programs $userlandPlan.Programs
    Install-BusyBox
    $automationSpec = Join-Path $RootfsBuild "SMOKE"
    if ($AutomationCommand) {
        Set-Content -Path $automationSpec -Value $AutomationCommand -NoNewline
    }
    elseif (Test-Path $automationSpec) {
        # Sin spec el arranque normal es el escritorio. El SMOKE de una corrida
        # anterior sobrevive en el rootfs y se volveria a hornear en el
        # initramfs, con lo cual init arrancaria ese runner en vez del
        # escritorio -- en silencio y hasta el proximo clean.
        Remove-Item -Path $automationSpec -Force
    }
    New-Initramfs -SourceRoot $RootfsBuild -OutputPath $InitramfsPath
    Copy-Item $DiskRoot $DiskBuildRoot -Recurse -Force
    New-Directory (Join-Path $DiskBuildRoot "bin")
    Copy-Item (Join-Path $RootfsBuild "bin/*") (Join-Path $DiskBuildRoot "bin") -Force
    Build-SvfsDiskImage -SourceRoot $DiskBuildRoot -OutputPath $DiskImage

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

    Copy-Item (Join-Path $ProjectRoot "boot/limine.conf") (Join-Path $ImageRoot "limine.conf") -Force
    Copy-Item $KernelElf (Join-Path $BootRoot "kernel.elf") -Force
    Copy-Item $InitramfsPath (Join-Path $BootRoot "initramfs.cpio") -Force
    # LiveCD: la imagen de disco viaja dentro de la ISO como segundo modulo de
    # Limine (ver boot/limine.conf). El kernel la monta como ramdisk read-only
    # cuando no hay disco IDE, haciendo la ISO autocontenida.
    # (Se usa $script:DiskImage porque mas arriba una asignacion local shadowea
    # $DiskImage con el objeto de imagen -- PowerShell es case-insensitive.)
    Copy-Item $script:DiskImage (Join-Path $BootRoot "disk.img") -Force
    Install-LimineImageFiles
    Copy-Item (Join-Path $ProjectRoot "boot/limine.conf") (Join-Path $BootRoot "limine/limine.conf") -Force
    Set-Content -Path (Join-Path $ImageRoot "startup.nsh") -Value "fs0:\EFI\BOOT\BOOTX64.EFI" -NoNewline
}

function Build-Iso {
    $xorriso = Require-Executable "xorriso" (Get-ToolchainCandidates "xorriso")
    Build-Kernel

    if (Test-Path $IsoImage) {
        Remove-Item $IsoImage -Force
    }

    if (Test-IsWindowsHost) {
        # El xorriso horneado en Windows es un build de Cygwin: no traduce
        # rutas Windows (ni "C:\..." ni "C:/...") como absolutas, las trata
        # como relativas al cwd. Hay que pasarle la forma /cygdrive/<unidad>/...
        # que reconoce.
        $isoImageArg = ConvertTo-CygwinPath $IsoImage
        $imageRootArg = ConvertTo-CygwinPath $ImageRoot
    } else {
        # xorriso nativo (Linux/macOS, de paquete del sistema): rutas
        # absolutas normales, sin traduccion cygdrive.
        $isoImageArg = $IsoImage
        $imageRootArg = $ImageRoot
    }

    # -eltorito-alt-boot es obligatorio entre el "-b" (BIOS) y el "-e" (EFI):
    # sin el, la segunda entrada de boot reemplaza a la primera en el catalogo
    # El Torito en vez de coexistir, y el firmware BIOS se queda sin entrada
    # que arrancar ("could not read from CDROM").
    $xorrisoArgs = @(
        "-as", "mkisofs",
        "-b", "boot/limine/limine-bios-cd.bin",
        "-no-emul-boot",
        "-boot-load-size", "4",
        "-boot-info-table",
        "-eltorito-alt-boot",
        "-e", "boot/limine/limine-uefi-cd.bin",
        "-no-emul-boot",
        "-efi-boot-part",
        "--efi-boot-image",
        "--protective-msdos-label",
        "-iso-level", "3",
        "-V", "SAVANXP",
        "-o", $isoImageArg,
        $imageRootArg
    )

    & $xorriso @xorrisoArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo la generacion de la ISO con xorriso."
    }

    $limineInstaller = Join-Path $LimineRoot "limine.exe"
    if (-not (Test-Path $limineInstaller)) {
        $limineInstaller = Join-Path $LimineRoot "limine"
    }
    if (-not (Test-Path $limineInstaller)) {
        if (Test-IsWindowsHost) {
            throw "No se encontro el instalador de Limine para completar el arranque BIOS de la ISO."
        }

        # La rama v10.x-binary solo trae "limine.exe" prebuildeado (Windows).
        # Fuera de Windows el deployer "limine" hay que compilarlo a mano
        # desde limine.c: el Makefile del repo lo arma con `cc -std=c99
        # limine.c -o limine` (target "all", por defecto).
        Write-Host "Compilando el deployer 'limine' (make)..."
        $make = Require-Executable "make" @("make")
        & $make -C $LimineRoot
        if ($LASTEXITCODE -ne 0) {
            throw "Fallo 'make' al compilar el deployer 'limine' en $LimineRoot."
        }

        $limineInstaller = Join-Path $LimineRoot "limine"
        if (-not (Test-Path $limineInstaller)) {
            throw "make no genero el binario 'limine' en $LimineRoot."
        }
    }

    & $limineInstaller bios-install $IsoImage
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo limine bios-install sobre $IsoImage."
    }

    Write-Host "ISO generada: $IsoImage"
}

function Get-AccelCpu([string]$AccelName) {
    if ($AccelName -eq "whpx") {
        return @{ Accel = "whpx"; Cpu = "qemu64" }
    }
    return @{ Accel = "tcg"; Cpu = "max" }
}

# Dispositivos de video y de entrada de la maquina QEMU. Sin -Virtio se arma el
# hardware base: VGA estandar (Limine entrega un framebuffer lineal y el kernel
# elige fb_gpu) y mouse PS/2 via i8042, igual que en VirtualBox. edid=on hace que
# OVMF publique 1280x800 como modo preferido; sin EDID el GOP de QemuVideoDxe
# solo ofrece los modos VESA clasicos y Limine cae a 1024x768.
function Get-QemuVideoInputDevices {
    if ($Virtio) {
        return @(
            "-device", "virtio-vga,xres=1280,yres=800",
            "-device", "virtio-tablet-pci"
        )
    }
    return @("-device", "VGA,edid=on,xres=1280,yres=800")
}

# Dispositivo de sonido. "auto" sigue a -Virtio (virtio-sound con virtio, AC'97
# sin el); "virtio"/"ac97" lo fuerzan para los harnesses que miden un driver
# concreto (virtio-count, ac97-count, ...).
function Get-QemuAudioDevice([string]$Audio) {
    if ($Audio -eq "virtio" -or ($Audio -eq "auto" -and $Virtio)) {
        return @("-device", "virtio-sound-pci,audiodev=audio1,streams=1")
    }
    return @("-device", "AC97,audiodev=audio1")
}

function Run-Qemu([switch]$WaitForDebugger) {
    $qemu = Require-Executable "qemu-system-x86_64" (Get-ToolchainCandidates "qemu-system-x86_64")
    Build-Kernel

    $ovmf = Resolve-OvmfPair
    Copy-Item $ovmf.Vars $VarsTemplate -Force
    if (Test-Path $DebugConLog) {
        Remove-Item $DebugConLog -Force
    }

    $accelCpu = Get-AccelCpu $Accel
    $args = @(
        "-machine", "q35,pcspk-audiodev=audio0",
        "-accel", $accelCpu.Accel,
        "-m", "256M",
        "-cpu", $accelCpu.Cpu,
        "-audiodev", "sdl,id=audio0",
        "-audiodev", "sdl,id=audio1",
        "-display", "gtk,grab-on-hover=on,show-cursor=off,window-close=on,zoom-to-fit=off",
        "-rtc", "base=localtime",
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
        "-global", "isa-debugcon.iobase=0xe9"
    )
    $args += Get-QemuVideoInputDevices
    $args += Get-QemuAudioDevice "auto"

    if ($WaitForDebugger) {
        $args += @("-s", "-S")
    }

    & $qemu @args
}

function Stop-AutomationQemu($Process, [int]$MonitorPort) {
    if ($null -eq $Process -or $Process.HasExited) {
        return
    }
    # Apagado ordenado: pedirle a QEMU que cierre por el monitor para que vacie
    # sus backends de bloque y cierre el archivo de disco limpiamente, en vez de
    # un TerminateProcess que puede dejar metadata SVFS2 a medio escribir (journal
    # en vuelo). Si el monitor no responde, caemos al kill forzado de siempre.
    if ($MonitorPort -gt 0) {
        try {
            $client = [System.Net.Sockets.TcpClient]::new()
            $connect = $client.BeginConnect("127.0.0.1", $MonitorPort, $null, $null)
            if ($connect.AsyncWaitHandle.WaitOne(2000) -and $client.Connected) {
                $client.EndConnect($connect)
                $stream = $client.GetStream()
                $payload = [System.Text.Encoding]::ASCII.GetBytes("quit`n")
                $stream.Write($payload, 0, $payload.Length)
                $stream.Flush()
            }
            $client.Close()
        } catch {
            # Best-effort: ignoramos y dejamos que actue el fallback.
        }
        if ($Process.WaitForExit(3000)) {
            return
        }
    }
    Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
}

function Run-AutomationQemu([string]$AutomationCommand, [string]$SuccessToken, [string]$FailureToken, [int]$TimeoutMinutes = 2, [ValidateSet("auto", "ac97", "virtio")][string]$Audio = "auto", [string]$WavPath, [scriptblock]$PreLaunch) {
    $qemu = Require-Executable "qemu-system-x86_64" (Get-ToolchainCandidates "qemu-system-x86_64")
    if ($NoTestApps) {
        Write-Host "Nota: los harnesses de automatizacion requieren las apps de testeo; se ignora -NoTestApps."
    }
    Build-Kernel -AutomationCommand $AutomationCommand -IncludeTestApps $true

    # Gancho post-build: Build-Kernel regenera build/disk.img desde el arbol
    # fuente disk/, asi que cualquier binario que viva solo como artefacto (p.
    # ej. las apps nativas instaladas con -Install) debe reinstalarse aca, entre
    # el rebuild y el lanzamiento de QEMU.
    if ($PreLaunch) {
        & $PreLaunch
    }

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

    # Puerto libre para el monitor HMP, usado por Stop-AutomationQemu para un
    # apagado ordenado (vacia el disco y cierra el archivo antes de salir).
    $monitorListener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $monitorListener.Start()
    $monitorPort = ([System.Net.IPEndPoint]$monitorListener.LocalEndpoint).Port
    $monitorListener.Stop()

    $args = @(
        "-machine", "q35,pcspk-audiodev=audio0",
        "-accel", "tcg",
        "-m", "256M",
        "-cpu", "max",
        "-audiodev", "none,id=audio0",
        "-display", "none",
        "-rtc", "base=localtime",
        "-drive", "if=pflash,format=raw,readonly=on,file=""$($ovmf.Code)""",
        "-drive", "if=pflash,format=raw,file=""$VarsTemplate""",
        "-drive", "file=fat:rw:build/image,format=raw",
        "-netdev", "user,id=net0",
        "-device", "rtl8139,netdev=net0",
        "-device", "isa-ide,id=svide",
        "-drive", "if=none,id=svdisk,media=disk,format=raw,file=""$DiskImage""",
        "-device", "ide-hd,drive=svdisk,bus=svide.0",
        "-serial", "file:$SmokeSerialLog",
        "-debugcon", "file:$DebugConLog",
        "-global", "isa-debugcon.iobase=0xe9",
        "-monitor", "tcp:127.0.0.1:$monitorPort,server,nowait",
        "-no-reboot",
        "-no-shutdown"
    )
    $args += Get-QemuVideoInputDevices

    # audiodev de audio1 (el que consume el dispositivo de sonido): por defecto
    # "none" (headless, sin captura). Con -WavPath se graba lo que reproduce el
    # device a un WAV, para medir underruns/glitches sin depender del oido.
    if ($WavPath) {
        $args += @("-audiodev", "wav,id=audio1,path=$WavPath")
    } else {
        $args += @("-audiodev", "none,id=audio1")
    }

    # Dispositivo de audio: sigue a -Virtio salvo que el harness pida uno
    # concreto (-Audio ac97 / -Audio virtio). Sin virtio queda el AC'97 de QEMU
    # (el mismo chip que emula VirtualBox) y el kernel cae al backend ac97.
    $args += Get-QemuAudioDevice $Audio

    $process = Start-Process -FilePath $qemu -ArgumentList $args -PassThru -RedirectStandardOutput $SmokeStdoutLog -RedirectStandardError $SmokeStderrLog
    try {
        $deadline = (Get-Date).AddMinutes($TimeoutMinutes)
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 1000
            if (-not (Test-Path $SmokeSerialLog)) {
                if ($process.HasExited) {
                    break
                }
                continue
            }

            $content = Get-Content $SmokeSerialLog -Raw
            if ($content -match [regex]::Escape($SuccessToken)) {
                Start-Sleep -Milliseconds 2000
                Stop-AutomationQemu $process $monitorPort
                Write-Host $SuccessToken
                return
            }
            if ($content -match [regex]::Escape($FailureToken)) {
                Stop-AutomationQemu $process $monitorPort
                throw "$FailureToken. Revisar $SmokeSerialLog"
            }
            if ($process.HasExited) {
                break
            }
        }
    } finally {
        if (-not $process.HasExited) {
            Stop-AutomationQemu $process $monitorPort
        }
    }

    if ($process.HasExited) {
        throw "$FailureToken aborted. Revisar $SmokeSerialLog y $SmokeStderrLog"
    }
    throw "$FailureToken timeout. Revisar $SmokeSerialLog y $SmokeStderrLog"
}

function Run-SmokeQemu {
    # Sigue al hardware de la maquina: sin -Virtio ya valida /dev/audio0 sobre el
    # AC'97 (el viejo target ac97-smoke, retirado por redundante) y /dev/gpu0
    # sobre fb_gpu; con -Virtio, sobre virtio-sound y virtio-gpu.
    Run-AutomationQemu -AutomationCommand "smoke" -SuccessToken "SMOKE PASS" -FailureToken "SMOKE FAIL" -TimeoutMinutes 2
}

# Harness headless del protocolo cliente del compositor (subsystems/native):
# construye e instala la app ventaneada nativa (nativegui) y el host que
# interpreta el rol del compositor (nativeguihost), luego arranca init con el
# spec "guihost". Valida secuencias/rects/pixeles, el input de teclado y el
# canal de mouse (fd 5) de punta a punta. Ver subsystems/native/test/guihost.c.
function Run-NativeGuihostQemu {
    $nativeBuild = Join-Path $ProjectRoot "subsystems/native/build.ps1"
    $userBuild = Join-Path $ToolRoot "build-user.ps1"
    $guihostSource = Join-Path $ProjectRoot "subsystems/native/test/guihost.c"

    Run-AutomationQemu -AutomationCommand "guihost" -SuccessToken "NATIVEGUI HOST PASS" -FailureToken "NATIVEGUI HOST FAIL" -TimeoutMinutes 3 -PreLaunch {
        & $nativeBuild -Name nativegui -Source haxe-gui -Install
        if ($LASTEXITCODE -ne 0) { throw "Fallo el build/install de nativegui." }
        & $userBuild -Source $guihostSource -Name nativeguihost
        if ($LASTEXITCODE -ne 0) { throw "Fallo el build/install de nativeguihost." }
    }.GetNewClosure()
}

# Programa de validacion del runtime nativo (subsystems/native/haxe/Main.hx):
# clases heap/@:valueType, String/Array, Null<T> (std::optional) y demo gfx.
# Corre headless via el spec "nativehello" y confirma "NATIVE HELLO PASS". Mismo
# gancho -PreLaunch que native-guihost porque Build-Kernel regenera disk.img.
function Run-NativeHelloQemu {
    $nativeBuild = Join-Path $ProjectRoot "subsystems/native/build.ps1"

    Run-AutomationQemu -AutomationCommand "nativehello" -SuccessToken "NATIVE HELLO PASS" -FailureToken "NATIVE HELLO FAIL" -TimeoutMinutes 3 -PreLaunch {
        & $nativeBuild -Name nativehello -Source haxe -Install
        if ($LASTEXITCODE -ne 0) { throw "Fallo el build/install de nativehello." }
    }.GetNewClosure()
}

# Primera app sxgui-style nativa (Fase 3): construye e instala sxguiapp
# (haxe-sxgui) + el harness sxguihost, y los corre headless. Valida el render
# del toolkit (fondo FACE + bisel levantado + texto Noto) bajo el compositor.
function Run-NativeSxguiQemu {
    $nativeBuild = Join-Path $ProjectRoot "subsystems/native/build.ps1"
    $userBuild = Join-Path $ToolRoot "build-user.ps1"
    $sxguihostSource = Join-Path $ProjectRoot "subsystems/native/test/sxguihost.c"

    Run-AutomationQemu -AutomationCommand "sxguihost" -SuccessToken "SXGUI HOST PASS" -FailureToken "SXGUI HOST FAIL" -TimeoutMinutes 3 -PreLaunch {
        & $nativeBuild -Name sxguiapp -Source haxe-sxgui -Install
        if ($LASTEXITCODE -ne 0) { throw "Fallo el build/install de sxguiapp." }
        & $userBuild -Source $sxguihostSource -Name sxguihost
        if ($LASTEXITCODE -ne 0) { throw "Fallo el build/install de sxguihost." }
    }.GetNewClosure()
}

function Run-Ac97StreamQemu {
    # Corre audiotest --stream (patron de alimentacion tipo-Doom) sobre AC'97 y
    # graba la salida a un WAV para medir underruns/glitches. Analizar el WAV con
    # tools/audio/wavgaps.py. OJO: bajo TCG el backend wav marca el ritmo a
    # tiempo-real del host y el guest no lo alcanza -> la captura no es fiel para
    # audio continuo. Para medir el driver usar ac97-count (audiodev none).
    $wav = Join-Path $BuildRoot "ac97-stream.wav"
    if (Test-Path $wav) { Remove-Item $wav -Force }
    Run-AutomationQemu -AutomationCommand "audiostream" -SuccessToken "AUDIO STREAM PASS" -FailureToken "AUDIO STREAM FAIL" -TimeoutMinutes 2 -Audio ac97 -WavPath $wav
    Write-Host "WAV capturado: $wav"
}

function Run-VirtioCountQemu {
    # audiotest --stream sobre virtio-sound con audiodev none, para medir el
    # contador de underruns del camino TX multi-buffer. Fuerza virtio-sound
    # aunque la maquina corra sin virtio: es el driver que este harness mide.
    Run-AutomationQemu -AutomationCommand "audiostream" -SuccessToken "AUDIO STREAM PASS" -FailureToken "AUDIO STREAM FAIL" -TimeoutMinutes 2 -Audio virtio
    $line = Select-String -Path $SmokeSerialLog -Pattern "virtio-sound: stop con" | Select-Object -Last 1
    if ($line) { Write-Host ("DESCARTES -> " + $line.Line.Trim()) } else { Write-Host "DESCARTES -> 0 (sin linea de descartes)" }
}

function Run-VirtioStreamQemu {
    # audiotest --stream sobre virtio-sound grabando a WAV para analizar gaps.
    $wav = Join-Path $BuildRoot "virtio-stream.wav"
    if (Test-Path $wav) { Remove-Item $wav -Force }
    Run-AutomationQemu -AutomationCommand "audiostream" -SuccessToken "AUDIO STREAM PASS" -FailureToken "AUDIO STREAM FAIL" -TimeoutMinutes 2 -Audio virtio -WavPath $wav
    Write-Host "WAV capturado: $wav"
}

function Run-Ac97CountQemu {
    # Corre audiotest --stream sobre AC'97 con audiodev none (sin pacing de wav):
    # QEMU avanza el CIV en tiempo virtual igual que el guest alimenta, asi el
    # contador de underruns del driver (ac97: stop underruns=N en el serial)
    # refleja si el colchon evita que el ring se vacie.
    Run-AutomationQemu -AutomationCommand "audiostream" -SuccessToken "AUDIO STREAM PASS" -FailureToken "AUDIO STREAM FAIL" -TimeoutMinutes 2 -Audio ac97
    $line = Select-String -Path $SmokeSerialLog -Pattern "ac97: stop underruns" | Select-Object -Last 1
    if ($line) { Write-Host ("UNDERRUNS -> " + $line.Line.Trim()) } else { Write-Host "no se encontro linea de underruns en $SmokeSerialLog" }
}

function Run-WindowdSmokeQemu {
    Run-AutomationQemu -AutomationCommand "windowd-selftest" -SuccessToken "WINDOWD SMOKE PASS" -FailureToken "WINDOWD SMOKE FAIL" -TimeoutMinutes 3
}

function Run-ProgmanSmokeQemu {
    Run-AutomationQemu -AutomationCommand "progman-selftest" -SuccessToken "PROGMAN SMOKE PASS" -FailureToken "PROGMAN SMOKE FAIL" -TimeoutMinutes 3
}

# Lector de recursos SXE (docs/SXE_FORMAT.md, fase 1). El grueso del selftest
# es parseo puro en memoria -- blobs bien formados y todos los degradados que
# ningun generador correcto produciria --, mas el camino de disco contra los
# binarios reales de la imagen, que hoy no traen recursos y deben resolverse
# limpio como "sin metadata".
function Run-SxeSmokeQemu {
    Run-AutomationQemu -AutomationCommand "sxe-selftest" -SuccessToken "SXE SMOKE PASS" -FailureToken "SXE SMOKE FAIL" -TimeoutMinutes 3
}

# Asociaciones de archivo (docs/SXE_FORMAT.md, fase 5). Ademas de validar el
# parseo y la precedencia con fixtures, reporta cuantos ejecutables abrio el
# escaneo real: es la magnitud a mirar antes de decidir si hace falta una cache.
function Run-FilesappSmokeQemu {
    Run-AutomationQemu -AutomationCommand "filesapp-selftest" -SuccessToken "FILESAPP SMOKE PASS" -FailureToken "FILESAPP SMOKE FAIL" -TimeoutMinutes 3
}

# Harness headless del camino de red (subsystems/posix/userland/nettest.c):
# valida el driver del NIC de punta a punta -- presencia por PCI, MAC propia,
# ARP, tx y rx -- haciendo ICMP echo contra el gateway del user-net de QEMU
# (10.0.2.2), el unico destino que slirp contesta de forma deterministica. Los
# contadores tx_frames/rx_frames del driver son la prueba de que el hardware
# movio trafico y no contesto el stack solo.
function Run-NetSmokeQemu {
    Run-AutomationQemu -AutomationCommand "netsmoke" -SuccessToken "NET SMOKE PASS" -FailureToken "NET SMOKE FAIL" -TimeoutMinutes 3
}

function Run-CursorReproQemu {
    Run-AutomationQemu -AutomationCommand "windowd-cursor-repro" -SuccessToken "CURSOR REPRO PASS" -FailureToken "CURSOR REPRO FAIL" -TimeoutMinutes 3
}

function Run-GpuSoakQemu([int]$Iterations) {
    $timeoutMinutes = [Math]::Max(6, [int][Math]::Ceiling($Iterations / 16.0))
    Run-AutomationQemu -AutomationCommand "gputest --soak $Iterations" -SuccessToken "SOAK PASS" -FailureToken "SOAK FAIL" -TimeoutMinutes $timeoutMinutes
}

switch ($Command) {
    "build" {
        Build-Kernel
    }
    "iso" {
        Build-Iso
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
    "ac97-stream" {
        Run-Ac97StreamQemu
    }
    "ac97-count" {
        Run-Ac97CountQemu
    }
    "virtio-count" {
        Run-VirtioCountQemu
    }
    "virtio-stream" {
        Run-VirtioStreamQemu
    }
    "windowd-smoke" {
        Run-WindowdSmokeQemu
    }
    "progman-smoke" {
        Run-ProgmanSmokeQemu
    }
    "sxe-smoke" {
        Run-SxeSmokeQemu
    }
    "filesapp-smoke" {
        Run-FilesappSmokeQemu
    }
    "net-smoke" {
        Run-NetSmokeQemu
    }
    "cursor-repro" {
        Run-CursorReproQemu
    }
    "gpu-soak" {
        Run-GpuSoakQemu -Iterations $GpuSoakIterations
    }
    "native-guihost" {
        Run-NativeGuihostQemu
    }
    "native-hello" {
        Run-NativeHelloQemu
    }
    "native-sxgui" {
        Run-NativeSxguiQemu
    }
    "clean" {
        if (Test-Path $BuildRoot) {
            Remove-Item -Recurse -Force $BuildRoot
        }
    }
}
