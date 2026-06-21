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

function Get-PngBitmapPixels([string]$Path) {
    Add-Type -AssemblyName System.Drawing

    if (-not (Test-Path $Path)) {
        throw "No se encontro el asset bitmap requerido: $Path"
    }

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

function Write-EmbeddedBitmap([System.Text.StringBuilder]$Builder, [string]$SymbolName, [hashtable]$Bitmap) {
    [void]$Builder.AppendLine(("static const uint32_t {0}_pixels[{1}] = {{" -f $SymbolName, ($Bitmap.Width * $Bitmap.Height)))

    for ($row = 0; $row -lt $Bitmap.Height; ++$row) {
        $values = @()
        for ($column = 0; $column -lt $Bitmap.Width; ++$column) {
            $index = ($row * $Bitmap.Width) + $column
            $values += ("0x{0:X8}u" -f $Bitmap.Pixels[$index])
        }
        $suffix = if ($row -eq ($Bitmap.Height - 1)) { "" } else { "," }
        [void]$Builder.AppendLine(("    {0}{1}" -f ($values -join ", "), $suffix))
    }

    [void]$Builder.AppendLine("};")
    [void]$Builder.AppendLine(("static const struct savanxp_embedded_bitmap_asset {0} = {{ {1}u, {2}u, {0}_pixels }};" -f $SymbolName, $Bitmap.Width, $Bitmap.Height))
    [void]$Builder.AppendLine("")
}

function Write-DesktopIconHeader([string]$Path, [array]$Entries) {
    $builder = New-Object System.Text.StringBuilder

    [void]$builder.AppendLine("#ifndef SAVANXP_DESKTOP_ICON_ASSETS_H")
    [void]$builder.AppendLine("#define SAVANXP_DESKTOP_ICON_ASSETS_H")
    [void]$builder.AppendLine("")
    [void]$builder.AppendLine("#include <stdint.h>")
    [void]$builder.AppendLine("")
    [void]$builder.AppendLine("struct savanxp_embedded_bitmap_asset {")
    [void]$builder.AppendLine("    uint32_t width;")
    [void]$builder.AppendLine("    uint32_t height;")
    [void]$builder.AppendLine("    const uint32_t* pixels;")
    [void]$builder.AppendLine("};")
    [void]$builder.AppendLine("")

    foreach ($entry in $Entries) {
        Write-EmbeddedBitmap -Builder $builder -SymbolName $entry.SymbolName -Bitmap $entry.Bitmap
    }

    [void]$builder.AppendLine("#endif")

    [System.IO.File]::WriteAllText($Path, $builder.ToString(), [System.Text.Encoding]::ASCII)
}

if (-not $ProjectRoot) {
    $ProjectRoot = Split-Path -Parent $PSScriptRoot
}

$outputDirectory = Split-Path -Parent $OutputPath
New-Directory $outputDirectory

$assetRoot = Join-Path $ProjectRoot "assets\\desktop\\icons"
$manifest = @(
    @{ SymbolName = "k_desktop_icon_desktop_16"; RelativePath = "16x16\\desktop.png" },
    @{ SymbolName = "k_desktop_icon_shell_16"; RelativePath = "16x16\\app-terminal.png" },
    @{ SymbolName = "k_desktop_icon_doom_16"; RelativePath = "16x16\\app-spider.png" },
    @{ SymbolName = "k_desktop_icon_gfx_demo_16"; RelativePath = "16x16\\app-libgfx-demo.png" },
    @{ SymbolName = "k_desktop_icon_key_test_16"; RelativePath = "16x16\\app-keyboard-settings.png" },
    @{ SymbolName = "k_desktop_icon_mouse_test_16"; RelativePath = "16x16\\app-mouse.png" },
    @{ SymbolName = "k_desktop_icon_desktop_32"; RelativePath = "32x32\\desktop.png" },
    @{ SymbolName = "k_desktop_icon_shell_32"; RelativePath = "32x32\\app-terminal.png" },
    @{ SymbolName = "k_desktop_icon_doom_32"; RelativePath = "32x32\\app-spider.png" },
    @{ SymbolName = "k_desktop_icon_gfx_demo_32"; RelativePath = "32x32\\app-libgfx-demo.png" },
    @{ SymbolName = "k_desktop_icon_key_test_32"; RelativePath = "32x32\\app-keyboard-settings.png" },
    @{ SymbolName = "k_desktop_icon_mouse_test_32"; RelativePath = "32x32\\app-mouse.png" },
    @{ SymbolName = "k_desktop_menu_strip_placeholder"; RelativePath = "..\\menu_strip_savanxp.png" }
)

$entries = @()
foreach ($item in $manifest) {
    $bitmap = Get-PngBitmapPixels -Path (Join-Path $assetRoot $item.RelativePath)
    $entries += @{
        SymbolName = $item.SymbolName
        Bitmap = $bitmap
    }
}

Write-DesktopIconHeader -Path $OutputPath -Entries $entries
