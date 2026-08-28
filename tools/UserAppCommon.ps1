Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Script:ProjectRoot = Split-Path -Parent $PSScriptRoot
$Script:BuildRoot = Join-Path $Script:ProjectRoot "build"
$Script:SdkBuildRoot = Join-Path $Script:BuildRoot "sdk"
$Script:SdkRoot = Join-Path $Script:ProjectRoot "subsystems/posix/sdk/v1"
$Script:DiskImage = Join-Path $Script:BuildRoot "disk.img"
$Script:SvfsSectorSize = 512
$Script:SvfsEntrySize = 64

. (Join-Path $PSScriptRoot "Toolchain.ps1")

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

function Ensure-Directory([string]$Path) {
    if (-not (Test-Path $Path)) {
        New-Item -ItemType Directory -Path $Path | Out-Null
    }
}

function Get-UserCompileFlags {
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
        "-I", (Join-Path $Script:SdkRoot "include"),
        # Formatos compartidos con el host (sxe/sxe_format.h): build.ps1 ya los
        # expone a las apps in-tree, y una app externa que traiga recursos
        # tiene que poder leerlos igual.
        "-I", (Join-Path $Script:ProjectRoot "include")
    )
}

function Get-ExternalSourceSpec([string]$SourcePath) {
    $sourceFull = (Resolve-Path $SourcePath).Path
    $sourceItem = Get-Item $sourceFull
    $excluded = @{}

    if (-not $sourceItem.PSIsContainer) {
        return [pscustomobject]@{
            Root = Split-Path -Parent $sourceFull
            Files = @($sourceFull)
            IncludeDirs = @((Split-Path -Parent $sourceFull))
        }
    }

    $excludeFile = Join-Path $sourceFull "compile-exclude.txt"
    if (Test-Path $excludeFile) {
        foreach ($entry in (Get-Content $excludeFile)) {
            $trimmed = $entry.Trim()
            if ([string]::IsNullOrWhiteSpace($trimmed) -or $trimmed.StartsWith("#")) {
                continue
            }
            $excluded[(Join-Path $sourceFull $trimmed)] = $true
        }
    }

    $sourceFiles = @(Get-ChildItem $sourceFull -Recurse -File -Include *.c,*.S | Sort-Object FullName)
    if ($excluded.Count -ne 0) {
        $sourceFiles = @($sourceFiles | Where-Object { -not $excluded.ContainsKey($_.FullName) })
    }
    if ($sourceFiles.Count -eq 0) {
        throw "El directorio '$SourcePath' no contiene fuentes .c o .S."
    }

    $includeDirs = @($sourceFull)
    $publicInclude = Join-Path $sourceFull "include"
    if (Test-Path $publicInclude) {
        $includeDirs += $publicInclude
    }

    return [pscustomobject]@{
        Root = $sourceFull
        Files = @($sourceFiles | ForEach-Object { $_.FullName })
        IncludeDirs = $includeDirs
    }
}

function Get-ExternalCompileFlags([string[]]$IncludeDirs) {
    $flags = @(Get-UserCompileFlags)
    foreach ($includeDir in $IncludeDirs) {
        $flags += @("-I", $includeDir)
    }
    return $flags
}

