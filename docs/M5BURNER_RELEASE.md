# OrcSDR M5Burner release

The current OrcSDR beta is one normal Tab5 package. It contains the P4 application
and a pinned ESP-Hosted 3.0.6 C6 image. M5Burner writes the P4 only; OrcSDR
offers a C6 update from Settings after it proves that the existing Hosted link
is reachable.

## Required order

1. Install **OrcSDR** through M5Burner. **This resets saved settings** (see below).
2. Open **Settings → Firmware & Updates**.
3. If the reachable C6 differs from 3.0.6, explicitly confirm **UPDATE C6 TO 3.0.6**.
4. After restart, verify `host=3.0.6 coprocessor=3.0.6 match=1`.

## Saved settings and M5Burner

An M5Burner install **does not preserve** P4 NVS: Wi-Fi profiles, location, screen rotation,
other preferences and the local event journal are reset, even without an erase option
([#117](https://github.com/hardcoreerik/OrcSDR/issues/117), observed on v0.3.0-beta.1, 2026-09-28). The
uploaded image is one contiguous file from address 0, and the NVS partition (0x9000 to 0xF000) is blank
padding inside it. Earlier documentation that said otherwise was wrong.

To **keep** settings, flash only the regions that hold code: bootloader at 0x2000, partition table at
0x8000 and the application at 0x10000. Nothing is written to NVS (0x9000) or the C6. The bundle's
`local-m5burner/firmware/` folder has those three files and a `flash.sh`; `install-orcsdr.ps1` does the same
from a source checkout. A packaged installer for users is planned.

A C6
that cannot establish Hosted transport is a manual recovery case; the separate
Bridge builder is retained only for support recovery and is not published.

## Build and inspect

From a clean checkout at the exact release tag:

```powershell
.\tools\release\build-m5burner.ps1
.\tools\release\test-m5burner-bundle.ps1 -BundlePath .\dist\OrcSDR-Tab5-<tag> -Version <tag>
```

The build pins the Espressif ESP-Hosted 3.0.6 source revision, emits C6
provenance and SHA-256, and produces the one M5Burner upload bundle plus a
local test zip. It never flashes hardware or uploads a listing.

## Publication record and future-release gate

Published versions are available through M5Burner and [GitHub
Releases](https://github.com/hardcoreerik/OrcSDR/releases).
For a future release, use **USER CUSTOM → Publish** privately first and test
with its Share Code. Only after the exact-tag hardware gate in
[M5BURNER_HARDWARE_GATE.md](M5BURNER_HARDWARE_GATE.md) passes may its GitHub
prerelease be created and its listing made public. M5Burner publishing details
are in the [official guide](https://docs.m5stack.com/en/uiflow/m5burner/publish).
