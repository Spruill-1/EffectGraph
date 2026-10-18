<#
.SYNOPSIS
    Builds EffectGraph for x64 and ARM64, packs EffectGraph.Native, the C#
    EffectGraph package and EffectGraph.Engine (the static engine library and
    its headers, which ShaderLab builds on) into packaging\out, and runs the C#
    smoke test against the packed packages.

.PARAMETER Version
    Package version; defaults to EFFECTGRAPH_VERSION_STRING in
    Engine/Api/EffectGraphVersion.h.

.PARAMETER PlatformToolset
    The MSVC toolset to build with, e.g. v143. A static library links only
    with its own toolset or a newer one, so the published EffectGraph.Engine
    is built with the oldest supported (v143, VS 2022). Defaults to the
    projects' choice for the installed Visual Studio.

.PARAMETER SkipBuild
    Pack what is already built (Release, both platforms, and the engine's
    package builds).
#>
param(
    [string]$Version,
    [string]$PlatformToolset,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$out = Join-Path $PSScriptRoot 'out'
$stage = Join-Path $PSScriptRoot 'obj\EffectGraph.Native'

if (-not $Version) {
    $Version = (Select-String -Path (Join-Path $root 'Engine\Api\EffectGraphVersion.h') `
        -Pattern 'EFFECTGRAPH_VERSION_STRING\s+"([^"]+)"').Matches[0].Groups[1].Value
}
Write-Host "EffectGraph $Version"
$toolsetArgs = @(if ($PlatformToolset) { "/p:PlatformToolset=$PlatformToolset" })

# ---- Build: the DLL (which generates the built-in headers and blobs) and the tool.
if (-not $SkipBuild) {
    $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * `
            -requires Microsoft.Component.MSBuild -property installationPath
    # The native MSBuild for this machine (see docs/development/build.md).
    $msb = if ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'Arm64') {
        "$vs\MSBuild\Current\Bin\arm64\MSBuild.exe" } else { "$vs\MSBuild\Current\Bin\amd64\MSBuild.exe" }
    foreach ($platform in 'x64', 'ARM64') {
        foreach ($project in 'EffectGraph.vcxproj', 'EffectGraphCompile.vcxproj') {
            & $msb (Join-Path $root $project) /p:Configuration=Release /p:Platform=$platform @toolsetArgs /m /v:m /nologo
            if ($LASTEXITCODE -ne 0) { throw "MSBuild failed: $project $platform" }
        }
        # The engine library for EffectGraph.Engine: both configurations, built
        # without /GL into EffectGraphEngine.Package\ (EffectGraphEngine.vcxproj).
        foreach ($configuration in 'Debug', 'Release') {
            & $msb (Join-Path $root 'EffectGraphEngine.vcxproj') /p:Configuration=$configuration /p:Platform=$platform `
                /p:EffectGraphPackageBuild=true @toolsetArgs /m /v:m /nologo
            if ($LASTEXITCODE -ne 0) { throw "MSBuild failed: EffectGraphEngine $platform $configuration" }
        }
    }
}

# ---- Stage EffectGraph.Native.
if (Test-Path $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
$native = Join-Path $PSScriptRoot 'EffectGraph.Native'
function Place([string]$from, [string]$to) {
    $dir = Join-Path $stage $to
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $files = @(Get-ChildItem -Path $from -File)
    if ($files.Count -eq 0) { throw "nothing at $from" }
    $files | Copy-Item -Destination $dir
}
Place (Join-Path $native 'EffectGraph.Native.nuspec') '.'
Place (Join-Path $native 'README.md') '.'
Place (Join-Path $native 'build\native\EffectGraph.Native.targets') 'build\native'
Place (Join-Path $root 'build\EffectGraph.targets') 'build\native'
Place (Join-Path $root 'ARM64\Release\EffectGraph\include\*.h') 'build\native\include'
Place (Join-Path $root 'ARM64\Release\EffectGraphBuiltIns\*.effectblob') 'effects'
foreach ($p in @(@{ Platform = 'x64'; Rid = 'win-x64'; Tool = 'x64' }, @{ Platform = 'ARM64'; Rid = 'win-arm64'; Tool = 'arm64' })) {
    $bin = Join-Path $root "$($p.Platform)\Release"
    Place "$bin\EffectGraph\EffectGraph.lib" "build\native\lib\$($p.Platform)"
    Place "$bin\EffectGraph\EffectGraph.dll" "runtimes\$($p.Rid)\native"
    Place "$bin\EffectGraphCompile\EffectGraphCompile.exe" "tools\$($p.Tool)"
}

New-Item -ItemType Directory -Force -Path $out | Out-Null
Get-ChildItem $out -Filter "EffectGraph*.$Version.nupkg" | Remove-Item -Force
& nuget pack (Join-Path $stage 'EffectGraph.Native.nuspec') -Version $Version -OutputDirectory $out -NoDefaultExcludes -NonInteractive
if ($LASTEXITCODE -ne 0) { throw 'nuget pack failed' }

# ---- Stage EffectGraph.Engine: every engine header, pch_engine.h, the
# generated built-in headers, and the library per platform and configuration.
$engineStage = Join-Path $PSScriptRoot 'obj\EffectGraph.Engine'
if (Test-Path $engineStage) { Remove-Item -LiteralPath $engineStage -Recurse -Force }
$engine = Join-Path $PSScriptRoot 'EffectGraph.Engine'
$stage = $engineStage
Place (Join-Path $engine 'EffectGraph.Engine.nuspec') '.'
Place (Join-Path $engine 'README.md') '.'
Place (Join-Path $engine 'build\native\EffectGraph.Engine.targets') 'build\native'
$include = Join-Path $engineStage 'build\native\include'
$headers = 0
foreach ($dir in 'Graph', 'Rendering', 'Effects', 'Engine') {
    foreach ($file in Get-ChildItem (Join-Path $root $dir) -Recurse -File -Filter '*.h') {
        $relative = [IO.Path]::GetRelativePath($root, $file.FullName).Replace('\', '/')
        $target = Join-Path $include ([IO.Path]::GetDirectoryName($relative))
        New-Item -ItemType Directory -Force -Path $target | Out-Null
        Copy-Item $file.FullName $target
        $headers++
    }
}
Copy-Item (Join-Path $root 'pch_engine.h') $include
# Built-in headers by bare name; the public headers are already under
# Engine\Api, which the targets put on the include path.
Get-ChildItem (Join-Path $root 'ARM64\Release\EffectGraph\include') -Filter '*.h' |
    Where-Object { $_.Name -notin 'EffectGraph.h', 'EffectGraphPlugin.h', 'EffectGraphVersion.h' } | Copy-Item -Destination $include
foreach ($platform in 'x64', 'ARM64') {
    foreach ($configuration in 'Debug', 'Release') {
        Place (Join-Path $root "$platform\$configuration\EffectGraphEngine.Package\EffectGraphEngine.lib") "build\native\lib\$platform\$configuration"
    }
}
Write-Host "EffectGraph.Engine: $headers engine headers"
& nuget pack (Join-Path $engineStage 'EffectGraph.Engine.nuspec') -Version $Version -OutputDirectory $out -NoDefaultExcludes -NonInteractive
if ($LASTEXITCODE -ne 0) { throw 'nuget pack failed (EffectGraph.Engine)' }

# ---- The C# package, built against the native one just packed. A private
# package folder: the global cache would keep an earlier pack of this version.
$packages = Join-Path $PSScriptRoot 'obj\packages'
if (Test-Path $packages) { Remove-Item -LiteralPath $packages -Recurse -Force }
$bindings = Join-Path $root 'bindings\csharp'
& dotnet pack (Join-Path $bindings 'EffectGraph\EffectGraph.csproj') -c Release -o $out "-p:RestorePackagesPath=$packages" `
    "-p:Version=$Version" "-p:EffectGraphBlobDir=$(Join-Path $root 'ARM64\Release\EffectGraphBuiltIns')\" --nologo
if ($LASTEXITCODE -ne 0) { throw 'dotnet pack failed' }

# ---- The test: a C# program that only has the packages, built portable (the
# DLL under runtimes\<rid>) and for one runtime (the DLL beside the
# assemblies): win-x64 everywhere (emulated on ARM64), win-arm64 where it runs.
$smoke = Join-Path $bindings 'EffectGraph.Smoke\EffectGraph.Smoke.csproj'
$runs = @(@(), @('-r', 'win-x64', '--self-contained'))
if ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'Arm64') { $runs += ,@('-r', 'win-arm64', '--self-contained') }
foreach ($run in $runs) {
    Write-Host "C# smoke test $(if ($run) { $run[1] } else { 'portable' })"
    & dotnet run --project $smoke -c Release @run "-p:RestorePackagesPath=$packages" "-p:EffectGraphVersion=$Version" --nologo
    if ($LASTEXITCODE -ne 0) { throw "C# smoke test failed: $run" }
}
Get-ChildItem $out -Filter "EffectGraph*.$Version.nupkg" | ForEach-Object { Write-Host "packed $($_.Name) ($([int]($_.Length / 1KB)) KB)" }
