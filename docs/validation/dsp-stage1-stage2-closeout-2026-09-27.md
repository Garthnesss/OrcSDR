# OrcSDR DSP Stage 1/2 closeout — 2026-09-27

## Scope and identity

This is dated engineering evidence for development branch `claude/dsp-multirate`
(draft PR #114, stacked on `claude/rc4-controls` PR #113), inspected at
`a20def70036264686175e4db7817485b19bfcd4a` plus the local D3 work. It is
not a published release or a claim about merged `main`. Stage 1 optimized the
existing production DSP without intentionally changing its output. Stage 2
investigated exact-rate multirate frontends; it did not integrate one into
production WFM. ESP-IDF migration and `esp-rtl-sdr` driver changes were out of
scope.

The test platform was the owner M5Stack Tab5 (ESP32-P4 revision 1.3, 360 MHz),
RTL-SDR Blog V4, ESP-IDF 5.5.4, and pinned `esp-rtl-sdr`
`f62c5cdaa73a18f544413e4eca0f1f62abce285e`. IQ blocks were 16,384
complex CU8 samples. The accepted Stage-1 release measurements used the
ordinary optimized native build with `ORCSDR_DSP_AB=0`, stage timing enabled,
and no Stage-2 frontend. Stage-2 exclusive-live evidence used a lab build
without the C6 image; it cannot establish normal Hosted operation. The normal
closeout build uses `ORCSDR_DSP_LAB=0`, `ORCSDR_DSP_AB=0` and the matching C6
image. Exact closeout build/flash results must be recorded below when run.

## Why the investigation was performed

The driver demonstrated physical IQ acquisition at 2.40, 2.56, 2.88 and
3.20 MS/s, while OrcSDR's established WFM demodulation assumes integer
relationships to 240 kS/s MPX; a custom device rate disables ordinary audio.
The existing one-pole/boxcar reduction is inexpensive but not a high-quality
anti-aliasing frontend. The Stage-2 hypothesis was that early cheap fixed-point
decimation followed by exact rational/polyphase filtering could feed the
proven 240 kS/s WFM decoder at all four physical rates. The radio was not
broken at its existing default rate.

## Stage 1 — accepted production optimization

The pre-optimization 2.40 MS/s WFM path used about 68% DSP load. Stage 1 kept
the existing demodulation mathematics and made it cheaper:

| Change | Why retained | Evidence |
|---|---|---|
| Block-local FM/AM/SSB hot state, committed once per IQ block | Avoid repeated global-state traffic and alias barriers | AM inner loop about 31 to 19 instructions/sample, 14 to zero persistent-state loads/stores/sample; same-IQ bit identity |
| Clipping fused into demod pass; strided power scan retained for squelch | Avoid full-rate level scan without losing squelch input | Level stage about 0.77–0.78 to 0.07 ms/block; same-IQ meters |
| `rds_process_mpx_block()` and chunked SD replay | Remove per-sample call/state cost | Live A/B state match; 8 s replay same full-state hash and 75,999 chips |
| DSP-task-owned demod/RDS/BFO/full reset requests | Prevent cross-task state mutation and mixed write-back | 4,000 mixed requests, 30 real hot retunes, none pending, no watchdog |
| `noinline` FM and SSB in release | Prevent measured compiler code-layout regression | FM demod about 3.00 ms/block out of line versus 3.73 ms when inlined |
| DSP stats/health and stage timing | Make overload and loss observable | Timing on/off difference below measurement noise |
| Overload yield | Avoid starving IDLE1 into watchdog reset | Converts overload to visible backlog/drop/yield; not a performance fix |

The old-vs-new A/B harness ran on the *same saved live IQ and initial DSP
state*. Across 64 blocks per mode, audio bytes, final per-block state and
meters were bit-identical for FM, NFM/Weather, AM, CB AM, CB LSB and CB USB;
FM MPX also matched. Live RDS A/B matched (126,200 chips), and SD RDS replay
matched. This was the appropriate strong test because Stage 1 changed
implementation cost, not intended signal behavior. Listening alone would not
detect a rare state or byte discrepancy. The A/B harness is development-only
and compiles out when `ORCSDR_DSP_AB=0`.

Release-build, 2.40 MS/s steady-window results are approximate:

| Workload | DSP load before → after | After block average |
|---|---:|---:|
| WFM stereo + RDS | 68% → 45% | 3.10 ms |
| AM | 52% → 35% | 2.45 ms |
| CB AM | 51% → 35% | 2.42 ms |
| CB LSB | no matched before → 30% | 2.07 ms |
| RF Lab / CB | 67% → 45% | 3.13 ms |

Steady windows had no sustained queue backlog, audio dropped chunks,
audio-ring overruns, speaker submission failures or watchdog resets. Startup
FM driver/pipeline drops were cumulative and did not grow in the steady
window. **Stage 1: ACCEPTED FOR PRODUCTION on this development branch.**
The evidence is Hardware-Verified and Regression-Tested for the tested setup,
not a release or broad RF acceptance claim.

## Stage 2 — multirate research, not production WFM

The Python/scipy designer generated a floating response oracle and Q15
coefficients, checking ripple, stopband, and accumulator bounds. Speed alone
would not justify fixed-point arithmetic that aliases RF energy or overflows.
Deterministic impulse, CW, multitone, band-edge, adjacent/fold-zone
interferers, full-scale tone/DC, noise, clipped input and alternating extrema
exposed gain, alias and overflow errors that short live listening could miss.
Fixed versus irregular chunking tested whether one continuous stream survives
API boundaries; exact one-second and long-window accounting tested for
periodic loss/duplication and eventual pitch drift.

Candidate D used sparse symmetric Q15 halfbands and a Q15 rational filter.
It passed numerical/filter, exact-rate and chunk tests. All four one-second
plans produced exactly 240,000 outputs. The 30-minute 168,344-iteration
soak had zero mismatches but **reinitialized** each iteration, so it is not
continuous-state evidence. The separate 12-minute persistent-state run
initialized each pair once, then fed one continuous deterministic IQ stream
in full versus irregular 1–7,001-sample pieces without resetting. It covered
71,930 blocks and 288,153 non-four-aligned pieces with zero output/count
mismatches under a priority-8 aggressor. The final count was exact or +1
initial phase-carry sample. This establishes streaming continuity, not RF or
audio acceptance.

Candidate D's CU8/dither frequency sweep covered 140–155 points per rate:

| MS/s | Measured passband gain through ±100 kHz | Transition at ±110/120/130/135 kHz (dB) | Worst sampled stop peak ≥140 kHz |
|---:|---:|---|---:|
| 2.40 | −0.05 to +0.05 dB | −2.46 / −7.78 / −21.62 / −33.05 | −60.56 dB |
| 2.56 | −0.04 to +0.05 dB | −2.45 / −7.77 / −21.67 / −33.15 | −60.65 dB |
| 2.88 | −0.04 to +0.04 dB | −2.41 / −7.76 / −21.84 / −33.51 | −61.95 dB |
| 3.20 | −0.04 to +0.05 dB | −2.43 / −7.76 / −21.73 / −33.29 | −60.97 dB |

The 2.88 log has a serial-capture gap. Stopband peaks approach the CU8/dither
measurement floor; these samples do not prove device rejection at every
frequency. Separately, analytic/Q15 coefficient response met the recorded
roughly 0.1 dB ripple and 60 dB rejection design targets.

An *exclusive live* test retained real RTL-SDR USB streaming, display,
spectrum and level work. The candidate processed every IQ block but discarded
its output; the existing FM discriminator/stereo/RDS/audio was skipped for
those blocks. This measured prospective replacement cost under actual Tab5
scheduling, not production audio. Offline timing was insufficient because
live p95/p99 latency tails were much wider.

| Candidate / physical rate | Frontend avg | Total DSP avg | Window / consequence |
|---|---:|---:|---|
| D / 2.40 MS/s | 4.372 ms | 5.293 ms | 77% load, no sustained drops, but already over the earlier 70% target without audio |
| D2 / 2.40 MS/s | about 3.55 ms | see audit | Specialization helped; still an exclusive test |
| D2 / 2.56 MS/s | 3.689 ms | 4.591 ms | 71% load and zero backlog/drops in a clean ~84 s window, without audio |
| D2 / 3.20 MS/s | 3.888 ms | 4.945 ms | About 94% of a 5.12 ms block interval, +268 pipeline drops |
| D3 / 3.20 MS/s | 3.763 ms | 4.822 ms | About 90% load, backlog and +426 pipeline drops; 9 USB overrun/drop counters |

D2 specialized D's same mathematics to separate generic overhead from
necessary filter work. It materially improved 2.40 MS/s, but could not make
3.20 MS/s WFM safe. D3 inserted an additional /2 Q15 halfband before the
rational filter (3.20M → 400k → 3/5 → 240k). Its final polyphase cost fell,
but HB3 consumed almost all the saving: offline net improvement only about
0.09 ms/block. Targeted D3 exact-count and fixed/irregular continuity tests
passed all four rates; its 3.20 live frontend still had wide tails, backlog
and loss. No D4 or production WFM integration was attempted. ARP4/ESP-DSP
exploration did not pass full-chain numeric/continuity gates; preemption was
not established as the root cause. PIE findings remain experimental.

**Stage 2 conclusion:** numerical/filter and streaming-state correctness:
YES for D, and targeted D3 checks; D2 improved 2.40 implementation cost;
universal 3.20 MS/s WFM realtime headroom: NO. Research completed. The
tested multirate frontends are not accepted as the universal production WFM
frontend on the current Tab5. This is an application CPU/DSP budget result,
**not** a finding that 3.20 MS/s USB acquisition is impossible.

## Code disposition and production-rate policy

| Change | Class | Final disposition / reason |
|---|---|---|
| Stage-1 hot state, level/clipping, RDS batching, DSP-owned resets, `noinline`, stats and overload yield | KEEP — production | Bit-identical and measured improvement/safety |
| Stage-1 A/B harness | KEEP — lab only | `ORCSDR_DSP_AB=0` removes buffers, commands and hooks from normal build |
| D/D2/D3, coefficients, designer and frontend lab | KEEP — lab only | `ORCSDR_DSP_LAB=0` omits lab sources; no frontend object/task/table is linked |
| Exclusive-live routing and `RTL_DSP LAB` commands | KEEP — lab only | Preprocessor-gated; normal demod/audio executes unchanged |
| Lab serial rate command and lab-only `RTL_STOP` authorization | KEEP — lab only | Preprocessor-gated; unavailable in normal firmware |
| Existing RF Lab rate override | KEEP — production | Predates Stage 2; explicit RF Lab acquisition control, not audio qualification |
| D/D2/D3 as production WFM frontend or default rate | REJECT | Never integrated; 3.20 live gate failed |

Normal `rtl_default_sample_rate()` selects 2.40 MS/s acquisition for FM,
NFM/Weather, AM, CB, Shortwave and Browse; 960 kS/s for other narrow/protocol
modes except 2.048 MS/s ADS-B. Existing WFM demod creates 240 kS/s MPX and
48 kHz output audio. 3.20 MS/s is Hardware-Verified as bounded *acquisition*
and may support future raw IQ, spectrum or scans only after each workload's
own acceptance. 3.20 MS/s WFM audio is unsupported by this production path.
3.20 MS/s NFM/WX, AM and SSB/CB audio are Not Verified, not proven impossible;
future mode-specific low-channel-rate designs may differ from WFM.

## Reproduction, evidence and open gates

The historical lab serial commands were `RTL_DSP LAB COUNT 1 12`,
`RTL_DSP LAB STREAMSOAK 1 12`, `RTL_DSP LAB TEST 12 3`,
`RTL_DSP LAB SWEEP 3 12`, `RTL_DSP LAB BENCH 128 12 3`,
`RTL_DSP LAB LIVE D3 INTERNAL`, and `RTL_DSP LAB RATE 3200000`.
Candidate D's full persistent-state transcript is local/untracked at
`artifacts/dsp-stage2/streamsoak-d-12min-float-preempt.txt`; sweep, D2 and D3
transcripts are likewise local/untracked under `artifacts/dsp-stage2/`.
[The detailed audit](../dsp/DSP_ARCHITECTURE_AUDIT.md) preserves exact
measurements and limitations. Raw transcripts are not published by this
commit. A future reproduction must use suitable antenna/RF sources and
separate build, flash, hardware, RF and release gates.

## Normal closeout verification

`& .\tools\build-tab5-idf.ps1` from `apps/orcsdr-tab5` passed on 2026-09-27.
The generated configure command set `ORCSDR_DSP_AB=0`,
`ORCSDR_DSP_LAB=0`, stage timing `1` and embedded the pinned Hosted 3.0.6
C6 image. The P4 binary was 3,745,152 bytes, SHA-256
`3F829E0E5C357D7D02687A4C8C4DF7C659FBFABCCB60D397503397F9F15E2ED6`.
This build was from `a20def7` plus the local D3 and closeout changes, before
the final commit. It does not itself verify hardware behavior.
After the FM SMART-gain hot-retune fix below, the normal C6-including rebuild
passed; its 3,745,152-byte P4 binary SHA-256 is
`60AD467750D48D743C5EB182E9697C413AFDFD4B7B1A740CDAFB8CC6F081800D`.
`idf.py -B build-native-hosted3 -p COM17 flash` passed on the owner P4,
including per-image flash hashes and hard reset. This was a second P4 flash,
not a C6 firmware update.

`python -m unittest discover -s tests -p test_documentation_truth.py -v`
passed 11 tests; `python tools/check_documentation_truth.py` had 0 errors and
0 warnings; `python tools/help_media.py validate` returned
`HELP_MEDIA_VALIDATE_OK`; `mkdocs build --strict` passed; `git diff --check`
passed. These validate documentation and source formatting, not radio/RF.

The P4 was flashed via `idf.py -B build-native-hosted3 -p COM17 flash` after
COM17 enumerated as the ESP32-P4 USB serial device (revision 1.3). Esptool
verified flash hashes and hard-reset the board. No C6 flash was performed.
`RTL_HEALTH` then showed `reset_reason=11` (the deliberate flash reset),
not a new watchdog reset. FM autostart readback was exactly 2,400,000 S/s,
with `RTL_START ESP_OK` and `RTL_WBFM_DSP ... decim=10/5`.

One 82.5-second FM counter window processed 2,399,988 samples/s at 52% load,
3.560 ms average/block, queue high-water 0, backlog 0 and overload yields 0.
IQ/USB loss counters stayed at six each through that window; audio dropped
chunks, ring overruns and submit failures stayed zero. After a UI tune and
radio restart, an 18.3-second window averaged 3.100 ms/block and 43% load;
its three pipeline/two driver drops and ten backlog blocks occurred in a
window containing transitions, not a clean steady-state result. These bounded
windows do not show catastrophic regression toward the old ~68% baseline.
Neither serial counters nor an unchanging reset reason prove audible pitch,
moving spectrum or suitable-station RDS.

Hosted is an **open independent failure**: boot logged `ESP-Hosted 3.0.6
unavailable`, and `RTL_WIFI_STATUS` reported `hosted_match=0`, although
`RTL_WIFI_C6_STATUS` read host/coprocessor 3.0.6, embedded image present and
`match=1`. The canonical `test-tab5-wifi-release.ps1 -Port COM17
-TimeoutSeconds 30` timed out waiting for Hosted match; its preserved local
log is `apps/orcsdr-tab5/tools/wifi-release-20260927-085318.log`. It recorded
`RTL_WIFI_BLOCKED hosted_init_or_version` and an FM restart at 2.40 MS/s after
the attempted scan. No DSP or C6 migration change was made in response.

The operator reported a moving spectrum but **no audible FM sound**. Initially
the Tab5 detected an attached audio cable and correctly muted its internal
amplifier. After that cable was unplugged, `AUDIO_CODEC` read
`hp_detect=0 amp_pin=1 muted=0 running=1`, yet the operator still heard no
sound. `RTL_SOUND` returned enabled=1, UI volume was 245, and playback
reported zero submit failures. The FM audible smoke gate therefore **failed**;
the cause was not established by those counters. A short built-in diagnostic
tone was clearly audible after the operator lowered volume, proving the
speaker/codec/playback route could work. The operator then rebooted the Tab5;
on 96.1 MHz, normal-pitch FM audio was audible and RDS worked. The operator
also saw moving spectrum. The post-reboot serial record showed stereo lock,
`RDS_STATUS block_locked=1`, PS `KZEL`, PI `5277`, RadioText, and zero audio
dropped chunks. `RTL_HEALTH reset_reason=7` reflects that deliberate software
reboot, not a task-watchdog result. This is bounded RF-Verified evidence for
the owner Tab5, Blog V4 and this FM station, **not** proof that the earlier
silence cannot recur. The reboot was necessary for recovery in this run and
the cause remains Not Verified. The operator subsequently confirmed audio in
Weather/NFM, AM, and CB AM/LSB/USB; there was no CB traffic, so these were
audio-path checks rather than CB RF reception proof. The operator also stopped
and restarted FM and hot-retuned back to 96.1 MHz: audio and the moving
spectrum recovered without another reboot. These successful transitions do
not explain or erase the earlier startup silence.

## FM SMART-gain hot-retune follow-up

The owner reported a noticeable gain-search delay on each V4 FM step.
`request_hot_retune_for()` had restarted FM SMART gain for every changed FM
frequency. `service_audio_auto_gain()` then reapplied the lowest tuner gain
and waited 500 ms per search step. The two-line restart trigger was removed
from FM hot retune; the existing LO apply/demod reset remains. Initial FM
session start and explicit SMART selection still request gain selection, and
steady-state clipping can still reduce gain. AM behavior is unchanged.
Build/flash passed as recorded above. The no-reset serial monitor observed
the V4 hot-retune through 96.2–96.6 MHz and back to 96.1 MHz, with the LO
applied on each step and no `RTL_FM_AUTO_GAIN start` event during those steps.
The operator confirmed the gain behavior is "much better." This verifies the
reported step-delay improvement on this device; post-step audio/RDS recovery
remains a separate operator check.

RF Lab remains an open production integration check. The operator reported
that DSP/sound suffered while its LIVE page played FM at 96.1 MHz. A fresh
23.6-second RF Lab window at 2.40 MS/s measured 64% DSP load and 252 us/block
in the spectrum stage, versus earlier regular-FM windows of 43–52% load;
it added no IQ pipeline, driver, or audio-drop counters and had zero queue
backlog. RF Lab also enables a separate 2048-point analysis at a 33 ms
interval with a 100 ms live display cadence. These measurements identify
extra work, but do not yet establish the cause of the audible symptom.
The operator subsequently switched through AM/CB/FM: regular FM audio was
clear. On returning to RF Lab, the sound-quality issue did not recur. The
initial symptom is therefore intermittent and not reproduced by that page
transition; no RF Lab/DSP behavior was changed for this report.

## Clean production candidate and Wi-Fi closeout — 2026-09-27

The preserved old P4 application artifact (`4A650413376D567A6E0E7C89C149F818CC9AACD8EF66D7A882FBB08708196661`) reproduced `esp_wifi_init()` failure `0x102` on two controlled boots. Clean committed `d46ad0b1` builds, and fresh builds of the prior local source modifications, initialized Wi-Fi successfully. No committed PR #114 change or individual local source edit was proven to cause the failure. The precise stale/generated build-state mechanism remains **Not Verified**. Production correction is to build committed source in a fresh build directory; neither TX buffers, CMake cache, the C6, nor DSP may be named as the proven cause.

An isolated clean worktree at `d46ad0b1d2210c265a18f6dea80cc2e77f7fee54` produced the following application with the repository's native ESP-IDF 5.5.4 build script and existing pinned dependencies. The build reported `v5.5.4-dirty` for the installed IDF tree. No temporary Wi-Fi config dump or forced TX-buffer override was present. No source or build configuration changes were made.

| Artifact / setting | Production-candidate result |
|---|---|
| `sdkconfig.defaults` SHA-256 | `C5F66E9DE8CD3B66A67985696A362DEBA560A8EE603F39B457E6B18671834C6A` |
| Generated `sdkconfig` SHA-256 | `FBB640DBE3BF65D480E6D2AC36209D6DA11DF922E9A176492ABD76AE565C4F7C` |
| `dependencies.lock` SHA-256 | `95338FC0B16B2DB5FF49DD7EEAE49A3F366989C107B4E25F744AEDF48ABE83A2` |
| P4 ELF SHA-256 | `2B54DAC861C53EFCD3CBABF9C9EBBDFF90F1974CDE80250A2AB66DDD5F5FB8DC` |
| P4 application BIN SHA-256 | `CD09A4041EDC726F7835DECC94DC35E12B69B3DB83857FF4053677264826C57A` |
| `ORCSDR_DSP_AB` / `ORCSDR_DSP_LAB` / `ORCSDR_DSP_STAGE_TIMING` | `0` / `0` / `1` |

The P4 application was flashed at `0x10000` and verified. The C6, NVS, partition table, bootloader, saved profiles, and credentials were not changed. Ten controlled reboot cycles were captured over COM17. All ten initialized a usable station with SDIO Hosted transport and host/C6 ESP-Hosted 3.0.6. Each had one initial association failure, one scheduled retry, an IP during splash, and Home reporting `station=1 hosted_match=1 stage=none error=0x0`. There were zero observed `0x102` errors, boot loops, or unexpected watchdog resets. This preserves the accepted SD → Wi-Fi startup/retry → RTL-SDR staging → splash gate/Home behavior; it does not imply the first association attempt succeeded.

The normal production firmware retains the accepted Stage-1 DSP changes. The Stage-1 A/B harness and Stage-2 D/D2/D3 multirate lab remain disabled; normal WFM still acquires at 2.40 MS/s and feeds the existing 240 kS/s MPX/48 kHz audio path. 3.20 MS/s WFM audio remains unsupported. In a representative live FM window at 96.1 MHz, `RTL_DSP STATS` returned 2.40 MS/s, 52% DSP load, zero queue backlog, zero IQ pipeline/driver drops, zero audio drops/overruns/submit failures, and no overload yield over 17.03 seconds. The earlier startup-spanning window is not used as a steady-state load measurement. Audible FM, moving spectrum, RDS, and other-mode smoke results must be recorded separately after operator observation.
