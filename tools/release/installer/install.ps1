#requires -Version 5.1
<#
OrcSDR settings-safe installer for the M5Stack Tab5.

Writes ONLY the bootloader (0x2000), partition table (0x8000) and application
(0x10000). It never erases flash and never writes the NVS partition (0x9000), so
Wi-Fi profiles, location, screen rotation and other saved settings are kept.
It never touches the C6 Wi-Fi radio; update that from Settings > Firmware & Updates.

Exit codes: 0 done, 2 file check failed, 3 no ESP32-P4 found / wrong chip,
            4 flashing failed, 5 cancelled / no port chosen.
#>
param(
  [string]$Port,
  [switch]$Yes,
  [switch]$DryRun,
  [string]$EsptoolPath,
  [int]$Baud = 460800
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$manifest = Get-Content -LiteralPath (Join-Path $here 'installer-manifest.json') -Raw | ConvertFrom-Json
$allowedOffsets = @('0x2000', '0x8000', '0x10000')

function Fail([int]$Code, [string]$Message) {
  Write-Host ''
  Write-Host "STOPPED: $Message" -ForegroundColor Red
  Write-Host 'Nothing was written to your Tab5.' -ForegroundColor Yellow
  exit $Code
}
function Get-Sha256([string]$Path) {
  $sha = [Security.Cryptography.SHA256]::Create()
  $stream = [IO.File]::OpenRead($Path)
  try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
  finally { $stream.Dispose(); $sha.Dispose() }
}

Write-Host ''
Write-Host "OrcSDR $($manifest.version) installer for the M5Stack Tab5" -ForegroundColor Cyan
Write-Host 'Your saved settings (Wi-Fi, location, rotation) are kept: NVS is never written.'
Write-Host ''

# 1. Check every file before touching the device.
$esptool = if ($EsptoolPath) { $EsptoolPath } else { Join-Path $here $manifest.esptool.file }
if (-not (Test-Path -LiteralPath $esptool -PathType Leaf)) { Fail 2 "Missing $($manifest.esptool.file). Unzip the whole folder first." }
if (-not $EsptoolPath -and (Get-Sha256 $esptool) -ne $manifest.esptool.sha256) { Fail 2 'esptool does not match its checksum. Download the installer again.' }
foreach ($image in $manifest.images) {
  if ($allowedOffsets -notcontains $image.offset) { Fail 2 "Refusing unexpected flash offset $($image.offset)." }
  $path = Join-Path $here $image.file
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { Fail 2 "Missing $($image.file). Unzip the whole folder first." }
  if ((Get-Item -LiteralPath $path).Length -ne [int64]$image.size) { Fail 2 "$($image.file) is the wrong size. Download the installer again." }
  if ((Get-Sha256 $path) -ne $image.sha256) { Fail 2 "$($image.file) does not match its checksum. Download the installer again." }
}
Write-Host "Checked $(@($manifest.images).Count) firmware files: OK" -ForegroundColor Green

# 2. Choose the port. Never guess when more than one device is present.
function Get-Candidates {
  @(Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue |
    Where-Object { $_.PNPDeviceID -match 'VID_303A' } |
    ForEach-Object { [pscustomobject]@{ Port = $_.DeviceID; Name = $_.Name } })
}
if (-not $Port) {
  $candidates = @(Get-Candidates)
  if ($candidates.Count -eq 0) {
    Fail 5 'No Espressif USB serial device found. Plug the Tab5 into this PC with a USB-C data cable, switch it on, and run this again.'
  }
  if ($candidates.Count -eq 1) {
    $Port = $candidates[0].Port
    Write-Host "Found one device: $($candidates[0].Name)"
    if (-not $Yes) {
      $answer = Read-Host "Install to $Port ? (Y/N)"
      if ($answer -notmatch '^[Yy]') { Fail 5 'Cancelled.' }
    }
  } else {
    Write-Host 'More than one Espressif device is connected:'
    for ($i = 0; $i -lt $candidates.Count; $i++) { Write-Host ("  [{0}] {1}  ({2})" -f ($i + 1), $candidates[$i].Port, $candidates[$i].Name) }
    $pick = Read-Host 'Type the number of your Tab5 (or press Enter to cancel)'
    if ($pick -notmatch '^\d+$' -or [int]$pick -lt 1 -or [int]$pick -gt $candidates.Count) { Fail 5 'Cancelled.' }
    $Port = $candidates[[int]$pick - 1].Port
  }
}
if ($Port -notmatch '^COM[0-9]+$') { Fail 5 "Port must look like COM5, not '$Port'." }

# 3. Ask the chip who it is. Only an ESP32-P4 may be written.
Write-Host "Checking what is connected on $Port ..."
# esptool prints harmless warnings on stderr (for example 'ESP32-P4 has no chip ID'); do not treat them as errors.
$previousPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try { $chipText = (& $esptool --port $Port --baud $Baud chip-id 2>&1 | ForEach-Object { "$_" } | Out-String) }
finally { $ErrorActionPreference = $previousPreference }
# Match the chip-type line itself, not any mention of the name (warnings can mention ESP32-P4).
if ($chipText -notmatch '(?m)^\s*Chip type:\s+ESP32-P4') {
  $found = if ($chipText -match 'Chip type:\s*(.+)') { $Matches[1].Trim() } else { 'no answer' }
  Fail 3 "The device on $Port is not an ESP32-P4 Tab5 ($found). Choose a different port."
}
Write-Host 'Tab5 (ESP32-P4) confirmed.' -ForegroundColor Green

# 4. The plan, then flash.
$writeArgs = @()
foreach ($image in $manifest.images) { $writeArgs += $image.offset; $writeArgs += (Join-Path $here $image.file) }
Write-Host ''
Write-Host 'Will write:'
foreach ($image in $manifest.images) { Write-Host ("  {0,-8} {1}" -f $image.offset, $image.file) }
Write-Host 'Will NOT write: NVS settings (0x9000), the C6 radio, or erase anything.'
if ($DryRun) { Write-Host 'DRY RUN: nothing written.'; Write-Host 'INSTALL_PLAN preserve_nvs=1'; exit 0 }
Write-Host ''
Write-Host 'Writing. Do not unplug the Tab5. This takes about 30 seconds ...'
& $esptool --chip esp32p4 --port $Port --baud $Baud --before default-reset --after hard-reset write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB @writeArgs
if ($LASTEXITCODE) { Fail 4 "esptool reported an error (exit $LASTEXITCODE). Unplug and replug the Tab5, then try again." }
Write-Host ''
Write-Host "DONE: OrcSDR $($manifest.version) installed. Your settings and Wi-Fi were kept." -ForegroundColor Green
Write-Host 'The Tab5 restarts by itself. Then open Settings > Firmware & Updates to check the C6 radio.'
exit 0
