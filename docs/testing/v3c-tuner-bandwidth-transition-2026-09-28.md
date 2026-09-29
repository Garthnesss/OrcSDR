# Live tuner-bandwidth transition (V3c, V4L, V4) — root cause, fix and hardware acceptance (2026-09-28)

Scope: RTL-SDR Blog V3c, V4L and V4 on the M5 Tab5, FM 96.1 MHz, 2.4 MS/s, RF Lab `NEXT BW`
(driver `esp_rtl_sdr_set_tuner_bandwidth`). Companion driver write-up:
`docs/captures/v3c_live_bandwidth_2026-09-28.md` in `esp-rtl-sdr`.

*Observed* = read from a capture, a chip readback or an IQ file. *Inferred* = not proven.

## Symptom

Selecting a tuner bandwidth moved the spectrum sideways and detuned the station while the UI and the
driver still reported 96.1 MHz. A normal STEP or a +100/-100 kHz retune re-centred it. On the V3c,
returning to AUTO also did not restore the wide passband. A reboot restored everything. The V3c was
found first; the V4L showed the same fault with larger offsets when it was run through the same gate.

## Three different "bandwidths" (not to be confused)

| control | what it is | retunes hardware? |
|---|---|---|
| FM DSP filter bandwidth (`rtl_filter_bandwidth_hz`, DSP FILTER BW) | app-side DSP after capture | no |
| RF Lab TUNER BW | R820T2/R860/R828 IF filter registers 0x0a/0x0b, PLL IF and RTL2832 demod IF word, written together | yes |
| IQ sample rate (2.4 MS/s here) | ADC/resampler rate | no |

This work concerns only the second. The FM DSP filter was not changed and stays separate in the UI.

## Root cause (two faults, both in `esp-rtl-sdr`)

1. **Demod IF write sequencing (frequency shift; V3c, V4L and, by the shared plan code, V4).** The
   bandwidth transaction wrote the RTL2832 IF bytes 0x19/0x1a/0x1b consecutively. The captured init IF
   sequence and a PC live-bandwidth capture of the same V3c read page 0x0a reg 0x01 after every demod
   write. Before the fix (observed): V3c +93.7 kHz at 200 kHz and -89.6 kHz back at AUTO; V4L -312,
   -4.7, +98.4, -124.1, -49.9, +385.6 kHz through 200k, 300k, 500k, 1.0M, 1.8M, 2.4M. Model
   (*inferred*; the RTL2832 mechanism is not established): each transaction ends with the new 0x19 byte
   combined with the previous transaction's 0x1a/0x1b bytes, so the shift is the effective demod IF
   minus the PLL IF. That predicts the V4L offsets to -310, 0, +100, -125, -50, +385 kHz and the V3c to
   +/-95 kHz. The UI kept saying 96.1 MHz because the requested RF never changed; the error was in the
   digital IF word, and a later retune rewrote it. Tuner PLL bytes were correct throughout.
2. **V3c AUTO filter state (passband).** The V3c boots with tuner registers 0x0a/0x0b = `d5/6b` (read
   back from the chip after a cold boot, six identical dumps). The AUTO plan wrote the PC's `c5/8f`, the
   PC's pairing with its own 1.815 MHz IF, together with the 3.570 MHz boot IF: a state neither the V3c
   boot nor any PC capture contains. After AUTO the left side stayed about 13 dB below boot.

Replaying the complete tuner tune after a bandwidth change (`esp-rtl-sdr` commit `be7ff03`) fixed neither
and left the left side attenuated; it is rejected and not part of this change.

## Architecture: variable IF, single source of truth

The V3c runs a **variable IF by design**: 3.570 MHz at boot and AUTO, and the IF of the selected
bandwidth plan otherwise (2.125, 2.025, 1.700, 1.750, 1.815 MHz). The applied bandwidth plan
(`MeasuredTunerBandwidthPlan`) is the single source of truth: filter registers, PLL IF and demod IF
word come from the same plan and are written in one paused transaction; while a plan is active,
retunes go through the same transaction. The demod IF records are one shared helper
(`measured_bandwidth_demod_if_records`, read after each IF byte) with a host test over V3c, V4 and V4L.
Plan values are vendor-driver control choices derived from captures, not measured analog passbands.

## Evidence (this repo: `docs/testing/evidence/v3c-tuner-bandwidth-2026-09-28/`)

V3c investigation and fix history:

