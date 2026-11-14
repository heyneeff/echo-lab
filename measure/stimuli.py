#!/usr/bin/env python3
"""Generate the test signals used to study an echo (EchoBoy as reference, then ours).

  ~/Developer/listen/.venv/bin/python measure/stimuli.py

A_repeats.wav : one click at 0.5 s, then silence (run with feedback on: per-repeat analysis)
B_single.wav  : 1 kHz tones at -30/-18/-6/0 dBFS (2 s each, 1 s gaps), a 20 Hz-20 kHz log sweep
                (4 s), pink noise (1 s), then 3 s silence (noise floor). Run with feedback 0.
Segment start times are written to stimuli.json for the analysis.
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


def main():
    os.makedirs(OUT, exist_ok=True)
    seg = {}

    # A: click + long decay window
    a = np.zeros(int(SR * 8.0))
    i = int(SR * 0.5)
    a[i:i + 2] = [db(-6), -db(-12)]  # short broadband click
    seg["A"] = {"click": 0.5, "length": len(a) / SR}
    sf.write(os.path.join(OUT, "A_repeats.wav"), np.stack([a, a], 1).astype(np.float32), SR,
             subtype="FLOAT")

    # B: tones, sweep, noise, silence
    parts, t, info = [], 0.0, {"tones": []}

    def add(x, label=None):
        nonlocal t
        if label:
            info[label] = t
        parts.append(x)
        t += len(x) / SR

    add(np.zeros(int(SR * 0.5)))
    for lvl in (-30, -18, -6, 0):
        n = int(SR * 2.0)
        tt = np.arange(n) / SR
        ramp = np.minimum(1, np.minimum(tt, tt[::-1]) / 0.01)  # 10 ms fades
        info["tones"].append({"start": t, "dbfs": lvl})
        add(db(lvl) * np.sin(2 * np.pi * 1000 * tt) * ramp)
        add(np.zeros(int(SR * 1.0)))
    n = int(SR * 4.0)
    tt = np.arange(n) / SR
    f0, f1 = 20.0, 20000.0
    k = np.log(f1 / f0)
    sweep = db(-12) * np.sin(2 * np.pi * f0 * 4.0 / k * (np.exp(tt / 4.0 * k) - 1))
    sweep *= np.minimum(1, np.minimum(tt, tt[::-1]) / 0.01)
    add(sweep, "sweep")
    add(np.zeros(int(SR * 1.0)))
    rng = np.random.default_rng(1)
    w = rng.standard_normal(int(SR * 1.0))
    f = np.fft.rfft(w)
    f[1:] /= np.sqrt(np.arange(1, len(f)))  # pink
    pink = np.fft.irfft(f, len(w))
    pink = pink / np.abs(pink).max() * db(-12)
    add(pink, "pink")
    add(np.zeros(int(SR * 1.0)))
    add(np.zeros(int(SR * 3.0)), "silence")
    b = np.concatenate(parts)
    info["length"] = len(b) / SR
    seg["B"] = info
    sf.write(os.path.join(OUT, "B_single.wav"), np.stack([b, b], 1).astype(np.float32), SR,
             subtype="FLOAT")
    json.dump(seg, open(os.path.join(OUT, "stimuli.json"), "w"), indent=1)
    print("A %.1fs, B %.1fs -> %s" % (seg["A"]["length"], info["length"], OUT))


if __name__ == "__main__":
    main()
