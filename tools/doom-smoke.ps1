param(
    [switch]$BuildFirst,
    [switch]$SkipBuild
)

# Smoke visual opcional del port de doomgeneric.
# En el flujo desktop-first actual no reemplaza a build.ps1 smoke ni a gputest
# como validacion principal del stack grafico.

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$BuildRoot = Join-Path $ProjectRoot "build"
$ImageRoot = Join-Path $BuildRoot "image"
$DiskImage = Join-Path $BuildRoot "disk.img"
$VarsTemplate = Join-Path $BuildRoot "OVMF_VARS_doom_smoke.fd"
$SerialLog = Join-Path $BuildRoot "doom-smoke-serial.log"
$DebugLog = Join-Path $BuildRoot "doom-smoke-debugcon.log"
$MenuShot = Join-Path $BuildRoot "doom-smoke-menu.ppm"
$LevelShot = Join-Path $BuildRoot "doom-smoke-level.ppm"
$FireShot = Join-Path $BuildRoot "doom-smoke-fire.ppm"
$QmpPort = 4444

function Resolve-ExistingPath([string[]]$Candidates) {
    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return $candidate
        }
    }

    return $null
}

if (-not $SkipBuild) {
    & powershell -ExecutionPolicy Bypass -File (Join-Path $ProjectRoot "build.ps1") build
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo build.ps1 build."
    }
}

$qemu = Resolve-ExistingPath @(
    "C:\Program Files\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64"
)
$ovmfCode = Resolve-ExistingPath @(
    "C:\Program Files\qemu\share\edk2-x86_64-code.fd",
    "C:\Program Files\qemu\share\OVMF_CODE.fd",
    "C:\msys64\mingw64\share\edk2-x86_64-code.fd",
    "C:\msys64\usr\share\edk2-ovmf\x64\OVMF_CODE.fd"
)
$ovmfVars = Resolve-ExistingPath @(
    "C:\Program Files\qemu\share\edk2-x86_64-vars.fd",
    "C:\Program Files\qemu\share\edk2-i386-vars.fd",
    "C:\Program Files\qemu\share\OVMF_VARS.fd",
    "C:\msys64\mingw64\share\edk2-x86_64-vars.fd",
    "C:\msys64\usr\share\edk2-ovmf\x64\OVMF_VARS.fd"
)

if (-not $qemu) {
    throw "No se encontro qemu-system-x86_64."
}
if (-not $ovmfCode -or -not $ovmfVars) {
    throw "No se encontro un par OVMF valido."
}
if (-not (Test-Path $ImageRoot) -or -not (Test-Path $DiskImage)) {
    throw "Faltan build/image o build/disk.img. Corre .\\build.ps1 build primero."
}

Copy-Item $ovmfVars $VarsTemplate -Force

foreach ($path in @($SerialLog, $DebugLog, $MenuShot, $LevelShot, $FireShot)) {
    if (Test-Path $path) {
        Remove-Item $path -Force
    }
}

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force

