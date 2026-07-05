Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# Orquesta SOLO la fase de compilar objetos (kernel + userland) via Ninja.
# El resto del pipeline (link, imagen SVFS2, ISO, QEMU) lo sigue manejando
# build.ps1 como siempre. Ninja aporta lo que un incremental casero por
# timestamp no puede garantizar: paralelismo real entre cores y tracking
# correcto de dependencias de headers via los .d que emite clang (-MMD).
#
# Requiere que Toolchain.ps1 (Get-ToolchainCandidates) y Require-Executable
# ya esten en scope (dot-sourced antes que este archivo).

# Escapa un token que va a aparecer en una linea "build <out>: <rule> <in>".
# Ahi ':' y ' ' son sintacticamente especiales para Ninja.
function Format-NinjaPathToken([string]$Path) {
    $result = $Path.Replace('\', '/')
    $result = $result.Replace('$', '$$')
    $result = $result.Replace(':', '$:')
    $result = $result.Replace(' ', '$ ')
    return $result
}

# Escapa un valor de variable (flags, ruta del compilador). Ninja lo pasa
# tal cual al comando, asi que ':' y ' ' no necesitan escape ahi - solo '$'.
function Format-NinjaVarValue([string]$Text) {
    return $Text.Replace('$', '$$')
}

# Cada edge: @{ SourcePath; ObjectPath; FlagsVar = "kernelflags"|"userflags"; LangFlag = "" | "-x c" | "-x assembler-with-cpp" }
function Write-NinjaCompileFile {
    param(
        [string]$NinjaPath,
        [string]$Clangxx,
        [string[]]$KernelFlags,
        [string[]]$UserFlags,
        [string[]]$UacpiFlags,
        [object[]]$Edges
    )

    $kernelFlagsValue = ($KernelFlags | ForEach-Object { Format-NinjaVarValue $_ }) -join ' '
    $userFlagsValue = ($UserFlags | ForEach-Object { Format-NinjaVarValue $_ }) -join ' '
    $uacpiFlagsValue = ($UacpiFlags | ForEach-Object { Format-NinjaVarValue $_ }) -join ' '

    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("ninja_required_version = 1.10")
    [void]$sb.AppendLine("clangxx = " + (Format-NinjaVarValue $Clangxx))
    [void]$sb.AppendLine("kernelflags = " + $kernelFlagsValue)
    [void]$sb.AppendLine("userflags = " + $userFlagsValue)
    [void]$sb.AppendLine("uacpiflags = " + $uacpiFlagsValue)
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine('rule compile')
    [void]$sb.AppendLine('  command = $clangxx -MMD -MF $out.d $langflag -c $in -o $out $flags')
    [void]$sb.AppendLine('  depfile = $out.d')
    [void]$sb.AppendLine('  deps = gcc')
    [void]$sb.AppendLine('  description = CC $out')
    [void]$sb.AppendLine("")

    foreach ($edge in $Edges) {
        $obj = Format-NinjaPathToken $edge.ObjectPath
        $src = Format-NinjaPathToken $edge.SourcePath
        [void]$sb.AppendLine("build ${obj}: compile ${src}")
        if ($edge.FlagsVar -eq "kernelflags") {
            [void]$sb.AppendLine('  flags = $kernelflags')
        } elseif ($edge.FlagsVar -eq "uacpiflags") {
            [void]$sb.AppendLine('  flags = $uacpiflags')
        } else {
            [void]$sb.AppendLine('  flags = $userflags')
        }
        if ($edge.LangFlag) {
            [void]$sb.AppendLine("  langflag = " + $edge.LangFlag)
        } else {
            [void]$sb.AppendLine('  langflag =')
        }
    }

    # Set-Content -Encoding UTF8 en Windows PowerShell 5.1 antepone BOM, que
    # el lexer de Ninja no acepta como primer byte del archivo.
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($NinjaPath, $sb.ToString(), $utf8NoBom)
}

function Invoke-NinjaCompile {
    param(
        [string]$BuildRoot,
        [string]$Clangxx,
        [string[]]$KernelFlags,
        [string[]]$UserFlags,
        [string[]]$UacpiFlags,
        [object[]]$Edges
    )

    if ($Edges.Count -eq 0) {
        return
    }

    $ninja = Require-Executable "ninja" (Get-ToolchainCandidates "ninja")
    $ninjaPath = Join-Path $BuildRoot "compile.ninja"
    Write-NinjaCompileFile -NinjaPath $ninjaPath -Clangxx $Clangxx -KernelFlags $KernelFlags -UserFlags $UserFlags -UacpiFlags $UacpiFlags -Edges $Edges

    & $ninja -C $BuildRoot -f "compile.ninja"
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo la compilacion via ninja."
    }
}