| run (stamp) | firmware / driver | what it shows |
|---|---|---|
| 115826 | restored `3C1023…F201` app | before: +93.7 / -89.6 kHz |
| 130329 | `be7ff03` candidate (rejected) | same shifts, left side stays down after retune |
| 131711 | PC, same V3c, RTL-SDR Blog driver | centred at every live transition (+0.5..+1.2 kHz) |
| 141720 | V3c-only read-after-write, V3c hot-swapped after a V4L boot | centred; AUTO leaves left side down |
| 143504 | same, cold boot | same (rules out V4L carry-over) |
| 150402 | + AUTO boot-filter fix with register readback | AUTO passband restored; tuner regs equal boot except gain regs 0x05/0x07 |
| 151049 | clean fix | AUTO within 0.8 dB of boot |

Full cycles (AUTO, 200k, 300k, 500k, 1.0M, 1.8M, 2.4M, AUTO, one continuous session, IQ at every stage):

| run | dongle | build | boot |
|---|---|---|---|
| 152250 (A) | V3c | fix + register readback (`6727710`) | cold |
| 153158 (B) | V3c | `dabba30` | cold |
| 154022 (C) | V3c | `dabba30` | cold, repeat |
| 155310 (D) | V3c | `dabba30` | cold, then unplug/replug |
| 161811 (E) | V3c | `d363fc5` (V3c-only fix) | cold |
| 170752 | V4L | before the V4L fix (V3c-only fix build) | dongle hot-swapped from a V3c session |
| 172449 | V4L | `07b418e` (shared fix) | dongle hot-swapped from a V3c session |
| 173318 | V4L | `07b418e` | after unplug/replug |
| 174442 | V4 | `07b418e` | cold, dongle swapped in beforehand |
| 180452 | V4 | `07b418e` | after unplug/replug |
| 183324 (F) | V3c | `07b418e` | cold |
| 184158 (G) | V3c | `07b418e` | cold, repeat |
| 185700 (H) | V3c | final production artifact: driver `2c6895d`, app sha256 `65639248D29DB6CC95E755EE939486C55079EF144BC11C7120D12F05497422FB` | cold |

The V4L and V4 runs used the `07b418e` build; the final artifact (`2c6895d`) differs from it only by driver documentation (the driver `src`, `private`, `include` and `tests` trees are identical), and was gated on the V3c (run H). Each `*-cycle-report.txt` is `tools/rtl-gate/cycle_report.py` output; raw 1 s IQ per stage (140 files, about 672 MB,
not committed) is listed with SHA-256 in `iq-sha256.txt`. The PC capture is `131711`; its pcapng
(sha256 `E4531CDD…BDB1`) is held locally.

Invalid or superseded runs, kept locally and not used as evidence: a first replug run in which the
harness took a Wi-Fi pause for the replug and the unplug landed inside the baseline IQ capture
(fixed in the script: it now requires `RTL_SDR_DISCONNECTED`), gate attempts stopped by a post-boot
Wi-Fi reconnect pausing the radio before the 8 s stability wait was added, a V4 replug attempt whose
wait window closed before the dongle was unplugged, and one attempt with a V4L attached to a V3c gate.

## Hardware acceptance (observed)

Every run: requested bandwidth = applied bandwidth, exactly one tuner tune per stage, no radio stop or
restart, zero rejected control records, zero USB overruns, zero IQ drops, zero audio drops. Manual gain is
constant within each run from the baseline on; it settles during boot, before the gate begins, and
differs between runs (V3c: 0 dB in A and D, 0.9 dB in B, C and E, 2.7 dB in F and G).

**V3c** carrier offset from the requested 96.1 MHz (kHz), and final-AUTO deviation from boot over twelve
offsets (-1.1..+1.1 MHz):

| run | max stage offset | final AUTO vs boot |
|---|---|---|
| A | +1.6 | within 0.6 dB |
| B | +1.1 | within 0.6 dB |
| C | +0.9 | within 0.8 dB |
| D (after unplug/replug) | +1.1 | within 0.5 dB |
| E | +1.0 | within 0.7 dB |
| F | +1.9 | up to 3.6 dB (irregular across offsets, -0.3..-3.6) |
| G (repeat of F) | +2.0 (boot itself +2.1) | within 0.4 dB |
| H (final production artifact) | +1.6 | within 0.5 dB (the retune after it within 1.1 dB) |

Run F's AUTO and the +100/-100 kHz retune after it both sat 1-3.6 dB under the boot baseline with the
manual gain unchanged, and G did not reproduce it. The cause is not established; the irregularity across
offsets fits broadcast-content drift, but that is a hypothesis.

Passband response at -900 kHz relative to the same run's boot baseline (dB), V3c runs A / B / C / D:

