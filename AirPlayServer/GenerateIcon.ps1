param(
    [string]$OutputPath = (Join-Path $PSScriptRoot 'AirPlayServer.ico'),
    [string]$PreviewPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing

function New-RoundedRectanglePath {
    param(
        [System.Drawing.RectangleF]$Rectangle,
        [float]$Radius
    )

    $diameter = $Radius * 2.0
    $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $path.AddArc($Rectangle.X, $Rectangle.Y, $diameter, $diameter, 180.0, 90.0)
    $path.AddArc($Rectangle.Right - $diameter, $Rectangle.Y, $diameter, $diameter, 270.0, 90.0)
    $path.AddArc($Rectangle.Right - $diameter, $Rectangle.Bottom - $diameter, $diameter, $diameter, 0.0, 90.0)
    $path.AddArc($Rectangle.X, $Rectangle.Bottom - $diameter, $diameter, $diameter, 90.0, 90.0)
    $path.CloseFigure()
    return $path
}

function New-IconFrame {
    param([int]$Size)

    $supersampling = 4
    $renderSize = $Size * $supersampling
    $bitmap = [System.Drawing.Bitmap]::new(
        $renderSize,
        $renderSize,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)

    try {
        $graphics.Clear([System.Drawing.Color]::Transparent)
        $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::GammaCorrected
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality

        [float]$r = $renderSize
        # A generous squircle silhouette reads more like a current Windows 11
        # app icon than the old near-square tile, especially on the taskbar.
        [float]$margin = $r * 0.070
        $tileRect = [System.Drawing.RectangleF]::new(
            $margin,
            $margin,
            $r - 2.0 * $margin,
            $r - 2.0 * $margin)
        $tilePath = New-RoundedRectanglePath $tileRect ($r * 0.285)
        $tileBrush = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
            $tileRect,
            [System.Drawing.Color]::FromArgb(255, 25, 54, 82),
            [System.Drawing.Color]::FromArgb(255, 9, 19, 31),
            55.0)
        $tileBorder = [System.Drawing.Pen]::new(
            [System.Drawing.Color]::FromArgb(255, 62, 112, 158),
            [Math]::Max(1.0, $r * 0.014))

        try {
            $graphics.FillPath($tileBrush, $tilePath)
            $graphics.DrawPath($tileBorder, $tilePath)
        }
        finally {
            $tileBorder.Dispose()
            $tileBrush.Dispose()
            $tilePath.Dispose()
        }

        $accent = [System.Drawing.Color]::FromArgb(255, 105, 178, 239)
        $screenRect = [System.Drawing.RectangleF]::new(
            $r * 0.205,
            $r * 0.235,
            $r * 0.590,
            $r * 0.365)
        $screenPath = New-RoundedRectanglePath $screenRect ($r * 0.085)
        $screenPen = [System.Drawing.Pen]::new($accent, [Math]::Max(2.0, $r * 0.052))
        $screenPen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round

        try {
            $graphics.DrawPath($screenPen, $screenPath)
        }
        finally {
            $screenPen.Dispose()
            $screenPath.Dispose()
        }

        # Match the lower tile color so the AirPlay cutout stays seamless over
        # the gradient while preserving the familiar receiver silhouette.
        $tileColor = [System.Drawing.Color]::FromArgb(255, 12, 27, 43)
        [System.Drawing.PointF[]]$cutout = @(
            [System.Drawing.PointF]::new($r * 0.500, $r * 0.410),
            [System.Drawing.PointF]::new($r * 0.750, $r * 0.795),
            [System.Drawing.PointF]::new($r * 0.250, $r * 0.795)
        )
        [System.Drawing.PointF[]]$receiver = @(
            [System.Drawing.PointF]::new($r * 0.500, $r * 0.455),
            [System.Drawing.PointF]::new($r * 0.695, $r * 0.750),
            [System.Drawing.PointF]::new($r * 0.305, $r * 0.750)
        )
        $cutoutBrush = [System.Drawing.SolidBrush]::new($tileColor)
        $accentBrush = [System.Drawing.SolidBrush]::new($accent)

        try {
            $graphics.FillPolygon($cutoutBrush, $cutout)
            $graphics.FillPolygon($accentBrush, $receiver)
        }
        finally {
            $accentBrush.Dispose()
            $cutoutBrush.Dispose()
        }
    }
    finally {
        $graphics.Dispose()
    }

    $frame = [System.Drawing.Bitmap]::new(
        $Size,
        $Size,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $resize = [System.Drawing.Graphics]::FromImage($frame)
    try {
        $resize.Clear([System.Drawing.Color]::Transparent)
        $resize.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
        $resize.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $resize.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $resize.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $resize.DrawImage(
            $bitmap,
            [System.Drawing.Rectangle]::new(0, 0, $Size, $Size),
            0,
            0,
            $renderSize,
            $renderSize,
            [System.Drawing.GraphicsUnit]::Pixel)
    }
    finally {
        $resize.Dispose()
        $bitmap.Dispose()
    }

    try {
        if ($PreviewPath -and $Size -eq 256) {
            $previewDirectory = Split-Path -Parent $PreviewPath
            if ($previewDirectory) {
                [System.IO.Directory]::CreateDirectory($previewDirectory) | Out-Null
            }
            $frame.Save($PreviewPath, [System.Drawing.Imaging.ImageFormat]::Png)
        }

        $stream = [System.IO.MemoryStream]::new()
        try {
            $frame.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
            return ,$stream.ToArray()
        }
        finally {
            $stream.Dispose()
        }
    }
    finally {
        $frame.Dispose()
    }
}

$sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
$frames = foreach ($size in $sizes) {
    [byte[]]$bytes = New-IconFrame $size
    [PSCustomObject]@{
        Size = $size
        Bytes = $bytes
    }
}

$outputDirectory = Split-Path -Parent $OutputPath
if ($outputDirectory) {
    [System.IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
}

$iconStream = [System.IO.MemoryStream]::new()
$writer = [System.IO.BinaryWriter]::new($iconStream)
try {
    $writer.Write([UInt16]0)
    $writer.Write([UInt16]1)
    $writer.Write([UInt16]$frames.Count)

    [UInt32]$offset = 6 + 16 * $frames.Count
    foreach ($frame in $frames) {
        $dimension = if ($frame.Size -eq 256) { 0 } else { $frame.Size }
        $writer.Write([byte]$dimension)
        $writer.Write([byte]$dimension)
        $writer.Write([byte]0)
        $writer.Write([byte]0)
        $writer.Write([UInt16]1)
        $writer.Write([UInt16]32)
        $writer.Write([UInt32]$frame.Bytes.Length)
        $writer.Write($offset)
        $offset += [UInt32]$frame.Bytes.Length
    }

    foreach ($frame in $frames) {
        $writer.Write([byte[]]$frame.Bytes)
    }
    $writer.Flush()
    [System.IO.File]::WriteAllBytes($OutputPath, $iconStream.ToArray())
}
finally {
    $writer.Dispose()
    $iconStream.Dispose()
}

Get-Item -LiteralPath $OutputPath
