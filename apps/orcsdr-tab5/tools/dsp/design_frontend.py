#!/usr/bin/env python3
"""Design the Stage-2 multirate frontend filters and emit dsp/frontend_coeffs.hpp.

Base chain: CU8 -> halfband /2 -> halfband /2 -> rational L/M polyphase.
Targeted D3 experiment: one additional Q15 /2 before the rational stage.
Both end at 240 kS/s complex baseband for WFM; production is unchanged.

Specs (complex baseband, symmetric around DC):
  channel passband  |f| <= 100 kHz, ripple <= 0.1 dB (polyphase stage)
  alias-free edge   |f| >= 140 kHz must be rejected >= 60 dB before any
                    decimation can fold it into |f| <= 100 kHz at 240k
  HB1/HB2           protect |f| <= 140 kHz from folding (>= 60 dB).
  HB3 (/8 only)     protects |f| <= 100 kHz; stopband starts at 200 kHz
                    at the tightest 600 kS/s input. Its Q6 output avoids
                    int16 overflow; the final output restores the 2x gain.

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
DIV8_PLANS = [  # one targeted /8 experiment: device rate, intermediate, L, M
    (2_400_000, 300_000, 4, 5),
    (2_560_000, 320_000, 3, 4),
    (2_880_000, 360_000, 2, 3),
    (3_200_000, 400_000, 3, 5),
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


def hb_metrics(h: np.ndarray, fs: float, guard_hz: float = STOP_HZ) -> tuple[float, float]:
    """(passband ripple dB through guard, worst rejection over the fold band)."""
    pb = response_db(h, fs, np.linspace(0, guard_hz, 400))
    fold = response_db(h, fs, np.linspace(fs / 2 - guard_hz, fs / 2, 400))
    return float(pb.max() - pb.min()), float(-fold.max())


def smallest_halfband(fs: float, guard_hz: float = STOP_HZ) -> np.ndarray:
    for taps in range(7, 200, 4):
        h = halfband(taps, fs, guard_hz)
        ripple, rej = hb_metrics(h, fs, guard_hz)
        if rej >= ATTEN_DB:
            return h
    raise RuntimeError("no halfband met spec")


def polyphase_proto(rate_in: int, L: int) -> np.ndarray:
    fs = rate_in * L
    wp = 10 ** (RIPPLE_DB / 40) - 1
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
        hq = q15(h * L) / (Q15 * L)
        pbq = response_db(hq, fs, np.linspace(0, PASS_HZ, 400))
        sbq = response_db(hq, fs, np.linspace(STOP_HZ, fs / 2, 4000))
        if (pb.max() - pb.min() <= RIPPLE_DB and -sb.max() >= ATTEN_DB and \
                pbq.max() - pbq.min() <= RIPPLE_DB and -sbq.max() >= ATTEN_DB):
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
    hb3 = smallest_halfband(600_000, PASS_HZ)
    report = []
    for name, h, fs, guard in (("HB1", hb1, 2.4e6, STOP_HZ),
                               ("HB2", hb2, 1.2e6, STOP_HZ),
                               ("HB3", hb3, 600e3, PASS_HZ)):
        ripple, rej = hb_metrics(h, fs, guard)
        hq = q15(h) / Q15
        rq, jq = hb_metrics(hq, fs, guard)
        nz = int(np.count_nonzero(np.abs(h) > 0))
        report.append(f"{name}: {len(h)} taps ({nz} nonzero, {(nz - 1) // 2} unique side taps) "
                      f"ripple {ripple:.4f} dB, fold rejection {rej:.1f} dB (float) / {jq:.1f} dB (Q15); "
                      f"sum|h| {np.abs(h).sum():.4f}; delay {(len(h) - 1) / (2 * fs) * 1e6:.2f} us")
    hb1_bound = math.ceil(16384 * np.abs(q15(hb1)).sum() / Q15) + 1
    hb2_bound = math.ceil(hb1_bound * np.abs(q15(hb2)).sum() / Q15) + 1
    hb3_acc_bound = hb2_bound * int(np.abs(q15(hb3)).sum()) + (1 << 15)
    assert hb3_acc_bound < 2**31
    # HB3 stores half-scale Q6 so its conservative any-input bound fits int16.
    hb3_bound = math.ceil(hb3_acc_bound / (2 * Q15))
    assert hb3_bound < 32768
    report.append(f"HB3 bound: acc {hb3_acc_bound} (<2^31), half-scale stage {hb3_bound} (<32768)")
    protos = []
    div8_protos = []
    for rate, mid, L, M in PLANS + DIV8_PLANS:
        h = polyphase_proto(mid, L)
        fs = mid * L
        pb = response_db(h / L, fs, np.linspace(0, PASS_HZ, 400))
        sb = response_db(h / L, fs, np.linspace(STOP_HZ, fs / 2, 4000))
        hq = q15(h) / Q15
        sbq = response_db(hq / L, fs, np.linspace(STOP_HZ, fs / 2, 4000))
        pbq = response_db(hq / L, fs, np.linspace(0, PASS_HZ, 400))
        phase_abs = max(sum(abs(int(v)) for v in q15(h)[p::L]) for p in range(L))
        is_div8 = (rate, mid, L, M) in DIV8_PLANS
        acc_bound = (hb3_bound if is_div8 else hb2_bound) * phase_abs
        assert acc_bound < 2**31
        per_phase = len(h) // L
        (div8_protos if is_div8 else protos).append((rate, mid, L, M, h))
        report.append(f"PP {'/8 ' if is_div8 else ''}{rate / 1e6:.2f}M: {mid // 1000}k x{L}/{M} proto {len(h)} taps "
                      f"({per_phase}/phase) ripple {pb.max() - pb.min():.4f} dB "
                      f"(Q15 {pbq.max() - pbq.min():.4f} dB) "
                      f"stop {-sb.max():.1f} dB (float) / {-sbq.max():.1f} dB (Q15); "
                      f"MAC per out {2 * per_phase}; delay {(len(h) - 1) / (2 * fs) * 1e6:.2f} us; "
                      f"Q15 acc_bound {acc_bound}; max|h| {np.abs(h).max():.4f}")
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
    for name, h in (("kHb1", hb1), ("kHb2", hb2), ("kHb3", hb3)):
        out.append(c_array(f"{name}F", "float", h))
        out.append(c_array(f"{name}Q15", "int16_t", q15(h)))
    for rate, mid, L, M, h in protos:
        tag = f"kPp{rate // 10000}"
        out.append(c_array(f"{tag}F", "float", h))
        out.append(c_array(f"{tag}Q15", "int16_t", q15(h)))
    for rate, mid, L, M, h in div8_protos:
        tag = f"kPpDiv8{rate // 10000}"
        out.append(c_array(f"{tag}F", "float", h))
        out.append(c_array(f"{tag}Q15", "int16_t", q15(h)))
    out.append("struct PpPlan {\n  uint32_t device_rate;\n  uint32_t mid_rate;\n  uint16_t L;\n"
               "  uint16_t M;\n  const float* proto_f;\n  const int16_t* proto_q15;\n  uint16_t taps;\n};")
    items = []
    for rate, mid, L, M, h in protos:
        tag = f"kPp{rate // 10000}"
        items.append(f"    {{{rate}u, {mid}u, {L}, {M}, {tag}F, {tag}Q15, {len(h)}}}")
    out.append("inline constexpr PpPlan kPlans[] = {\n" + ",\n".join(items) + "};")
    items = []
    for rate, mid, L, M, h in div8_protos:
        tag = f"kPpDiv8{rate // 10000}"
        items.append(f"    {{{rate}u, {mid}u, {L}, {M}, {tag}F, {tag}Q15, {len(h)}}}")
    out.append("inline constexpr PpPlan kDiv8Plans[] = {\n" + ",\n".join(items) + "};")
    out.append("")
    out.append("}  // namespace orcsdr::dsp::coeffs")
    Path(args.out).write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
