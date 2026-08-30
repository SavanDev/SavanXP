# Verificacion VISUAL de la sesion grafica, headless.
#
# Los harnesses (smoke, windowd-smoke, ...) asertan estado y geometria, no
# apariencia: pasan en verde con la pantalla mal. Este script arranca el sistema
# sin ventana, le manda teclas, y saca capturas PNG para mirar con los ojos.
#
# Existe porque ya se perdio una vez: hubo capturas hechas a mano en build/ sin
# ningun script que las hubiera producido. Es tooling de diagnostico, no parte
# del build -- build.ps1 no lo invoca.
#
# Uso:
#   .\tools\shoot.ps1                       # escenario 'desktop'
#   .\tools\shoot.ps1 -Scenario clipboard
#   .\tools\shoot.ps1 -Scenario alttab -OutDir build\shots
#
# Las teclas van por QMP (input-send-event) y no por el sendkey del monitor HMP,
# porque QMP permite SOSTENER un modificador: sin eso no se puede capturar un
# Alt+Tab con el switcher abierto, ni un Ctrl+C, que necesitan que la
# modificadora siga apretada mientras llega la letra.

[CmdletBinding()]
param(
    [ValidateSet("desktop", "alttab", "clipboard", "taskbar")]
    [string]$Scenario = "desktop",

    [string]$OutDir,

    # Segundos de espera despues del handoff antes del primer paso. La sesion
    # (windowd + shellui + progman) tarda en estar pintada, y bajo TCG el tiempo
    # varia bastante entre corridas.
    [int]$BootWait = 45,

    [switch]$KeepRunning
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "UserAppCommon.ps1")

if (-not $OutDir) {
    $OutDir = Join-Path $ProjectRoot "build/shots"
}
if (-not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
}

# El guest arranca al escritorio SOLO si no hay un spec de automatizacion
# plantado. Cualquier harness deja el suyo, asi que despues de un smoke el
# sistema bootea ese runner y las capturas salen de la consola en vez de la
# sesion grafica. Se falla temprano y con el remedio, en vez de dejar al que
# mira una captura desconcertante.
$automationSpec = Join-Path $ProjectRoot "build/image/SMOKE"
if (Test-Path $automationSpec) {
    throw "Hay un spec de automatizacion plantado ($automationSpec): el guest arrancaria ese harness y no el escritorio. Corre '.\build.ps1 build' primero."
}

$image = Join-Path $ProjectRoot "build/image"
if (-not (Test-Path $image)) {
    throw "No existe build/image. Corre '.\build.ps1 build' primero."
}

$qemu = Require-Executable "qemu-system-x86_64" (Get-ToolchainCandidates "qemu-system-x86_64")
$python = Get-PythonExecutable
$ovmf = Resolve-OvmfPair

$serialLog = Join-Path $OutDir "shoot-serial.log"
$varsCopy = Join-Path $OutDir "shoot-vars.fd"
foreach ($stale in @($serialLog, $varsCopy)) {
    if (Test-Path $stale) {
        Remove-Item $stale -Force
    }
}
Copy-Item $ovmf.Vars $varsCopy

# Puerto libre para QMP.
$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
$listener.Start()
$qmpPort = ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
$listener.Stop()

# Hardware "base" (VGA estandar + PS/2 + AC'97), el mismo que arma build.ps1 por
# defecto. Se repite aca a proposito: build.ps1 es un script monolitico sin
# funciones exportables, y refactorizarlo para compartir esta lista seria un
# cambio mucho mas grande que este harness.
$qemuArgs = @(
    "-machine", "q35,pcspk-audiodev=audio0",
    "-accel", "tcg",
    "-m", "256M",
    "-cpu", "max",
    "-audiodev", "none,id=audio0",
    "-audiodev", "none,id=audio1",
    "-display", "none",
    "-rtc", "base=localtime",
    "-drive", "if=pflash,format=raw,readonly=on,file=""$($ovmf.Code)""",
    "-drive", "if=pflash,format=raw,file=""$varsCopy""",
    "-drive", "file=fat:rw:build/image,format=raw",
    "-netdev", "user,id=net0",
    "-device", "rtl8139,netdev=net0",
    "-device", "isa-ide,id=svide",
    "-drive", "if=none,id=svdisk,media=disk,format=raw,file=""$(Join-Path $ProjectRoot 'build/disk.img')""",
    "-device", "ide-hd,drive=svdisk,bus=svide.0",
    "-device", "VGA,edid=on,xres=1280,yres=800",
    "-device", "AC97,audiodev=audio1",
    "-serial", "file:$serialLog",
    "-qmp", "tcp:127.0.0.1:$qmpPort,server,nowait",
    "-no-reboot",
    "-no-shutdown"
)

Write-Host "shoot: escenario '$Scenario', salida en $OutDir"

Push-Location $ProjectRoot
try {
    $process = Start-Process -FilePath $qemu -ArgumentList $qemuArgs -PassThru `
        -RedirectStandardOutput (Join-Path $OutDir "shoot-qemu-out.log") `
        -RedirectStandardError (Join-Path $OutDir "shoot-qemu-err.log")

    try {
        & $python (Join-Path $PSScriptRoot "shoot_session.py") `
            --port $qmpPort --serial $serialLog --out $OutDir `
            --scenario $Scenario --boot-wait $BootWait
        if ($LASTEXITCODE -ne 0) {
            throw "El escenario '$Scenario' fallo. Revisar $serialLog"
        }
    }
    finally {
        if (-not $KeepRunning) {
            if (-not $process.HasExited) {
                $process.Kill()
            }
            $process.WaitForExit(10000) | Out-Null
        }
    }
}
finally {
    Pop-Location
}

Get-ChildItem -Path $OutDir -Filter "*.png" | Sort-Object Name | ForEach-Object {
    Write-Host "  $($_.Name)"
}