| stage | A | B | C | D |
|---|---|---|---|---|
| 200k | -11.7 | -14.1 | -12.8 | -10.7 |
| 300k | -11.6 | -14.1 | -12.8 | -10.6 |
| 500k | -11.5 | -14.0 | -12.7 | -10.5 |
| 1.0M | -9.1 | -10.9 | -9.7 | -8.4 |
| 1.8M | +1.3 | +1.6 | +3.9 | +1.7 |
| 2.4M | +1.7 | +1.8 | +2.0 | +1.9 |
| final AUTO | -0.6 | +0.4 | +0.8 | +0.2 |

200k and 300k are the same plan and give the same passband.

**V4L** carrier offset (kHz) through the stages, before and with the shared fix:

| stage | before | fix, cold | fix, unplug/replug |
|---|---|---|---|
| 200k | -312.0 | -1.7 | -2.5 |
| 300k | -4.7 | -1.9 | -1.1 |
| 500k | +98.4 | +0.3 | +0.8 |
| 1.0M | -124.1 | +0.5 | +0.9 |
| 1.8M | -49.9 | +0.3 | +0.7 |
| 2.4M | +385.6 | +0.5 | +0.8 |
| final AUTO | +0.3 | -0.9 | +1.0 |

Final AUTO within 0.3 dB (cold) and 0.5 dB (replug) of boot. The V4L narrows both sides symmetrically.

**V4** (not measured before the fix): carrier within +1.2 kHz (cold) and +/-1.4 kHz (replug) at every
stage; final AUTO within 0.2 dB (cold) and 0.4 dB (replug) of boot; the V4 narrows the left side only.

Run A register and IF readback (V3c), taken after each stage's own IF write:

| stage | tuner 0a/0b | PLL IF | demod IF word |
|---|---|---|---|
| boot | d5/6b | 3.570 MHz | 38 11 12 |
| 200k, 300k | c5/e6 | 2.125 | 3b 47 1d |
| 500k | c5/e8 | 2.025 | 3b 80 00 |
| 1.0M | c5/eb | 1.700 | 3c 38 e4 |
| 1.8M | c5/ac | 1.750 | 3c 1c 72 |
| 2.4M | c5/8f | 1.815 | 3b f7 78 |
| final AUTO | d5/6b | 3.570 | 38 11 12 |

In the V3c replug run the log shows `usb disconnected`, `RTL_SDR_DISCONNECTED`, a new device, `PROBE_OK`,
`HOTPLUG_RESUME`, a fresh init and streaming before the cycle ran; the V4L and V4 replug runs show the same
sequence.

An earlier reporting slip is corrected here: register dumps taken inside a transaction precede that
transaction's own IF write, so they show the previous stage's IF word. The table above is read after
the write (`bw_end` dump).

## Not established

- The internal RTL2832 mechanism of the read-after-write (latch, flush or delay).
- Analog passband widths in Hz.
- V4 behaviour before the fix (not run). The V4L result and the shared plan words suggest it shifted too.
- The cause of run F's level deviation.
- Explicit bandwidth stages on the V3c read about +2 to +5 dB higher than boot on the right side in some
  runs; not investigated.
- Low RDS/pilot values after a centred retune (V3c): seen in every run, kept separate, not investigated.
- Tuner registers 0x10..0x1f were not read back: a 32-byte tuner read failed on the bridge and the 16-byte
  read (0x00..0x0f) succeeded.
- The restored `3C1023…F201` app's exact driver revision.

## Reproduce

See `apps/orcsdr-tab5/tools/rtl-gate/README.md`. In short: build with the pin below, flash the app only,
run `run-tab5-ui-regression.ps1 -DongleGate <V3c|V4L|V4> -GateIq -GateBandwidths 200000,300000,500000,1000000,1800000,2400000,0`
and `cycle_report.py <prefix> --plot`.

## Merge sequence (proposed, nothing merged)

1. `esp-rtl-sdr`: publish `codex/bandwidth-transition-v3-v4` (two fix commits and one docs commit on PR #29's
   `c6d50dd`), review, merge without squashing so the commit ids stay the ones pinned here; then tag/version
   as the maintainer decides.
2. `OrcSDR`: `codex/v3c-tuner-bw-acceptance` (harness and tools, this document and evidence, dependency pin +
   lockfile) onto the gold branch `codex/v3c-am-rtl-agc`. If the driver merge changes the commit id, update
   `main/idf_component.yml` and regenerate `dependencies.lock` (`idf.py reconfigure`), then rebuild from a clean
   build directory and keep the other locked dependencies at their gold versions.
3. Not to be merged: `be7ff03`, the diagnostic register-readback branches (`codex/v3c-auto-diag`,
   `codex/v3c-auto-fix-diag`), the superseded driver branch `codex/v3c-bandwidth-transition` (V3c-only, pinned
   by the first draft of this work) and `8b25e07` (a separate Nooelec change).
