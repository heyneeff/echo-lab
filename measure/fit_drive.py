#!/usr/bin/env python3
"""fit_drive.py: fit a DriveVoice to a measured saturator style, by rendering.

  ~/Developer/listen/.venv/bin/python measure/fit_drive.py A [--ref decap]
  env: FIT_EVALS (per stage, default 90)

One voice per style, fitted across Drive 2/5/8 jointly (so the Drive knob tracks the whole range).
Staged: gain staging -> character (knee/asym/bias/emphasis) -> tone (band/tilt), two rounds.
Writes measure/fits_drive/<style>.json.
"""
import json
import os
import subprocess
import sys
import tempfile

import numpy as np
from scipy.optimize import minimize

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import analyze_drive as AD  # noqa: E402

RENDER = os.path.join(ROOT, "build", "render_drive")
STIM = os.path.join(HERE, "stimuli", "D_drive.wav")
S = json.load(open(os.path.join(HERE, "stimuli", "drive.json")))
EVALS = int(os.environ.get("FIT_EVALS", "90"))
DRIVES = (2, 5, 8)

PARAMS = {  # name: (initial, lo, hi, log)
    "baseGainDb": (-3.0, -30, 15, False), "gainPerDriveDb": (3.3, 0.5, 6.0, False),
    "ceiling": (0.34, 0.05, 1.5, True), "outDb": (0.0, -12, 12, False),
    "knee": (0.5, 0.0, 0.97, False), "asym": (0.1, -0.85, 0.85, False), "bias": (0.05, -0.4, 0.4, False), "biasTrack": (0.3, -1.2, 1.2, False),
    "emphHz": (3000, 300, 12000, True), "emphDb": (0.0, -12, 12, False),
    "lowHz": (12, 5, 300, True), "highHz": (20000, 2000, 22000, True), "tiltDb": (0.0, -6, 6, False),
}
STAGES = [
    ("gain", ["baseGainDb", "gainPerDriveDb", "ceiling", "outDb"]),
    ("character", ["knee", "asym", "bias", "biasTrack", "emphHz", "emphDb"]),
    ("tone", ["lowHz", "highHz", "tiltDb"]),
]
DRY = AD.load(STIM)


def render(voice, drive, out):
    vj = out + ".json"
    json.dump(voice, open(vj, "w"))
    subprocess.run([RENDER, STIM, out, "--voice-json", vj, "--drive", str(drive)], check=True)
    return AD.analyze(DRY, AD.load(out), S)


def err(ref, ours, stage):
    e = 0.0
    for d in DRIVES:
        a, b = ref[d], ours[d]
        if stage in ("gain", "character", "all"):
            for name, w in (("stair1k", 4.0), ("stair100", 2.0)):
                for ra, rb in zip(a[name], b[name]):
                    e += w * (ra["gain_db"] - rb["gain_db"]) ** 2
        if stage in ("character", "all"):
            for name in ("stair1k", "stair100"):
                for ra, rb in zip(a[name], b[name]):
                    if ra["in_dbfs"] >= -24:
                        for h in ("2", "3", "5"):
                            e += 0.08 * (max(ra["harmonics_db"][h], -90) - max(rb["harmonics_db"][h], -90)) ** 2
            for ya, yb in zip(a["curve"]["y"], b["curve"]["y"]):
                if ya is not None and yb is not None:
                    e += 40.0 * (ya - yb) ** 2
            e += 0.05 * (a["smpte_imd_db"] - b["smpte_imd_db"]) ** 2 + 0.05 * (a["ccif_imd_db"] - b["ccif_imd_db"]) ** 2
        if stage in ("tone", "all"):
            for name in ("sweepLo", "sweepHi"):
                for k, v in a[name].items():
                    if 60 <= float(k) <= 16500:
                        e += 0.5 * (v - b[name][k]) ** 2
    return e


def main():
    style = sys.argv[1]
    ref_tag = sys.argv[sys.argv.index("--ref") + 1] if "--ref" in sys.argv else "decap"
    ref = {d: json.load(open(os.path.join(HERE, "profiles", ref_tag, "%s_d%d.json" % (style, d)))) for d in DRIVES}
    # polarity straight from the measured curve: rising input -> falling output means inverted
    ys = ref[5]["curve"]["y"]
    invert = (ys[-1] or 0) < (ys[0] or 0)
    voice = {k: v[0] for k, v in PARAMS.items()}
    voice["invert"] = 1 if invert else 0
    # start the clipper where the measurement says it is: the two output ceilings at heavy drive
    yc = ref[8]["curve"]["y"]
    hi, lo = abs(yc[-1] or 0.3), abs(yc[0] or 0.3)          # ends of the curve (|output| at x=+1 / -1)
    pos, neg = (lo, hi) if invert else (hi, lo)               # undo the polarity flip
    voice["ceiling"] = max((pos + neg) / 2, 0.05)
    voice["asym"] = float(np.clip((pos - neg) / max(pos + neg, 1e-6), -0.85, 0.85))
    print("  init from curve: ceiling %.3f asym %+.3f" % (voice["ceiling"], voice["asym"]), flush=True)
    errs = {}
    with tempfile.TemporaryDirectory() as tmp:
        def profile(v):
            return {d: render(v, d, os.path.join(tmp, "d%d.wav" % d)) for d in DRIVES}

        for rnd in range(2):
            for stage, keys in STAGES:
                best = {"e": 1e18, "v": dict(voice)}

                def f(x):
                    v = dict(voice)
                    for xi, k in zip(x, keys):
                        _, lo, hi, lg = PARAMS[k]
                        v[k] = float(np.clip(np.exp(xi) if lg else xi, lo, hi))
                    try:
                        e = err(ref, profile(v), stage)
                    except Exception:
                        e = 1e12
                    if e < best["e"]:
                        best.update(e=e, v=v)
                    return e
                x0 = [np.log(voice[k]) if PARAMS[k][3] else voice[k] for k in keys]
                minimize(f, x0, method="Nelder-Mead", options={"maxfev": EVALS, "xatol": 1e-3, "fatol": 0.05})
                voice = best["v"]
                errs[stage] = best["e"]
                print("  r%d %-9s e=%9.2f %s" % (rnd, stage, best["e"], {k: round(voice[k], 3) for k in keys}), flush=True)
        final = profile(voice)
        total = err(ref, final, "all")
    os.makedirs(os.path.join(HERE, "fits_drive"), exist_ok=True)
    json.dump({"style": style, "voice": voice, "errors": errs, "total": total,
               "ours": {str(d): final[d] for d in DRIVES}},
              open(os.path.join(HERE, "fits_drive", style + ".json"), "w"), indent=1)
    print("fit %s: total %.1f %s" % (style, total, {k: round(v, 1) for k, v in errs.items()}))


if __name__ == "__main__":
    main()
