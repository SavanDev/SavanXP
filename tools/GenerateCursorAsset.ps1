param(
    [string]$SourcePath,
    [string]$OutputPath
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function New-Directory([string]$Path) {
    if (-not (Test-Path $Path)) {
        New-Item -ItemType Directory -Path $Path | Out-Null
    }
}

function Write-CursorHeader([string]$Path, [int]$Width, [int]$Height, [int]$HotspotX, [int]$HotspotY, [uint32[]]$Pixels) {
    $builder = New-Object System.Text.StringBuilder

    [void]$builder.AppendLine("#ifndef SAVANXP_CURSOR_ASSET_H")
    [void]$builder.AppendLine("#define SAVANXP_CURSOR_ASSET_H")
    [void]$builder.AppendLine("")
    [void]$builder.AppendLine("#define DESKTOP_CURSOR_WIDTH $Width")
    [void]$builder.AppendLine("#define DESKTOP_CURSOR_HEIGHT $Height")
    [void]$builder.AppendLine("#define DESKTOP_CURSOR_HOTSPOT_X $HotspotX")
    [void]$builder.AppendLine("#define DESKTOP_CURSOR_HOTSPOT_Y $HotspotY")
    [void]$builder.AppendLine("")
    [void]$builder.AppendLine("static const unsigned int k_desktop_cursor_pixels[DESKTOP_CURSOR_WIDTH * DESKTOP_CURSOR_HEIGHT] = {")

    for ($row = 0; $row -lt $Height; ++$row) {
        $values = @()
        for ($column = 0; $column -lt $Width; ++$column) {
            $index = ($row * $Width) + $column
            $values += ("0x{0:X8}u" -f $Pixels[$index])
        }
        $suffix = if ($row -eq ($Height - 1)) { "" } else { "," }
        [void]$builder.AppendLine(("    {0}{1}" -f ($values -join ", "), $suffix))
    }

    [void]$builder.AppendLine("};")
    [void]$builder.AppendLine("")
    [void]$builder.AppendLine("#endif")

    [System.IO.File]::WriteAllText($Path, $builder.ToString(), [System.Text.Encoding]::ASCII)
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
        HotspotX = 0
        HotspotY = 0
        Pixels = $pixels
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
            HotspotX = 0
            HotspotY = 0
            Pixels = $pixels
        }
    } finally {
        $bitmap.Dispose()
    }
}

$outputDirectory = Split-Path -Parent $OutputPath
New-Directory $outputDirectory

if ($SourcePath -and (Test-Path $SourcePath)) {
    $cursor = Get-PngCursorPixels -Path $SourcePath
} else {
    $cursor = Get-FallbackCursorPixels
}

Write-CursorHeader -Path $OutputPath -Width $cursor.Width -Height $cursor.Height -HotspotX $cursor.HotspotX -HotspotY $cursor.HotspotY -Pixels $cursor.Pixels
