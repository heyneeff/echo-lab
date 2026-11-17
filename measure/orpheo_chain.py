#!/usr/bin/env python3
"""orpheo_chain.py: run the demo groove through Prometheus ORPHEO -> Echo Nymph ORPHEO -> Letheverb ORPHEO
(the three ORPHEO presets, chained) and measure how they sit together.

  ~/Developer/listen/.venv/bin/python measure/orpheo_chain.py [--in song.wav]

Reports, for the dry groove and after each stage: level, tone balance by band (watching the 200-500 Hz
"mud" zone and the 4-8 kHz "fizz" zone), stereo width, headroom, and the echo's repeat tail.
"""
import json
import os
import subprocess
import sys
import tempfile

import numpy as np
import soundfile as sf

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SR = 48000
DRIVE = os.path.join(ROOT, "build", "render_drive")
ECHO = os.path.join(ROOT, "build", "plugin", "bin", "Release", "render")
VERB = os.path.join(ROOT, "build", "verb", "render_verb")
ORPHEO_DRIVE = {"gainPerDriveDb": 3.0, "baseGainDb": -8.5, "ceiling": 0.30, "knee": 0.5, "asym": -0.22, "bias": 0.05,
                "biasTrack": 0.2, "lowHz": 25, "highHz": 18000, "tiltDb": -0.5, "outDb": 0.3, "punishDb": 15}
PRESET = {  # mirrors the ORPHEO presets in ui/prototype/faces.html
    "drive": ["--drive", "4.5", "--tone", "0.5", "--mix", "1"],
    "echo": ["--style", "ORPHEO", "--mode", "1", "--time", "188", "--timeR", "250", "--feedback", "0.25", "--mix", "0.28",
             "--sat", "4", "--lowcut", "150", "--duck", "0.35", "--release", "300", "--width", "1", "--tail", "4"],
    "verb": ["--style", "6", "--size", "1.1", "--decay", "2.6", "--pre", "45", "--mix", "0.30", "--damp", "5500",
             "--lowcut", "180", "--duck", "0.25", "--release", "350", "--mod", "1.3", "--early", "0.6", "--secs", "3"],
}