function Get-ObjectNameForSource([string]$SourceRoot, [string]$SourceFile) {
    $rootPath = $SourceRoot
    if (-not $rootPath.EndsWith([System.IO.Path]::DirectorySeparatorChar) -and -not $rootPath.EndsWith([System.IO.Path]::AltDirectorySeparatorChar)) {
        $rootPath += [System.IO.Path]::DirectorySeparatorChar
    }
    $rootUri = New-Object System.Uri($rootPath)
    $fileUri = New-Object System.Uri($SourceFile)
    $relative = [System.Uri]::UnescapeDataString($rootUri.MakeRelativeUri($fileUri).ToString()).Replace('/', '\')
    $sanitized = $relative.Replace('\', '_').Replace('/', '_').Replace(':', '_')
    return [System.IO.Path]::ChangeExtension($sanitized, ".o")
}

function Get-UInt32Le([byte[]]$Buffer, [int]$Offset) {
    return [System.BitConverter]::ToUInt32($Buffer, $Offset)
}

function Set-UInt32Le([byte[]]$Buffer, [int]$Offset, [uint32]$Value) {
    $bytes = [System.BitConverter]::GetBytes($Value)
    [Array]::Copy($bytes, 0, $Buffer, $Offset, 4)
}

function Get-AsciiField([byte[]]$Buffer, [int]$Offset, [int]$Capacity) {
    $length = 0
    while ($length -lt $Capacity -and $Buffer[$Offset + $length] -ne 0) {
        $length += 1
    }
    return [System.Text.Encoding]::ASCII.GetString($Buffer, $Offset, $length)
}

function Set-AsciiField([byte[]]$Buffer, [int]$Offset, [string]$Text, [int]$Capacity) {
    for ($i = 0; $i -lt $Capacity; $i += 1) {
        $Buffer[$Offset + $i] = 0
    }
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($Text)
    $count = [Math]::Min($bytes.Length, $Capacity - 1)
    [Array]::Copy($bytes, 0, $Buffer, $Offset, $count)
}

# --- Recursos SXE (docs/SXE_FORMAT.md, fase 2) -------------------------------
#
# Viven aca y no en build.ps1 para que los dos caminos de build -- el in-tree
# (build.ps1) y el de apps externas (build-user.ps1) -- estampen con la MISMA
# implementacion. Duplicar el paso significaria que la verificacion de no-alloc
# se aplica en uno y no en el otro, que es exactamente el modo de falla que la
# verificacion existe para evitar.

function Get-PythonExecutable {
    # "python" primero: en Windows, "python3" suele resolver al alias-stub de
    # la Microsoft Store (existe para Get-Command pero falla al ejecutarlo).
    return Require-Executable "python" @("python", "python3")
}

# llvm-objcopy y llvm-readelf viven al lado de clang en el bundle de LLVM. El
# manifiesto del toolchain los lista desde el bootstrap, pero el fallback al
# directorio de clang va ANTES del nombre pelado para que un arbol
# bootstrapeado antes de que esas claves existieran igual buildee sin depender
# del PATH.
function Get-LlvmToolCandidates([string]$Name) {
    $candidates = [System.Collections.ArrayList]@(Get-ToolchainCandidates $Name)
    $clang = Resolve-Executable (Get-ToolchainCandidates "clang")
    if ($clang) {
        $sibling = Join-Path (Split-Path -Parent $clang) ($Name + [IO.Path]::GetExtension($clang))
        [void]$candidates.Insert($candidates.Count - 1, $sibling)
    }
    return $candidates.ToArray()
}

# Convierte los manifiestos .sxres que haya en $ManifestDirs a los blobs
# .sxmeta/.sxicon de $OutputDir.
function Invoke-SxeResourceGenerator([string[]]$ManifestDirs, [string]$OutputDir) {
    $existing = @($ManifestDirs | Where-Object { $_ -and (Test-Path $_) })
    if ($existing.Count -eq 0) {
        return
    }
    Ensure-Directory $OutputDir

    $python = Get-PythonExecutable
    $arguments = @(
        (Join-Path $PSScriptRoot "gen_sxe_resources.py"),
        "--project-root", $Script:ProjectRoot,
        "--output-dir", $OutputDir
    )
    foreach ($directory in $existing) {
        $arguments += @("--manifest-dir", $directory)
    }

    # La salida se CAPTURA en vez de dejarla caer al pipeline: esta funcion se
    # llama desde Build-ExternalUserProgram, y en PowerShell todo lo que un
    # comando escribe sin capturar se suma al valor de retorno de la funcion
    # que lo contiene. Un "sxe: N manifiestos" suelto convertia esa ruta
    # devuelta en un array de dos elementos.
    $output = & $python @arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        $output | ForEach-Object { Write-Host $_ }
        throw "Fallo la generacion de los recursos SXE."
    }
    $output | ForEach-Object { Write-Host $_ }
}

# Estampa los recursos de un programa en su binario ya linkeado.
#
# Las secciones tienen que quedar SIN SHF_ALLOC: si se mapean, cada icono pasa
# a ser RAM residente por proceso y el argumento entero de meter recursos en el
# ejecutable se cae EN SILENCIO -- no hay sintoma visible, solo memoria de mas.
# Por eso la verificacion con readelf no es opcional.
function Add-SxeResources([string]$Name, [string]$BinaryPath, [string]$ResourceDir, [string]$Objcopy, [string]$Readelf) {
    $sections = @()
    foreach ($pair in @(
        @{ Section = ".sxmeta"; Path = (Join-Path $ResourceDir "$Name.sxmeta") },
        @{ Section = ".sxicon"; Path = (Join-Path $ResourceDir "$Name.sxicon") }
    )) {
        if (Test-Path $pair.Path) {
            $sections += $pair
        }
    }
    if ($sections.Count -eq 0) {
        # Sin manifiesto no se toca el binario. Un ejecutable sin recursos es
        # un ejecutable de primera clase (docs/SXE_FORMAT.md).
        return
    }

    if (-not $Objcopy) { $Objcopy = Require-Executable "llvm-objcopy" (Get-LlvmToolCandidates "llvm-objcopy") }
    if (-not $Readelf) { $Readelf = Require-Executable "llvm-readelf" (Get-LlvmToolCandidates "llvm-readelf") }

    $objcopyArguments = @()
    foreach ($pair in $sections) {
        $objcopyArguments += @("--add-section", "$($pair.Section)=$($pair.Path)")
    }
    $stamped = "$BinaryPath.sxe-tmp"
    # Capturada por el mismo motivo que en Invoke-SxeResourceGenerator: nada de
    # esta funcion debe llegar al pipeline de quien la llama.
    $objcopyOutput = & $Objcopy @objcopyArguments $BinaryPath $stamped 2>&1
    if ($LASTEXITCODE -ne 0) {
        $objcopyOutput | ForEach-Object { Write-Host $_ }
        throw "Fallo el estampado de recursos SXE en '$Name'."
    }
    Move-Item -Path $stamped -Destination $BinaryPath -Force

    # --section-details imprime el valor CRUDO de sh_flags en hexa, asi que la
    # comprobacion es el bit exacto (SHF_ALLOC = 0x2) y no depende de columnas
    # ni de los nombres que readelf le ponga a los flags.
    $details = & $Readelf --section-details $BinaryPath
    if ($LASTEXITCODE -ne 0) {
        throw "No se pudieron leer los section headers de '$Name'."
    }
    foreach ($pair in $sections) {
        $found = $false
        for ($index = 0; $index -lt $details.Count; $index += 1) {
            if ($details[$index] -notmatch "^\s*\[\s*\d+\]\s+$([regex]::Escape($pair.Section))\s*$") {
                continue
            }
            $found = $true
            $flagsLine = if ($index + 2 -lt $details.Count) { $details[$index + 2] } else { "" }
            if ($flagsLine -notmatch '^\s*\[([0-9a-fA-F]{16})\]') {
                throw "No se pudo leer sh_flags de $($pair.Section) en '$Name'."
            }
            if (([Convert]::ToUInt64($Matches[1], 16) -band 0x2) -ne 0) {
                throw "La seccion $($pair.Section) de '$Name' quedo marcada SHF_ALLOC: se mapearia en cada proceso."
            }
            break
        }
        if (-not $found) {
            throw "El binario '$Name' no quedo con la seccion $($pair.Section) tras el estampado."
        }
    }
}

function Build-ExternalUserProgram([string]$SourcePath, [string]$ProgramName, [string]$OutputPath, [int]$HeapMiB = 0) {
    $compiler = Require-Executable "clang" (Get-ToolchainCandidates "clang")
    $linker = Require-Executable "ld.lld" (Get-ToolchainCandidates "ld.lld")
    $sourceSpec = Get-ExternalSourceSpec $SourcePath
    $compileFlags = Get-ExternalCompileFlags $sourceSpec.IncludeDirs
    $outputFull = [System.IO.Path]::GetFullPath($OutputPath)
    $objectRoot = Join-Path $Script:SdkBuildRoot $ProgramName
    Ensure-Directory $objectRoot
    Ensure-Directory (Split-Path -Parent $outputFull)

    $libcObject = Join-Path $objectRoot "libc.o"
    $posixObject = Join-Path $objectRoot "posix.o"
    $gfxObject = Join-Path $objectRoot "gfx.o"
    $gfx2dObject = Join-Path $objectRoot "gfx2d.o"
    $setjmpObject = Join-Path $objectRoot "setjmp.o"
    $crtObject = Join-Path $objectRoot "crt0.o"
    $appObjects = @()

    foreach ($sourceFile in $sourceSpec.Files) {
        $extension = [System.IO.Path]::GetExtension($sourceFile).ToLowerInvariant()
        $objectPath = Join-Path $objectRoot (Get-ObjectNameForSource $sourceSpec.Root $sourceFile)
        if ($extension -eq ".s") {
            & $compiler -c $sourceFile -o $objectPath @compileFlags
        } else {
            & $compiler -c -x c $sourceFile -o $objectPath @compileFlags
        }
        if ($LASTEXITCODE -ne 0) {
            throw "Fallo la compilacion de '$sourceFile'."
        }
        $appObjects += $objectPath
    }

    & $compiler -c -x c (Join-Path $Script:SdkRoot "runtime/libc.c") -o $libcObject @compileFlags
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo la compilacion del runtime libc."
    }

    # El malloc ya no depende de una arena de BSS sobredimensionada: arranca con
    # un bootstrap chico y pide arenas al kernel (section_create + map_view) a
    # medida que las necesita, asi que por default no se fuerza -DSX_HEAP_SIZE.
    # -HeapMiB sigue disponible para clavar toda la arena adelantada en la BSS,
    # pero acordarse de que cada MiB de ahi es RAM residente por proceso aunque
    # la app no lo toque: sobredimensionarla es lo que hacia fallar un exec por
    # falta de memoria cuando ya habia otra instancia corriendo.
    $posixFlags = @()
    if ($HeapMiB -gt 0) {
        $posixFlags += "-DSX_HEAP_SIZE=(${HeapMiB}u*1024u*1024u)"
    }
    & $compiler -c -x c (Join-Path $Script:SdkRoot "runtime/posix.c") -o $posixObject @posixFlags @compileFlags
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo la compilacion del runtime posix."
    }

    & $compiler -c -x c (Join-Path $Script:SdkRoot "runtime/gfx.c") -o $gfxObject @compileFlags
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo la compilacion del runtime gfx."
    }

    & $compiler -c -x c (Join-Path $Script:SdkRoot "runtime/gfx2d.c") -o $gfx2dObject @compileFlags
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo la compilacion del runtime gfx2d."
    }

    & $compiler -c (Join-Path $Script:SdkRoot "runtime/setjmp.S") -o $setjmpObject @compileFlags
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo la compilacion del runtime setjmp."
    }

    & $compiler -c (Join-Path $Script:SdkRoot "runtime/crt0.S") -o $crtObject @compileFlags
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo la compilacion de crt0."
    }

    & $linker -nostdlib -static -T (Join-Path $Script:SdkRoot "linker.ld") -o $outputFull $crtObject $libcObject $posixObject $gfxObject $gfx2dObject $setjmpObject @appObjects
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo el link de '$SourcePath'."
    }

    # Una app externa declara sus recursos con un <nombre>.sxres al lado de su
    # fuente. Sin manifiesto el binario sale igual que siempre.
    $resourceRoot = Join-Path $objectRoot "sxe"
    Invoke-SxeResourceGenerator -ManifestDirs @($sourceSpec.Root) -OutputDir $resourceRoot
    Add-SxeResources -Name $ProgramName -BinaryPath $outputFull -ResourceDir $resourceRoot

    return $outputFull
}