$driver = Start-Job -ArgumentList $MenuShot, $LevelShot, $FireShot, $QmpPort -ScriptBlock {
    param($MenuPath, $LevelPath, $FirePath, $Port)

    Start-Sleep -Seconds 14

    $client = [System.Net.Sockets.TcpClient]::new("127.0.0.1", $Port)
    $stream = $client.GetStream()
    $reader = [System.IO.StreamReader]::new($stream)
    $writer = [System.IO.StreamWriter]::new($stream)
    $writer.AutoFlush = $true

    function Invoke-DriverQmp($Json) {
        $script:writer.WriteLine($Json)
        return $script:reader.ReadLine()
    }

    function Invoke-DriverHmp($CommandLine) {
        $escaped = $CommandLine.Replace('\', '\\').Replace('"', '\"')
        return Invoke-DriverQmp ('{"execute":"human-monitor-command","arguments":{"command-line":"' + $escaped + '"}}')
    }

    function Send-DriverKey($Key, $DelayMs) {
        Invoke-DriverHmp ("sendkey " + $Key) | Out-Null
        Start-Sleep -Milliseconds $DelayMs
    }

    function Capture-DriverFrame($Path) {
        Invoke-DriverHmp ("screendump " + $Path) | Out-Null
        return (Get-FileHash $Path -Algorithm SHA256).Hash
    }

    function Wait-DriverFrameChange($ReferenceHash, $Path, $Attempts, $DelayMs) {
        for ($attempt = 0; $attempt -lt $Attempts; ++$attempt) {
            Start-Sleep -Milliseconds $DelayMs
            $hash = Capture-DriverFrame $Path
            if ($hash -ne $ReferenceHash) {
                return $hash
            }
        }

        return $ReferenceHash
    }

    $null = $reader.ReadLine()
    $writer.WriteLine('{"execute":"qmp_capabilities"}')
    $null = $reader.ReadLine()

    foreach ($key in @("d", "o", "o", "m", "g", "e", "n", "e", "r", "i", "c")) {
        Send-DriverKey $key 100
    }
    Send-DriverKey "ret" 500

    Start-Sleep -Seconds 8
    $menuHash = Capture-DriverFrame $MenuPath

    Send-DriverKey "ret" 500
    Send-DriverKey "ret" 500
    Send-DriverKey "ret" 500

    $levelHash = Wait-DriverFrameChange $menuHash $LevelPath 12 1000

    Send-DriverKey "tab" 250
    $fireHash = Wait-DriverFrameChange $levelHash $FirePath 8 500

    Send-DriverKey "f10" 400
    Send-DriverKey "y" 400
    Start-Sleep -Seconds 2
    $writer.WriteLine('{"execute":"quit"}')

    $reader.Dispose()
    $writer.Dispose()
    $client.Dispose()
}

& $qemu `
    -machine q35,pcspk-audiodev=audio0 `
    -m 256M `
    -cpu max `
    -audiodev none,id=audio0 `
    -audiodev none,id=audio1 `
    -drive "if=pflash,format=raw,readonly=on,file=$ovmfCode" `
    -drive "if=pflash,format=raw,file=$VarsTemplate" `
    -drive "file=fat:rw:$ImageRoot,format=raw" `
    -netdev user,id=net0 `
    -device rtl8139,netdev=net0 `
    -device virtio-vga,xres=1280,yres=800 `
    -device virtio-sound-pci,audiodev=audio1,streams=1 `
    -device virtio-tablet-pci `
    -device isa-ide,id=svide `
    -drive "if=none,id=svdisk,media=disk,format=raw,file=$DiskImage" `
    -device ide-hd,drive=svdisk,bus=svide.0 `
    -serial "file:$SerialLog" `
    -debugcon "file:$DebugLog" `
    -global isa-debugcon.iobase=0xe9 `
    -rtc "base=localtime" `
    -no-reboot `
    -no-shutdown `
    -qmp "tcp:127.0.0.1:$QmpPort,server=on,wait=off"

$jobOutput = Receive-Job $driver -Wait -AutoRemoveJob
if ($jobOutput) {
    $jobOutput | Out-String | Write-Output
}

foreach ($path in @($MenuShot, $LevelShot, $FireShot, $SerialLog, $DebugLog)) {
    if (-not (Test-Path $path)) {
        throw "Falta artefacto esperado: $path"
    }
}

$menuHash = (Get-FileHash $MenuShot -Algorithm SHA256).Hash
$levelHash = (Get-FileHash $LevelShot -Algorithm SHA256).Hash
$fireHash = (Get-FileHash $FireShot -Algorithm SHA256).Hash
$serialText = Get-Content $SerialLog -Raw

if ($serialText -notmatch "handoff: starting /bin/init") {
    throw "La VM no llego a handoff de init."
}
if ($serialText -match "desktop: compositor startup failed") {
    throw "El desktop no pudo iniciar de forma estable."
}
if ($menuHash -eq $levelHash) {
    throw "La captura del menu y la de partida son iguales; smoke inconclusa."
}
if ($levelHash -eq $fireHash) {
    throw "La captura despues de la accion en partida no cambio; smoke inconclusa."
}

$returnedToDesktop = ($serialText -notmatch "init: desktop unstable")

[pscustomobject]@{
    SerialLog = $SerialLog
    DebugLog = $DebugLog
    MenuScreenshot = $MenuShot
    LevelScreenshot = $LevelShot
    FireScreenshot = $FireShot
    ReturnedToDesktop = $returnedToDesktop
    MenuHash = $menuHash
    LevelHash = $levelHash
    FireHash = $fireHash
} | Format-List
