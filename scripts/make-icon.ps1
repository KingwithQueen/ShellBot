param(
    [string]$SourcePath,
    [string]$DestinationPath
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
if(!$SourcePath){$SourcePath=Join-Path $root 'assets\branding\shellbot-v1\shellbot-icon.png'}
if(!$DestinationPath){$DestinationPath=Join-Path $root 'assets\branding\shellbot-v1\shellbot.ico'}
$source=[IO.Path]::GetFullPath($SourcePath);$destination=[IO.Path]::GetFullPath($DestinationPath)
if($source -eq $destination -or [IO.Path]::GetExtension($destination) -ine '.ico'){throw 'Use a separate ICO destination.'}
Add-Type -AssemblyName System.Drawing
$sourceHash=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
$sizes=@(16,20,24,32,40,48,64,96,128,256)
$image=[Drawing.Image]::FromFile($source)
$frames=@()
try{
    if($image.RawFormat.Guid -ne [Drawing.Imaging.ImageFormat]::Png.Guid){throw 'PNG source required.'}
    foreach($size in $sizes){
        $bitmap=[Drawing.Bitmap]::new($size,$size,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $graphics=[Drawing.Graphics]::FromImage($bitmap)
        $attributes=[Drawing.Imaging.ImageAttributes]::new()
        $stream=[IO.MemoryStream]::new()
        try{
            $graphics.Clear([Drawing.Color]::Transparent)
            $graphics.CompositingMode=[Drawing.Drawing2D.CompositingMode]::SourceCopy
            $graphics.CompositingQuality=[Drawing.Drawing2D.CompositingQuality]::HighQuality
            $graphics.InterpolationMode=[Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.PixelOffsetMode=[Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $attributes.SetWrapMode([Drawing.Drawing2D.WrapMode]::TileFlipXY)
            $scale=[Math]::Min($size/[double]$image.Width,$size/[double]$image.Height)
            $width=[Math]::Max(1,[int][Math]::Round($image.Width*$scale))
            $height=[Math]::Max(1,[int][Math]::Round($image.Height*$scale))
            $rectangle=[Drawing.Rectangle]::new([int](($size-$width)/2),[int](($size-$height)/2),$width,$height)
            $graphics.DrawImage($image,$rectangle,0,0,$image.Width,$image.Height,[Drawing.GraphicsUnit]::Pixel,$attributes)
            $bitmap.Save($stream,[Drawing.Imaging.ImageFormat]::Png)
            $frames+=@{size=$size;bytes=$stream.ToArray()}
        }finally{$stream.Dispose();$attributes.Dispose();$graphics.Dispose();$bitmap.Dispose()}
    }
}finally{$image.Dispose()}
$temporary=$destination+'.tmp-'+[Guid]::NewGuid().ToString('N')
$output=[IO.File]::Open($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write)
$writer=[IO.BinaryWriter]::new($output)
try{
    $writer.Write([uint16]0);$writer.Write([uint16]1);$writer.Write([uint16]$frames.Count)
    $offset=6+16*$frames.Count
    foreach($frame in $frames){
        $dimension=if($frame.size -eq 256){0}else{$frame.size}
        $writer.Write([byte]$dimension);$writer.Write([byte]$dimension);$writer.Write([byte]0);$writer.Write([byte]0)
        $writer.Write([uint16]1);$writer.Write([uint16]32);$writer.Write([uint32]$frame.bytes.Length);$writer.Write([uint32]$offset)
        $offset+=$frame.bytes.Length
    }
    foreach($frame in $frames){$writer.Write([byte[]]$frame.bytes)}
}finally{$writer.Dispose()}
if((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() -cne $sourceHash){throw 'Source changed during conversion.'}
Move-Item -LiteralPath $temporary -Destination $destination -Force
[pscustomobject]@{source=$source;source_sha256=$sourceHash;ico=$destination;ico_sha256=(Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant();sizes=$sizes;encoding='32-bit RGBA PNG frames';source_unchanged=$true}