$Script:Svfs2Magic = "SVFS2"
$Script:Svfs2JournalMagic = "SVJNL2"
$Script:Svfs2PrimarySuperblockLba = 0
$Script:Svfs2SecondarySuperblockLba = 1
$Script:Svfs2JournalLba = 2
$Script:Svfs2BlockBitmapSectors = 32
$Script:Svfs2InodeBitmapSectors = 1
$Script:Svfs2InodeTableSectors = 64
$Script:Svfs2JournalMetadataSectors = $Script:Svfs2BlockBitmapSectors + $Script:Svfs2InodeBitmapSectors + $Script:Svfs2InodeTableSectors
$Script:Svfs2JournalSectors = 1 + $Script:Svfs2JournalMetadataSectors
$Script:Svfs2BlockBitmapLba = $Script:Svfs2JournalLba + $Script:Svfs2JournalSectors
$Script:Svfs2InodeBitmapLba = $Script:Svfs2BlockBitmapLba + $Script:Svfs2BlockBitmapSectors
$Script:Svfs2InodeTableLba = $Script:Svfs2InodeBitmapLba + $Script:Svfs2InodeBitmapSectors
$Script:Svfs2DataLba = $Script:Svfs2InodeTableLba + $Script:Svfs2InodeTableSectors
$Script:Svfs2MaxInodes = 256
$Script:Svfs2RootInode = 1
$Script:Svfs2InodeSize = 128
$Script:Svfs2DirEntrySize = 80
$Script:Svfs2MaxExtents = 8
$Script:Svfs2TotalSectors = 131072

# Ruta al header canonico del formato (fuente de verdad unica compartida con el
# kernel). Los $Script:Svfs2* de arriba son un espejo en PowerShell que
# Assert-Svfs2FormatMatchesHeader valida contra este archivo en cada build.
$Script:Svfs2FormatHeader = Join-Path $Script:ProjectRoot "include/svfs/svfs_format.h"

# Verifica que las constantes de layout de SVFS2 en PowerShell coincidan con
# include/svfs/svfs_format.h. Historicamente el formato vivia duplicado a mano
# entre el kernel (C++) y el host (PowerShell) y las copias se desincronizaban;
# esta comprobacion convierte esa divergencia en un error de build inmediato en
# vez de una corrupcion silenciosa de la imagen. Parsea solo los #define base
# (literales enteros) y re-deriva los LBAs compuestos con las mismas formulas
# que el header, cazando tanto drift de constantes como de formulas.
function Assert-Svfs2FormatMatchesHeader {
    if (-not (Test-Path $Script:Svfs2FormatHeader)) {
        throw "No se encuentra el header canonico de formato SVFS2 en '$($Script:Svfs2FormatHeader)'."
    }
    $text = [System.IO.File]::ReadAllText($Script:Svfs2FormatHeader)

    $base = @{}
    foreach ($line in ($text -split "`n")) {
        if ($line -match '^\s*#define\s+(SVFS_[A-Z0-9_]+)\s+([0-9]+)u?\s*(?:/\*.*)?$') {
            $base[$Matches[1]] = [uint32]$Matches[2]
        }
    }

    # Magia del superblock: se reconstruye desde el array de chars del header.
    $sbMagic = ""
    if ($text -match 'svfs_superblock_magic\[8\]\s*=\s*\{([^}]*)\}') {
        foreach ($m in [regex]::Matches($Matches[1], "'([^']+)'")) {
            $ch = $m.Groups[1].Value
            if ($ch -eq '\0') { break }
            $sbMagic += $ch
        }
    }

    $required = @(
        'SVFS_SECTOR_SIZE', 'SVFS_PRIMARY_SB_LBA', 'SVFS_SECONDARY_SB_LBA', 'SVFS_JOURNAL_LBA',
        'SVFS_BLOCK_BITMAP_SECTORS', 'SVFS_INODE_BITMAP_SECTORS', 'SVFS_INODE_TABLE_SECTORS',
        'SVFS_MAX_INODES', 'SVFS_ROOT_INODE', 'SVFS_MAX_EXTENTS', 'SVFS_INODE_SIZE', 'SVFS_DIR_ENTRY_SIZE')
    foreach ($key in $required) {
        if (-not $base.ContainsKey($key)) {
            throw "El header '$($Script:Svfs2FormatHeader)' no define '$key' como literal entero; no se puede validar el formato SVFS2."
        }
    }

    # LBAs compuestos re-derivados con las mismas formulas que el header.
    $journalMetadata = $base['SVFS_BLOCK_BITMAP_SECTORS'] + $base['SVFS_INODE_BITMAP_SECTORS'] + $base['SVFS_INODE_TABLE_SECTORS']
    $journalSectors = 1 + $journalMetadata
    $blockBitmapLba = $base['SVFS_JOURNAL_LBA'] + $journalSectors
    $inodeBitmapLba = $blockBitmapLba + $base['SVFS_BLOCK_BITMAP_SECTORS']
    $inodeTableLba = $inodeBitmapLba + $base['SVFS_INODE_BITMAP_SECTORS']
    $dataLba = $inodeTableLba + $base['SVFS_INODE_TABLE_SECTORS']

    $checks = @(
        @{ Name = 'SectorSize'; Header = $base['SVFS_SECTOR_SIZE']; Ps = $Script:SvfsSectorSize }
        @{ Name = 'PrimarySuperblockLba'; Header = $base['SVFS_PRIMARY_SB_LBA']; Ps = $Script:Svfs2PrimarySuperblockLba }
        @{ Name = 'SecondarySuperblockLba'; Header = $base['SVFS_SECONDARY_SB_LBA']; Ps = $Script:Svfs2SecondarySuperblockLba }
        @{ Name = 'JournalLba'; Header = $base['SVFS_JOURNAL_LBA']; Ps = $Script:Svfs2JournalLba }
        @{ Name = 'BlockBitmapSectors'; Header = $base['SVFS_BLOCK_BITMAP_SECTORS']; Ps = $Script:Svfs2BlockBitmapSectors }
        @{ Name = 'InodeBitmapSectors'; Header = $base['SVFS_INODE_BITMAP_SECTORS']; Ps = $Script:Svfs2InodeBitmapSectors }
        @{ Name = 'InodeTableSectors'; Header = $base['SVFS_INODE_TABLE_SECTORS']; Ps = $Script:Svfs2InodeTableSectors }
        @{ Name = 'MaxInodes'; Header = $base['SVFS_MAX_INODES']; Ps = $Script:Svfs2MaxInodes }
        @{ Name = 'RootInode'; Header = $base['SVFS_ROOT_INODE']; Ps = $Script:Svfs2RootInode }
        @{ Name = 'MaxExtents'; Header = $base['SVFS_MAX_EXTENTS']; Ps = $Script:Svfs2MaxExtents }
        @{ Name = 'InodeSize'; Header = $base['SVFS_INODE_SIZE']; Ps = $Script:Svfs2InodeSize }
        @{ Name = 'DirEntrySize'; Header = $base['SVFS_DIR_ENTRY_SIZE']; Ps = $Script:Svfs2DirEntrySize }
        @{ Name = 'JournalMetadataSectors'; Header = $journalMetadata; Ps = $Script:Svfs2JournalMetadataSectors }
        @{ Name = 'JournalSectors'; Header = $journalSectors; Ps = $Script:Svfs2JournalSectors }
        @{ Name = 'BlockBitmapLba'; Header = $blockBitmapLba; Ps = $Script:Svfs2BlockBitmapLba }
        @{ Name = 'InodeBitmapLba'; Header = $inodeBitmapLba; Ps = $Script:Svfs2InodeBitmapLba }
        @{ Name = 'InodeTableLba'; Header = $inodeTableLba; Ps = $Script:Svfs2InodeTableLba }
        @{ Name = 'DataLba'; Header = $dataLba; Ps = $Script:Svfs2DataLba }
    )

    $mismatches = @()
    foreach ($check in $checks) {
        if ([uint32]$check.Header -ne [uint32]$check.Ps) {
            $mismatches += ("  $($check.Name): header=$($check.Header) vs PowerShell=$($check.Ps)")
        }
    }
    if ($sbMagic -ne $Script:Svfs2Magic) {
        $mismatches += ("  Magic: header='$sbMagic' vs PowerShell='$($Script:Svfs2Magic)'")
    }

    if ($mismatches.Count -gt 0) {
        $message = @(
            "Las constantes de formato SVFS2 en PowerShell divergen de include/svfs/svfs_format.h:"
        ) + $mismatches + @(
            "Actualiza los `$Script:Svfs2* en tools/UserAppCommon.ps1 para que coincidan con el header (fuente de verdad)."
        )
        throw ($message -join [Environment]::NewLine)
    }
}

