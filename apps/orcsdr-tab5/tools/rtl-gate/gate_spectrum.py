"""V3c gate IQ snapshots (.cu8): spectra, carrier offset, left/right balance.

Usage: gate_spectrum.py PREFIX [PREFIX_B]   (two prefixes -> before/after figure)
Carrier offset: power-weighted centroid of the station (+/-120 kHz window around the
strongest near-centre peak, re-centred twice), noise-floor subtracted. The WBFM
spectrum centroid tracks the carrier; the max-bin 'peak' is reported only for reference.
Offset is the station position minus 0 Hz (the requested 96.1 MHz)."""
import json, os, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

RATE, NFFT = 2_400_000, 4096
ORDER = ["baseline", "bw_200000", "bw_0", "retune", "step"]
LABEL = {"baseline": "baseline (AUTO, after boot)", "bw_200000": "BW 200 kHz",
         "bw_0": "BW back to AUTO", "retune": "after retune (Tab5: +100/-100 kHz; PC: explicit 96.1)",
         "step": "after +100/-100 kHz step"}


def psd(path):
    raw = np.fromfile(path, dtype=np.uint8).astype(np.float32)
    iq = ((raw[0::2] - 127.5) + 1j * (raw[1::2] - 127.5)) / 127.5
    n = len(iq) // NFFT
    win = np.hanning(NFFT)
    frames = iq[: n * NFFT].reshape(n, NFFT) * win
    p = np.mean(np.abs(np.fft.fftshift(np.fft.fft(frames, axis=1), axes=1)) ** 2, axis=0)
    p /= np.sum(win ** 2)
    p[NFFT // 2] = 0.5 * (p[NFFT // 2 - 1] + p[NFFT // 2 + 1])  # suppress DC spur
    f = np.fft.fftshift(np.fft.fftfreq(NFFT, 1 / RATE))
    return f, p, iq


def band_db(f, p, lo, hi):
    m = (f >= lo) & (f < hi)
    return 10 * np.log10(np.mean(p[m]))


def carrier(f, p):
    near = np.abs(f) < 0.3e6
    smooth = np.convolve(p, np.ones(9) / 9, mode="same")
    c = f[near][np.argmax(smooth[near])]
    peak = c
    for _ in range(3):
        m = np.abs(f - c) <= 120e3
        w = np.clip(p[m] - np.percentile(p, 20), 0, None)
        c = float(np.sum(f[m] * w) / np.sum(w))
    return c, peak


def analyse(prefix):
    rows = {}
    for s in ORDER:
        path = f"{prefix}-{s}.cu8"
        if not os.path.exists(path):
            continue
        f, p, iq = psd(path)
        c, peak = carrier(f, p)
        left, right = band_db(f, p, -1.0e6, -0.15e6), band_db(f, p, 0.15e6, 1.0e6)
        rows[s] = dict(f=f, pdb=10 * np.log10(p + 1e-20), carrier_khz=c / 1e3,
                       peak_khz=peak / 1e3, left_db=left, right_db=right,
                       r_minus_l_db=right - left,
                       noise_db=float(np.percentile(10 * np.log10(p + 1e-20), 20)),
                       clip_pct=float(100 * np.mean((np.abs(iq.real) > 0.99) |
                                                    (np.abs(iq.imag) > 0.99))))
    return rows


def table(name, rows):
    print(f"-- {name}")
    print(f"{'stage':<11}{'carrier kHz':>12}{'maxbin kHz':>11}{'left dB':>9}{'right dB':>9}"
          f"{'R-L dB':>8}{'floor dB':>9}{'clip %':>8}")
    for s, r in rows.items():
        print(f"{s:<11}{r['carrier_khz']:12.1f}{r['peak_khz']:11.1f}{r['left_db']:9.1f}"
              f"{r['right_db']:9.1f}{r['r_minus_l_db']:8.1f}{r['noise_db']:9.1f}{r['clip_pct']:8.3f}")


def main(prefixes):
    runs = [(os.path.basename(p), analyse(p)) for p in prefixes]
    for name, rows in runs:
        table(name, rows)
        with open(f"{[p for p in prefixes if os.path.basename(p) == name][0]}-metrics.json", "w") as fh:
            json.dump({s: {k: v for k, v in r.items() if k not in ("f", "pdb")}
                       for s, r in rows.items()}, fh, indent=2)
    stages = [s for s in ORDER if all(s in rows for _, rows in runs)]
    cols = len(runs)
    fig, axes = plt.subplots(len(stages), cols, figsize=(8 * cols, 2.9 * len(stages)),
                             sharex=True, sharey="row", squeeze=False)
    for ci, (name, rows) in enumerate(runs):
        for ri, s in enumerate(stages):
            r, ax = rows[s], axes[ri][ci]
            ax.plot(r["f"] / 1e6, r["pdb"], lw=0.6)
            ax.axvline(0, color="k", lw=0.8, ls=":")
            ax.axvline(r["carrier_khz"] / 1e3, color="tab:red", lw=1.0)
            ax.axvspan(-1.0, -0.15, color="tab:red", alpha=0.05)
            ax.axvspan(0.15, 1.0, color="tab:green", alpha=0.05)
            ax.set_title(f"{name}\n{LABEL[s]}: carrier {r['carrier_khz']:+.1f} kHz, "
                         f"R-L {r['r_minus_l_db']:+.1f} dB", fontsize=9)
            ax.grid(alpha=0.3)
            if ci == 0:
                ax.set_ylabel("dBFS/bin")
    for ax in axes[-1]:
        ax.set_xlabel("offset from requested 96.1 MHz (MHz); red = carrier estimate")
    fig.tight_layout()
    out = (prefixes[-1] if cols == 1 else prefixes[-1] + "-vs-baseline") + "-spectrum.png"
    fig.savefig(out, dpi=100)
    print(out)


if __name__ == "__main__":
    main(sys.argv[1:])
