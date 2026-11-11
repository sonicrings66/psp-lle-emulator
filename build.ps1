<#
.SYNOPSIS
  Configure and build psp-lle-beta with a portable GCC (w64devkit) + Ninja. Run from anywhere.

.DESCRIPTION
  Finds the toolchain, puts its bin folder first on PATH (gcc needs as.exe / ld.exe from there), throws away a CMake cache that was
  created in a different folder (a copied build directory), configures a Release build and builds it.

  Toolchain lookup, first hit wins:
    1. -Toolchain <folder>, or the PSP_TOOLCHAIN environment variable   (the folder that contains bin\g++.exe, i.e. w64devkit)
    2. <project root>\tools\w64devkit, <this folder>\tools\w64devkit, <project root>\..\tools\w64devkit
    3. C:\w64devkit, D:\w64devkit, E:\w64devkit, and <any sibling folder of the project root>\tools\w64devkit
    4. a g++ already on PATH

  That sibling-folder scan is what lets this reuse a toolchain already fetched for a sibling checkout (e.g. ../psp-lle-emu/tools)
  without any copying or path changes.

  Ninja: <toolchain>\..\ninja\ninja.exe, <project root>\tools\ninja\ninja.exe, or ninja on PATH.

  No toolchain? Run .\setup-toolchain.ps1, or download w64devkit (a single self-extracting archive, no installer) from
  https://github.com/skeeto/w64devkit/releases and Ninja from https://github.com/ninja-build/ninja/releases, extract them anywhere
  (for example <project root>\tools\), and re-run this script, or pass -Toolchain / set PSP_TOOLCHAIN.

.EXAMPLE
  .\build.ps1                       # Release build in build_rel (everything)
  .\build.ps1 -Target psp_boot_driver
  .\build.ps1 -Toolchain D:\tools\w64devkit -BuildDir build_prof -CxxFlags "-DPSP_BETA_PROFILE"
#>
param(
    [string]$Toolchain = $env:PSP_TOOLCHAIN,
    [string]$BuildDir = "build_rel",
    [string]$Target = "",
    [string]$Config = "Release",
    [string]$CxxFlags = "",
    [int]$Jobs = 0                      # compile jobs (0 = ninja decides: every core). Use 2 on a laptop you want to keep using.
)

$ErrorActionPreference = "Stop"
$src = Split-Path -Parent $MyInvocation.MyCommand.Path          # psp-lle-beta
$root = Split-Path -Parent $src                                   # project root

function Find-Toolchain {
    $cands = @()
    if ($Toolchain) { $cands += $Toolchain }
    $cands += @("$root\tools\w64devkit", "$src\tools\w64devkit", "$(Split-Path -Parent $root)\tools\w64devkit", "C:\w64devkit", "D:\w64devkit", "E:\w64devkit")
    foreach ($c in $cands) { if ($c -and (Test-Path "$c\bin\g++.exe")) { return (Resolve-Path $c).Path } }
    # a toolchain kept inside a sibling checkout (<parent>\<any folder>\tools\w64devkit)
    $parent = Split-Path -Parent $root
    if ($parent) { foreach ($d in (Get-ChildItem $parent -Directory -ErrorAction SilentlyContinue)) { $c = "$($d.FullName)\tools\w64devkit"; if (Test-Path "$c\bin\g++.exe") { return $c } } }
    return $null
}

$tc = Find-Toolchain
$gxx = $null
if ($tc) {
    $env:PATH = "$tc\bin;" + $env:PATH
    $gxx = "$tc\bin\g++.exe"
} else {
    $onPath = Get-Command g++ -ErrorAction SilentlyContinue
    if ($onPath) { $gxx = $onPath.Source }
}
if (-not $gxx) {
    Write-Host "No C++ compiler found." -ForegroundColor Red
    Write-Host "Run .\setup-toolchain.ps1 once (it downloads the pinned w64devkit + Ninja into $root\tools and checks their SHA-256), then re-run this script."
    Write-Host "Or by hand:"
    Write-Host "Download w64devkit (portable GCC, no installer): https://github.com/skeeto/w64devkit/releases"
    Write-Host "and Ninja: https://github.com/ninja-build/ninja/releases"
    Write-Host "Extract them anywhere (for example $root\tools\), then re-run, or pass -Toolchain <w64devkit folder> / set PSP_TOOLCHAIN."
    exit 1
}

$ninja = $null
foreach ($c in @($(if ($tc) { "$(Split-Path -Parent $tc)\ninja\ninja.exe" }), "$root\tools\ninja\ninja.exe", "$src\tools\ninja\ninja.exe")) {
    if ($c -and (Test-Path $c)) { $ninja = (Resolve-Path $c).Path; break }
}
if (-not $ninja) { $n = Get-Command ninja -ErrorAction SilentlyContinue; if ($n) { $ninja = $n.Source } }
if (-not $ninja) {
    Write-Host "Ninja not found. Run .\setup-toolchain.ps1, or get ninja-win.zip from https://github.com/ninja-build/ninja/releases and extract ninja.exe next to the toolchain (<tools>\ninja\ninja.exe) or onto PATH." -ForegroundColor Red
    exit 1
}
Write-Host "compiler: $gxx"
Write-Host "ninja:    $ninja"
Write-Host "source:   $src"
Write-Host "build:    $src\$BuildDir ($Config)"

$bd = Join-Path $src $BuildDir
$cache = Join-Path $bd "CMakeCache.txt"
if (Test-Path $cache) {
    # A build directory copied from another folder keeps that folder's absolute paths and breaks CMake/Ninja ("manifest still dirty after 100 tries").
    $cacheHome = (Select-String -Path $cache -Pattern "^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$" | Select-Object -First 1)
    if ($cacheHome) {
        $h = ($cacheHome.Matches[0].Groups[1].Value -replace "/", "\").TrimEnd("\")
        if ($h -ne $src.TrimEnd("\")) {
            Write-Host "build directory was created for '$h', not here: discarding it and configuring afresh" -ForegroundColor Yellow
            Remove-Item -Recurse -Force $bd
        }
    }
}

$cfg = @("-S", $src, "-B", $bd, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=$Config", "-DCMAKE_CXX_COMPILER=$gxx", "-DCMAKE_MAKE_PROGRAM=$ninja")
if ($CxxFlags) { $cfg += "-DCMAKE_CXX_FLAGS=$CxxFlags" }
& cmake @cfg
if ($LASTEXITCODE -ne 0) { Write-Host "CMake configure failed." -ForegroundColor Red; exit $LASTEXITCODE }

$bld = @("--build", $bd)
if ($Target) { $bld += @("--target", $Target) }
if ($Jobs -gt 0) { $bld += @("--", "-j$Jobs") }
& cmake @bld
if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed. If the link step says 'Permission denied', the emulator window is still open: close it and re-run." -ForegroundColor Red
    exit $LASTEXITCODE
}
Write-Host "Done. Run: $bd\tests\beta_tests.exe, or $bd\tests\psp_boot_driver.exe <instruction-budget> --window" -ForegroundColor Green