function Get-UInt16Le([byte[]]$Buffer, [int]$Offset) {
    return [System.BitConverter]::ToUInt16($Buffer, $Offset)
}

function Set-UInt16Le([byte[]]$Buffer, [int]$Offset, [uint16]$Value) {
    $bytes = [System.BitConverter]::GetBytes($Value)
    [Array]::Copy($bytes, 0, $Buffer, $Offset, 2)
}

function Get-Svfs2Checksum([byte[]]$Buffer, [int]$Offset, [int]$Length, [int]$ChecksumOffset) {
    [uint32]$value = 2166136261
    for ($index = 0; $index -lt $Length; $index += 1) {
        $absolute = $Offset + $index
        $byte = if ($ChecksumOffset -ge 0 -and $absolute -ge $ChecksumOffset -and $absolute -lt ($ChecksumOffset + 4)) {
            0
        } else {
            $Buffer[$absolute]
        }
        $mixed = ([uint64]$value -bxor [uint64]$byte)
        $value = [uint32]((([uint64]$mixed * [uint64]16777619) % [uint64]4294967296))
    }
    return $value
}

function Get-Svfs2Superblock([byte[]]$Buffer, [int]$Offset) {
    return [pscustomobject]@{
        Magic = Get-AsciiField $Buffer $Offset 8
        Version = Get-UInt32Le $Buffer ($Offset + 8)
        Checksum = Get-UInt32Le $Buffer ($Offset + 12)
        Sequence = Get-UInt32Le $Buffer ($Offset + 16)
        Flags = Get-UInt32Le $Buffer ($Offset + 20)
        TotalSectors = Get-UInt32Le $Buffer ($Offset + 24)
        JournalLba = Get-UInt32Le $Buffer ($Offset + 28)
        JournalSectors = Get-UInt32Le $Buffer ($Offset + 32)
        BlockBitmapLba = Get-UInt32Le $Buffer ($Offset + 36)
        BlockBitmapSectors = Get-UInt32Le $Buffer ($Offset + 40)
        InodeBitmapLba = Get-UInt32Le $Buffer ($Offset + 44)
        InodeBitmapSectors = Get-UInt32Le $Buffer ($Offset + 48)
        InodeTableLba = Get-UInt32Le $Buffer ($Offset + 52)
        InodeTableSectors = Get-UInt32Le $Buffer ($Offset + 56)
        DataLba = Get-UInt32Le $Buffer ($Offset + 60)
        MaxInodes = Get-UInt32Le $Buffer ($Offset + 64)
        RootInode = Get-UInt32Le $Buffer ($Offset + 68)
    }
}

function Test-Svfs2Superblock([byte[]]$Buffer, [int]$Offset) {
    $superblock = Get-Svfs2Superblock $Buffer $Offset
    if (-not $superblock.Magic.StartsWith($Script:Svfs2Magic)) {
        return $null
    }
    if ($superblock.Version -ne 2 -or
        $superblock.JournalLba -ne $Script:Svfs2JournalLba -or
        $superblock.BlockBitmapLba -ne $Script:Svfs2BlockBitmapLba -or
        $superblock.InodeBitmapLba -ne $Script:Svfs2InodeBitmapLba -or
        $superblock.InodeTableLba -ne $Script:Svfs2InodeTableLba -or
        $superblock.DataLba -ne $Script:Svfs2DataLba -or
        $superblock.MaxInodes -ne $Script:Svfs2MaxInodes -or
        $superblock.RootInode -ne $Script:Svfs2RootInode) {
        return $null
    }
    $checksum = Get-Svfs2Checksum $Buffer $Offset $Script:SvfsSectorSize ($Offset + 12)
    if ($checksum -ne $superblock.Checksum) {
        return $null
    }
    return $superblock
}

