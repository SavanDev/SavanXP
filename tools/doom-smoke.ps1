param(
    [switch]$BuildFirst
)

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

if ($BuildFirst) {
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

    Start-Sleep -Seconds 8

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

    $null = $reader.ReadLine()
    $writer.WriteLine('{"execute":"qmp_capabilities"}')
    $null = $reader.ReadLine()

    foreach ($key in @("d", "o", "o", "m", "g", "e", "n", "e", "r", "i", "c")) {
        Send-DriverKey $key 100
    }
    Send-DriverKey "ret" 500

    Start-Sleep -Seconds 6
    Invoke-DriverHmp ("screendump " + $MenuPath) | Out-Null

    Send-DriverKey "ret" 500
    Send-DriverKey "ret" 500
    Send-DriverKey "ret" 500

    Start-Sleep -Seconds 3
    Invoke-DriverHmp ("screendump " + $LevelPath) | Out-Null

    Send-DriverKey "ctrl" 250
    Start-Sleep -Milliseconds 300
    Invoke-DriverHmp ("screendump " + $FirePath) | Out-Null

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
    -drive "if=pflash,format=raw,readonly=on,file=$ovmfCode" `
    -drive "if=pflash,format=raw,file=$VarsTemplate" `
    -drive "file=fat:rw:$ImageRoot,format=raw" `
    -netdev user,id=net0 `
    -device rtl8139,netdev=net0 `
    -device isa-ide,id=svide `
    -drive "if=none,id=svdisk,media=disk,format=raw,file=$DiskImage" `
    -device ide-hd,drive=svdisk,bus=svide.0 `
    -serial "file:$SerialLog" `
    -debugcon "file:$DebugLog" `
    -global isa-debugcon.iobase=0xe9 `
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

if ($serialText -notmatch "savanxp:/\$") {
    throw "La VM no llego al shell actual."
}
if ($menuHash -eq $levelHash) {
    throw "La captura del menu y la de partida son iguales; smoke inconclusa."
}
if ($levelHash -eq $fireHash) {
    throw "La captura despues de disparar no cambio; smoke inconclusa."
}

$returnedToShell = ([regex]::Matches($serialText, "savanxp:/\$")).Count -ge 2

[pscustomobject]@{
    SerialLog = $SerialLog
    DebugLog = $DebugLog
    MenuScreenshot = $MenuShot
    LevelScreenshot = $LevelShot
    FireScreenshot = $FireShot
    ReturnedToShell = $returnedToShell
    MenuHash = $menuHash
    LevelHash = $levelHash
    FireHash = $fireHash
} | Format-List
