#requires -Version 5.1
<#
.SYNOPSIS
Builds the settings-safe installer zips for one release from an existing M5Burner bundle.

.DESCRIPTION
Reads the bootloader, partition table and application that build-m5burner.ps1 already
produced, adds a pinned Espressif esptool executable (checksum-verified, no Python needed)
and the install scripts, and writes one zip per platform. Never flashes hardware and never
uploads anything. esptool is downloaded once into .orcsdr-cache/esptool if it is not there
(pass -NoDownload to forbid that).
#>
param(
  [Parameter(Mandatory)] [string]$Version,
  [string]$BundlePath,
  [string[]]$Platforms = @('windows-amd64'),
  [string]$CacheDirectory,
  [string]$OutputDirectory,
  [switch]$NoDownload
)
$ErrorActionPreference = 'Stop'

function Get-Sha256([string]$Path) {
  $sha = [Security.Cryptography.SHA256]::Create()
  $stream = [IO.File]::OpenRead($Path)
  try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
  finally { $stream.Dispose(); $sha.Dispose() }
}

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if ($Version -notmatch '^v\d+\.\d+\.\d+(-(alpha|beta|rc)\.\d+)?$') { throw "Use an exact release tag, not '$Version'." }
if (-not $BundlePath) { $BundlePath = Join-Path $repo "dist\OrcSDR-Tab5-$Version" }
if (-not $CacheDirectory) { $CacheDirectory = Join-Path $repo '.orcsdr-cache\esptool' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo "dist\OrcSDR-Tab5-$Version-installer" }
$pin = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'esptool-release.json') -Raw | ConvertFrom-Json

$fw = Join-Path $BundlePath 'local-m5burner\firmware'
$imageSpec = @(
  @{ offset = '0x2000';  file = 'bootloader_0x2000.bin' },
  @{ offset = '0x8000';  file = 'partition-table_0x8000.bin' },
  @{ offset = '0x10000'; file = 'orcsdr_tab5_0x10000.bin' }
)
foreach ($spec in $imageSpec) {
  $path = Join-Path $fw $spec.file
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing $path. Run build-m5burner.ps1 first." }
  $spec.path = $path
  $spec.sha256 = Get-Sha256 $path
  $spec.size = (Get-Item -LiteralPath $path).Length
}
$uploadSha = (Get-Content -LiteralPath (Join-Path $BundlePath 'm5burner-upload.json') -Raw | ConvertFrom-Json).sha256
New-Item -ItemType Directory -Force -Path $CacheDirectory, $OutputDirectory | Out-Null

foreach ($platform in $Platforms) {
  $asset = $pin.assets.$platform
  if (-not $asset) { throw "No pinned esptool for platform '$platform'." }
  $archive = Join-Path $CacheDirectory $asset.file
  if (-not (Test-Path -LiteralPath $archive)) {
    if ($NoDownload) { throw "$($asset.file) is not cached and -NoDownload was given." }
    Write-Host "Downloading $($asset.file) from $($pin.source_repository) ($($pin.version)) ..."
    gh release download $pin.version --repo 'espressif/esptool' --pattern $asset.file --dir $CacheDirectory
    if ($LASTEXITCODE) { throw 'Could not download esptool.' }
  }
  if ((Get-Sha256 $archive) -ne $asset.zip_sha256) { throw "$($asset.file) does not match its pinned SHA-256." }

  $extract = Join-Path $CacheDirectory "x-$platform"
  if (Test-Path -LiteralPath $extract) { Remove-Item -LiteralPath $extract -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $extract | Out-Null
  if ($asset.file.EndsWith('.zip')) { Expand-Archive -LiteralPath $archive -DestinationPath $extract -Force }
  else { tar -xzf $archive -C $extract; if ($LASTEXITCODE) { throw 'Could not unpack esptool.' } }
  $toolDir = Join-Path $extract $asset.folder
  $tool = Join-Path $toolDir $asset.executable
  if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) { throw "$($asset.executable) not found in $($asset.file)." }
  $toolSha = Get-Sha256 $tool

  $name = "OrcSDR-Tab5-$Version-installer-$platform"
  $stage = Join-Path $OutputDirectory $name
  if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $stage | Out-Null
  foreach ($spec in $imageSpec) { Copy-Item -LiteralPath $spec.path -Destination (Join-Path $stage $spec.file) }
  Copy-Item -LiteralPath $tool -Destination (Join-Path $stage $asset.executable)
  Copy-Item -LiteralPath (Join-Path $toolDir 'LICENSE') -Destination (Join-Path $stage 'esptool-LICENSE.txt')
  $forWindows = $platform -like 'windows-*'
  $scripts = if ($forWindows) { @('install.ps1', 'install.bat') } else { @('install.sh') }
  foreach ($script in $scripts) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot "installer\$script") -Destination (Join-Path $stage $script) }

  if ($forWindows) {
    [ordered]@{
      version = $Version
      esptool = [ordered]@{ file = $asset.executable; sha256 = $toolSha; version = $pin.version; source = $pin.source_repository }
      images = @($imageSpec | ForEach-Object { [ordered]@{ offset = $_.offset; file = $_.file; size = $_.size; sha256 = $_.sha256 } })
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stage 'installer-manifest.json') -Encoding UTF8
  } else {
    $lines = @("version=$Version", "esptool_file=$($asset.executable)", "esptool_sha256=$toolSha")
    $lines += $imageSpec | ForEach-Object { "image=$($_.offset)|$($_.file)|$($_.size)|$($_.sha256)" }
    ($lines -join "`n") + "`n" | Set-Content -LiteralPath (Join-Path $stage 'installer-manifest.txt') -Encoding ASCII -NoNewline
  }

  $launch = if ($forWindows) { 'Double-click install.bat.' } else { 'Run ./install.sh in a terminal.' }
  @(
    "OrcSDR $Version settings-safe installer for the M5Stack Tab5 ($platform)",
    '',
    'WHAT THIS DOES',
    '  Installs OrcSDR and KEEPS your saved settings (Wi-Fi, location, screen rotation).',
    '  It writes only the bootloader (0x2000), partition table (0x8000) and application (0x10000).',
    '  It never erases anything, never writes the settings area (0x9000), and never touches the C6 Wi-Fi radio.',
    '  It refuses to write to anything that is not an ESP32-P4 (your Tab5).',
    '',
    'HOW TO USE',
    '  1. Unzip this whole folder.',
    '  2. Plug the Tab5 into this computer with a USB-C DATA cable and switch it on.',
    "  3. $launch",
    '  4. If more than one device is connected, pick your Tab5 from the list.',
    '  5. Wait for DONE. The Tab5 restarts by itself.',
    '  6. Open Settings > Firmware & Updates and check the C6 radio reports 3.0.6.',
    '',
    'The files are checked against their SHA-256 before anything is written.',
    "The M5Burner listing is not a settings-safe install: an M5Burner install resets saved settings.",
    "Combined firmware image SHA-256 (M5Burner upload): $uploadSha",
    '',
    "esptool $($pin.version) from $($pin.source_repository) is included unmodified under the GPL-2.0-or-later (see esptool-LICENSE.txt).",
    ($(if ($platform -notlike 'windows-*') { 'NOTE: the Linux and macOS installer has not been tested on real hardware.' } else { '' }))
  ) | Set-Content -LiteralPath (Join-Path $stage 'README.txt') -Encoding UTF8

  $zip = Join-Path $OutputDirectory "$name.zip"
  if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
  Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
  Write-Host "INSTALLER_OK platform=$platform zip=$zip sha256=$(Get-Sha256 $zip) size=$((Get-Item -LiteralPath $zip).Length)"
}
