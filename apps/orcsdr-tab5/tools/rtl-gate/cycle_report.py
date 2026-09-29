"""Per-stage report for a V3c bandwidth-cycle gate run.

Usage: cycle_report.py PREFIX [--plot]   (PREFIX-console.log plus PREFIX-<stage>.cu8 files)
Stage windows are cut from the console log: a stage starts at its
'RTL_DRIVER_RESULT action="BW n"' line and ends at 'RTL_DONGLE_GATE_IQ stage=bw_n'.
Register/IF columns come from V3C_DIAG tag=bw_end dumps (diagnostic builds only)."""
import json, os, re, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gate_spectrum as gs

prefix = sys.argv[1]
DO_PLOT = "--plot" in sys.argv[2:]
log_path = prefix + "-console.log"
if not os.path.exists(log_path):
    log_path = prefix + ".log"  # the gate log carries the same lines
lines = open(log_path, errors="replace").read().splitlines()


def band(f, pdb, lo, hi):
    m = (f >= lo) & (f < hi)
    return 10 * np.log10(np.mean(10 ** (pdb[m] / 10)))


def load(stage):
    path = f"{prefix}-{stage}.cu8"
    f, p, iq = gs.psd(path)
    c, _ = gs.carrier(f, p)
    pdb = 10 * np.log10(p + 1e-20)
    return dict(f=f, p=p, pdb=pdb, carrier=c / 1e3,
                left=band(f, pdb, -900e3, -150e3), right=band(f, pdb, 150e3, 900e3),
                floor=float(np.percentile(pdb, 20)))


# ---- stage windows from the console log --------------------------------------
gate_start = next(i for i, l in enumerate(lines) if "RTL_DONGLE_GATE_BEGIN" in l)
stages, cur = [], None
for i in range(gate_start, len(lines)):
    l = lines[i]
    m = re.search(r'RTL_DRIVER_RESULT action="BW (\d+)"', l)
    if m:
        cur = dict(bw=int(m.group(1)), start=i)
        continue
    m = re.search(r"RTL_DONGLE_GATE_IQ stage=(bw_\d+)", l)
    if m and cur:
        cur["end"] = i; cur["name"] = m.group(1); stages.append(cur); cur = None

rx_diag = re.compile(r"V3C_DIAG tag=(\w+) rf=(\d+) pll_if=(\d+) bw_req=(\d+) bw_valid=(\d) err=(-?\d+) "
                     r"n=(\d+) tuner_r00\.\.= ((?:[0-9a-f]{2} )*)\| demod_p1_r15\.\.r1b= ((?:[0-9a-f]{2} ?){7})")
diag_all = []
for i, l in enumerate(lines):
    m = rx_diag.search(l)
    if m:
        tag, rf, pif, bw, valid, err, n, t, d = m.groups()
        diag_all.append(dict(i=i, tag=tag, rf=int(rf), pif=int(pif), bw=int(bw), err=int(err),
                             t=[int(x, 16) for x in t.split()], d=[int(x, 16) for x in d.split()]))
boot = next((d for d in diag_all if d["tag"] == "start"), None)

base = load("baseline")
rows = []
for s in stages:
    seg = lines[s["start"]:s["end"]]
    txt = "\n".join(seg)
    st = load(s["name"])
    dg = [d for d in diag_all if s["start"] <= d["i"] < s["end"] and d["tag"] == "bw_end"]
    drv = [re.search(r"overruns=(\d+) drops=(\d+).*bw_requested_hz=(\d+) bw_applied_hz=(\d+)", l)
           for l in seg if "RTL_DRIVER_STATUS" in l]
    drv = [m for m in drv if m]
    aud = [json.loads(l) for l in seg if l.startswith('{"type":"rtl_audio_test"')]
    rows.append(dict(
        stage=s["name"], bw=s["bw"], st=st,
        tunes=len(re.findall(r"esp_rtl_sdr: tune rf=", txt)),
        hot=len(re.findall(r"hot retune applied", txt)),
        stops=len(re.findall(r"RTL_STOP_REQUESTED|init begin|RTL_WIFI_COEX event=connect", txt)),
        rejected=len(re.findall(r"ctrl record rejected|hot retune EP0 failed|RTL_DRIVER_ERROR", txt)),
        diag=dg[-1] if dg else None,
        ovr=int(drv[-1].group(1)) if drv else None, drops=int(drv[-1].group(2)) if drv else None,
        breq=int(drv[-1].group(3)) if drv else None, bapp=int(drv[-1].group(4)) if drv else None,
        adrop=aud[-1]["audio_drops"] if aud else None, uovr=aud[-1]["usb_overruns"] if aud else None,
        udrop=aud[-1]["usb_drops"] if aud else None))

