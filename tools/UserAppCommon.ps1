Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Script:ProjectRoot = Split-Path -Parent $PSScriptRoot
$Script:BuildRoot = Join-Path $Script:ProjectRoot "build"
$Script:SdkBuildRoot = Join-Path $Script:BuildRoot "sdk"
$Script:SdkRoot = Join-Path $Script:ProjectRoot "sdk\\v1"
$Script:DiskImage = Join-Path $Script:BuildRoot "disk.img"
$Script:SvfsSectorSize = 512
$Script:SvfsEntrySize = 64

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
        "-I", (Join-Path $Script:SdkRoot "include")
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

function Build-ExternalUserProgram([string]$SourcePath, [string]$ProgramName, [string]$OutputPath) {
    $compiler = Require-Executable "clang" @("clang", "C:\\Program Files\\LLVM\\bin\\clang.exe")
    $linker = Require-Executable "ld.lld" @("ld.lld", "C:\\Program Files\\LLVM\\bin\\ld.lld.exe")
    $sourceSpec = Get-ExternalSourceSpec $SourcePath
    $compileFlags = Get-ExternalCompileFlags $sourceSpec.IncludeDirs
    $outputFull = [System.IO.Path]::GetFullPath($OutputPath)
    $objectRoot = Join-Path $Script:SdkBuildRoot $ProgramName
    Ensure-Directory $objectRoot
    Ensure-Directory (Split-Path -Parent $outputFull)

    $libcObject = Join-Path $objectRoot "libc.o"
    $posixObject = Join-Path $objectRoot "posix.o"
    $gfxObject = Join-Path $objectRoot "gfx.o"
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

    & $compiler -c -x c (Join-Path $Script:SdkRoot "runtime\\libc.c") -o $libcObject @compileFlags
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo la compilacion del runtime libc."
    }

    & $compiler -c -x c (Join-Path $Script:SdkRoot "runtime\\posix.c") -o $posixObject @compileFlags
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo la compilacion del runtime posix."
    }

    & $compiler -c -x c (Join-Path $Script:SdkRoot "runtime\\gfx.c") -o $gfxObject @compileFlags
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo la compilacion del runtime gfx."
    }

    & $compiler -c (Join-Path $Script:SdkRoot "runtime\\setjmp.S") -o $setjmpObject @compileFlags
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo la compilacion del runtime setjmp."
    }

    & $compiler -c (Join-Path $Script:SdkRoot "runtime\\crt0.S") -o $crtObject @compileFlags
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo la compilacion de crt0."
    }

    & $linker -nostdlib -static -T (Join-Path $Script:SdkRoot "linker.ld") -o $outputFull $crtObject $libcObject $posixObject $gfxObject $setjmpObject @appObjects
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo el link de '$SourcePath'."
    }

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

function Set-Svfs2Superblock([byte[]]$Buffer, [int]$Offset, [uint32]$Sequence, [uint32]$Flags, [uint32]$TotalSectors) {
    for ($i = 0; $i -lt $Script:SvfsSectorSize; $i += 1) {
        $Buffer[$Offset + $i] = 0
    }
    Set-AsciiField $Buffer $Offset $Script:Svfs2Magic 8
    Set-UInt32Le $Buffer ($Offset + 8) 2
    Set-UInt32Le $Buffer ($Offset + 16) $Sequence
    Set-UInt32Le $Buffer ($Offset + 20) $Flags
    Set-UInt32Le $Buffer ($Offset + 24) $TotalSectors
    Set-UInt32Le $Buffer ($Offset + 28) $Script:Svfs2JournalLba
    Set-UInt32Le $Buffer ($Offset + 32) $Script:Svfs2JournalSectors
    Set-UInt32Le $Buffer ($Offset + 36) $Script:Svfs2BlockBitmapLba
    Set-UInt32Le $Buffer ($Offset + 40) $Script:Svfs2BlockBitmapSectors
    Set-UInt32Le $Buffer ($Offset + 44) $Script:Svfs2InodeBitmapLba
    Set-UInt32Le $Buffer ($Offset + 48) $Script:Svfs2InodeBitmapSectors
    Set-UInt32Le $Buffer ($Offset + 52) $Script:Svfs2InodeTableLba
    Set-UInt32Le $Buffer ($Offset + 56) $Script:Svfs2InodeTableSectors
    Set-UInt32Le $Buffer ($Offset + 60) $Script:Svfs2DataLba
    Set-UInt32Le $Buffer ($Offset + 64) $Script:Svfs2MaxInodes
    Set-UInt32Le $Buffer ($Offset + 68) $Script:Svfs2RootInode
    Set-UInt32Le $Buffer ($Offset + 12) (Get-Svfs2Checksum $Buffer $Offset $Script:SvfsSectorSize ($Offset + 12))
}

