# Generates src/ExplorerExtras/assets/app.ico - a folder with an up chevron.
# Multi-resolution ICO (PNG-compressed entries) so the tray icon stays crisp at
# any DPI. Run only when the icon design changes; the .ico is committed.

Add-Type -AssemblyName System.Drawing

$sizes = 16, 20, 24, 32, 48, 64, 128, 256
$outDir = Join-Path $PSScriptRoot '..\src\ExplorerExtras\assets'
$null = New-Item -ItemType Directory -Force -Path $outDir
$outPath = Join-Path $outDir 'app.ico'

function New-IconBitmap([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    $u = $s / 32.0   # design grid is 32x32

    # Folder body
    $bodyRect = New-Object System.Drawing.RectangleF (3 * $u), (9 * $u), (26 * $u), (19 * $u)
    $tabRect = New-Object System.Drawing.RectangleF (3 * $u), (5 * $u), (12 * $u), (7 * $u)
    $folder = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.Point 0, 0),
        (New-Object System.Drawing.Point 0, $s),
        [System.Drawing.Color]::FromArgb(255, 250, 196, 84),
        [System.Drawing.Color]::FromArgb(255, 234, 152, 24))

    $r = [Math]::Max(1.0, 2.5 * $u)
    foreach ($rect in @($tabRect, $bodyRect)) {
        $path = New-Object System.Drawing.Drawing2D.GraphicsPath
        $d = $r * 2
        $path.AddArc($rect.X, $rect.Y, $d, $d, 180, 90)
        $path.AddArc($rect.Right - $d, $rect.Y, $d, $d, 270, 90)
        $path.AddArc($rect.Right - $d, $rect.Bottom - $d, $d, $d, 0, 90)
        $path.AddArc($rect.X, $rect.Bottom - $d, $d, $d, 90, 90)
        $path.CloseFigure()
        $g.FillPath($folder, $path)
        $path.Dispose()
    }

    # Up chevron
    $penWidth = [Math]::Max(1.5, 3.2 * $u)
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 255, 255, 255)), $penWidth
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $pts = @(
        (New-Object System.Drawing.PointF (10.5 * $u), (21 * $u)),
        (New-Object System.Drawing.PointF (16 * $u), (15 * $u)),
        (New-Object System.Drawing.PointF (21.5 * $u), (21 * $u))
    )
    $g.DrawLines($pen, [System.Drawing.PointF[]]$pts)
    $g.DrawLine($pen, (16 * $u), (15.5 * $u), (16 * $u), (24 * $u))

    $pen.Dispose(); $folder.Dispose(); $g.Dispose()
    return $bmp
}

# Render each size to PNG bytes
$pngs = @()
foreach ($s in $sizes) {
    $bmp = New-IconBitmap $s
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngs += , $ms.ToArray()
    $ms.Dispose(); $bmp.Dispose()
}

# Assemble ICONDIR + ICONDIRENTRY[] + image data
$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter($out)
$w.Write([UInt16]0)              # reserved
$w.Write([UInt16]1)              # type: icon
$w.Write([UInt16]$sizes.Count)

$offset = 6 + (16 * $sizes.Count)
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $w.Write([Byte]$(if ($s -ge 256) { 0 } else { $s }))
    $w.Write([Byte]$(if ($s -ge 256) { 0 } else { $s }))
    $w.Write([Byte]0)            # palette count
    $w.Write([Byte]0)            # reserved
    $w.Write([UInt16]1)          # colour planes
    $w.Write([UInt16]32)         # bits per pixel
    $w.Write([UInt32]$pngs[$i].Length)
    $w.Write([UInt32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($png in $pngs) { $w.Write($png) }
$w.Flush()

[System.IO.File]::WriteAllBytes($outPath, $out.ToArray())
$w.Dispose(); $out.Dispose()

Write-Output "wrote $outPath ($((Get-Item $outPath).Length) bytes, $($sizes.Count) sizes)"
