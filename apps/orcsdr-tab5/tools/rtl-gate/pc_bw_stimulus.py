"""Black-box PC stimulus for the V3c live tuner-bandwidth capture (clean-room: the
RTL-SDR Blog release rtlsdr.dll is driven only through its public API; USBPcap is the
evidence). One continuous open session:

  open -> 2.4 MS/s -> 96.1 MHz -> manual gain 2.7 dB -> baseline IQ
  -> BW 200 kHz -> IQ -> BW AUTO(0) -> IQ -> explicit tune 96.1 MHz -> IQ
  -> +100 kHz -> -100 kHz (same as the Tab5 gate) -> IQ -> close

Each API call is bracketed by a >=1.5 s idle gap and logged with a wall-clock epoch
so the pcap can be cut per stage. Usage: pc_bw_stimulus.py DLL OUT_PREFIX
"""
import ctypes as C
import json
import sys
import time

RATE, FREQ, GAIN = 2_400_000, 96_100_000, 27
KEEP, SETTLE = 4_800_000, 262_144   # bytes: 1 s kept, ~55 ms discarded after a change

dll_path, prefix = sys.argv[1], sys.argv[2]
lib = C.CDLL(dll_path)
dev = C.c_void_p()
events = []


def mark(name, **kw):
    ev = dict(t=time.time(), stage=name, **kw)
    events.append(ev)
    print(json.dumps(ev), flush=True)


def call(name, fn, *args):
    time.sleep(1.5)
    t0 = time.time()
    rc = fn(dev, *args)
    mark(name, t_start=t0, t_end=time.time(), rc=rc, args=[a.value if hasattr(a, "value") else a for a in args])
    if rc != 0:
        print(f"WARNING {name} rc={rc}", flush=True)
    return rc


def read_iq(stage):
    time.sleep(0.5)
    n = C.c_int(0)
    junk = (C.c_ubyte * SETTLE)()
    lib.rtlsdr_read_sync(dev, junk, SETTLE, C.byref(n))
    buf = (C.c_ubyte * KEEP)()
    t0 = time.time()
    rc = lib.rtlsdr_read_sync(dev, buf, KEEP, C.byref(n))
    open(f"{prefix}-{stage}.cu8", "wb").write(bytes(buf)[: n.value])
    mark(f"iq_{stage}", t_start=t0, t_end=time.time(), rc=rc, bytes=n.value)


count = lib.rtlsdr_get_device_count()
m, p, s = (C.create_string_buffer(256) for _ in range(3))
lib.rtlsdr_get_device_usb_strings(0, m, p, s)
mark("enumerate", count=count, manufacturer=m.value.decode(errors="replace"),
     product=p.value.decode(errors="replace"), serial=s.value.decode(errors="replace"))
if count < 1:
    sys.exit("no RTL-SDR present")

time.sleep(1.5)
rc = lib.rtlsdr_open(C.byref(dev), 0)
mark("open", rc=rc)
if rc != 0:
    sys.exit(f"open failed rc={rc}")
try:
    call("set_sample_rate", lib.rtlsdr_set_sample_rate, C.c_uint32(RATE))
    call("set_center_freq_96100000", lib.rtlsdr_set_center_freq, C.c_uint32(FREQ))
    call("set_tuner_gain_mode_manual", lib.rtlsdr_set_tuner_gain_mode, C.c_int(1))
    call("set_tuner_gain_27", lib.rtlsdr_set_tuner_gain, C.c_int(GAIN))
    call("reset_buffer", lib.rtlsdr_reset_buffer)
    read_iq("baseline")
    call("set_tuner_bandwidth_200000", lib.rtlsdr_set_tuner_bandwidth, C.c_uint32(200_000))
    read_iq("bw_200000")
    call("set_tuner_bandwidth_0", lib.rtlsdr_set_tuner_bandwidth, C.c_uint32(0))
    read_iq("bw_0")
    call("set_center_freq_96100000_explicit", lib.rtlsdr_set_center_freq, C.c_uint32(FREQ))
    read_iq("retune")
    call("set_center_freq_96200000", lib.rtlsdr_set_center_freq, C.c_uint32(FREQ + 100_000))
    call("set_center_freq_96100000_step", lib.rtlsdr_set_center_freq, C.c_uint32(FREQ))
    read_iq("step")
finally:
    time.sleep(1.5)
    mark("close", rc=lib.rtlsdr_close(dev))
    json.dump(events, open(f"{prefix}-events.json", "w"), indent=1)
