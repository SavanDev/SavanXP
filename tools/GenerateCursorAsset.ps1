param(
    [string]$ProjectRoot,
    [string]$OutputPath
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function New-Directory([string]$Path) {
    if (-not (Test-Path $Path)) {
        New-Item -ItemType Directory -Path $Path | Out-Null
    }
}

function Get-PngCursorPixels([string]$Path) {
    Add-Type -AssemblyName System.Drawing

    $bitmap = [System.Drawing.Bitmap]::new($Path)
    try {
        $width = $bitmap.Width
        $height = $bitmap.Height
        $pixels = New-Object 'System.UInt32[]' ($width * $height)

        for ($row = 0; $row -lt $height; ++$row) {
            for ($column = 0; $column -lt $width; ++$column) {
                $pixel = $bitmap.GetPixel($column, $row)
                $alpha = [uint32]$pixel.A
                $red = [uint32]$pixel.R
                $green = [uint32]$pixel.G
                $blue = [uint32]$pixel.B
                $pixels[($row * $width) + $column] = [uint32](($alpha -shl 24) -bor ($red -shl 16) -bor ($green -shl 8) -bor $blue)
            }
        }

        return @{
            Width = $width
            Height = $height
            Pixels = $pixels
        }
    } finally {
        $bitmap.Dispose()
    }
}

function Get-FallbackCursorPixels {
    $rows = @(
        "#",
        "##",
        "#.#",
        "#..#",
        "#...#",
        "#....#",
        "#.....#",
        "#......#",
        "#.......#",
        "#........#",
        "#.........#",
        "#....#######",
        "#..##...#",
        "###.#...#",
        "   #...#",
        "   #..#",
        "    ##",
        "    #"
    )

    $width = 13
    $height = $rows.Length
    $pixels = New-Object 'System.UInt32[]' ($width * $height)

    for ($row = 0; $row -lt $height; ++$row) {
        $text = $rows[$row]
        for ($column = 0; $column -lt $width; ++$column) {
            $character = if ($column -lt $text.Length) { $text[$column] } else { ' ' }
            $index = ($row * $width) + $column
            if ($character -eq '#') {
                $pixels[$index] = [uint32]4278190080
            } elseif ($character -eq '.') {
                $pixels[$index] = [uint32]4294967295
            } else {
                $pixels[$index] = [uint32]0
            }
        }
    }

    return @{
        Width = $width
        Height = $height
        Pixels = $pixels
    }
}

function Write-CursorPixelArray([System.Text.StringBuilder]$Builder, [string]$SymbolName, [hashtable]$Cursor) {
    [void]$Builder.AppendLine(("static const unsigned int {0}[{1}] = {{" -f $SymbolName, ($Cursor.Width * $Cursor.Height)))

    for ($row = 0; $row -lt $Cursor.Height; ++$row) {
        $values = @()
        for ($column = 0; $column -lt $Cursor.Width; ++$column) {
            $index = ($row * $Cursor.Width) + $column
            $values += ("0x{0:X8}u" -f $Cursor.Pixels[$index])
        }
        $suffix = if ($row -eq ($Cursor.Height - 1)) { "" } else { "," }
        [void]$Builder.AppendLine(("    {0}{1}" -f ($values -join ", "), $suffix))
    }

    [void]$Builder.AppendLine("};")
    [void]$Builder.AppendLine("")
}

function Write-CursorHeader([string]$Path, [array]$Entries) {
    $builder = New-Object System.Text.StringBuilder

    [void]$builder.AppendLine("#ifndef SAVANXP_CURSOR_ASSET_H")
    [void]$builder.AppendLine("#define SAVANXP_CURSOR_ASSET_H")
    [void]$builder.AppendLine("")

    foreach ($entry in $Entries) {
        Write-CursorPixelArray -Builder $builder -SymbolName "$($entry.SymbolName)_pixels" -Cursor $entry.Cursor
    }

    [void]$builder.AppendLine("struct desktop_cursor_asset {")
    [void]$builder.AppendLine("    int width;")
    [void]$builder.AppendLine("    int height;")
    [void]$builder.AppendLine("    int hotspot_x;")
    [void]$builder.AppendLine("    int hotspot_y;")
    [void]$builder.AppendLine("    const unsigned int *pixels;")
    [void]$builder.AppendLine("};")
    [void]$builder.AppendLine("")

    [void]$builder.AppendLine("static const struct desktop_cursor_asset k_desktop_cursor_assets[SAVANXP_CURSOR_SHAPE_COUNT] = {")
    foreach ($entry in $Entries) {
        [void]$builder.AppendLine(("    [{0}] = {{ {1}, {2}, {3}, {4}, {5}_pixels }}," -f `
            $entry.Shape, $entry.Cursor.Width, $entry.Cursor.Height, $entry.HotspotX, $entry.HotspotY, $entry.SymbolName))
    }
    [void]$builder.AppendLine("};")
    [void]$builder.AppendLine("")

    [void]$builder.AppendLine("#endif")

    [System.IO.File]::WriteAllText($Path, $builder.ToString(), [System.Text.Encoding]::ASCII)
}

if (-not $ProjectRoot) {
    $ProjectRoot = Split-Path -Parent $PSScriptRoot
}

$assetRoot = Join-Path $ProjectRoot "assets/desktop/cursors"

# Order matches enum savanxp_cursor_shape (subsystems/posix/sdk/v1/include/savanxp/syscall.h).
# Hotspots are picked by hand per shape -- PNGs carry no hotspot metadata.
$manifest = @(
    @{ Shape = "SAVANXP_CURSOR_ARROW"; SymbolName = "k_desktop_cursor_arrow"; File = "arrow.png"; HotspotX = 3; HotspotY = 1 },
    @{ Shape = "SAVANXP_CURSOR_WAIT"; SymbolName = "k_desktop_cursor_wait"; File = "wait.png"; HotspotX = 9; HotspotY = 9 },
    @{ Shape = "SAVANXP_CURSOR_TEXT"; SymbolName = "k_desktop_cursor_text"; File = "text.png"; HotspotX = 3; HotspotY = 8 },
    @{ Shape = "SAVANXP_CURSOR_MOVE"; SymbolName = "k_desktop_cursor_move"; File = "move.png"; HotspotX = 9; HotspotY = 9 },
    @{ Shape = "SAVANXP_CURSOR_RESIZE_H"; SymbolName = "k_desktop_cursor_resize_h"; File = "resize_h.png"; HotspotX = 9; HotspotY = 5 },
    @{ Shape = "SAVANXP_CURSOR_RESIZE_V"; SymbolName = "k_desktop_cursor_resize_v"; File = "resize_v.png"; HotspotX = 5; HotspotY = 9 },
    @{ Shape = "SAVANXP_CURSOR_UNAVAILABLE"; SymbolName = "k_desktop_cursor_unavailable"; File = "unavailable.png"; HotspotX = 9; HotspotY = 9 },
    @{ Shape = "SAVANXP_CURSOR_LINK"; SymbolName = "k_desktop_cursor_link"; File = "link.png"; HotspotX = 5; HotspotY = 2 }
)

$outputDirectory = Split-Path -Parent $OutputPath
New-Directory $outputDirectory

$entries = @()
foreach ($item in $manifest) {
    $sourcePath = Join-Path $assetRoot $item.File
    if (Test-Path $sourcePath) {
        $cursor = Get-PngCursorPixels -Path $sourcePath
    } elseif ($item.Shape -eq "SAVANXP_CURSOR_ARROW") {
        $cursor = Get-FallbackCursorPixels
    } else {
        throw "No se encontro el asset de cursor requerido: $sourcePath"
    }

    $entries += @{
        Shape = $item.Shape
        SymbolName = $item.SymbolName
        HotspotX = $item.HotspotX
        HotspotY = $item.HotspotY
        Cursor = $cursor
    }
}

Write-CursorHeader -Path $OutputPath -Entries $entries
