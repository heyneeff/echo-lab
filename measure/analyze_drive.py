#!/usr/bin/env python3
"""analyze_drive.py: turn drive captures into profiles, measured against the dry chain.

  ~/Developer/listen/.venv/bin/python measure/analyze_drive.py [--tag decap] [run ...]

Per run -> profiles/<tag>/<run>.json:
  curve     : static transfer curve y(x) at 41 points over [-1, 1] (from the 50 Hz full-scale sine),
              plus hysteresis (how much the up/down halves differ: memory / filtering in the loop)
  stair1k / stair100 : per input level -> gain dB, THD %, harmonics 2..9 (dB re fundamental)
  sweepLo / sweepHi : magnitude response (dB re 1 kHz), 1/3-octave bands
  smpte, ccif : intermodulation products (dB re main tone)
"""
import argparse
import json
import os

import numpy as np
import soundfile as sf

HERE = os.path.dirname(os.path.abspath(__file__))
SR = 44100
BANDS = [f for f in (25 * 2 ** (i / 3) for i in range(30)) if f < 18000]


def db(x):
    return 20 * np.log10(np.maximum(np.abs(x), 1e-12))


def load(path):
    x, sr = sf.read(path, dtype="float64", always_2d=True)
    assert sr == SR
    return x.mean(1)


def align(ref, y):
    """Integer-sample offset of y relative to ref (plugin latency), from the first 3 s."""
    n = int(3 * SR)
    a, b = ref[:n], y[:n]
    c = np.fft.irfft(np.fft.rfft(b, 2 * n) * np.conj(np.fft.rfft(a, 2 * n)))
    lag = int(np.argmax(np.abs(c[:4000])))
    return lag


def seg(x, s, lag=0, pad_start=0.0, pad_end=0.0):
    a = int((s["start"] + pad_start) * SR) + lag
    b = int((s["start"] + s["length"] - pad_end) * SR) + lag
    return x[a:b]


def harmonics(y, f0):
    n = len(y)
    spec = np.abs(np.fft.rfft(y * np.blackman(n)))

    def amp(f):
        i = int(round(f * n / SR))
        return spec[max(i - 3, 0):i + 4].max() if i < len(spec) - 4 else 1e-12
    fund = amp(f0)
    h = {str(k): round(float(db(amp(k * f0) / fund)), 1) for k in range(2, 10) if k * f0 < SR / 2 - 200}
    thd = np.sqrt(sum((amp(k * f0) / fund) ** 2 for k in range(2, 10) if k * f0 < SR / 2 - 200))
    return fund, h, float(thd)


def band_levels(x, nfft=1 << 18):
    sp = np.abs(np.fft.rfft(x, nfft))
    fr = np.fft.rfftfreq(nfft, 1 / SR)
    out = []
    for fc in BANDS:
        m = (fr >= fc / 2 ** (1 / 6)) & (fr < fc * 2 ** (1 / 6))
        out.append(np.sqrt((sp[m] ** 2).mean()) if m.any() else 1e-12)
    return np.array(out)


