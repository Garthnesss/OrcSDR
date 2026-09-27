#!/usr/bin/env python3
"""Design the Stage-2 multirate frontend filters and emit dsp/frontend_coeffs.hpp.

Chain (Candidate B): CU8 -> halfband /2 -> halfband /2 -> rational L/M polyphase
(which is also the channel filter) -> 240 kS/s complex baseband for WFM.

Specs (complex baseband, symmetric around DC):
  channel passband  |f| <= 100 kHz, ripple <= 0.1 dB (polyphase stage)
  alias-free edge   |f| >= 140 kHz must be rejected >= 60 dB before any
                    decimation can fold it into |f| <= 100 kHz at 240k
  halfbands         only protect |f| <= 140 kHz from folding (>= 60 dB);
                    they are designed at the tightest rate (2.40 MS/s) and
                    reused at 2.56/2.88/3.20, where they have more margin.

Run:  python tools/dsp/design_frontend.py            (writes the header)
      python tools/dsp/design_frontend.py --report   (prints specs only)
"""
from __future__ import annotations

import argparse
import math
from pathlib import Path

import numpy as np
from scipy import signal

PASS_HZ = 100e3
STOP_HZ = 140e3
ATTEN_DB = 60.0
RIPPLE_DB = 0.1
OUT_RATE = 240e3
PLANS = [  # device rate, intermediate after /4, L, M
    (2_400_000, 600_000, 2, 5),
    (2_560_000, 640_000, 3, 8),
    (2_880_000, 720_000, 1, 3),
    (3_200_000, 800_000, 3, 10),
]
Q15 = 32768


def halfband(taps: int, fs: float, edge_hz: float) -> np.ndarray:
    """Equiripple halfband (taps = 4k+3): zero every other tap except the center."""
    assert taps % 4 == 3
    # Equiripple design with symmetric bands is a halfband up to rounding;
    # force the exact structure (zeros at even offsets, 0.5 center) and make
    # the DC gain exactly 1: 0.5 + 2 * sum(side taps) = 1.
    fp = edge_hz / fs
    h = signal.remez(taps, [0, fp, 0.5 - fp, 0.5], [1, 0], fs=1.0)
    c = taps // 2
    for k in range(taps):
        d = k - c
        if d != 0 and d % 2 == 0:
            h[k] = 0.0
    h[c] = 0.5
    side = h[c + 1::2].copy()
    side *= 0.25 / side.sum()
    h[c + 1::2] = side
    h[c - 1::-2] = side
    return h


def response_db(h: np.ndarray, fs: float, freqs: np.ndarray) -> np.ndarray:
    _, H = signal.freqz(h, worN=freqs, fs=fs)
    return 20 * np.log10(np.maximum(np.abs(H), 1e-12))


def hb_metrics(h: np.ndarray, fs: float) -> tuple[float, float]:
    """(passband ripple dB over |f|<=STOP_HZ, worst rejection over the fold band)."""
    pb = response_db(h, fs, np.linspace(0, STOP_HZ, 400))
    fold = response_db(h, fs, np.linspace(fs / 2 - STOP_HZ, fs / 2, 400))
    return float(pb.max() - pb.min()), float(-fold.max())


def smallest_halfband(fs: float) -> np.ndarray:
    for taps in range(7, 200, 4):
        h = halfband(taps, fs, STOP_HZ)
        ripple, rej = hb_metrics(h, fs)
        if rej >= ATTEN_DB:
            return h
    raise RuntimeError("no halfband met spec")


def polyphase_proto(rate_in: int, L: int) -> np.ndarray:
    fs = rate_in * L
    wp = 10 ** (RIPPLE_DB / 20) - 1
    ws = 10 ** (-ATTEN_DB / 20)
    for taps in range(15, 2000, 1):
        if taps % L:
            continue
        try:
            h = signal.remez(taps, [0, PASS_HZ, STOP_HZ, fs / 2], [1, 0],
                             weight=[1 / wp, 1 / ws], fs=fs, maxiter=100)
        except Exception:
            continue
        pb = response_db(h, fs, np.linspace(0, PASS_HZ, 400))
        sb = response_db(h, fs, np.linspace(STOP_HZ, fs / 2, 4000))
        if pb.max() - pb.min() <= 2 * RIPPLE_DB and -sb.max() >= ATTEN_DB:
            return h * L  # unity passband gain after zero-stuffing by L
    raise RuntimeError("no prototype met spec")


