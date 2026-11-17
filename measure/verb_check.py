#!/usr/bin/env python3
"""verb_check.py: measure a reverb impulse response the way rooms are measured.

  ~/Developer/listen/.venv/bin/python measure/verb_check.py ir.wav [--want 2.5]

  RT60 per octave band (Schroeder backward integration, T20 fit from -5 to -25 dB, x3),
  mixing time (normalised echo density reaches 1, Abel & Huang 2006), and the stereo correlation.
"""
import json
import sys

import numpy as np
import soundfile as sf
from scipy.signal import butter, sosfiltfilt


def rt60(x, sr):
    e = np.cumsum((x ** 2)[::-1])[::-1]
    e = 10 * np.log10(e / e[0] + 1e-30)
    t = np.arange(len(e)) / sr
    i5 = np.argmax(e <= -5)
    i25 = np.argmax(e <= -25)
    if i25 <= i5:
        return None
    slope = np.polyfit(t[i5:i25], e[i5:i25], 1)[0]  # dB per second
    return -60.0 / slope if slope < 0 else None


def echo_density(x, sr, win_ms=20):
    """Normalised echo density profile; returns the time (ms) it first reaches 1 (fully mixed)."""
    w = int(sr * win_ms / 1000)
    win = np.hanning(w)
    win /= win.sum()
    out = []
    for i in range(0, min(len(x), int(0.5 * sr)) - w, w // 4):
        seg = x[i:i + w]
        sd = np.sqrt(np.sum(win * seg ** 2))
        if sd == 0:
            out.append((i, 0))
            continue
        frac = np.sum(win * (np.abs(seg) > sd))
        out.append((i, frac / 0.3173))  # erfc(1/sqrt 2): Gaussian noise gives 1
    for i, d in out:
        if d >= 0.97:
            return 1000 * (i + w / 2) / sr
    return None


def main():
    path = sys.argv[1]
    x, sr = sf.read(path)
    L, R = x[:, 0], x[:, 1]
    bands = {}
    for fc in (125, 500, 2000, 8000):
        lo, hi = fc / np.sqrt(2), min(fc * np.sqrt(2), sr / 2 * 0.95)
        sos = butter(4, [lo, hi], btype="band", fs=sr, output="sos")
        bands[fc] = rt60(sosfiltfilt(sos, L), sr)
    full = rt60(L, sr)
    corr = float(np.corrcoef(L[int(0.05 * sr):], R[int(0.05 * sr):])[0, 1])
    res = {"rt60": round(full, 2) if full else None, "bands": {k: (round(v, 2) if v else None) for k, v in bands.items()},
           "mixing_ms": echo_density(L, sr), "lr_corr": round(corr, 3), "peak": float(np.max(np.abs(x)))}
    print(json.dumps(res))


if __name__ == "__main__":
    main()
