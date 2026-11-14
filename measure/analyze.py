#!/usr/bin/env python3
"""analyze.py: turn captures into style profiles.

  ~/Developer/listen/.venv/bin/python measure/analyze.py [--tag echoboy] [style ...]

Per style -> profiles/<tag>/<style>.json :
  repeats   : per-repeat level (dB) and the loop transfer |R[k+1]/R[k]| in 1/3-octave bands
  response  : single-echo magnitude response from the sweep (dB re 1 kHz)
  levels    : per input level: output gain, THD, harmonics 2..10 (dB re fundamental)
  flutter   : pitch deviation of the 1 kHz echo (cents rms / peak) and its strongest rates
  noise     : noise floor (dBFS) and its spectral tilt; hum lines if present
"""
import argparse
import json
import os
import sys

import numpy as np
import soundfile as sf
from scipy.signal import butter, hilbert, sosfiltfilt

HERE = os.path.dirname(os.path.abspath(__file__))
SR = 44100
DELAY = 0.25
BANDS = [25 * 2 ** (i / 3) for i in range(30)]  # 25 Hz .. ~12.7 kHz, 1/3 octave centers
BANDS = [f for f in BANDS if f < 18000]


def db(x):
    return 20 * np.log10(np.maximum(np.abs(x), 1e-12))


def mono(x):
    return x.mean(1) if x.ndim == 2 else x


def band_levels(spec, freqs):
    out = []
    for fc in BANDS:
        lo, hi = fc / 2 ** (1 / 6), fc * 2 ** (1 / 6)
        m = (freqs >= lo) & (freqs < hi)
        out.append(float(np.sqrt((spec[m] ** 2).mean())) if m.any() else 0.0)
    return np.array(out)


def onset(x, thresh_db):
    env = np.abs(x)
    idx = np.where(db(env) > thresh_db)[0]
    return idx[0] if len(idx) else None


# ----------------------------------------------------------------------------- pass A

def analyze_repeats(path, seg):
    x = mono(sf.read(path, dtype="float64")[0])
    first = onset(x, -50)
    if first is None:
        return {"error": "silent"}
    start = max(first - int(0.005 * SR), 0)
    n = int(0.2 * SR)  # 200 ms window per repeat (echo spacing is 250 ms)
    reps, specs = [], []
    nfft = 1 << 14
    freqs = np.fft.rfftfreq(nfft, 1 / SR)
    for k in range(24):
        a = start + int(k * DELAY * SR)
        w = x[a:a + n]
        if len(w) < n:
            break
        e = np.sqrt((w ** 2).sum())
        if db(e) < -70:
            break
        reps.append(float(db(e)))
        specs.append(np.abs(np.fft.rfft(w * np.hanning(n), nfft)))
    loop = []
    for k in range(min(len(specs) - 1, 6)):
        r = band_levels(specs[k + 1], freqs) / np.maximum(band_levels(specs[k], freqs), 1e-12)
        loop.append(db(r))
    loop_db = np.median(np.array(loop), 0) if loop else np.zeros(len(BANDS))
    # spectral centroid per repeat (how fast it darkens)
    cents = [float((s * freqs).sum() / (s.sum() + 1e-12)) for s in specs]
    return {"repeat_db": [round(v, 2) for v in reps],
            "repeat_step_db": [round(reps[i + 1] - reps[i], 2) for i in range(len(reps) - 1)],
            "loop_transfer_db": {str(round(f)): round(float(v), 2) for f, v in zip(BANDS, loop_db)},
            "centroid_hz": [round(c) for c in cents]}


# ----------------------------------------------------------------------------- pass B

def tone_metrics(y, f0=1000.0):
    n = len(y)
    w = np.blackman(n)
    spec = np.abs(np.fft.rfft(y * w))
    freqs = np.fft.rfftfreq(n, 1 / SR)

    def amp(f):
        i = int(round(f * n / SR))
        return spec[max(i - 3, 0):i + 4].max()
    fund = amp(f0)
    harm = {str(h): round(float(db(amp(h * f0) / fund)), 1) for h in range(2, 11) if h * f0 < SR / 2 - 500}
    thd = np.sqrt(sum((amp(h * f0) / fund) ** 2 for h in range(2, 11) if h * f0 < SR / 2 - 500))
    # flutter: instantaneous frequency of the band-passed tone
    sos = butter(4, [f0 * 0.7, f0 * 1.4], "bandpass", fs=SR, output="sos")
    a = hilbert(sosfiltfilt(sos, y))
    ph = np.unwrap(np.angle(a))
    inst = np.diff(ph) * SR / (2 * np.pi)
    inst = inst[int(0.1 * SR):-int(0.1 * SR)]
    dec = inst[::44]  # ~1 kHz rate
    cents = 1200 * np.log2(np.maximum(dec, 1) / np.median(dec))
    cents = cents - cents.mean()
    fs2 = SR / 44
    sp = np.abs(np.fft.rfft(cents * np.hanning(len(cents)), 1 << 15))
    fr = np.fft.rfftfreq(1 << 15, 1 / fs2)
    band = (fr > 0.2) & (fr < 40)
    order = np.argsort(sp * band)[::-1]
    rates = []
    for i in order:
        if not band[i]:
            continue
        if all(abs(fr[i] - r) > 0.3 for r in rates):
            rates.append(float(fr[i]))
        if len(rates) == 3:
            break
    return {"out_rms_dbfs": round(float(db(np.sqrt((y ** 2).mean()) * np.sqrt(2))), 2),
            "thd_pct": round(float(thd * 100), 3), "harmonics_db": harm,
            "flutter_cents_rms": round(float(cents.std()), 2),
            "flutter_cents_pp": round(float(np.percentile(cents, 99) - np.percentile(cents, 1)), 2),
            "flutter_rates_hz": [round(r, 2) for r in rates]}


