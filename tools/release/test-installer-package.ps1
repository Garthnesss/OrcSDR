#requires -Version 5.1
<#
Hardware-free test of the settings-safe installer. Uses fake esptool stubs, so it needs no device
and no download. Point -PackageDirectory at an unpacked installer folder (default: the one
build-installer.ps1 just made for -Version).
#>
param(
  [string]$Version,
  [string]$PackageDirectory
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $PackageDirectory) {
  $PackageDirectory = Join-Path $repo "dist\OrcSDR-Tab5-$Version-installer\OrcSDR-Tab5-$Version-installer-windows-amd64"
}
$work = Join-Path ([IO.Path]::GetTempPath()) ("orcsdr-installer-test-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $work | Out-Null
$failures = 0
function Check([string]$Name, [bool]$Ok, [string]$Detail = '') {
  if ($Ok) { Write-Host "PASS  $Name" } else { Write-Host "FAIL  $Name $Detail"; $script:failures++ }
}
function New-Stub([string]$Name, [string]$ChipLine) {
  $path = Join-Path $work $Name
  # Like the real esptool, warn on stderr before answering.
  Set-Content -LiteralPath $path -Encoding ASCII -Value @('@echo off', 'echo WARNING: ESP32-P4 has no chip ID. Reading MAC address instead. 1>&2', "echo $ChipLine", 'exit /b 0')
  return $path
}
function Copy-Package([string]$Name) {
  $dest = Join-Path $work $Name
  Copy-Item -LiteralPath $PackageDirectory -Destination $dest -Recurse
  return $dest
}
function Invoke-Installer([string]$Dir, [string[]]$ExtraArgs) {
  $output = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Dir 'install.ps1') @ExtraArgs 2>&1 | Out-String
  return [pscustomobject]@{ Code = $LASTEXITCODE; Text = $output }
}

try {
  $p4 = New-Stub 'esptool-p4.cmd' 'Chip type:          ESP32-P4 (revision v1.3)'
  $s3 = New-Stub 'esptool-s3.cmd' 'Chip type:          ESP32-S3 (revision v0.2)'

  # 1. Good package, real Tab5 answer: dry run succeeds and the plan is exactly the three code regions.
  $dir = Copy-Package 'good'
  $r = Invoke-Installer $dir @('-Port', 'COM99', '-DryRun', '-EsptoolPath', $p4)
  Check 'dry run on an ESP32-P4 exits 0' ($r.Code -eq 0) $r.Text
  Check 'dry run prints preserve_nvs=1' ($r.Text -match 'INSTALL_PLAN preserve_nvs=1')
  $offsets = @([regex]::Matches($r.Text, '(?m)^\s+(0x[0-9a-f]+)\s+\S+\.bin') | ForEach-Object { $_.Groups[1].Value })
  Check 'plan writes exactly 0x2000, 0x8000, 0x10000' (($offsets -join ',') -eq '0x2000,0x8000,0x10000') ($offsets -join ',')
  Check 'plan never lists NVS 0x9000 or address 0' (($offsets -notcontains '0x9000') -and ($offsets -notcontains '0x0'))

  # 2. Some other Espressif chip on the port: refuse, exit 3.
  $r = Invoke-Installer $dir @('-Port', 'COM99', '-DryRun', '-EsptoolPath', $s3)
  Check 'a non-P4 chip is refused with exit 3' ($r.Code -eq 3) "code=$($r.Code)"

  # 3. Tampered image: refuse before touching the device, exit 2.
  $bad = Copy-Package 'tampered'
  $app = Join-Path $bad 'orcsdr_tab5_0x10000.bin'
  $bytes = [IO.File]::ReadAllBytes($app); $bytes[100] = $bytes[100] -bxor 0xFF; [IO.File]::WriteAllBytes($app, $bytes)
  $r = Invoke-Installer $bad @('-Port', 'COM99', '-DryRun', '-EsptoolPath', $p4)
  Check 'a changed firmware file is refused with exit 2' ($r.Code -eq 2) "code=$($r.Code)"

  # 4. Truncated image: wrong size, exit 2.
  $short = Copy-Package 'short'
  $app = Join-Path $short 'orcsdr_tab5_0x10000.bin'
  $bytes = [IO.File]::ReadAllBytes($app); [IO.File]::WriteAllBytes($app, $bytes[0..($bytes.Length - 5)])
  $r = Invoke-Installer $short @('-Port', 'COM99', '-DryRun', '-EsptoolPath', $p4)
  Check 'a truncated firmware file is refused with exit 2' ($r.Code -eq 2) "code=$($r.Code)"

  # 5. A manifest offset that could overwrite settings is refused.
  $evil = Copy-Package 'offset'
  $mPath = Join-Path $evil 'installer-manifest.json'
  (Get-Content -LiteralPath $mPath -Raw).Replace('"0x8000"', '"0x9000"') | Set-Content -LiteralPath $mPath -Encoding UTF8
  $r = Invoke-Installer $evil @('-Port', 'COM99', '-DryRun', '-EsptoolPath', $p4)
  Check 'an NVS offset in the manifest is refused with exit 2' ($r.Code -eq 2) "code=$($r.Code)"

  # 6. Static guarantees: no erase anywhere in the scripts.
  $scriptText = (Get-Content -LiteralPath (Join-Path $repo 'tools\release\installer\install.ps1') -Raw) + (Get-Content -LiteralPath (Join-Path $repo 'tools\release\installer\install.sh') -Raw)
  $eraseCode = ($scriptText -split "`n" | Where-Object { $_ -notmatch '^\s*(#|<#)' -and $_ -match 'erase-flash|erase_flash|--erase-all|erase-region|erase_region' })
  Check 'installer scripts contain no erase command' (@($eraseCode).Count -eq 0) ($eraseCode -join '; ')
} finally {
  Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
if ($failures) { Write-Host "INSTALLER_PACKAGE_TEST_FAILED failures=$failures"; exit 1 }
Write-Host 'INSTALLER_PACKAGE_TEST_OK'
exit 0
