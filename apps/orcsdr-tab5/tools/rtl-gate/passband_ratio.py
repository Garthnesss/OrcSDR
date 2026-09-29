"""Passband response of stage X relative to the boot baseline, from raw IQ.

Usage: passband_ratio.py PREFIX   (uses PREFIX-{baseline,bw_200000,bw_0,retune}.cu8)
Ratio = smoothed PSD(stage) / smoothed PSD(baseline), in dB, at fixed offsets from the
requested 96.1 MHz. It is a filter-response estimate only where the input is the same
signal at both times (a stationary station or noise floor); the carrier at 0 Hz and the
moving RDS/audio lobes are excluded by using wide smoothing. Gain must be constant
between the compared stages (it is within one gate run)."""
import sys
import numpy as np

RATE, NFFT = 2_400_000, 8192


def psd(path):
    raw = np.fromfile(path, dtype=np.uint8).astype(np.float32)
    iq = ((raw[0::2] - 127.5) + 1j * (raw[1::2] - 127.5)) / 127.5
    n = len(iq) // NFFT
    w = np.hanning(NFFT)
    fr = iq[: n * NFFT].reshape(n, NFFT) * w
    p = np.mean(np.abs(np.fft.fftshift(np.fft.fft(fr, axis=1), axes=1)) ** 2, axis=0)
    f = np.fft.fftshift(np.fft.fftfreq(NFFT, 1 / RATE))
    return f, p


def smooth(f, p, centre, half=25e3):
    m = np.abs(f - centre) <= half
    return 10 * np.log10(np.mean(p[m]) + 1e-20)


prefix = sys.argv[1]
stages = ["baseline", "bw_200000", "bw_0", "retune"]
data = {s: psd(f"{prefix}-{s}.cu8") for s in stages}
offs = [-1100, -900, -800, -700, -600, -500, -400, -300, -200, -150, 150, 200, 300, 400, 500,
        600, 700, 800, 900, 1100]
print(f"{'offset kHz':>10}" + "".join(f"{s:>12}" for s in ["boot dB", "200k-boot", "AUTO-boot", "retune-boot"]))
for o in offs:
    b = smooth(*data["baseline"], o * 1e3)
    row = [b] + [smooth(*data[s], o * 1e3) - b for s in stages[1:]]
    print(f"{o:>10}" + "".join(f"{v:>12.1f}" for v in row))
