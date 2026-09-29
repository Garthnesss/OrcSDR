#!/usr/bin/env bash
# OrcSDR settings-safe installer for the M5Stack Tab5 (Linux / macOS). UNTESTED on real hardware.
# Writes ONLY 0x2000 (bootloader), 0x8000 (partition table), 0x10000 (application).
# Never erases flash, never writes NVS (0x9000), never touches the C6 radio.
# Exit codes: 0 done, 2 file check failed, 3 no ESP32-P4 / wrong chip, 4 flashing failed, 5 cancelled.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
PORT=""; YES=0; DRY=0; ESPTOOL_OVERRIDE=""; BAUD=460800
while [ $# -gt 0 ]; do
  case "$1" in
    --port) PORT="$2"; shift 2;;
    --yes) YES=1; shift;;
    --dry-run) DRY=1; shift;;
    --esptool) ESPTOOL_OVERRIDE="$2"; shift 2;;
    *) echo "Unknown option: $1"; exit 5;;
  esac
done
fail() { echo; echo "STOPPED: $2"; echo "Nothing was written to your Tab5."; exit "$1"; }
sha256() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
manifest() { # manifest <key>  (flat key=value lines written by the build)
  grep -E "^$1=" "$HERE/installer-manifest.txt" | head -n1 | cut -d= -f2-
}

VERSION="$(manifest version)"
echo; echo "OrcSDR $VERSION installer for the M5Stack Tab5"
echo "Your saved settings (Wi-Fi, location, rotation) are kept: NVS is never written."; echo

ESPTOOL="${ESPTOOL_OVERRIDE:-$HERE/$(manifest esptool_file)}"
[ -f "$ESPTOOL" ] || fail 2 "Missing esptool. Unpack the whole folder first."
[ -x "$ESPTOOL" ] || chmod +x "$ESPTOOL" 2>/dev/null
if [ -z "$ESPTOOL_OVERRIDE" ] && [ "$(sha256 "$ESPTOOL")" != "$(manifest esptool_sha256)" ]; then
  fail 2 "esptool does not match its checksum. Download the installer again."
fi
IMAGES=""
n=0
while IFS='|' read -r off file size sum; do
  case "$off" in 0x2000|0x8000|0x10000) ;; *) fail 2 "Refusing unexpected flash offset $off.";; esac
  [ -f "$HERE/$file" ] || fail 2 "Missing $file. Unpack the whole folder first."
  [ "$(wc -c < "$HERE/$file" | tr -d ' ')" = "$size" ] || fail 2 "$file is the wrong size. Download the installer again."
  [ "$(sha256 "$HERE/$file")" = "$sum" ] || fail 2 "$file does not match its checksum. Download the installer again."
  IMAGES="$IMAGES $off $HERE/$file"; n=$((n+1))
done < <(grep -E '^image=' "$HERE/installer-manifest.txt" | cut -d= -f2-)
echo "Checked $n firmware files: OK"

if [ -z "$PORT" ]; then
  CANDS=$(ls /dev/ttyACM* /dev/cu.usbmodem* 2>/dev/null)
  COUNT=$(printf '%s\n' "$CANDS" | grep -c . )
  [ "$COUNT" -ge 1 ] || fail 5 "No USB serial device found. Plug in the Tab5 with a USB-C data cable and try again."
  if [ "$COUNT" -eq 1 ]; then
    PORT="$CANDS"; echo "Found one device: $PORT"
    if [ "$YES" -eq 0 ]; then read -r -p "Install to $PORT ? (y/N) " a; case "$a" in y|Y) ;; *) fail 5 "Cancelled.";; esac; fi
  else
    echo "More than one serial device is connected:"; printf '%s\n' "$CANDS" | nl -w2 -s'  '
    read -r -p "Type the number of your Tab5: " pick
    PORT=$(printf '%s\n' "$CANDS" | sed -n "${pick}p"); [ -n "$PORT" ] || fail 5 "Cancelled."
  fi
fi

echo "Checking what is connected on $PORT ..."
CHIP="$("$ESPTOOL" --port "$PORT" --baud "$BAUD" chip-id 2>&1)"
echo "$CHIP" | grep -Eq '^[[:space:]]*Chip type:[[:space:]]+ESP32-P4' || fail 3 "The device on $PORT is not an ESP32-P4 Tab5. Choose a different port."
echo "Tab5 (ESP32-P4) confirmed."
echo; echo "Will write:"; echo "$IMAGES" | xargs -n2 | sed 's/^/  /'
echo "Will NOT write: NVS settings (0x9000), the C6 radio, or erase anything."
if [ "$DRY" -eq 1 ]; then echo "DRY RUN: nothing written."; echo "INSTALL_PLAN preserve_nvs=1"; exit 0; fi
echo; echo "Writing. Do not unplug the Tab5 ..."
# shellcheck disable=SC2086
"$ESPTOOL" --chip esp32p4 --port "$PORT" --baud "$BAUD" --before default-reset --after hard-reset write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB $IMAGES || fail 4 "esptool reported an error. Unplug and replug the Tab5, then try again."
echo; echo "DONE: OrcSDR $VERSION installed. Your settings and Wi-Fi were kept."
echo "Then open Settings > Firmware & Updates to check the C6 radio."
