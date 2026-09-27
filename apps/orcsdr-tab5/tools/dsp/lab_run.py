#!/usr/bin/env python3
"""Send one DSP lab command over COM17 without resetting the Tab5.

Example: python tools/dsp/lab_run.py 'RTL_DSP LAB CONT' --until LAB_CONT_DONE
Requires pyserial; optionally pass --log to retain the device transcript.
"""

import argparse
from contextlib import nullcontext
import sys
import time
from pathlib import Path

import serial


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("command")
    ap.add_argument("--port", default="COM17")
    ap.add_argument("--until", required=True)
    ap.add_argument("--timeout", type=float, default=300)
    ap.add_argument("--log", type=Path)
    args = ap.parse_args()

    port = serial.Serial(port=None, baudrate=115200, timeout=0.5)
    port.dtr = False
    port.rts = False
    port.port = args.port
    with port:
        port.reset_input_buffer()
        port.write((args.command + "\n").encode("ascii"))
        deadline = time.monotonic() + args.timeout
        with args.log.open("x", encoding="utf-8") if args.log else nullcontext() as log:
            while time.monotonic() < deadline:
                line = port.readline().decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                print(line, flush=True)
                if args.log:
                    print(line, file=log, flush=True)
                if args.until in line:
                    return 0
    print(f"timeout waiting for {args.until}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