def analyze_single(path, seg):
    x = mono(sf.read(path, dtype="float64")[0])
    first = onset(x, -55)  # the -30 dBFS tone's echo
    if first is None:
        return {"error": "silent"}
    t0 = seg["tones"][0]["start"]
    off = first / SR - t0  # recording offset including the 250 ms echo delay
    out = {"offset_s": round(off, 3), "levels": []}
    for tone in seg["tones"]:
        a = int((tone["start"] + off + 0.3) * SR)
        b = int((tone["start"] + off + 1.7) * SR)
        m = tone_metrics(x[a:b])
        m["in_dbfs"] = tone["dbfs"]
        m["gain_db"] = round(m["out_rms_dbfs"] - tone["dbfs"], 2)
        out["levels"].append(m)
    # single-echo magnitude response from the sweep (ratio to the clean sweep)
    stim = mono(sf.read(os.path.join(HERE, "stimuli", "B_single.wav"), dtype="float64")[0])
    s0 = seg["sweep"]
    xa = stim[int(s0 * SR):int((s0 + 4.0) * SR)]
    ya = x[int((s0 + off) * SR):int((s0 + off + 4.0) * SR)]
    nfft = 1 << 18
    fr = np.fft.rfftfreq(nfft, 1 / SR)
    X = band_levels(np.abs(np.fft.rfft(xa, nfft)), fr)
    Y = band_levels(np.abs(np.fft.rfft(ya, nfft)), fr)
    resp = db(Y / np.maximum(X, 1e-12))
    ref = resp[np.argmin(np.abs(np.array(BANDS) - 1000))]
    out["response_db"] = {str(round(f)): round(float(v - ref), 2) for f, v in zip(BANDS, resp)}
    # noise floor (silence at the end)
    ns = int((seg["silence"] + off + 0.3) * SR)
    nz = x[ns:ns + int(2.0 * SR)]
    if len(nz) > SR:
        spec = np.abs(np.fft.rfft(nz * np.hanning(len(nz))))
        f2 = np.fft.rfftfreq(len(nz), 1 / SR)
        bl = db(band_levels(spec, f2))
        out["noise"] = {"rms_dbfs": round(float(db(np.sqrt((nz ** 2).mean()))), 1),
                        "bands_db_rel": {str(round(f)): round(float(v - bl.max()), 1)
                                         for f, v in zip(BANDS, bl)},
                        "hum": [h for h in (50, 60, 100, 120, 180)
                                if spec[int(h * len(nz) / SR)] > 8 * np.median(spec[(f2 > 30) & (f2 < 400)])]}
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", default="echoboy")
    ap.add_argument("styles", nargs="*")
    a = ap.parse_args()
    seg = json.load(open(os.path.join(HERE, "stimuli", "stimuli.json")))
    cap = os.path.join(HERE, "captures", a.tag)
    outdir = os.path.join(HERE, "profiles", a.tag)
    os.makedirs(outdir, exist_ok=True)
    styles = a.styles or sorted({f.split("__")[0] for f in os.listdir(cap) if f.endswith(".wav")})
    for st in styles:
        prof = {"style": st}
        pa = os.path.join(cap, st + "__A.wav")
        pb = os.path.join(cap, st + "__B.wav")
        if os.path.exists(pa):
            prof["repeats"] = analyze_repeats(pa, seg["A"])
        if os.path.exists(pb):
            prof["single"] = analyze_single(pb, seg["B"])
        json.dump(prof, open(os.path.join(outdir, st + ".json"), "w"), indent=1)
        r, s = prof.get("repeats", {}), prof.get("single", {})
        lv = s.get("levels", [{}])
        print("%-14s step %s  thd@-6 %s%%  flutter %s c rms %s Hz  noise %s dBFS" % (
            st, r.get("repeat_step_db", [])[:4],
            lv[2].get("thd_pct") if len(lv) > 2 else "-",
            lv[1].get("flutter_cents_rms") if len(lv) > 1 else "-",
            lv[1].get("flutter_rates_hz") if len(lv) > 1 else "-",
            s.get("noise", {}).get("rms_dbfs")), flush=True)


if __name__ == "__main__":
    main()