function Get-Svfs2BitmapBit([byte[]]$Buffer, [uint32]$Bit) {
    # OJO: usar division entera (shift), no [int]($Bit / 8): el operador / de
    # PowerShell devuelve double y [int] redondea (banker's rounding), con lo
    # que bit%8>=5 cae en el byte equivocado y desincroniza con el indexado
    # floor del kernel (kernel/svfs.cpp bitmap_test/bitmap_set).
    $byteIndex = [int]($Bit -shr 3)
    $shift = [int]($Bit % 8)
    return ($Buffer[$byteIndex] -band (1 -shl $shift)) -ne 0
}

function Get-Svfs2InodeOffset($Image, [uint32]$InodeId) {
    return ($Script:Svfs2InodeTableLba * $Script:SvfsSectorSize) + (($InodeId - 1) * $Script:Svfs2InodeSize)
}

function Get-Svfs2Inode($Image, [uint32]$InodeId) {
    $offset = Get-Svfs2InodeOffset $Image $InodeId
    $extents = @()
    for ($index = 0; $index -lt $Script:Svfs2MaxExtents; $index += 1) {
        $extentOffset = $offset + 20 + ($index * 8)
        $extents += [pscustomobject]@{
            StartLba = Get-UInt32Le $Image.Bytes $extentOffset
            SectorCount = Get-UInt32Le $Image.Bytes ($extentOffset + 4)
        }
    }
    return [pscustomobject]@{
        Id = Get-UInt32Le $Image.Bytes $offset
        Type = Get-UInt16Le $Image.Bytes ($offset + 4)
        Size = Get-UInt32Le $Image.Bytes ($offset + 8)
        LinkCount = Get-UInt32Le $Image.Bytes ($offset + 12)
        ExtentCount = Get-UInt32Le $Image.Bytes ($offset + 16)
        Extents = $extents
    }
}

function Get-Svfs2InodeCapacityBytes($Inode) {
    [uint64]$capacity = 0
    foreach ($extent in $Inode.Extents | Select-Object -First $Inode.ExtentCount) {
        $capacity += [uint64]$extent.SectorCount * [uint64]$Script:SvfsSectorSize
    }
    return [uint32]$capacity
}

function Read-Svfs2InodeBytes($Image, $Inode) {
    $result = New-Object byte[] $Inode.Size
    $written = 0
    foreach ($extent in $Inode.Extents | Select-Object -First $Inode.ExtentCount) {
        if ($written -ge $Inode.Size) {
            break
        }
        $extentOffset = $extent.StartLba * $Script:SvfsSectorSize
        $extentLength = [Math]::Min($Inode.Size - $written, $extent.SectorCount * $Script:SvfsSectorSize)
        [Array]::Copy($Image.Bytes, $extentOffset, $result, $written, $extentLength)
        $written += $extentLength
    }
    return ,$result
}

function Get-Svfs2DirEntries($Image, [uint32]$InodeId) {
    $inode = Get-Svfs2Inode $Image $InodeId
    $bytes = Read-Svfs2InodeBytes $Image $inode
    $entries = @()
    for ($offset = 0; $offset + $Script:Svfs2DirEntrySize -le $bytes.Length; $offset += $Script:Svfs2DirEntrySize) {
        $inodeRef = Get-UInt32Le $bytes $offset
        $type = Get-UInt16Le $bytes ($offset + 4)
        $nameLength = Get-UInt16Le $bytes ($offset + 6)
        $name = if ($nameLength -gt 0) { Get-AsciiField $bytes ($offset + 8) 64 } else { "" }
        $entries += [pscustomobject]@{
            InodeId = $inodeRef
            Type = $type
            NameLength = $nameLength
            Name = $name
        }
    }
    return $entries
}

function Find-Svfs2Path($Image, [string]$RelativePath) {
    if ([string]::IsNullOrWhiteSpace($RelativePath)) {
        return [pscustomobject]@{ InodeId = $Script:Svfs2RootInode; ParentInodeId = $Script:Svfs2RootInode; Name = ""; Type = 2 }
    }

    $current = $Script:Svfs2RootInode
    $parent = $Script:Svfs2RootInode
    $match = $null
    foreach ($component in $RelativePath.Split('/')) {
        $entries = Get-Svfs2DirEntries $Image $current
        $match = $entries | Where-Object { $_.InodeId -ne 0 -and $_.Name -eq $component } | Select-Object -First 1
        if (-not $match) {
            return $null
        }
        $parent = $current
        $current = [uint32]$match.InodeId
    }
    return [pscustomobject]@{
        InodeId = $current
        ParentInodeId = $parent
        Name = ($RelativePath.Split('/') | Select-Object -Last 1)
        Type = [uint16]$match.Type
    }
}

function Get-Svfs2TypeName([uint16]$Type) {
    switch ($Type) {
        1 { return "file" }
        2 { return "directory" }
        default { return "type $Type" }
    }
}

function Get-Svfs2PathInfo($Image, [string]$Path) {
    $relative = if ($Path -eq "/disk" -or $Path -eq "/disk/") {
        ""
    } else {
        Get-RelativeSvfsPath $Path
    }

    $entry = Find-Svfs2Path $Image $relative
    if (-not $entry) {
        return $null
    }

    $inode = Get-Svfs2Inode $Image $entry.InodeId
    return [pscustomobject]@{
        Path = if ($relative) { "/disk/$relative" } else { "/disk" }
        RelativePath = $relative
        Entry = $entry
        Inode = $inode
    }
}

function Read-Svfs2FileBytesByPath($Image, [string]$Path) {
    $info = Get-Svfs2PathInfo $Image $Path
    if (-not $info) {
        return $null
    }
    if ($info.Entry.Type -ne 1 -or $info.Inode.Type -ne 1) {
        throw "'$Path' no es un archivo regular en SVFS2."
    }
    return Read-Svfs2InodeBytes $Image $info.Inode
}

