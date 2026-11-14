#!/usr/bin/env python3
"""capture.py: play the test signals through the echo on track "EchoBoy" and record the result.

  ~/Developer/listen/.venv/bin/python measure/capture.py [--styles "Studio Tape,Space Echo"] [--tag ref]

Setup (done once, see STUDY.md): track "EchoBoy" holds the plugin (monitoring In); stimulus
tracks "_stimA"/"_stimB" are Simplers holding A_repeats.wav / B_single.wav, routed into it.
For each style: pass A (feedback on, click) and pass B (feedback 0, tones/sweep/noise/silence),
Mix 100% (wet only), 250 ms single echo. Takes land in captures/<tag>/<style>__A.wav / __B.wav.
"""
import argparse
import glob
import json
import os
import shutil
import socket
import time

HERE = os.path.dirname(os.path.abspath(__file__))
STIM = os.path.join(HERE, "stimuli")
PROJECT = "/Users/neeff/Ableton Songs/7-1-26 FIRST SONG - Cinematic BYOB  Project"
RECORDED = os.path.join(PROJECT, "Samples", "Recorded")
BPM = 120.0
RESAMPLE = "_resample"


class Live:
    def __init__(self, port=9877):
        self.s = socket.create_connection(("localhost", port), timeout=30)

    def cmd(self, type_, **params):
        self.s.sendall(json.dumps({"type": type_, "params": params}).encode())
        buf = b""
        while True:
            chunk = self.s.recv(65536)
            if not chunk:
                raise ConnectionError("closed")
            buf += chunk
            try:
                r = json.loads(buf.decode())
                break
            except json.JSONDecodeError:
                continue
        if r.get("status") == "error":
            raise RuntimeError(r.get("message"))
        return r.get("result", {})

    def track(self, name):
        n = self.cmd("get_session_info")["track_count"]
        for i in range(n):
            if self.cmd("get_track_info", track_index=i)["name"] == name:
                return i
        return None

    def params(self, t, path=(0,)):
        return self.cmd("ext_get_nested_device_parameters", track_index=t,
                        device_path=list(path))["parameters"]

    def setp(self, t, values, path=(0,)):
        """Set params, then re-read: plugin displays update a moment after the value changes."""
        r = self.cmd("ext_set_device_parameters", track_index=t, device_path=list(path),
                     values=[{"param": k, "value": v} for k, v in values.items()])
        errs = {x["param"]: x["error"] for x in r["results"] if "error" in x}
        time.sleep(0.12)
        now = {p["name"]: p["display"] for p in self.params(t, path)}
        out = {k: now.get(k) for k in values}
        out.update(errs)
        return out


def ensure_stim(live, name, wav, beats):
    t = live.track(name)
    if t is None:
        live.cmd("create_midi_track", index=-1)
        t = live.cmd("get_session_info")["track_count"] - 1
        live.cmd("set_track_name", track_index=t, name=name)
        live.cmd("ext_load_user_library_file", track_index=t, relative_path="Samples/echo-lab/" + wav)
    # unity gain, no velocity, no filter/LFO/pitch env (by display-exact raw values found in setup)
    ps = {p["name"]: p for p in live.params(t)}
    vals = {}
    for nm in ("Vol < Vel",):
        vals[nm] = 0.0
    for nm in ("F On", "L On", "Pe On"):
        vals[nm] = 0.0
    live.setp(t, vals)
    if ps["Volume"]["display"].strip() not in ("0.00 dB", "0.0 dB"):
        lo, hi = ps["Volume"]["min"], ps["Volume"]["max"]
        for _ in range(20):  # bisect Volume to 0 dB
            mid = (lo + hi) / 2
            d = live.setp(t, {"Volume": mid})["Volume"]
            v = float(d.split()[0]) if "inf" not in d else -999
            if abs(v) < 0.05:
                break
            lo, hi = (mid, hi) if v < 0 else (lo, mid)
    live.cmd("set_track_output_routing", track_index=t, routing_type="EchoBoy", routing_channel="")
    info = live.cmd("get_track_info", track_index=t)
    if not info["clip_slots"][0]["has_clip"]:
        live.cmd("create_clip", track_index=t, clip_index=0, length=beats)
        live.cmd("add_notes_to_clip", track_index=t, clip_index=0, notes=[
            {"pitch": 60, "start_time": 0.0, "duration": beats - 0.25, "velocity": 127, "mute": False}])
    return t


