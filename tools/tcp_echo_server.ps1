# Otro extremo del harness de TCP (subsystems/posix/userland/tcptest.c).
#
# Escucha en la loopback del host, que es adonde el user-net de QEMU manda lo
# que el guest dirige a 10.0.2.2. No hace falta abrir nada en el firewall ni
# tener red de verdad: el trafico no sale de la maquina.
#
# Protocolo, en lineas terminadas en \n:
#
#   BULK <n>   devuelve n bytes del patron, de un tiron
#   ECHO <n>   lee n bytes y los devuelve tal cual, a medida que llegan
#   QUIT       cierra
#
# El patron es el mismo que genera el guest: byte i = (i * 31 + 7) & 0xff. Que
# sea aritmetico y no aleatorio es lo que permite verificar sin compartir un
# archivo, y que dependa del indice es lo que hace que un tramo reensamblado al
# reves no pase desapercibido.

param(
    [Parameter(Mandatory = $true)][int]$Port,
    [int]$IdleTimeoutSeconds = 240
)

$ErrorActionPreference = "Stop"

function Get-PatternBytes([int]$Offset, [int]$Count) {
    $bytes = New-Object byte[] $Count
    for ($index = 0; $index -lt $Count; ++$index) {
        $bytes[$index] = [byte](((($Offset + $index) * 31) + 7) -band 0xff)
    }
    return $bytes
}

function Read-Line([System.Net.Sockets.NetworkStream]$Stream) {
    $builder = New-Object System.Text.StringBuilder
    while ($true) {
        $value = $Stream.ReadByte()
        if ($value -lt 0) {
            return $null
        }
        if ($value -eq 10) {
            return $builder.ToString().TrimEnd("`r")
        }
        [void]$builder.Append([char]$value)
    }
}

$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
$listener.Start()
Write-Host "tcp_echo_server: escuchando en 127.0.0.1:$Port"

$deadline = (Get-Date).AddSeconds($IdleTimeoutSeconds)
try {
    while ((Get-Date) -lt $deadline) {
        if (-not $listener.Pending()) {
            Start-Sleep -Milliseconds 50
            continue
        }

        $client = $listener.AcceptTcpClient()
        $client.NoDelay = $true
        $client.ReceiveTimeout = 60000
        $client.SendTimeout = 60000
        Write-Host "tcp_echo_server: conexion aceptada"
        $stream = $client.GetStream()
        try {
            while ($true) {
                $line = Read-Line $stream
                if ($null -eq $line -or $line -eq "QUIT") {
                    break
                }

                if ($line -match "^BULK (\d+)$") {
                    $total = [int]$Matches[1]
                    Write-Host "tcp_echo_server: BULK $total"
                    $sent = 0
                    while ($sent -lt $total) {
                        $chunk = [Math]::Min(4096, $total - $sent)
                        $bytes = Get-PatternBytes $sent $chunk
                        $stream.Write($bytes, 0, $chunk)
                        $sent += $chunk
                    }
                    $stream.Flush()
                }
                elseif ($line -match "^ECHO (\d+)$") {
                    $total = [int]$Matches[1]
                    Write-Host "tcp_echo_server: ECHO $total"
                    $buffer = New-Object byte[] 4096
                    $received = 0
                    while ($received -lt $total) {
                        $want = [Math]::Min($buffer.Length, $total - $received)
                        $read = $stream.Read($buffer, 0, $want)
                        if ($read -le 0) {
                            break
                        }
                        # Se devuelve enseguida y no al final: el guest manda un
                        # chunk y espera el suyo antes del siguiente, asi que
                        # juntar todo trabaria la conversacion.
                        $stream.Write($buffer, 0, $read)
                        $stream.Flush()
                        $received += $read
                    }
                }
                else {
                    Write-Host "tcp_echo_server: comando desconocido '$line'"
                    break
                }
            }
        }
        finally {
            $stream.Dispose()
            $client.Close()
            Write-Host "tcp_echo_server: conexion cerrada"
        }

        $deadline = (Get-Date).AddSeconds($IdleTimeoutSeconds)
    }
}
finally {
    $listener.Stop()
}