function Test-Svfs2Consistency($Image) {
    $issues = New-Object System.Collections.Generic.List[string]
    $queue = New-Object System.Collections.Generic.Queue[object]
    $visited = @{}
    $claimedSectors = @{}
    $reportedAliases = @{}
    $reportedOverlaps = @{}

    $queue.Enqueue([pscustomobject]@{
        Path = "/disk"
        RelativePath = ""
        InodeId = $Script:Svfs2RootInode
        ExpectedType = 2
    })

    while ($queue.Count -gt 0) {
        $current = $queue.Dequeue()
        if ($visited.ContainsKey($current.InodeId)) {
            $firstPath = [string]$visited[$current.InodeId]
            if ($firstPath -ne $current.Path) {
                $aliasKey = "{0}|{1}" -f $current.InodeId, (($firstPath, $current.Path | Sort-Object) -join "|")
                if (-not $reportedAliases.ContainsKey($aliasKey)) {
                    $reportedAliases[$aliasKey] = $true
                    $issues.Add(("$($current.Path): inode $($current.InodeId) ya estaba referenciado por $firstPath"))
                }
            }
            continue
        }
        $visited[$current.InodeId] = $current.Path

        try {
            $inode = Get-Svfs2Inode $Image ([uint32]$current.InodeId)
        } catch {
            $issues.Add(("$($current.Path): inode $($current.InodeId) no se pudo leer ({0})" -f $_.Exception.Message))
            continue
        }

        if (-not (Get-Svfs2BitmapBit $Image.InodeBitmap ([uint32]($current.InodeId - 1)))) {
            $issues.Add(("$($current.Path): el inode $($current.InodeId) no esta marcado en el bitmap"))
        }

        if ($inode.Id -ne $current.InodeId) {
            $issues.Add(("$($current.Path): inode esperado $($current.InodeId) pero se leyo $($inode.Id)"))
            continue
        }

        if ($inode.Type -ne $current.ExpectedType) {
            $issues.Add(("$($current.Path): la entrada dice {0} pero el inode $($current.InodeId) es {1}" -f (Get-Svfs2TypeName $current.ExpectedType), (Get-Svfs2TypeName $inode.Type)))
        }

        $capacity = Get-Svfs2InodeCapacityBytes $inode
        if ($inode.Size -gt $capacity) {
            $issues.Add(("$($current.Path): inode $($current.InodeId) declara size=$($inode.Size) pero su capacidad es $capacity"))
        }

        $missingBlocks = $false
        foreach ($extent in $inode.Extents | Select-Object -First $inode.ExtentCount) {
            if (($extent.StartLba -eq 0) -xor ($extent.SectorCount -eq 0)) {
                $issues.Add(("$($current.Path): inode $($current.InodeId) tiene extent invalido start=$($extent.StartLba) sectors=$($extent.SectorCount)"))
                continue
            }
            if ($extent.StartLba -eq 0 -and $extent.SectorCount -eq 0) {
                continue
            }

            $extentLast = [uint64]$extent.StartLba + [uint64]$extent.SectorCount - 1
            if ($extentLast -ge [uint64]$Image.TotalSectors) {
                $issues.Add(("$($current.Path): inode $($current.InodeId) usa extent fuera de rango start=$($extent.StartLba) sectors=$($extent.SectorCount)"))
                continue
            }

            for ($sector = 0; $sector -lt $extent.SectorCount; $sector += 1) {
                $absoluteSector = [uint32]($extent.StartLba + $sector)
                if (-not (Get-Svfs2BitmapBit $Image.BlockBitmap $absoluteSector)) {
                    $missingBlocks = $true
                }

                if ($claimedSectors.ContainsKey($absoluteSector)) {
                    $owner = $claimedSectors[$absoluteSector]
                    if ($owner.InodeId -ne $current.InodeId) {
                        $low = [Math]::Min([int]$owner.InodeId, [int]$current.InodeId)
                        $high = [Math]::Max([int]$owner.InodeId, [int]$current.InodeId)
                        $overlapKey = "$low|$high"
                        if (-not $reportedOverlaps.ContainsKey($overlapKey)) {
                            $reportedOverlaps[$overlapKey] = $true
                            $issues.Add(("$($current.Path): inode $($current.InodeId) comparte sectores con $($owner.Path) (inode $($owner.InodeId))"))
                        }
                    }
                    continue
                }

                $claimedSectors[$absoluteSector] = [pscustomobject]@{
                    InodeId = $current.InodeId
                    Path = $current.Path
                }
            }
        }
        if ($missingBlocks) {
            $issues.Add(("$($current.Path): inode $($current.InodeId) usa bloques no marcados en el bitmap"))
        }

        if ($inode.Type -ne 2) {
            continue
        }

        foreach ($entry in Get-Svfs2DirEntries $Image ([uint32]$current.InodeId)) {
            if ($entry.InodeId -eq 0) {
                continue
            }

            $entryPath = if ($current.RelativePath) {
                "/disk/$($current.RelativePath)/$($entry.Name)"
            } else {
                "/disk/$($entry.Name)"
            }

            if ([string]::IsNullOrWhiteSpace($entry.Name) -or $entry.NameLength -eq 0) {
                $issues.Add(("${entryPath}: entrada de directorio vacia para inode $($entry.InodeId)"))
                continue
            }
            if ($entry.NameLength -ne $entry.Name.Length) {
                $issues.Add(("${entryPath}: NameLength=$($entry.NameLength) no coincide con longitud real $($entry.Name.Length)"))
            }
            if ($entry.InodeId -gt $Script:Svfs2MaxInodes) {
                $issues.Add(("${entryPath}: referencia a inode invalido $($entry.InodeId)"))
                continue
            }

            $queue.Enqueue([pscustomobject]@{
                Path = $entryPath
                RelativePath = if ($current.RelativePath) { "$($current.RelativePath)/$($entry.Name)" } else { $entry.Name }
                InodeId = [uint32]$entry.InodeId
                ExpectedType = [uint16]$entry.Type
            })
        }
    }

    return $issues.ToArray()
}

function Assert-Svfs2Consistency($Image, [string]$DiskPath = "") {
    $issues = @(Test-Svfs2Consistency $Image)
    if ($issues.Count -eq 0) {
        return
    }

    $resolvedDiskPath = ""
    if (-not [string]::IsNullOrWhiteSpace($DiskPath) -and -not $DiskPath.TrimStart().StartsWith("@{")) {
        $resolvedDiskPath = $DiskPath
    } elseif ($null -ne $Image -and $Image.PSObject.Properties.Name -contains "DiskPath" -and -not [string]::IsNullOrWhiteSpace([string]$Image.DiskPath)) {
        $resolvedDiskPath = [string]$Image.DiskPath
    }

    $prefix = if ([string]::IsNullOrWhiteSpace($resolvedDiskPath)) {
        "La imagen SVFS2 tiene inconsistencias:"
    } else {
        "La imagen SVFS2 '$resolvedDiskPath' tiene inconsistencias:"
    }

    $details = ($issues | Select-Object -First 8 | ForEach-Object { "- $_" }) -join [Environment]::NewLine
    $suffix = if ($issues.Count -gt 8) {
        [Environment]::NewLine + ("- ... y {0} mas" -f ($issues.Count - 8))
    } else {
        ""
    }

    throw ($prefix + [Environment]::NewLine + $details + $suffix)
}

function Get-RelativeSvfsPath([string]$Path) {
    if (-not $Path.StartsWith("/disk")) {
        throw "La ruta destino debe vivir bajo /disk."
    }
    $relative = $Path.Substring(5).TrimStart("/")
    if ([string]::IsNullOrWhiteSpace($relative)) {
        throw "La ruta destino debe apuntar a un archivo o subdirectorio bajo /disk."
    }
    if ($relative.Length -gt 255) {
        throw "La ruta relativa '$relative' excede el limite de 255 caracteres de SVFS2."
    }
    return $relative
}