print(f"stages found: {len(rows)}  (boot dump: {'yes' if boot else 'no'})")
if boot:
    print("boot tuner r0a/r0b = %02x/%02x  demod IF = %s" % (boot["t"][10], boot["t"][11],
          " ".join(f"{x:02x}" for x in boot["d"][4:7])))
print(f"\nbaseline: carrier {base['carrier']:+.1f} kHz  left {base['left']:.1f}  right {base['right']:.1f}  floor {base['floor']:.1f} dB")
hdr = (f"{'stage':<11}{'req/app':>15}{'carrier':>9}{'L dB':>7}{'R dB':>7}{'L-b':>6}{'R-b':>6}"
       f"{'r0a/0b':>9}{'PLL IF':>8}{'IF word':>10}{'tunes':>6}{'hot':>4}{'stop':>5}{'rej':>4}"
       f"{'ovr':>4}{'drp':>4}{'aud':>4}{'usb':>4}")
print(hdr)
for r in rows:
    s, d = r["st"], r["diag"]
    regs = f"{d['t'][10]:02x}/{d['t'][11]:02x}" if d and len(d["t"]) > 11 else "--"
    ifw = " ".join(f"{x:02x}" for x in d["d"][4:7]) if d else "--"
    pif = f"{d['pif']/1e6:.3f}" if d else "--"
    print(f"{r['stage']:<11}{str(r['breq'])+'/'+str(r['bapp']):>15}{s['carrier']:>+9.1f}{s['left']:>7.1f}{s['right']:>7.1f}"
          f"{s['left']-base['left']:>+6.1f}{s['right']-base['right']:>+6.1f}{regs:>9}{pif:>8}{ifw:>10}"
          f"{r['tunes']:>6}{r['hot']:>4}{r['stops']:>5}{r['rejected']:>4}{r['ovr']!s:>4}{r['drops']!s:>4}{r['adrop']!s:>4}{r['udrop']!s:>4}")

# ---- passband response vs boot at fixed offsets ------------------------------
offs = [-1100, -900, -700, -500, -300, -150, 150, 300, 500, 700, 900, 1100]


def sm(f, p, c):
    m = np.abs(f - c) <= 25e3
    return 10 * np.log10(np.mean(p[m]) + 1e-20)


print("\npassband vs boot (dB, 50 kHz windows):")
print(f"{'stage':<11}" + "".join(f"{o:>7}" for o in offs))
for r in rows + [dict(stage="retune", st=load("retune"))]:
    s = r["st"]
    print(f"{r['stage']:<11}" + "".join(f"{sm(s['f'], s['p'], o*1e3) - sm(base['f'], base['p'], o*1e3):>+7.1f}" for o in offs))


# ---- optional all-stage figure -------------------------------------------------
if DO_PLOT:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    names = ["baseline"] + [r["stage"] for r in rows] + ["retune"]
    cols = 3
    rws = (len(names) + cols - 1) // cols
    fig, axes = plt.subplots(rws, cols, figsize=(16, 2.8 * rws), sharex=True, sharey=True, squeeze=False)
    for ax, nm in zip(axes.flat, names):
        s = base if nm == "baseline" else load(nm)
        ax.plot(s["f"] / 1e6, s["pdb"], lw=0.5)
        ax.axvline(0, color="k", lw=0.7, ls=":")
        ax.axvline(s["carrier"] / 1e3, color="tab:red", lw=1.0)
        ax.set_title(f"{nm}: carrier {s['carrier']:+.1f} kHz, L {s['left']:.1f} R {s['right']:.1f} dB", fontsize=9)
        ax.grid(alpha=0.3)
    for ax in axes[-1]:
        ax.set_xlabel("offset from requested 96.1 MHz (MHz)")
    fig.tight_layout()
    out = prefix + "-cycle.png"
    fig.savefig(out, dpi=90)
    print("plot:", out)