def groove(seconds=8.0):
    """The demo groove from the faces page: kick, snare, hats, a minor stab and a bass line at 120 BPM."""
    rng = np.random.default_rng(7)
    n = int(seconds * SR); L = np.zeros(n); spb = 0.5
    def put(sig, t, pan=0.0):
        i = int(t * SR); j = min(n, i + len(sig)); L[i:j] += sig[:j - i]
    t = lambda d: np.arange(int(d * SR)) / SR
    def kick():
        tt = t(.4); f = 42 + 98 * np.exp(-tt / .03); return np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-tt / .09)
    def snare():
        tt = t(.2); return (rng.standard_normal(len(tt)) * .5 * np.exp(-tt / .045) + .4 * np.sin(2 * np.pi * 190 * tt) * np.exp(-tt / .03))
    def hat():
        tt = t(.05); w = rng.standard_normal(len(tt)); return np.diff(np.concatenate([[0], w])) * .12 * np.exp(-tt / .01)
    def tone(midi, dur, amp, cut):
        tt = t(dur); f = 440 * 2 ** ((midi - 69) / 12); saw = 2 * ((tt * f) % 1) - 1
        a = np.exp(-2 * np.pi * cut / SR); y = np.zeros_like(saw); z = 0.0
        for k in range(len(saw)): z = saw[k] + a * (z - saw[k]); y[k] = z
        return y * amp * np.exp(-tt / (dur / 3))
    bass = [45, 45, 48, 43]
    for b in range(int(seconds / spb)):
        tb = b * spb
        if b % 4 in (0, 2): put(kick(), tb)
        if b % 4 in (1, 3): put(snare(), tb)
        put(hat(), tb + spb / 2)
        if b % 2 == 0: put(tone(bass[(b // 4) % 4] - 12, .22, .5, 900), tb + spb * .5)
        if b % 4 == 3:
            for m in (57, 60, 64): put(tone(m + (2 if b % 16 == 15 else 0), .16, .16, 3500), tb + spb * .75)
    L = L / np.max(np.abs(L)) * 0.5
    return np.stack([L, L], 1).astype(np.float32)


def bands(x):
    m = x.mean(1); Y = np.abs(np.fft.rfft(m * np.hanning(len(m)))) ** 2; f = np.fft.rfftfreq(len(m), 1 / SR)
    tot = Y[(f > 20) & (f < 20000)].sum()
    edges = {"sub<60": (20, 60), "low 60-200": (60, 200), "MUD 200-500": (200, 500), "mid 0.5-2k": (500, 2000),
             "pres 2-4k": (2000, 4000), "FIZZ 4-8k": (4000, 8000), "air 8k+": (8000, 20000)}
    return {k: round(10 * np.log10(Y[(f >= a) & (f < b)].sum() / tot + 1e-20), 1) for k, (a, b) in edges.items()}


def stats(x):
    m, s = x.mean(1), (x[:, 0] - x[:, 1]) / 2
    rms = np.sqrt(np.mean(x ** 2)); pk = np.max(np.abs(x))
    return {"rms_db": round(20 * np.log10(rms + 1e-12), 1), "peak_db": round(20 * np.log10(pk + 1e-12), 1),
            "crest_db": round(20 * np.log10(pk / (rms + 1e-12)), 1),
            "side_vs_mid_db": round(20 * np.log10(np.sqrt(np.mean(s ** 2)) / (np.sqrt(np.mean(m ** 2)) + 1e-12) + 1e-12), 1),
            "lr_corr": round(float(np.corrcoef(x[:, 0], x[:, 1])[0, 1]), 2)}


def echo_tail(tmp):
    """One click through the ORPHEO echo alone (wet only): how fast do the repeats fall away?"""
    click = np.zeros((SR * 6, 2), np.float32); click[0] = .8
    sf.write(tmp + "/click.wav", click, SR, subtype="FLOAT")
    args = [a if a != "0.28" else "1" for a in PRESET["echo"]]
    subprocess.run([ECHO, tmp + "/click.wav", tmp + "/clickout.wav"] + args, check=True, capture_output=True)
    y, _ = sf.read(tmp + "/clickout.wav"); e = np.abs(y[:, 0]); first = e[int(.05 * SR):].max()
    env = np.maximum.accumulate(e[::-1])[::-1]  # remaining-peak envelope
    for thr in (30, 50):
        idx = np.argmax(env < first * 10 ** (-thr / 20))
        print(f"  echo repeats fall {thr} dB below the first repeat after {idx / SR:.2f} s")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        if "--in" in sys.argv:
            x, sr = sf.read(sys.argv[sys.argv.index("--in") + 1], dtype="float32")
            x = x if x.ndim == 2 else np.stack([x, x], 1)
        else:
            x = groove()
        sf.write(tmp + "/0.wav", x, SR, subtype="FLOAT")
        json.dump(ORPHEO_DRIVE, open(tmp + "/v.json", "w"))
        subprocess.run([DRIVE, tmp + "/0.wav", tmp + "/1.wav", "--voice-json", tmp + "/v.json"] + PRESET["drive"], check=True, capture_output=True)
        subprocess.run([ECHO, tmp + "/1.wav", tmp + "/2.wav"] + PRESET["echo"], check=True, capture_output=True)
        subprocess.run([VERB, tmp + "/3.wav", "--in", tmp + "/2.wav"] + PRESET["verb"], check=True, capture_output=True)
        stages = [("dry", "0"), ("+ drive", "1"), ("+ echo", "2"), ("+ reverb", "3")]
        print("%-10s %s" % ("", "  ".join("%-12s" % k for k in bands(x))))
        for name, f in stages:
            y, _ = sf.read(tmp + "/%s.wav" % f, dtype="float32"); y = y[:len(x)]
            b = bands(y); s = stats(y)
            print("%-10s %s   | %s" % (name, "  ".join("%-12s" % v for v in b.values()), s))
        echo_tail(tmp)
        out = os.path.join(ROOT, "build", "orpheo_chain.wav")
        y, _ = sf.read(tmp + "/3.wav", dtype="float32"); sf.write(out, y, SR, subtype="PCM_24")
        print("rendered:", out)


if __name__ == "__main__":
    main()