function Open-SvfsImage([string]$DiskPath) {
    if (-not (Test-Path $DiskPath)) {
        throw "No existe '$DiskPath'. Ejecuta primero '.\\build.ps1 build' para crear la imagen base."
    }

    $bytes = [System.IO.File]::ReadAllBytes($DiskPath)
    if ($bytes.Length -lt ($Script:SvfsSectorSize * $Script:Svfs2DataLba)) {
        throw "La imagen '$DiskPath' no parece un disco SVFS2 valido."
    }

    $primary = Test-Svfs2Superblock $bytes 0
    $secondary = Test-Svfs2Superblock $bytes $Script:SvfsSectorSize
    $superblock = if ($secondary -and (-not $primary -or $secondary.Sequence -gt $primary.Sequence)) { $secondary } else { $primary }
    if (-not $superblock) {
        throw "La imagen '$DiskPath' no contiene un superblock SVFS2 valido."
    }

    return [pscustomobject]@{
        Bytes = $bytes
        DiskPath = $DiskPath
        TotalSectors = $superblock.TotalSectors
        BlockBitmap = [byte[]]($bytes[($Script:Svfs2BlockBitmapLba * $Script:SvfsSectorSize)..(($Script:Svfs2BlockBitmapLba * $Script:SvfsSectorSize) + ($Script:Svfs2BlockBitmapSectors * $Script:SvfsSectorSize) - 1)])
        InodeBitmap = [byte[]]($bytes[($Script:Svfs2InodeBitmapLba * $Script:SvfsSectorSize)..(($Script:Svfs2InodeBitmapLba * $Script:SvfsSectorSize) + ($Script:Svfs2InodeBitmapSectors * $Script:SvfsSectorSize) - 1)])
    }
}

# --- Tool nativo libsvfs (svfs-cli) --------------------------------------------
# Toda la ESCRITURA de imagenes SVFS2 pasa por el tool nativo del core portable
# (libsvfs), no por PowerShell. Estas funciones lo compilan y lo manejan; las de
# arriba (Get-*/Read-*/Test-*/Open-*/Assert-*) son solo lectura/validacion.

# Compila el tool de host libsvfs (svfs-cli), con cache por mtime. Devuelve la
# ruta al ejecutable.
function Build-SvfsCli {
    $clang = Require-Executable "clang" (Get-ToolchainCandidates "clang")
    $toolsOut = Join-Path $Script:BuildRoot "tools"
    Ensure-Directory $toolsOut
    $exe = Join-Path $toolsOut ("svfs-cli" + $(if (Test-IsWindowsHost) { ".exe" } else { "" }))

    $sources = @(
        (Join-Path $Script:ProjectRoot "libsvfs/svfs_core.c"),
        (Join-Path $Script:ProjectRoot "libsvfs/svfs_cli.c")
    )
    $deps = $sources + @(
        (Join-Path $Script:ProjectRoot "libsvfs/svfs_core.h"),
        (Join-Path $Script:ProjectRoot "include/svfs/svfs_format.h")
    )

    $needsBuild = -not (Test-Path $exe)
    if (-not $needsBuild) {
        $exeTime = (Get-Item $exe).LastWriteTimeUtc
        foreach ($dep in $deps) {
            if ((Get-Item $dep).LastWriteTimeUtc -gt $exeTime) {
                $needsBuild = $true
                break
            }
        }
    }

    if ($needsBuild) {
        Write-Host "Compilando svfs-cli (tool de host libsvfs)..."
        $compileArgs = @(
            "-std=c11", "-O2", "-Wall", "-Wextra", "-D_CRT_SECURE_NO_WARNINGS",
            "-I", (Join-Path $Script:ProjectRoot "include"),
            "-I", (Join-Path $Script:ProjectRoot "libsvfs")
        ) + $sources + @("-o", $exe)
        & $clang @compileArgs
        if ($LASTEXITCODE -ne 0) {
            throw "Fallo la compilacion de svfs-cli."
        }
    }

    return $exe
}

