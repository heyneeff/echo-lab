#!/usr/bin/env python3
"""capture_drive.py: run the drive test signal through the saturator on track "Decapitator".

  ~/Developer/listen/.venv/bin/python measure/capture_drive.py [--tag decap]

Grid: styles A E N T P x Drive 2/5/8, plus Punish on (style A, drive 5). AutoGain off, tone flat,
mix 100 %. Takes land in captures/<tag>/<style>_d<drive>[_punish].wav (+ .json with settings).
"""
import argparse
import json
import os

from capture import HERE, Live, ensure_resample, ensure_stim, take

TRACK = "Decapitator"
STYLES = ["A", "E", "N", "T", "P"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", default="decap")
    ap.add_argument("--only", default="")
    a = ap.parse_args()
    seg = json.load(open(os.path.join(HERE, "stimuli", "drive.json")))
    live = Live()
    live.cmd("set_tempo", tempo=120.0)
    dev = live.track(TRACK)
    if dev is None:
        raise SystemExit("no track named Decapitator")
    live.cmd("set_track_monitoring", track_index=dev, monitoring="in")
    beats = (seg["length"] + 0.5) * 2  # 120 BPM
    stim = ensure_stim(live, "_stimD", "D_drive.wav", round(beats + 1), target=TRACK)
    rec = ensure_resample(live)
    outdir = os.path.join(HERE, "captures", a.tag)
    os.makedirs(outdir, exist_ok=True)
    runs = [("dry", 0, False)] + [(s, d, False) for s in STYLES for d in (2, 5, 8)] + [(s, 5, True) for s in STYLES]
    for style, drive, punish in runs:
        name = "%s_d%d%s" % (style, drive, "_punish" if punish else "") if style != "dry" else "dry"
        if a.only and a.only not in name:
            continue
        if style == "dry":  # chain reference: plugin switched off
            live.setp(dev, {"Device On": 0.0})
            take(live, stim, rec, seg["length"], os.path.join(outdir, "dry.wav"))
            live.setp(dev, {"Device On": 1.0})
            print("captured dry", flush=True)
            continue
        shown = live.setp(dev, {
            # Style is a 0..4 list parameter (A E N T P); the others are 0..1
            "Style": float(STYLES.index(style)), "Drive": drive / 10.0, "Punish": 1.0 if punish else 0.0,
            "LowCut": 0.0, "Tone": 0.5, "HighCut": 1.0, "Mix": 1.0, "AutoGain": 0.0,
            "LowThump": 0.0, "HighSlope": 0.0, "OutputTrim": 1.0})
        take(live, stim, rec, seg["length"], os.path.join(outdir, name + ".wav"))
        json.dump({"style": style, "drive": drive, "punish": punish, "shown": shown},
                  open(os.path.join(outdir, name + ".json"), "w"), indent=1)
        print("captured", name, shown, flush=True)


if __name__ == "__main__":
    main()