def q15(values: np.ndarray) -> np.ndarray:
    return np.clip(np.round(values * Q15), -Q15, Q15 - 1).astype(np.int32)


def c_array(name: str, ctype: str, values, per_line: int = 8) -> str:
    if ctype == "float":
        items = [f"{v:.9e}f" for v in values]
    else:
        items = [str(int(v)) for v in values]
    lines = [", ".join(items[i:i + per_line]) for i in range(0, len(items), per_line)]
    body = ",\n    ".join(lines)
    return f"inline constexpr {ctype} {name}[{len(items)}] = {{\n    {body}}};\n"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--out", default=str(Path(__file__).resolve().parents[2] / "dsp" / "frontend_coeffs.hpp"))
    args = ap.parse_args()

    hb1 = smallest_halfband(2_400_000)
    hb2 = smallest_halfband(1_200_000)
    report = []
    for name, h, fs in (("HB1", hb1, 2.4e6), ("HB2", hb2, 1.2e6)):
        ripple, rej = hb_metrics(h, fs)
        hq = q15(h) / Q15
        rq, jq = hb_metrics(hq, fs)
        nz = int(np.count_nonzero(np.abs(h) > 0))
        report.append(f"{name}: {len(h)} taps ({nz} nonzero, {(nz - 1) // 2} unique side taps) "
                      f"ripple {ripple:.4f} dB, fold rejection {rej:.1f} dB (float) / {jq:.1f} dB (Q15); "
                      f"sum|h| {np.abs(h).sum():.4f}")
    protos = []
    for rate, mid, L, M in PLANS:
        h = polyphase_proto(mid, L)
        fs = mid * L
        pb = response_db(h / L, fs, np.linspace(0, PASS_HZ, 400))
        sb = response_db(h / L, fs, np.linspace(STOP_HZ, fs / 2, 4000))
        hq = q15(h) / Q15
        sbq = response_db(hq / L, fs, np.linspace(STOP_HZ, fs / 2, 4000))
        per_phase = len(h) // L
        protos.append((rate, mid, L, M, h))
        report.append(f"PP {rate / 1e6:.2f}M: {mid // 1000}k x{L}/{M} proto {len(h)} taps "
                      f"({per_phase}/phase) ripple {pb.max() - pb.min():.4f} dB "
                      f"stop {-sb.max():.1f} dB (float) / {-sbq.max():.1f} dB (Q15); "
                      f"MAC per out {2 * per_phase}; max|h| {np.abs(h).max():.4f}")
    print("\n".join(report))
    if args.report:
        return

    out = ["// GENERATED by tools/dsp/design_frontend.py - do not edit by hand.",
           "// Stage-2 multirate frontend benchmark filters (see docs/dsp/DSP_ARCHITECTURE_AUDIT.md).",
           "//"]
    out += [f"// {line}" for line in report]
    out += ["#pragma once", "", "#include <cstdint>", "", "namespace orcsdr::dsp::coeffs {", ""]
    out.append(f"inline constexpr float kPassHz = {PASS_HZ:.1f}f;")
    out.append(f"inline constexpr float kStopHz = {STOP_HZ:.1f}f;")
    out.append(f"inline constexpr float kAttenDb = {ATTEN_DB:.1f}f;")
    out.append("")
    for name, h in (("kHb1", hb1), ("kHb2", hb2)):
        out.append(c_array(f"{name}F", "float", h))
        out.append(c_array(f"{name}Q15", "int16_t", q15(h)))
    for rate, mid, L, M, h in protos:
        tag = f"kPp{rate // 10000}"
        out.append(c_array(f"{tag}F", "float", h))
        out.append(c_array(f"{tag}Q15", "int16_t", q15(h)))
    out.append("struct PpPlan {\n  uint32_t device_rate;\n  uint32_t mid_rate;\n  uint16_t L;\n"
               "  uint16_t M;\n  const float* proto_f;\n  const int16_t* proto_q15;\n  uint16_t taps;\n};")
    items = []
    for rate, mid, L, M, h in protos:
        tag = f"kPp{rate // 10000}"
        items.append(f"    {{{rate}u, {mid}u, {L}, {M}, {tag}F, {tag}Q15, {len(h)}}}")
    out.append("inline constexpr PpPlan kPlans[] = {\n" + ",\n".join(items) + "};")
    out.append("")
    out.append("}  // namespace orcsdr::dsp::coeffs")
    Path(args.out).write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