# Genera el manifiesto TAB-separado que consume 'svfs-cli apply' recorriendo el
# arbol del host (el walk especifico del OS vive aca, no en el tool). bin y tmp
# primero, luego el arbol ordenado.
function New-SvfsManifest([string]$SourceRoot) {
    $manifestPath = Join-Path $Script:BuildRoot "diskfs-manifest.txt"
    $builder = [System.Text.StringBuilder]::new()
    [void]$builder.Append("mkdir`tbin`n")
    [void]$builder.Append("mkdir`ttmp`n")
    if (Test-Path $SourceRoot) {
        $items = Get-ChildItem -Path $SourceRoot -Recurse | Sort-Object FullName
        foreach ($item in $items) {
            $relative = $item.FullName.Substring($SourceRoot.Length).TrimStart('\', '/').Replace('\', '/')
            if ([string]::IsNullOrWhiteSpace($relative)) {
                continue
            }
            if ($item.PSIsContainer) {
                [void]$builder.Append("mkdir`t$relative`n")
            } else {
                [void]$builder.Append("file`t$relative`t$($item.FullName)`n")
            }
        }
    }
    [System.IO.File]::WriteAllText($manifestPath, $builder.ToString(), (New-Object System.Text.UTF8Encoding($false)))
    return $manifestPath
}

# Codigo de salida con el que svfs-cli reporta SVFS_ERR_NO_SPACE (el volumen
# tiene espacio libre pero no una corrida contigua del tamano pedido). Espeja
# SVFS_CLI_EXIT_NO_SPACE en libsvfs/svfs_cli.c; si se desincroniza, el unico
# efecto es perder el reintento automatico y volver a fallar el build duro.
$Script:SvfsCliExitNoSpace = 3

# Enumera el arbol de una imagen SVFS2 (solo lectura, con los helpers Get-*).
# Devuelve Dirs (rutas relativas a la raiz) y Files (ruta + inodo), en orden de
# recorrido por niveles: padres antes que hijos, listo para volcar a un
# manifiesto de svfs-cli.
function Get-Svfs2Inventory($Image) {
    $dirs = New-Object System.Collections.Generic.List[string]
    $files = New-Object System.Collections.Generic.List[object]
    $visited = @{}

    $pending = New-Object System.Collections.Generic.Queue[object]
    $pending.Enqueue([pscustomobject]@{ InodeId = [uint32]$Script:Svfs2RootInode; Prefix = "" })
    while ($pending.Count -gt 0) {
        $current = $pending.Dequeue()
        if ($visited.ContainsKey($current.InodeId)) {
            continue # imagen con inodos aliaseados: no reentrar
        }
        $visited[$current.InodeId] = $true

        foreach ($entry in Get-Svfs2DirEntries $Image ([uint32]$current.InodeId)) {
            if ($entry.InodeId -eq 0 -or [string]::IsNullOrWhiteSpace($entry.Name)) {
                continue
            }
            $relative = if ($current.Prefix) { "$($current.Prefix)/$($entry.Name)" } else { $entry.Name }
            if ($entry.Type -eq 2) {
                $dirs.Add($relative)
                $pending.Enqueue([pscustomobject]@{ InodeId = [uint32]$entry.InodeId; Prefix = $relative })
            } else {
                $files.Add([pscustomobject]@{ RelativePath = $relative; InodeId = [uint32]$entry.InodeId })
            }
        }
    }

    return [pscustomobject]@{ Dirs = $dirs.ToArray(); Files = $files.ToArray() }
}

# Rutas relativas de los archivos que un manifiesto de svfs-cli escribe.
function Get-SvfsManifestFilePaths([string]$ManifestPath) {
    $paths = @{}
    foreach ($line in [System.IO.File]::ReadAllLines($ManifestPath)) {
        if ([string]::IsNullOrWhiteSpace($line) -or $line.StartsWith("#")) {
            continue
        }
        $fields = $line.Split("`t")
        if ($fields.Count -ge 3 -and $fields[0] -eq "file") {
            $paths[$fields[1]] = $true
        }
    }
    return $paths
}

# Desfragmenta una imagen reconstruyendola desde cero: extrae lo que el
# manifiesto NO produce, formatea una imagen nueva y lo reinstala con el tool.
#
# Lo que el manifiesto no produce es exactamente lo que hay que preservar: son
# los datos que viven en el disco y no en el arbol de build (/disk/bin/doomgeneric,
# los WAD, lo que el usuario haya guardado). Los archivos del manifiesto no se
# copian porque el apply que sigue los reinstala desde el host, y son ademas los
# unicos cuyos sectores pudo haber pisado un apply fallido: svfs-cli solo flushea
# metadata si termina bien, y el allocator nunca elige sectores de un archivo
# ajeno al manifiesto porque el bitmap que carga del disco los tiene marcados.
#
# La imagen nueva se arma aparte y se mueve al final: si algo falla, la original
# queda intacta y no se pierden datos por un intento de compactacion. La copia
# extraida queda en build/svfs-carryover como red de seguridad (la sobrescribe la
# proxima compactacion, y 'build.ps1 clean' la borra).
function Invoke-SvfsCompaction([string]$Exe, [string]$ImagePath, [string]$ManifestPath) {
    $image = Open-SvfsImage $ImagePath
    $inventory = Get-Svfs2Inventory $image
    $produced = Get-SvfsManifestFilePaths $ManifestPath

    $carryRoot = Join-Path $Script:BuildRoot "svfs-carryover"
    if (Test-Path $carryRoot) {
        Remove-Item -Recurse -Force $carryRoot
    }
    Ensure-Directory $carryRoot

    $lines = New-Object System.Collections.Generic.List[string]
    foreach ($dir in $inventory.Dirs) {
        $lines.Add("mkdir`t$dir")
    }

    $preservedBytes = 0
    $preservedCount = 0
    foreach ($file in $inventory.Files) {
        if ($produced.ContainsKey($file.RelativePath)) {
            continue
        }
        $hostPath = Join-Path $carryRoot ($file.RelativePath -replace '/', '\')
        Ensure-Directory (Split-Path -Parent $hostPath)
        $bytes = Read-Svfs2InodeBytes $image (Get-Svfs2Inode $image $file.InodeId)
        [System.IO.File]::WriteAllBytes($hostPath, $bytes)
        $lines.Add("file`t$($file.RelativePath)`t$hostPath")
        $preservedCount += 1
        $preservedBytes += $bytes.Length
        Write-Host ("  se preserva /disk/{0} ({1:N0} bytes)" -f $file.RelativePath, $bytes.Length)
    }
    if ($preservedCount -eq 0) {
        Write-Host "  la imagen no tiene archivos ajenos a esta build que preservar."
    }

    $carryManifest = Join-Path $Script:BuildRoot "svfs-carryover-manifest.txt"
    [System.IO.File]::WriteAllText($carryManifest, (($lines -join "`n") + "`n"), (New-Object System.Text.UTF8Encoding($false)))

    $staging = "$ImagePath.compact"
    if (Test-Path $staging) {
        Remove-Item $staging -Force
    }
    & $Exe create $staging $image.TotalSectors
    if ($LASTEXITCODE -ne 0) {
        throw "svfs-cli create fallo compactando '$ImagePath'."
    }
    & $Exe apply $staging $carryManifest
    if ($LASTEXITCODE -ne 0) {
        throw "svfs-cli apply fallo reinstalando los datos preservados al compactar '$ImagePath'. La imagen original quedo intacta."
    }
    Move-Item $staging $ImagePath -Force

    Write-Host ("Imagen compactada: {0} archivo(s) preservado(s), {1:N0} bytes." -f $preservedCount, $preservedBytes)
}

# Aplica un manifiesto con svfs-cli. Si el allocator se queda sin corrida
# contigua, compacta la imagen y reintenta una vez en vez de abortar el build.
#
# La imagen se preserva entre builds para no perder los datos del usuario, y cada
# binario que crece mas de lo que da su extent se reubica, asi que el espacio
# libre se va fragmentando: eventualmente no hay corrida contigua aunque sobre
# espacio. El workaround era borrar build/disk.img a mano; esto lo hace solo y sin
# perder datos, porque la compactacion reinstala todo lo que el manifiesto no
# produce. La garantia de persistencia sigue verificandose despues del reintento
# (ver Get-SvfsPersistenceSnapshot / Assert-SvfsPersistenceRetained en build.ps1).
function Invoke-SvfsApply([string]$Exe, [string]$ImagePath, [string]$ManifestPath, [string]$FailureMessage) {
    & $Exe apply $ImagePath $ManifestPath
    if ($LASTEXITCODE -eq 0) {
        return
    }
    if ($LASTEXITCODE -ne $Script:SvfsCliExitNoSpace) {
        throw $FailureMessage
    }

    Write-Host "La imagen SVFS2 '$ImagePath' no tiene corrida contigua libre; compactandola y reintentando..."
    Invoke-SvfsCompaction -Exe $Exe -ImagePath $ImagePath -ManifestPath $ManifestPath

    & $Exe apply $ImagePath $ManifestPath
    if ($LASTEXITCODE -ne 0) {
        throw ($FailureMessage + " La imagen ya estaba compactada, asi que no es fragmentacion: al disco le falta espacio real.")
    }
}

# Instala archivos/directorios sueltos en una imagen SVFS2 existente via el tool
# (svfs-cli apply). $Operations es una lista de hashtables:
#   @{ Dir = "bin" }                                 -> mkdir bin
#   @{ File = "/disk/bin/foo"; Source = "C:\ruta" }  -> instala el archivo
# Las rutas ('Dir'/'File') aceptan prefijo /disk; se normalizan a relativas a la
# raiz de SVFS. Reemplaza el viejo Open/Ensure-SvfsDirectory/Install-SvfsFile/Save.
function Install-SvfsFilesWithTool([string]$DiskImagePath, [object[]]$Operations) {
    $exe = Build-SvfsCli
    $lines = New-Object System.Collections.Generic.List[string]
    foreach ($op in $Operations) {
        if ($op.ContainsKey("Dir")) {
            $rel = ($op.Dir -replace '^/disk/?', '').Trim('/')
            if ($rel) { $lines.Add("mkdir`t$rel") }
        } elseif ($op.ContainsKey("File")) {
            $rel = ($op.File -replace '^/disk/?', '').Trim('/')
            if (-not $rel) { throw "Install-SvfsFilesWithTool: ruta de archivo vacia." }
            $lines.Add("file`t$rel`t$($op.Source)")
        }
    }
    $manifest = Join-Path $Script:BuildRoot "svfs-install-manifest.txt"
    [System.IO.File]::WriteAllText($manifest, (($lines -join "`n") + "`n"), (New-Object System.Text.UTF8Encoding($false)))
    Invoke-SvfsApply -Exe $exe -ImagePath $DiskImagePath -ManifestPath $manifest `
        -FailureMessage "svfs-cli apply fallo instalando en '$DiskImagePath'."
}

