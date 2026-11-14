#!/usr/bin/env python3
"""fit.py: fit our engine's style settings to a measured reference profile, by rendering.

  ~/Developer/listen/.venv/bin/python measure/fit.py "Space Echo" [--ref echoboy]
  env: FIT_EVALS (per stage, default 120), FIT_POLES (1-4 playback filter poles, or "auto")

Staged, like an engineer would do it. Each stage fits only the settings that shape one
measurement, holding the rest:
  1. tone   : playback filters (wetHP, wetLP, poles)       vs the single-echo response
  2. loop   : feedback gain, loop HP/LP/shelf/peak           vs per-repeat loop transfer
  3. drive  : saturation drive/knee/bias, output level       vs gain + harmonics at 4 levels
  4. wobble : wow/flutter depth (rate taken from measurement) vs pitch deviation
Writes measure/fits/<style>.json (values, per-stage errors, our final profile).
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
import analyze as A  # noqa: E402

RENDER = os.path.join(ROOT, "build", "render")
SEG = json.load(open(os.path.join(HERE, "stimuli", "stimuli.json")))
EVALS = int(os.environ.get("FIT_EVALS", "120"))

# name: (initial, lo, hi, log-scale)
PARAMS = {
    "wetHPHz": (20, 10, 600, True), "wetLPHz": (16000, 1500, 21000, True),
    "wetBrickHz": (18000, 2000, 21000, True),
    "loopGain": (1.0, 0.6, 1.4, False), "loopHPHz": (30, 5, 600, True),
    "loopLPHz": (18000, 1500, 21000, True), "loopTiltDb": (2.0, 0.0, 12.0, False),
    "shelfHz": (1500, 300, 8000, True), "loopPeakHz": (450, 150, 3000, True),
    "loopPeakDb": (0.0, -6.0, 6.0, False), "loopPeakQ": (0.8, 0.3, 3.0, True),
    "satDrive": (1.0, 0.05, 8.0, True), "satKnee": (0.7, 0.0, 0.97, False),
    "satBias": (0.0, 0.0, 0.4, False), "outputDb": (-3.0, -15, 9, False),
}
STAGES = [
    ("tone", ["wetHPHz", "wetLPHz", "wetBrickHz"]),
    ("loop", ["loopGain", "loopHPHz", "loopLPHz", "loopTiltDb", "shelfHz",
              "loopPeakHz", "loopPeakDb", "loopPeakQ"]),
    ("drive", ["satDrive", "satKnee", "satBias", "outputDb"]),
]


def render(style, feedback, stim, out):
    sj = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
    json.dump(style, sj)
    sj.close()
    subprocess.run([RENDER, stim, out, "--style-json", sj.name, "--time", "250",
                    "--feedback", str(feedback), "--mix", "1"], check=True)
    os.unlink(sj.name)


def prof(style, fb, tmp, want=("A", "B")):
    out = {}
    if "A" in want:
        a = os.path.join(tmp, "A.wav")
        render(style, fb * style["loopGain"], os.path.join(HERE, "stimuli", "A_repeats.wav"), a)
        out["repeats"] = A.analyze_repeats(a, SEG["A"])
    if "B" in want:
        b = os.path.join(tmp, "B.wav")
        render(style, 0.0, os.path.join(HERE, "stimuli", "B_single.wav"), b)
        out["single"] = A.analyze_single(b, SEG["B"])
    return out


def err_tone(ref, ours):
    rr, ro = ref["single"]["response_db"], ours["single"]["response_db"]
    return sum((max(rr[k], -60) - max(ro[k], -60)) ** 2 for k in rr if 80 <= float(k) <= 16000)


def err_loop(ref, ours):
    lr, lo = ref["repeats"]["loop_transfer_db"], ours["repeats"]["loop_transfer_db"]
    rr = ref["single"]["response_db"]
    return sum((lr[k] - lo[k]) ** 2 for k in lr if 40 <= float(k) <= 14000 and rr.get(k, 0) > -30)


def err_drive(ref, ours):
    e = 0.0
    for a, b in zip(ref["single"]["levels"], ours["single"]["levels"]):
        e += 4.0 * (a["gain_db"] - b["gain_db"]) ** 2
        if a["in_dbfs"] >= -18:
            for h in ("2", "3", "5"):
                e += 0.02 * (max(a["harmonics_db"][h], -100) - max(b["harmonics_db"][h], -100)) ** 2
    return e


ERRS = {"tone": (err_tone, ("B",)), "loop": (err_loop, ("A", "B")), "drive": (err_drive, ("B",))}


def fit_stage(name, keys, style, ref, fb, tmp):
    errf, want = ERRS[name]
    best = {"e": 1e18, "style": dict(style)}

    def unpack(v):
        st = dict(style)
        for x, k in zip(v, keys):
            _, lo, hi, lg = PARAMS[k]
            st[k] = float(np.clip(np.exp(x) if lg else x, lo, hi))
        return st

    def f(v):
        st = unpack(v)
        try:
            e = errf(ref, prof(st, fb, tmp, want))
        except Exception:
            e = 1e12
        if e < best["e"]:
            best.update(e=e, style=st)
        return e
    starts = [dict(style)]
    if name == "tone":  # coarse grid first: tone filters can sit anywhere from 1.5 to 20 kHz
        grid = [(hp, lp, br) for hp in (20, 80, 200) for lp in (1500, 2500, 4000, 7000, 14000)
                for br in (5000, 7000, 9000, 13000, 20000)]
        scored = []
        for hp, lp, br in grid:
            st = dict(style, wetHPHz=hp, wetLPHz=lp, wetBrickHz=br)
            try:
                scored.append((errf(ref, prof(st, fb, tmp, want)), st))
            except Exception:
                pass
        scored.sort(key=lambda r: r[0])
        best.update(e=scored[0][0], style=scored[0][1])
        starts = [scored[0][1], scored[1][1]]
    if name == "drive":  # saturation has local minima: also start from softer knees
        starts += [dict(style, satKnee=k, satDrive=d) for k, d in ((0.3, 1.0), (0.5, 1.5), (0.0, 0.7))]
    for st0 in starts:
        x0 = [np.log(st0[k]) if PARAMS[k][3] else st0[k] for k in keys]
        minimize(f, x0, method="Nelder-Mead",
                 options={"maxfev": EVALS // len(starts) + 20, "xatol": 1e-3, "fatol": 0.02})
    print("  %-5s e=%8.2f  %s" % (name, best["e"], {k: round(best["style"][k], 3) for k in keys}),
          flush=True)
    return best["style"], best["e"]


def main():
    style_name = sys.argv[1]
    ref_tag = sys.argv[sys.argv.index("--ref") + 1] if "--ref" in sys.argv else "echoboy"
    ref = json.load(open(os.path.join(HERE, "profiles", ref_tag, style_name + ".json")))
    meta = json.load(open(os.path.join(HERE, "captures", ref_tag, style_name + ".json")))
    fb = float(meta.get("A_feedback", "0.62"))
    poles_env = os.environ.get("FIT_POLES", "auto")
    pole_options = [1, 2, 3, 4] if poles_env == "auto" else [int(poles_env)]
    with tempfile.TemporaryDirectory() as tmp:
        results = []
        for poles in pole_options:
            style = {k: v[0] for k, v in PARAMS.items()}
            style.update(name="fit", satType=3, wetLPPoles=poles)
            errs = {}
            for _round in range(2):  # two passes: later stages change what earlier ones see
                for name, keys in STAGES:
                    style, errs[name] = fit_stage(name, keys, style, ref, fb, tmp)
            results.append((sum(errs.values()), poles, style, dict(errs)))
            print(" poles=%d total %.2f" % (poles, results[-1][0]), flush=True)
        total, poles, style, errs = min(results, key=lambda r: r[0])
        # stage 4: modulation (rate from the measurement, depth by bisection on rendered output)
        lv = ref["single"]["levels"][1]
        target, rate = lv["flutter_cents_rms"], (lv["flutter_rates_hz"] or [0])[0]
        if target > 0.3:
            key = "flutterCents" if rate >= 3.0 else "wowCents"
            style.update({"flutterRateHz": rate} if key == "flutterCents" else {"wowRateHz": max(rate, 0.3)})
            lo, hi = 0.0, max(target * 4, 2.0)
            for _ in range(12):
                style[key] = (lo + hi) / 2
                got = prof(style, fb, tmp, ("B",))["single"]["levels"][1]["flutter_cents_rms"]
                lo, hi = (style[key], hi) if got < target else (lo, style[key])
            print("  wobble %s %.2f cents @ %.2f Hz (target %.2f rms)" % (key, style[key], rate, target))
        final = prof(style, fb, tmp)
    os.makedirs(os.path.join(HERE, "fits"), exist_ok=True)
    json.dump({"style": style_name, "values": style, "errors": errs, "total": total,
               "ref_feedback_display": fb, "ours": final},
              open(os.path.join(HERE, "fits", style_name + ".json"), "w"), indent=1)
    print("fit %s: poles %d  errors %s" % (style_name, poles, {k: round(v, 1) for k, v in errs.items()}))


if __name__ == "__main__":
    main()
