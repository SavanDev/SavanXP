param(
    [string]$ProjectRoot
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

Add-Type -AssemblyName System.Drawing

function New-Directory([string]$Path) {
    if (-not (Test-Path $Path)) {
        New-Item -ItemType Directory -Path $Path | Out-Null
    }
}

function New-Color([int]$R, [int]$G, [int]$B, [int]$A = 255) {
    return [System.Drawing.Color]::FromArgb($A, $R, $G, $B)
}

function New-Canvas([int]$Width, [int]$Height) {
    $bitmap = [System.Drawing.Bitmap]::new($Width, $Height)
    $bitmap.MakeTransparent()
    return $bitmap
}

function Set-PixelSafe([System.Drawing.Bitmap]$Bitmap, [int]$X, [int]$Y, [System.Drawing.Color]$Color) {
    if ($X -ge 0 -and $Y -ge 0 -and $X -lt $Bitmap.Width -and $Y -lt $Bitmap.Height) {
        $Bitmap.SetPixel($X, $Y, $Color)
    }
}

function Fill-Rect([System.Drawing.Bitmap]$Bitmap, [int]$X, [int]$Y, [int]$Width, [int]$Height, [System.Drawing.Color]$Color) {
    for ($row = 0; $row -lt $Height; ++$row) {
        for ($column = 0; $column -lt $Width; ++$column) {
            Set-PixelSafe $Bitmap ($X + $column) ($Y + $row) $Color
        }
    }
}

function Stroke-Rect([System.Drawing.Bitmap]$Bitmap, [int]$X, [int]$Y, [int]$Width, [int]$Height, [System.Drawing.Color]$Color) {
    for ($column = 0; $column -lt $Width; ++$column) {
        Set-PixelSafe $Bitmap ($X + $column) $Y $Color
        Set-PixelSafe $Bitmap ($X + $column) ($Y + $Height - 1) $Color
    }
    for ($row = 0; $row -lt $Height; ++$row) {
        Set-PixelSafe $Bitmap $X ($Y + $row) $Color
        Set-PixelSafe $Bitmap ($X + $Width - 1) ($Y + $row) $Color
    }
}

function Scale-BitmapNearest([System.Drawing.Bitmap]$Bitmap, [int]$Factor) {
    $scaled = [System.Drawing.Bitmap]::new($Bitmap.Width * $Factor, $Bitmap.Height * $Factor)
    for ($row = 0; $row -lt $Bitmap.Height; ++$row) {
        for ($column = 0; $column -lt $Bitmap.Width; ++$column) {
            $color = $Bitmap.GetPixel($column, $row)
            for ($dy = 0; $dy -lt $Factor; ++$dy) {
                for ($dx = 0; $dx -lt $Factor; ++$dx) {
                    $scaled.SetPixel(($column * $Factor) + $dx, ($row * $Factor) + $dy, $color)
                }
            }
        }
    }
    return $scaled
}

function Save-Png([System.Drawing.Bitmap]$Bitmap, [string]$Path) {
    $directory = Split-Path -Parent $Path
    New-Directory $directory
    $Bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
}

function New-DesktopIcon16 {
    $ink = New-Color 41 53 60
    $screen = New-Color 19 139 145
    $screenGlow = New-Color 79 216 220
    $gold = New-Color 232 189 78
    $base = New-Color 198 212 220
    $shadow = New-Color 113 128 139
    $bmp = New-Canvas 16 16

    Stroke-Rect $bmp 1 2 14 10 $ink
    Fill-Rect $bmp 2 3 12 8 $screen
    Fill-Rect $bmp 3 4 10 1 $screenGlow
    Fill-Rect $bmp 5 12 6 1 $base
    Fill-Rect $bmp 6 13 4 1 $shadow
    Fill-Rect $bmp 4 14 8 1 $base
    Fill-Rect $bmp 4 5 2 2 $gold
    Fill-Rect $bmp 7 5 3 2 $gold
    Fill-Rect $bmp 11 5 1 4 $gold
    Fill-Rect $bmp 4 8 6 1 $gold
    return $bmp
}

function New-TerminalIcon16 {
    $frame = New-Color 195 201 206
    $header = New-Color 27 114 106
    $screen = New-Color 17 22 29
    $green = New-Color 104 229 148
    $white = New-Color 229 240 236
    $bmp = New-Canvas 16 16

    Fill-Rect $bmp 1 2 14 12 $frame
    Fill-Rect $bmp 2 3 12 2 $header
    Fill-Rect $bmp 2 5 12 8 $screen
    Set-PixelSafe $bmp 3 4 $white
    Set-PixelSafe $bmp 4 4 $white
    Fill-Rect $bmp 4 7 1 3 $green
    Set-PixelSafe $bmp 5 8 $green
    Set-PixelSafe $bmp 6 9 $green
    Fill-Rect $bmp 7 10 4 1 $white
    return $bmp
}

function New-DoomIcon16 {
    $outline = New-Color 68 28 26
    $skin = New-Color 188 58 52
    $highlight = New-Color 235 111 91
    $horn = New-Color 241 215 172
    $eye = New-Color 242 247 244
    $mouth = New-Color 33 13 14
    $bmp = New-Canvas 16 16

    Fill-Rect $bmp 4 1 2 2 $horn
    Fill-Rect $bmp 10 1 2 2 $horn
    Fill-Rect $bmp 3 3 10 9 $skin
    Stroke-Rect $bmp 3 3 10 9 $outline
    Fill-Rect $bmp 4 4 8 2 $highlight
    Fill-Rect $bmp 5 6 2 2 $eye
    Fill-Rect $bmp 9 6 2 2 $eye
    Fill-Rect $bmp 6 9 4 1 $mouth
    Set-PixelSafe $bmp 5 10 $mouth
    Set-PixelSafe $bmp 10 10 $mouth
    Fill-Rect $bmp 6 11 4 1 $mouth
    return $bmp
}

function New-GfxDemoIcon16 {
    $frame = New-Color 48 58 74
    $panel = New-Color 27 32 40
    $cyan = New-Color 48 214 217
    $magenta = New-Color 220 72 201
    $gold = New-Color 236 181 63
    $green = New-Color 90 220 114
    $bmp = New-Canvas 16 16

    Fill-Rect $bmp 1 2 14 12 $panel
    Stroke-Rect $bmp 1 2 14 12 $frame
    for ($offset = 0; $offset -lt 6; ++$offset) {
        Set-PixelSafe $bmp (3 + $offset) (10 - $offset) $cyan
        Set-PixelSafe $bmp (4 + $offset) (10 - $offset) $cyan
        Set-PixelSafe $bmp (5 + $offset) (4 + $offset) $magenta
        Set-PixelSafe $bmp (6 + $offset) (4 + $offset) $magenta
    }
    Fill-Rect $bmp 10 8 3 3 $gold
    Fill-Rect $bmp 3 11 10 1 $green
    return $bmp
}

function New-KeyboardIcon16 {
    $body = New-Color 188 195 203
    $outline = New-Color 74 86 96
    $key = New-Color 244 247 250
    $accent = New-Color 101 197 128
    $shadow = New-Color 131 145 156
    $bmp = New-Canvas 16 16

    Fill-Rect $bmp 1 4 14 8 $body
    Stroke-Rect $bmp 1 4 14 8 $outline
    Fill-Rect $bmp 2 5 12 1 $accent
    for ($row = 0; $row -lt 3; ++$row) {
        for ($column = 0; $column -lt 5; ++$column) {
            Fill-Rect $bmp (2 + ($column * 2)) (7 + ($row * 1)) 1 1 $key
        }
    }
    Fill-Rect $bmp 4 10 8 1 $shadow
    return $bmp
}

function New-MouseIcon16 {
    $outline = New-Color 99 109 119
    $shell = New-Color 239 243 245
    $shadow = New-Color 207 214 219
    $wheel = New-Color 61 181 176
    $bmp = New-Canvas 16 16

    Fill-Rect $bmp 4 2 8 11 $shell
    Stroke-Rect $bmp 4 2 8 11 $outline
    Fill-Rect $bmp 5 3 6 8 $shell
    Fill-Rect $bmp 7 4 2 3 $wheel
    Fill-Rect $bmp 7 7 2 1 $outline
    Fill-Rect $bmp 5 11 6 1 $shadow
    Set-PixelSafe $bmp 5 13 $outline
    Set-PixelSafe $bmp 10 13 $outline
    return $bmp
}

function New-MenuStripArt {
    $width = 64
    $height = 360
    $bitmap = [System.Drawing.Bitmap]::new($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)

    try {
        $graphics.Clear((New-Color 10 84 61))

        for ($row = 0; $row -lt $height; ++$row) {
            $ratio = [double]$row / [double]($height - 1)
            $red = [int](10 + (18 * $ratio))
            $green = [int](84 + (38 * $ratio))
            $blue = [int](61 + (24 * $ratio))
            $pen = [System.Drawing.Pen]::new((New-Color $red $green $blue))
            try {
                $graphics.DrawLine($pen, 0, $row, $width - 1, $row)
            } finally {
                $pen.Dispose()
            }
        }

        $graphics.FillRectangle(([System.Drawing.SolidBrush]::new((New-Color 5 67 48))), 0, 0, 8, $height)
        $graphics.FillRectangle(([System.Drawing.SolidBrush]::new((New-Color 13 104 77))), 8, 0, $width - 8, $height)
        $graphics.FillRectangle(([System.Drawing.SolidBrush]::new((New-Color 229 192 90))), 8, 286, 48, 4)

        $accentBrush = [System.Drawing.SolidBrush]::new((New-Color 164 243 211 88))
        try {
            $points = [System.Drawing.Point[]]@(
                [System.Drawing.Point]::new(6, 34),
                [System.Drawing.Point]::new(56, 86),
                [System.Drawing.Point]::new(56, 108),
                [System.Drawing.Point]::new(18, 78)
            )
            $graphics.FillPolygon($accentBrush, $points)
        } finally {
            $accentBrush.Dispose()
        }

        $shineBrush = [System.Drawing.SolidBrush]::new((New-Color 230 249 237 55))
        try {
            $points = [System.Drawing.Point[]]@(
                [System.Drawing.Point]::new(18, 0),
                [System.Drawing.Point]::new(54, 0),
                [System.Drawing.Point]::new(64, 42),
                [System.Drawing.Point]::new(28, 42)
            )
            $graphics.FillPolygon($shineBrush, $points)
        } finally {
            $shineBrush.Dispose()
        }

        $icon = New-DesktopIcon16
        try {
            $bigIcon = Scale-BitmapNearest $icon 2
            try {
                $graphics.DrawImage($bigIcon, 10, 18, 32, 32)
            } finally {
                $bigIcon.Dispose()
            }
        } finally {
            $icon.Dispose()
        }

        $whitePen = [System.Drawing.Pen]::new((New-Color 230 246 239))
        $mintPen = [System.Drawing.Pen]::new((New-Color 156 230 198))
        $tealBrush = [System.Drawing.SolidBrush]::new((New-Color 188 244 236))
        try {
            foreach ($y in @(68, 80, 92, 138, 170, 202, 234, 266, 318, 328, 338)) {
                $graphics.DrawLine($whitePen, 12, $y, 38, $y)
            }
            foreach ($y in @(118, 150, 182, 214, 246)) {
                $graphics.FillEllipse($tealBrush, 12, $y, 6, 6)
            }
            $graphics.DrawLine($mintPen, 12, 118, 52, 118)
        } finally {
            $whitePen.Dispose()
            $mintPen.Dispose()
            $tealBrush.Dispose()
        }
    } finally {
        $graphics.Dispose()
    }

    return $bitmap
}

function Write-IconSet([string]$BasePath, [string]$Name, [scriptblock]$Factory) {
    $icon16 = & $Factory
    try {
        $icon32 = Scale-BitmapNearest $icon16 2
        try {
            Save-Png $icon16 (Join-Path $BasePath "16x16\\$Name")
            Save-Png $icon32 (Join-Path $BasePath "32x32\\$Name")
        } finally {
            $icon32.Dispose()
        }
    } finally {
        $icon16.Dispose()
    }
}

if (-not $ProjectRoot) {
    $ProjectRoot = Split-Path -Parent $PSScriptRoot
}

$assetRoot = Join-Path $ProjectRoot "assets\\desktop\\icons"
New-Directory (Join-Path $assetRoot "16x16")
New-Directory (Join-Path $assetRoot "32x32")

Write-IconSet -BasePath $assetRoot -Name "desktop.png" -Factory ${function:New-DesktopIcon16}
Write-IconSet -BasePath $assetRoot -Name "app-terminal.png" -Factory ${function:New-TerminalIcon16}
Write-IconSet -BasePath $assetRoot -Name "app-spider.png" -Factory ${function:New-DoomIcon16}
Write-IconSet -BasePath $assetRoot -Name "app-libgfx-demo.png" -Factory ${function:New-GfxDemoIcon16}
Write-IconSet -BasePath $assetRoot -Name "app-keyboard-settings.png" -Factory ${function:New-KeyboardIcon16}
Write-IconSet -BasePath $assetRoot -Name "app-mouse.png" -Factory ${function:New-MouseIcon16}

$menuStrip = New-MenuStripArt
try {
    Save-Png $menuStrip (Join-Path $ProjectRoot "assets\\desktop\\menu_strip_savanxp.png")
} finally {
    $menuStrip.Dispose()
}
