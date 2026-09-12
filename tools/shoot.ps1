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
#   .\tools\shoot.ps1 -Scenario wheel       # rueda del mouse, de punta a punta
#   .\tools\shoot.ps1 -Scenario appwiz     # necesita un programa externo instalado
#   .\tools\shoot.ps1 -Scenario system     # propiedades del sistema + administrador de tareas
#   .\tools\shoot.ps1 -Scenario alttab -OutDir build\shots
#
# Las teclas van por QMP (input-send-event) y no por el sendkey del monitor HMP,
# porque QMP permite SOSTENER un modificador: sin eso no se puede capturar un
# Alt+Tab con el switcher abierto, ni un Ctrl+C, que necesitan que la
# modificadora siga apretada mientras llega la letra.

[CmdletBinding()]
param(
    [ValidateSet("desktop", "alttab", "clipboard", "files", "shell", "appwiz", "system", "taskbar", "kbdlayout", "wheel", "notepadwheel", "bench", "saturate", "spin")]
    [string]$Scenario = "desktop",

    [string]$OutDir,

    # Hardware paravirtualizado (virtio-vga + virtio-tablet + virtio-keyboard +
    # virtio-net + virtio-blk), el mismo set que "build.ps1 -Virtio". Sin esto
    # se arma el hardware base, que es lo que emula VirtualBox.
    [switch]$Virtio,

    # Acelerador. tcg es determinista y es lo que usan los harnesses; kvm/whpx
    # sirven para medir, no para asertar -- ver docs/GRAPHICS_PERF.md.
    [ValidateSet("tcg", "kvm", "whpx")]
    [string]$Accel = "tcg",

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
# El spec vive en el rootfs, que es lo que se hornea en el initramfs -- NO en
# build/image, que es la ESP FAT. Apuntaba ahi y por eso el guard nunca salto:
# despues de un "build.ps1 net-smoke" este script arrancaba el guest, corria el
# runner de red en vez del escritorio, y fallaba con un error que no tenia nada
# que ver.
$automationSpec = Join-Path $ProjectRoot "build/rootfs/SMOKE"
if (Test-Path $automationSpec) {
    throw "Hay un spec de automatizacion plantado ($automationSpec): el guest arrancaria ese harness y no el escritorio. Corre '.\build.ps1 build' primero."
}

$image = Join-Path $ProjectRoot "build/image"
if (-not (Test-Path $image)) {
    throw "No existe build/image. Corre '.\build.ps1 build' primero."
}

# El escenario de Agregar o quitar programas navega el launcher por teclado, y
# la cantidad de solapas depende de que haya instalado: con Doom hay un grupo
# Games y System es la cuarta, sin Doom es la tercera. En vez de adivinar se
# exige el estado en el que la captura ademas sirve para algo -- una lista de
# desinstalables vacia no muestra nada.
if ($Scenario -eq "appwiz" -or $Scenario -eq "system") {
    $diskImage = Join-Path $ProjectRoot "build/disk.img"
    $sxfs = Open-SxfsImage $diskImage
    if (-not (Get-SxfsPathInfo $sxfs "/disk/bin/doomgeneric")) {
        throw "El escenario '$Scenario' necesita un programa externo instalado. Corre '.\sdk\doomgeneric\build.ps1' primero."
    }
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

# Hardware "base" (VGA estandar + PS/2 + AC'97) por defecto, o el set
# paravirtualizado con -Virtio: el mismo criterio que build.ps1. Se repite aca a
# proposito: build.ps1 es un script monolitico sin funciones exportables, y
# refactorizarlo para compartir estas listas seria un cambio mucho mas grande
# que este harness.
# El cpu tiene que seguir al acelerador: con kvm "max" no sirve y hay que pasar
# "host"; bajo whpx "max"/"host" crashean OVMF y va "qemu64". Mismo criterio que
# Get-AccelCpu en build.ps1.
$cpuModel = switch ($Accel) { "kvm" { "host" } "whpx" { "qemu64" } default { "max" } }

$videoInputDevices = if ($Virtio) {
    @("-device", "virtio-vga,xres=1280,yres=800",
      "-device", "virtio-tablet-pci",
      "-device", "virtio-keyboard-pci")
} else {
    @("-device", "VGA,edid=on,xres=1280,yres=800")
}
$nicDevice = if ($Virtio) { @("-device", "virtio-net-pci,netdev=net0") }
             else { @("-device", "rtl8139,netdev=net0") }
$audioDevice = if ($Virtio) { @("-device", "virtio-sound-pci,audiodev=audio1,streams=2") }
               else { @("-device", "AC97,audiodev=audio1") }
$diskDrive = @("-drive", "if=none,id=svdisk,media=disk,format=raw,file=""$(Join-Path $ProjectRoot 'build/disk.img')""")
$diskDevices = if ($Virtio) { $diskDrive + @("-device", "virtio-blk-pci,drive=svdisk") }
               else { @("-device", "isa-ide,id=svide") + $diskDrive + @("-device", "ide-hd,drive=svdisk,bus=svide.0") }

$qemuArgs = @(
    "-machine", "q35,pcspk-audiodev=audio0",
    "-accel", $Accel,
    "-m", "256M",
    "-cpu", $cpuModel,
    "-audiodev", "none,id=audio0",
    "-audiodev", "none,id=audio1",
    "-display", "none",
    "-rtc", "base=localtime",
    "-drive", "if=pflash,format=raw,readonly=on,file=""$($ovmf.Code)""",
    "-drive", "if=pflash,format=raw,file=""$varsCopy""",
    "-drive", "file=fat:rw:build/image,format=raw",
    "-netdev", "user,id=net0",
    "-serial", "file:$serialLog",
    "-qmp", "tcp:127.0.0.1:$qmpPort,server,nowait",
    "-no-reboot",
    "-no-shutdown"
) + $nicDevice + $diskDevices + $videoInputDevices + $audioDevice

Write-Host "shoot: escenario '$Scenario', salida en $OutDir"

# Con -Virtio el puntero es virtio-tablet, que es ABSOLUTO: el truco de
# empujar contra la esquina para encontrar el origen solo vale para el PS/2
# relativo. Ver move_to en shoot_session.py.
$absPointerArg = if ($Virtio) { @("--abs-pointer") } else { @() }

Push-Location $ProjectRoot
try {
    $process = Start-Process -FilePath $qemu -ArgumentList $qemuArgs -PassThru `
        -RedirectStandardOutput (Join-Path $OutDir "shoot-qemu-out.log") `
        -RedirectStandardError (Join-Path $OutDir "shoot-qemu-err.log")

    try {
        & $python (Join-Path $PSScriptRoot "shoot_session.py") `
            --port $qmpPort --serial $serialLog --out $OutDir `
            --scenario $Scenario --boot-wait $BootWait $absPointerArg
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