function Get-Svfs2BitmapBit([byte[]]$Buffer, [uint32]$Bit) {
    $byteIndex = [int]($Bit / 8)
    $shift = [int]($Bit % 8)
    return ($Buffer[$byteIndex] -band (1 -shl $shift)) -ne 0
}

function Set-Svfs2BitmapBit([byte[]]$Buffer, [uint32]$Bit, [bool]$Value) {
    $byteIndex = [int]($Bit / 8)
    $shift = [int]($Bit % 8)
    $mask = [byte](1 -shl $shift)
    if ($Value) {
        $Buffer[$byteIndex] = [byte]($Buffer[$byteIndex] -bor $mask)
    } else {
        $Buffer[$byteIndex] = [byte]($Buffer[$byteIndex] -band ([byte](0xff -bxor $mask)))
    }
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

function Set-Svfs2Inode($Image, [uint32]$InodeId, [uint16]$Type, [uint32]$Size, [object[]]$Extents) {
    $offset = Get-Svfs2InodeOffset $Image $InodeId
    for ($i = 0; $i -lt $Script:Svfs2InodeSize; $i += 1) {
        $Image.Bytes[$offset + $i] = 0
    }
    Set-UInt32Le $Image.Bytes $offset $InodeId
    Set-UInt16Le $Image.Bytes ($offset + 4) $Type
    Set-UInt32Le $Image.Bytes ($offset + 8) $Size
    Set-UInt32Le $Image.Bytes ($offset + 12) 1
    Set-UInt32Le $Image.Bytes ($offset + 16) ([uint32]$Extents.Count)
    for ($index = 0; $index -lt $Extents.Count -and $index -lt $Script:Svfs2MaxExtents; $index += 1) {
        $extentOffset = $offset + 20 + ($index * 8)
        Set-UInt32Le $Image.Bytes $extentOffset ([uint32]$Extents[$index].StartLba)
        Set-UInt32Le $Image.Bytes ($extentOffset + 4) ([uint32]$Extents[$index].SectorCount)
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

function Write-Svfs2InodeBytes($Image, [uint32]$InodeId, [byte[]]$Data) {
    $inode = Get-Svfs2Inode $Image $InodeId
    $capacity = Get-Svfs2InodeCapacityBytes $inode
    if ($Data.Length -gt $capacity) {
        throw "SVFS2: capacidad insuficiente para inode $InodeId."
    }
    $written = 0
    foreach ($extent in $inode.Extents | Select-Object -First $inode.ExtentCount) {
        $extentOffset = $extent.StartLba * $Script:SvfsSectorSize
        $extentLength = $extent.SectorCount * $Script:SvfsSectorSize
        for ($i = 0; $i -lt $extentLength; $i += 1) {
            $Image.Bytes[$extentOffset + $i] = 0
        }
        if ($written -lt $Data.Length) {
            $copyLength = [Math]::Min($Data.Length - $written, $extentLength)
            [Array]::Copy($Data, $written, $Image.Bytes, $extentOffset, $copyLength)
            $written += $copyLength
        }
    }
    Set-Svfs2Inode $Image $InodeId $inode.Type ([uint32]$Data.Length) ($inode.Extents | Select-Object -First $inode.ExtentCount)
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

function Set-Svfs2DirEntries($Image, [uint32]$InodeId, [object[]]$Entries) {
    $bytes = New-Object byte[] ($Entries.Count * $Script:Svfs2DirEntrySize)
    for ($index = 0; $index -lt $Entries.Count; $index += 1) {
        $offset = $index * $Script:Svfs2DirEntrySize
        Set-UInt32Le $bytes $offset ([uint32]$Entries[$index].InodeId)
        Set-UInt16Le $bytes ($offset + 4) ([uint16]$Entries[$index].Type)
        Set-UInt16Le $bytes ($offset + 6) ([uint16]$Entries[$index].NameLength)
        if ($Entries[$index].NameLength -gt 0) {
            Set-AsciiField $bytes ($offset + 8) $Entries[$index].Name 64
        }
    }
    Write-Svfs2InodeBytes $Image $InodeId $bytes
}

function Find-Svfs2FreeRun($Image, [uint32]$RequiredSectors) {
    $runStart = 0
    $runLength = 0
    $bitmapOffset = $Script:Svfs2BlockBitmapLba * $Script:SvfsSectorSize
    for ($sector = $Script:Svfs2DataLba; $sector -lt $Image.TotalSectors; $sector += 1) {
        if (-not (Get-Svfs2BitmapBit $Image.BlockBitmap $sector)) {
            if ($runLength -eq 0) {
                $runStart = $sector
            }
            $runLength += 1
            if ($runLength -ge $RequiredSectors) {
                return $runStart
            }
        } else {
            $runLength = 0
        }
    }
    return 0
}

function Set-Svfs2ExtentBits($Image, $Extents, [bool]$Value) {
    foreach ($extent in $Extents) {
        for ($sector = 0; $sector -lt $extent.SectorCount; $sector += 1) {
            Set-Svfs2BitmapBit $Image.BlockBitmap ([uint32]($extent.StartLba + $sector)) $Value
        }
    }
}

function Ensure-Svfs2Capacity($Image, [uint32]$InodeId, [uint32]$RequiredSize) {
    $inode = Get-Svfs2Inode $Image $InodeId
    if ($RequiredSize -le (Get-Svfs2InodeCapacityBytes $inode)) {
        return
    }

    $requiredSectors = [uint32][Math]::Ceiling($RequiredSize / [double]$Script:SvfsSectorSize)
    $start = Find-Svfs2FreeRun $Image $requiredSectors
    if ($start -eq 0) {
        throw "SVFS2: no hay espacio contiguo suficiente para inode $InodeId."
    }

    $existing = Read-Svfs2InodeBytes $Image $inode
    Set-Svfs2ExtentBits $Image ($inode.Extents | Select-Object -First $inode.ExtentCount) $false
    $newExtent = [pscustomobject]@{ StartLba = $start; SectorCount = $requiredSectors }
    Set-Svfs2ExtentBits $Image @($newExtent) $true
    Set-Svfs2Inode $Image $InodeId $inode.Type $inode.Size @($newExtent)
    Write-Svfs2InodeBytes $Image $InodeId $existing
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

function Repair-Svfs2ReachableMetadata($Image) {
    $queue = New-Object System.Collections.Generic.Queue[object]
    $visited = @{}
    $fixedInodeBits = 0
    $fixedBlockBits = 0

    $queue.Enqueue([pscustomobject]@{
        InodeId = $Script:Svfs2RootInode
    })

    while ($queue.Count -gt 0) {
        $current = $queue.Dequeue()
        if ($visited.ContainsKey($current.InodeId)) {
            continue
        }
        $visited[$current.InodeId] = $true

        try {
            $inode = Get-Svfs2Inode $Image ([uint32]$current.InodeId)
        } catch {
            continue
        }

        $inodeBit = [uint32]($current.InodeId - 1)
        if (-not (Get-Svfs2BitmapBit $Image.InodeBitmap $inodeBit)) {
            Set-Svfs2BitmapBit $Image.InodeBitmap $inodeBit $true
            $fixedInodeBits += 1
        }

        foreach ($extent in $inode.Extents | Select-Object -First $inode.ExtentCount) {
            if ($extent.StartLba -eq 0 -or $extent.SectorCount -eq 0) {
                continue
            }

            for ($sector = 0; $sector -lt $extent.SectorCount; $sector += 1) {
                $absoluteSector = [uint32]($extent.StartLba + $sector)
                if ($absoluteSector -ge $Image.TotalSectors) {
                    break
                }
                if (-not (Get-Svfs2BitmapBit $Image.BlockBitmap $absoluteSector)) {
                    Set-Svfs2BitmapBit $Image.BlockBitmap $absoluteSector $true
                    $fixedBlockBits += 1
                }
            }
        }

        if ($inode.Type -ne 2) {
            continue
        }

        foreach ($entry in Get-Svfs2DirEntries $Image ([uint32]$current.InodeId)) {
            if ($entry.InodeId -eq 0 -or $entry.InodeId -gt $Script:Svfs2MaxInodes) {
                continue
            }

            $queue.Enqueue([pscustomobject]@{
                InodeId = [uint32]$entry.InodeId
            })
        }
    }

    return [pscustomobject]@{
        InodeBitsFixed = $fixedInodeBits
        BlockBitsFixed = $fixedBlockBits
        ReachableInodes = $visited.Count
    }
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

function Get-Svfs2NextFreeInode($Image) {
    for ($inodeId = 1; $inodeId -le $Script:Svfs2MaxInodes; $inodeId += 1) {
        if (-not (Get-Svfs2BitmapBit $Image.InodeBitmap ([uint32]($inodeId - 1)))) {
            Set-Svfs2BitmapBit $Image.InodeBitmap ([uint32]($inodeId - 1)) $true
            return [uint32]$inodeId
        }
    }
    throw "SVFS2: no hay mas inodos libres."
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

function Split-SvfsRelativePath([string]$RelativePath) {
    $lastSlash = $RelativePath.LastIndexOf('/')
    if ($lastSlash -lt 0) {
        return [pscustomobject]@{
            Parent = ""
            Leaf = $RelativePath
        }
    }

    return [pscustomobject]@{
        Parent = $RelativePath.Substring(0, $lastSlash)
        Leaf = $RelativePath.Substring($lastSlash + 1)
    }
}

function Ensure-SvfsDirectory($Image, [string]$RelativePath) {
    if ([string]::IsNullOrWhiteSpace($RelativePath)) {
        return
    }

    $current = ""
    foreach ($component in $RelativePath.Split('/')) {
        if ($component.Length -gt 63) {
            throw "El componente '$component' excede el limite de 63 caracteres."
        }
        $current = if ($current) { "$current/$component" } else { $component }
        $existing = Find-Svfs2Path $Image $current
        if ($existing) {
            if ($existing.Type -ne 2) {
                throw "'$current' ya existe y no es un directorio en SVFS2."
            }
            continue
        }

        $parent = Find-Svfs2Path $Image (Split-SvfsRelativePath $current).Parent
        $entries = @(Get-Svfs2DirEntries $Image $parent.InodeId)
        $inodeId = Get-Svfs2NextFreeInode $Image
        $entries += [pscustomobject]@{ InodeId = $inodeId; Type = 2; NameLength = $component.Length; Name = $component }
        Set-Svfs2Inode $Image $inodeId 2 0 @()
        Ensure-Svfs2Capacity $Image $parent.InodeId ([uint32](($entries.Count) * $Script:Svfs2DirEntrySize))
        Set-Svfs2DirEntries $Image $parent.InodeId $entries
    }
}

function Install-SvfsFile($Image, [string]$DestinationPath, [byte[]]$Data) {
    $relative = Get-RelativeSvfsPath $DestinationPath
    $split = Split-SvfsRelativePath $relative
    $leaf = $split.Leaf
    if ($leaf.Length -gt 63) {
        throw "El nombre '$leaf' excede el limite de 63 caracteres de SVFS2."
    }
    $parentRelative = $split.Parent
    Ensure-SvfsDirectory $Image $parentRelative
    $parent = Find-Svfs2Path $Image $parentRelative
    $existing = Find-Svfs2Path $Image $relative

    if ($existing -and $existing.Type -eq 2) {
        throw "'$DestinationPath' ya existe y no es un archivo."
    }

    if (-not $existing) {
        $inodeId = Get-Svfs2NextFreeInode $Image
        $entries = @(Get-Svfs2DirEntries $Image $parent.InodeId)
        $entries += [pscustomobject]@{ InodeId = $inodeId; Type = 1; NameLength = $leaf.Length; Name = $leaf }
        Set-Svfs2Inode $Image $inodeId 1 0 @()
        Ensure-Svfs2Capacity $Image $parent.InodeId ([uint32](($entries.Count) * $Script:Svfs2DirEntrySize))
        Set-Svfs2DirEntries $Image $parent.InodeId $entries
        $existing = Find-Svfs2Path $Image $relative
    }

    Ensure-Svfs2Capacity $Image $existing.InodeId ([uint32]$Data.Length)
    Write-Svfs2InodeBytes $Image $existing.InodeId $Data
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

function Save-SvfsImage($Image) {
    [Array]::Copy($Image.BlockBitmap, 0, $Image.Bytes, $Script:Svfs2BlockBitmapLba * $Script:SvfsSectorSize, $Image.BlockBitmap.Length)
    [Array]::Copy($Image.InodeBitmap, 0, $Image.Bytes, $Script:Svfs2InodeBitmapLba * $Script:SvfsSectorSize, $Image.InodeBitmap.Length)
    Set-Svfs2Superblock $Image.Bytes 0 1 1 $Image.TotalSectors
    Set-Svfs2Superblock $Image.Bytes $Script:SvfsSectorSize 1 1 $Image.TotalSectors
    [System.IO.File]::WriteAllBytes($Image.DiskPath, $Image.Bytes)
}

function Initialize-Svfs2DiskImage([string]$SourceRoot, [string]$OutputPath) {
    $recreate = $true
    if (Test-Path $OutputPath) {
        try {
            $existing = Open-SvfsImage $OutputPath
            $recreate = $false
        } catch {
            Remove-Item $OutputPath -Force
        }
    }
    if (-not $recreate) {
        return
    }

    Ensure-Directory (Split-Path -Parent $OutputPath)
    $bytes = New-Object byte[] ($Script:Svfs2TotalSectors * $Script:SvfsSectorSize)
    $image = [pscustomobject]@{
        Bytes = $bytes
        DiskPath = $OutputPath
        TotalSectors = $Script:Svfs2TotalSectors
        BlockBitmap = (New-Object byte[] ($Script:Svfs2BlockBitmapSectors * $Script:SvfsSectorSize))
        InodeBitmap = (New-Object byte[] ($Script:Svfs2InodeBitmapSectors * $Script:SvfsSectorSize))
    }

    for ($sector = 0; $sector -lt $Script:Svfs2DataLba; $sector += 1) {
        Set-Svfs2BitmapBit $image.BlockBitmap ([uint32]$sector) $true
    }
    Set-Svfs2BitmapBit $image.InodeBitmap 0 $true
    Set-Svfs2Inode $image $Script:Svfs2RootInode 2 0 @()
    Ensure-SvfsDirectory $image "bin"
    Ensure-SvfsDirectory $image "tmp"

    if (Test-Path $SourceRoot) {
        $items = Get-ChildItem -Path $SourceRoot -Recurse | Sort-Object FullName
        foreach ($item in $items) {
            $relative = $item.FullName.Substring($SourceRoot.Length).TrimStart('\').Replace('\', '/')
            if ([string]::IsNullOrWhiteSpace($relative)) {
                continue
            }
            if ($item.PSIsContainer) {
                Ensure-SvfsDirectory $image $relative
            } else {
                Install-SvfsFile $image ("/disk/$relative") ([System.IO.File]::ReadAllBytes($item.FullName))
            }
        }
    }

    Save-SvfsImage $image
}
