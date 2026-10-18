<#
.SYNOPSIS
  Generate the video fixtures used by the video source tests.

.DESCRIPTION
  The clips are generated rather than checked in (binary media in git is
  permanent history weight). ShaderLabTests counts a skip for each test that
  needs an absent clip (a failure under --require-fixtures), so CI -- which
  has no ffmpeg -- is unaffected.

  The exception is the video_index_* clips (under 100 KB each), which are
  checked in; this script regenerates them. Tests\fixtures\manifest.json
  lists every fixture and pins each checked-in clip's SHA-256, which this
  script prints: after regenerating one on purpose, copy its hash there.

  The two seek clips are 1 s at 30 fps with a SINGLE keyframe, which is the point:
  seeking to frame 17 must decode forward from frame 0, exercising the
  frame-accurate seek, and the stream is hardware-decodable so the zero-copy
  path runs.

    video_hdr10_hevc_30f.mp4  3840x2160 HEVC Main10 (P010 decode), BT.2020/PQ
                              tagged. 4K also makes the decode+upload timing
                              the test prints meaningful.
    video_sdr_h264_30f.mp4    320x180 H.264 High 4:2:0 with B-frames (NV12
                              decode). 180 is not a multiple of 16, so the
                              decoder pads the surface to 192 rows -- which is
                              what caught the chroma-plane offset bug.

  Do NOT use lossless x264: it produces High 4:4:4, which the Windows H.264
  decoder cannot decode (ReadSample reports end-of-stream on the first read).

.PARAMETER Ffmpeg
  ffmpeg.exe or its directory. Default: PATH, then the winget Gyan.FFmpeg
  install location.

.EXAMPLE
  pwsh -File Tests\fixtures\MakeVideoFixtures.ps1
#>
param([string]$Ffmpeg)
$ErrorActionPreference = 'Stop'

if ($Ffmpeg) {
    $ff = if (Test-Path $Ffmpeg -PathType Container) { Join-Path $Ffmpeg 'ffmpeg.exe' } else { $Ffmpeg }
} else {
    $cmd = Get-Command ffmpeg -ErrorAction SilentlyContinue
    $ff = if ($cmd) { $cmd.Source } else {
        (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Gyan.FFmpeg*\*\bin\ffmpeg.exe" -ErrorAction SilentlyContinue |
            Select-Object -First 1).FullName
    }
}
if (-not $ff -or -not (Test-Path $ff)) {
    throw "ffmpeg not found. Pass -Ffmpeg <path> or install it: winget install Gyan.FFmpeg"
}

$out = $PSScriptRoot
& $ff -hide_banner -loglevel error -y `
    -f lavfi -i "testsrc2=size=3840x2160:rate=30:duration=1" `
    -c:v libx265 -pix_fmt yuv420p10le -tag:v hvc1 `
    -x265-params "keyint=300:min-keyint=300:scenecut=0:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:log-level=error" `
    -crf 22 (Join-Path $out 'video_hdr10_hevc_30f.mp4')
if ($LASTEXITCODE) { throw "ffmpeg failed (HEVC fixture)" }

& $ff -hide_banner -loglevel error -y `
    -f lavfi -i "testsrc2=size=320x180:rate=30:duration=1" `
    -c:v libx264 -profile:v high -pix_fmt yuv420p -g 300 -keyint_min 300 -sc_threshold 0 -bf 2 -crf 20 `
    (Join-Path $out 'video_sdr_h264_30f.mp4')
if ($LASTEXITCODE) { throw "ffmpeg failed (H.264 fixture)" }

# Frame-index clips for TestVideoPlaybackRate (checked in; regenerate only if
# the encoding changes). Ten vertical bars carry the frame index in binary,
# bar b white when bit b is set, in the top half, and the complement in the
# bottom half so a reader can tell a decoded bit from compression noise.
$bars = "lum='16+219*if(lt(Y,H/2),mod(trunc(N/pow(2,trunc(X*10/W))),2),1-mod(trunc(N/pow(2,trunc(X*10/W))),2))':cb=128:cr=128"
# The _g600 clip has a single keyframe in its 10 s, for the seek-cost tests.
foreach ($clip in @(@{ Size = '320x180'; Rate = 60 }, @{ Size = '320x180'; Rate = 30 },
                    @{ Size = '320x180'; Rate = 24 }, @{ Size = '1280x720'; Rate = 60 },
                    @{ Size = '1280x720'; Rate = 60; Gop = 600 })) {
    $height = $clip.Size.Split('x')[1]
    $gop = if ($clip.Gop) { $clip.Gop } else { $clip.Rate * 2 }
    $suffix = if ($clip.Gop) { "_g$($clip.Gop)" } else { '' }
    $keyframeArgs = if ($clip.Gop) { @('-keyint_min', $gop, '-sc_threshold', 0) } else { @() }
    $name = "video_index_h264_$($height)p$($clip.Rate)$suffix.mp4"
    & $ff -hide_banner -loglevel error -y `
        -f lavfi -i "color=c=black:size=$($clip.Size):rate=$($clip.Rate):duration=10" `
        -vf "format=yuv420p,geq=$bars" `
        -c:v libx264 -profile:v high -pix_fmt yuv420p -g $gop @keyframeArgs -bf 2 -crf 23 `
        -color_primaries bt709 -color_trc bt709 -colorspace bt709 (Join-Path $out $name)
    if ($LASTEXITCODE) { throw "ffmpeg failed ($name)" }
}

Get-ChildItem $out -Filter 'video_*.mp4' |
    Select-Object Name, Length, @{ Name = 'SHA256'; Expression = { (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() } }
