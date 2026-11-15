#!/usr/bin/env python3
"""fit_punish.py: after fit_drive.py, set each voice's punishDb so Punish (at Drive 5) matches the
reference's small-signal gain. Updates measure/fits_drive/<style>.json in place."""
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fit_drive as F  # noqa: E402
import subprocess  # noqa: E402

for style in sys.argv[1:] or ["A", "E", "N", "T", "P"]:
    path = os.path.join(HERE, "fits_drive", style + ".json")
    if not os.path.exists(path):
        continue
    fit = json.load(open(path))
    ref = json.load(open(os.path.join(HERE, "profiles", "decap", style + "_d5_punish.json")))
    target = {r["in_dbfs"]: r["gain_db"] for r in ref["stair1k"]}[-30]
    v = dict(fit["voice"])
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "p.wav")
        lo, hi = 0.0, 40.0
        for _ in range(12):
            v["punishDb"] = (lo + hi) / 2
            json.dump(v, open(out + ".json", "w"))
            subprocess.run([F.RENDER, F.STIM, out, "--voice-json", out + ".json", "--drive", "5", "--punish", "1"], check=True)
            got = {r["in_dbfs"]: r["gain_db"] for r in F.AD.analyze(F.DRY, F.AD.load(out), F.S)["stair1k"]}[-30]
            lo, hi = (v["punishDb"], hi) if got < target else (lo, v["punishDb"])
    fit["voice"]["punishDb"] = round((lo + hi) / 2, 2)
    json.dump(fit, open(path, "w"), indent=1)
    print("%s punishDb = %.2f (target gain %.2f dB)" % (style, fit["voice"]["punishDb"], target))
