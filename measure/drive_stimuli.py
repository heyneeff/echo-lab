#!/usr/bin/env python3
"""Test signals for studying a saturator/drive (Decapitator as reference, then ours).

  ~/Developer/listen/.venv/bin/python measure/drive_stimuli.py   -> stimuli/D_drive.wav + drive.json

One file, segments separated by 0.4 s of silence (drives have no tail):
  static   : 50 Hz sine at 0 dBFS, 2 s       -> the transfer curve (output vs input, sample by sample)
  stair1k  : 1 kHz, -36..0 dBFS in 3 dB steps, 0.5 s each -> gain, compression, harmonics per level
  stair100 : 100 Hz, same steps               -> low-frequency behaviour (bass, 'thump')
  sweepLo  : 20 Hz-20 kHz log sweep at -30 dBFS, 3 s -> small-signal tone shaping
  sweepHi  : same at -6 dBFS                  -> tone shaping when driven
  smpte    : 60 Hz + 7 kHz (4:1) at -6 dBFS, 1.5 s  -> intermodulation (bass modulating highs)
  ccif     : 1000 + 1200 Hz at -6 dBFS, 1.5 s -> difference-tone grit on chords
"""
import json
import os

import numpy as np
import soundfile as sf

SR = 44100
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "stimuli")


def db(x):
    return 10 ** (x / 20)


def fade(x, ms=10):
    n = len(x)
    t = np.arange(n)
    r = np.minimum(1, np.minimum(t, t[::-1]) / (SR * ms / 1000))
    return x * r


def main():
    parts, t, info = [], 0.0, {}

    def add(x, label=None, extra=None):
        nonlocal t
        if label:
            info[label] = dict(start=t, length=len(x) / SR, **(extra or {}))
        parts.append(x)
        t += len(x) / SR

    gap = np.zeros(int(SR * 0.4))
    add(gap)
    tt = np.arange(int(SR * 2.0)) / SR
    add(fade(np.sin(2 * np.pi * 50 * tt), 40), "static", {"freq": 50, "dbfs": 0})
    add(gap)
    for name, f in (("stair1k", 1000), ("stair100", 100)):
        steps = list(range(-36, 1, 3))
        seg = []
        for lv in steps:
            tt = np.arange(int(SR * 0.5)) / SR
            seg.append(fade(db(lv) * np.sin(2 * np.pi * f * tt), 8))
        add(np.concatenate(seg), name, {"freq": f, "steps": steps, "step_s": 0.5})
        add(gap)
    for name, lv in (("sweepLo", -30), ("sweepHi", -6)):
        n = int(SR * 3.0)
        tt = np.arange(n) / SR
        k = np.log(20000 / 20)
        add(fade(db(lv) * np.sin(2 * np.pi * 20 * 3.0 / k * (np.exp(tt / 3.0 * k) - 1))), name, {"dbfs": lv})
        add(gap)
    tt = np.arange(int(SR * 1.5)) / SR
    a = db(-6)
    add(fade(a * (0.8 * np.sin(2 * np.pi * 60 * tt) + 0.2 * np.sin(2 * np.pi * 7000 * tt))), "smpte",
        {"f1": 60, "f2": 7000})
    add(gap)
    add(fade(a * 0.5 * (np.sin(2 * np.pi * 1000 * tt) + np.sin(2 * np.pi * 1200 * tt))), "ccif",
        {"f1": 1000, "f2": 1200})
    add(gap)
    x = np.concatenate(parts)
    info["length"] = len(x) / SR
    os.makedirs(OUT, exist_ok=True)
    sf.write(os.path.join(OUT, "D_drive.wav"), np.stack([x, x], 1).astype(np.float32), SR, subtype="FLOAT")
    json.dump(info, open(os.path.join(OUT, "drive.json"), "w"), indent=1)
    print("D_drive.wav %.1fs" % info["length"])


if __name__ == "__main__":
    main()
