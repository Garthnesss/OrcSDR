param(
  [string]$OutDir = 'C:\Tools\esp-rtl-sdr-lab\captures\v3c-live-bw-20260928',
  [string]$Dll = 'C:\Tools\esp-rtl-sdr-lab\v4l-driver-1.4.0\x64\rtlsdr.dll',
  [int]$CaptureSeconds = 75
)
$ErrorActionPreference = 'Stop'
$tshark = 'C:\Program Files\Wireshark\tshark.exe'
$stim = Join-Path $PSScriptRoot 'pc_bw_stimulus.py'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$pcap = Join-Path $OutDir "$stamp-v3c-live-bw.pcapng"
$prefix = Join-Path $OutDir "$stamp-v3c-pc"
if (Test-Path $pcap) { throw "exists: $pcap" }

# All USBPcap roots; the dongle's bus is not known in advance. Timed autostop keeps
# the pcapng tail intact.
$ifaces = & $tshark -D | Select-String 'USBPcap' | ForEach-Object { ($_ -split '\.')[0].Trim() }
$targs = @(); foreach ($i in $ifaces) { $targs += @('-i', $i) }
$targs += @('-a', "duration:$CaptureSeconds", '-w', $pcap)
$ts = Start-Process -FilePath $tshark -ArgumentList $targs -PassThru -NoNewWindow `
  -RedirectStandardError (Join-Path $OutDir "$stamp-tshark.err")
Start-Sleep -Seconds 4
python $stim $Dll $prefix 2>&1 | Tee-Object -FilePath "$prefix-stimulus.log"
$stimExit = $LASTEXITCODE
Write-Host "stimulus exit=$stimExit; waiting for tshark autostop..."
$ts.WaitForExit()
Get-ChildItem $OutDir -Filter "$stamp*" | ForEach-Object {
  [pscustomobject]@{ File = $_.Name; Bytes = $_.Length; Sha256 = (Get-FileHash $_.FullName).Hash }
} | Tee-Object -Variable manifest | Format-Table -AutoSize | Out-String -Width 220
$manifest | ConvertTo-Json | Set-Content (Join-Path $OutDir "$stamp-manifest.json")
