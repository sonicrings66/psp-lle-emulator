<#
.SYNOPSIS
  Get the portable build toolchain (w64devkit GCC + Ninja) into <project root>\tools. Run once per checkout, then use build.ps1.

.DESCRIPTION
  The toolchain is NOT part of the repository (it is 594 MB extracted and git-ignored). This script fetches the exact versions the
  project is built and tested with, checks their SHA-256, and extracts them:

      w64devkit 2.9.1  (GCC 16.2, self-extracting 7z, ~59 MB)   https://github.com/skeeto/w64devkit/releases/tag/v2.9.1
      Ninja 1.13.2     (single exe in a zip, ~0.3 MB)           https://github.com/ninja-build/ninja/releases/tag/v1.13.2

  Nothing is installed system-wide and PATH is not touched. If the two archives are already on disk (for example copied from another
  checkout's tools\ folder, or from a sibling psp-lle-emu checkout) pass -ArchiveDir and nothing is downloaded.

.PARAMETER Dest        Where to put the toolchain (default: <project root>\tools).
.PARAMETER ArchiveDir  A folder that already holds w64devkit-x64-2.9.1.7z.exe and/or ninja-win.zip (used instead of downloading, hashes still checked).
.PARAMETER Force       Re-extract even if the tools are already there.

.EXAMPLE
  .\setup-toolchain.ps1
  .\setup-toolchain.ps1 -ArchiveDir "..\tools"
#>
param(
    [string]$Dest = "",
    [string]$ArchiveDir = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$src = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $src
if (-not $Dest) { $Dest = Join-Path $root "tools" }

$items = @(
    @{ Name = "w64devkit"; File = "w64devkit-x64-2.9.1.7z.exe"
       Url = "https://github.com/skeeto/w64devkit/releases/download/v2.9.1/w64devkit-x64-2.9.1.7z.exe"
       Sha256 = "9208c19755cd4964b7915b9afcf02c66d493a4c870c4b3e83f6c538d9c1237a5"; Check = "w64devkit\bin\g++.exe" },
    @{ Name = "ninja"; File = "ninja-win.zip"
       Url = "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip"
       Sha256 = "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65"; Check = "ninja\ninja.exe" }
)

New-Item -ItemType Directory -Force $Dest | Out-Null
Write-Host "toolchain folder: $Dest"

foreach ($it in $items) {
    $target = Join-Path $Dest $it.Check
    if ((Test-Path $target) -and -not $Force) { Write-Host "$($it.Name): already present ($target)"; continue }

    # 1. find or fetch the archive
    $archive = $null
    foreach ($d in @($ArchiveDir, $Dest)) {
        if ($d -and (Test-Path (Join-Path $d $it.File))) { $archive = Join-Path $d $it.File; break }
    }
    if (-not $archive) {
        $archive = Join-Path $Dest $it.File
        Write-Host "$($it.Name): downloading $($it.Url)"
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $old = $ProgressPreference; $ProgressPreference = "SilentlyContinue"
        try { Invoke-WebRequest -Uri $it.Url -OutFile $archive -UseBasicParsing } finally { $ProgressPreference = $old }
    } else { Write-Host "$($it.Name): using $archive" }

    # 2. verify
    $hash = (Get-FileHash $archive -Algorithm SHA256).Hash.ToLower()
    if ($hash -ne $it.Sha256) {
        Write-Host "$($it.Name): SHA-256 mismatch for $archive" -ForegroundColor Red
        Write-Host "  expected $($it.Sha256)"; Write-Host "  got      $hash"
        Write-Host "  Delete the file and re-run, or download it by hand from the release page (see the top of this script)."
        exit 1
    }

    # 3. extract
    if ($it.Name -eq "w64devkit") {
        & $archive "-o$Dest" -y | Out-Null            # self-extracting 7z: creates <Dest>\w64devkit
    } else {
        $nd = Join-Path $Dest "ninja"
        New-Item -ItemType Directory -Force $nd | Out-Null
        Expand-Archive -Path $archive -DestinationPath $nd -Force
    }
    if (-not (Test-Path $target)) { Write-Host "$($it.Name): extraction did not produce $target" -ForegroundColor Red; exit 1 }
    Write-Host "$($it.Name): ready ($target)"
}

$gxx = Join-Path $Dest "w64devkit\bin\g++.exe"
if (Test-Path $gxx) { $env:PATH = (Join-Path $Dest "w64devkit\bin") + ";" + $env:PATH; & $gxx --version | Select-Object -First 1 }
$nj = Join-Path $Dest "ninja\ninja.exe"
if (Test-Path $nj) { Write-Host ("ninja " + (& $nj --version)) }
Write-Host "Done. Next: .\build.ps1" -ForegroundColor Green