def analyze(dry, y, S):
    lag = align(dry, y)
    out = {"latency_samples": lag}
    # static curve: bin output vs input over the middle of the 50 Hz sine
    xd = seg(dry, S["static"], 0, 0.3, 0.3)
    yd = seg(y, S["static"], lag, 0.3, 0.3)
    peak = np.abs(xd).max()
    xn = xd / peak
    edges = np.linspace(-1, 1, 42)
    centers = (edges[:-1] + edges[1:]) / 2
    rising = np.gradient(xn) > 0
    curve, hyst = [], []
    for lo, hi in zip(edges[:-1], edges[1:]):
        m = (xn >= lo) & (xn < hi)
        if not m.any():
            curve.append(None)
            hyst.append(None)
            continue
        curve.append(round(float(np.median(yd[m]) / peak), 5))
        up, dn = m & rising, m & ~rising
        hyst.append(round(float(np.median(yd[up]) - np.median(yd[dn])) / peak, 5) if up.any() and dn.any() else 0.0)
    out["curve"] = {"x": [round(float(c), 4) for c in centers], "y": curve, "hysteresis": hyst}
    # staircases
    for name in ("stair1k", "stair100"):
        s = S[name]
        rows = []
        for i, lv in enumerate(s["steps"]):
            sub = {"start": s["start"] + i * s["step_s"], "length": s["step_s"]}
            xs = seg(dry, sub, 0, 0.08, 0.08)
            ys = seg(y, sub, lag, 0.08, 0.08)
            fx, _, _ = harmonics(xs, s["freq"])
            fy, h, thd = harmonics(ys, s["freq"])
            rows.append({"in_dbfs": lv, "gain_db": round(float(db(fy / fx)), 2),
                         "thd_pct": round(thd * 100, 3), "harmonics_db": h})
        out[name] = rows
    # tone curves
    for name in ("sweepLo", "sweepHi"):
        xs, ys = seg(dry, S[name], 0, 0.02, 0.02), seg(y, S[name], lag, 0.02, 0.02)
        r = db(band_levels(ys) / band_levels(xs))
        ref = r[int(np.argmin(np.abs(np.array(BANDS) - 1000)))]
        out[name] = {str(round(f)): round(float(v - ref), 2) for f, v in zip(BANDS, r)}
        out[name + "_gain_db"] = round(float(ref), 2)
    # intermodulation
    for name, (f1, f2) in (("smpte", (60, 7000)), ("ccif", (1000, 1200))):
        ys = seg(y, S[name], lag, 0.1, 0.1)
        n = len(ys)
        sp = np.abs(np.fft.rfft(ys * np.blackman(n)))

        def amp(f):
            i = int(round(f * n / SR))
            return sp[max(i - 2, 0):i + 3].max()
        if name == "smpte":
            main = amp(f2)
            prods = sum(amp(f2 + k * f1) ** 2 + amp(f2 - k * f1) ** 2 for k in (1, 2))
        else:
            main = amp(f1)
            prods = sum(amp(f) ** 2 for f in (f2 - f1, 2 * f1 - f2, 2 * f2 - f1))
        out[name + "_imd_db"] = round(float(db(np.sqrt(prods) / main)), 1)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", default="decap")
    ap.add_argument("runs", nargs="*")
    a = ap.parse_args()
    S = json.load(open(os.path.join(HERE, "stimuli", "drive.json")))
    cap = os.path.join(HERE, "captures", a.tag)
    dry = load(os.path.join(cap, "dry.wav"))
    outdir = os.path.join(HERE, "profiles", a.tag)
    os.makedirs(outdir, exist_ok=True)
    runs = a.runs or sorted(f[:-4] for f in os.listdir(cap) if f.endswith(".wav") and f != "dry.wav")
    print("%-12s %7s %7s %7s %8s %8s %6s  curve y(±1)" % ("run", "g@-30", "g@-6", "g@0", "thd@-6", "thd@0", "H2/H3@-6"))
    for r in runs:
        p = analyze(dry, load(os.path.join(cap, r + ".wav")), S)
        json.dump(p, open(os.path.join(outdir, r + ".json"), "w"), indent=1)
        st = {row["in_dbfs"]: row for row in p["stair1k"]}
        h = st[-6]["harmonics_db"]
        cy = p["curve"]["y"]
        print("%-12s %7.2f %7.2f %7.2f %8.2f %8.2f %3.0f/%-3.0f  %s / %s" % (
            r, st[-30]["gain_db"], st[-6]["gain_db"], st[0]["gain_db"], st[-6]["thd_pct"], st[0]["thd_pct"],
            h["2"], h["3"], cy[0], cy[-1]), flush=True)


if __name__ == "__main__":
    main()