def ensure_resample(live):
    t = live.track(RESAMPLE)
    if t is None:
        live.cmd("create_audio_track", index=-1)
        t = live.cmd("get_session_info")["track_count"] - 1
        live.cmd("set_track_name", track_index=t, name=RESAMPLE)
        live.cmd("set_track_input_routing", track_index=t, routing_type="Resampling", routing_channel="")
        live.cmd("set_track_monitoring", track_index=t, monitoring="off")
    return t


def newest(since):
    for _ in range(40):
        hits = [f for f in glob.glob(os.path.join(RECORDED, "*.wav"))
                if RESAMPLE in os.path.basename(f) and os.path.getmtime(f) >= since - 1]
        if hits:
            time.sleep(0.5)
            return max(hits, key=os.path.getmtime)
        time.sleep(0.25)
    return None


def take(live, stim_t, rec_t, seconds, out):
    live.cmd("set_track_arm", track_index=stim_t, arm=False)
    live.cmd("set_track_arm", track_index=rec_t, arm=True)
    if not live.cmd("get_track_info", track_index=rec_t).get("arm"):
        raise RuntimeError("could not arm recorder")
    t0 = time.time()
    live.cmd("start_recording")
    live.cmd("start_playback")
    live.cmd("fire_clip", track_index=stim_t, clip_index=0)
    time.sleep(seconds + 2.5)
    live.cmd("stop_playback")
    live.cmd("stop_recording")
    live.cmd("stop_clip", track_index=stim_t, clip_index=0)
    f = newest(t0)
    if not f:
        raise RuntimeError("no take recorded")
    shutil.copy2(f, out)
    os.remove(f)
    asd = f + ".asd"
    if os.path.exists(asd):
        os.remove(asd)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--styles", default="")
    ap.add_argument("--tag", default="echoboy")
    ap.add_argument("--feedback", type=float, default=0.5)
    ap.add_argument("--saturation", type=float, default=0.0)
    ap.add_argument("--passes", default="AB")
    a = ap.parse_args()
    seg = json.load(open(os.path.join(STIM, "stimuli.json")))
    live = Live()
    live.cmd("set_tempo", tempo=BPM)
    eb = live.track("EchoBoy")
    sa = ensure_stim(live, "_stimA", "A_repeats.wav", 16.0)
    sb = ensure_stim(live, "_stimB", "B_single.wav", 46.0)
    rec = ensure_resample(live)
    ps = {p["name"]: p for p in live.params(eb)}
    styles = ps["Style"].get("value_items") or []
    if not styles:  # AU params are continuous 0..1; item list read once from the display sweep
        styles = json.load(open(os.path.join(HERE, "echoboy_styles.json")))
    want = [s.strip() for s in a.styles.split(",") if s.strip()] or [s for s in styles if s != "Edited Style"]
    outdir = os.path.join(HERE, "captures", a.tag)
    os.makedirs(outdir, exist_ok=True)
    n = len(styles) - 1
    for st in want:
        idx = styles.index(st)
        base = {"Mode": 0.0, "Echo1Mode": 0.0, "Mix": 1.0, "Saturation": a.saturation,
                "LowCut": 0.0, "HighCut": 0.0, "Groove": 0.5, "Feel": 0.5,
                "InputGain": 0.5, "OutputGain": 0.5, "Style": idx / n}
        shown = live.setp(eb, base)
        # 250 ms echo time: bisect the Time parameter by its display
        lo, hi = 0.0, 1.0
        for _ in range(20):
            mid = (lo + hi) / 2
            d = live.setp(eb, {"Echo1Time": mid})["Echo1Time"]
            v = float(d.split()[0])
            if abs(v - 250.0) < 0.05:
                break
            lo, hi = (mid, hi) if v < 250 else (lo, mid)
        meta = {"style": st, "shown": shown, "time": d}
        if "A" in a.passes:
            meta["A_feedback"] = live.setp(eb, {"Feedback": a.feedback})["Feedback"]
            take(live, sa, rec, seg["A"]["length"], os.path.join(outdir, st + "__A.wav"))
        if "B" in a.passes:
            meta["B_feedback"] = live.setp(eb, {"Feedback": 0.0})["Feedback"]
            take(live, sb, rec, seg["B"]["length"], os.path.join(outdir, st + "__B.wav"))
        json.dump(meta, open(os.path.join(outdir, st + ".json"), "w"), indent=1)
        print("captured", st, meta.get("A_feedback"), d, flush=True)


if __name__ == "__main__":
    main()
