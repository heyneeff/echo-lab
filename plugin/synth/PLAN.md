# Morpheus: the full plan (M3 → M5 + the design pass)

## Context

Morpheus (branch `wavetable`) has finished M1 (SCAN, vintage tables, six filter models) and M2 (the
modulation system: 16-slot matrix, LFOs, Env 3, macros, MIDI, glide/legato). Neeff wants:

- **Every remaining milestone planned in one place.**
- **A real design pass**, so that everything is laid out intuitively, taking cues from Serum and other softsynths.

The current face is a hero + a single rail with tabs. That worked for one oscillator, but it hides too much
once OSC B, noise, warp, FX, the matrix and a stage view arrive.

**What the best softsynths share** (the cues we take, not their look):

- **Signal flows left → right:** sources → filter → amp/out. Serum and Vital put the oscillators first, then
  the filter, then the output.
- **Modulation lives along the bottom** as tabs (Serum ENV/LFO tabs, Vital's mod section, Pigments'
  modulation strip). It's always visible, and you drag from it onto any knob.
- **Big jobs get their own pages:** SYNTH / FX / MATRIX / global settings (Serum, Vital, Pigments). Each page
  stays uncluttered.
- **Presets are browsed in the header**, always visible, with prev/next.

Morpheus keeps its own identity: the black chassis, the Dreamscape at the centre, vector screens,
source-coloured rings and the ORPHEO macros.

## Order

M3a engine → D design pass (built around everything M3 adds) → M4 FX → M5 play-it-live.
Each step ships installed, validated (47/47), with all `ctest` suites passing, checked in the real window
(`editorhost` + `LAB_EVAL` + `LAB_SNAPSHOT`), and committed on `wavetable`.

---

## M3a: Oscillator B, noise, warp, wavetable files (engine first)

All in `dsp/WavetableSynth.h` + `dsp/Modulation.h`, with parameters appended in `plugin/synth/params.h`
(IDs append-only; a new oscillator at Level 0 leaves old sets sounding identical).

- **OSC B:**
  - Its own table: a second atomic table pointer, `setTableB`, a Table B parameter (built-in or GROWN).
  - Position B, Octave B (−3..+3), Semi B (±12), Fine B (±100 ct), Level B (0 = off, nothing computed).
  - **Osc A Level** (default 100).
  - Unison, detune and spread apply to both oscillators.
- **Noise:** Noise Level and Noise Color (White, Pink (Kellet), Brown, Vinyl (hiss + random crackle)). It's
  per voice, follows the amp envelope, and goes through the filter.
- **Warp per oscillator** (mode + amount, both modulatable):
  - **Sync:** slave ratio 1–8. The reset jump is PolyBLEP-corrected, and the slave reads at the band-limit
    level for its own frequency.
  - **Bend+ / Bend−:** a rational phase curve. It's cheap, because there's no pow() per sample.
  - **Mirror:** forward then reversed.
  - **Asym:** phase distortion (PWM on any table).
  - **Quantize:** deliberately lo-fi steps.
  - **FM:** phase modulation from the other oscillator.
  - **Ring:** from the other oscillator.
  - **Band-limiting:** each warp reads the table at the band-limit level for its *maximum instantaneous
    phase speed*, computed per block. The reshaping itself then never adds aliasing; only FM/ring sidebands
    can (inherent).
- **New destinations** (appended): Position B, Level B, Warp A, Warp B, Noise.
- **Wavetable files:**
  - **Import:** in the face, detect a wavetable `.wav` (a `clm ` chunk, or a multiple of 2048 samples with
    "wavetable"/"wt" in the name). Decode it at its own sample rate (no resampling). The plugin turns each
    2048 cycle straight into a spectrum (no pitch detection): up to 64 frames, picked evenly.
  - **Export:** `~/Music/Morpheus/Tables/<NAME>.wav`, 64 × 2048 float with a `clm ` chunk. The cycles come
    from level 1 of the table, so they're band-limited exactly.
- **Tests** (`harness/test_osc.cpp`):
  - Level B 0 is bit-identical to before.
  - Octave/Semi/Fine land within 0.1%.
  - Aliasing at C7 per warp mode is reported:
    - continuous warps (Bend, Mirror, Asym) < −60 dB;
    - Sync measured and reported (target < −50 dB);
    - FM/Ring with B at the same pitch measured.
  - Noise colours slope correctly (white flat, pink −3 dB/oct, brown −6 dB/oct within 1 dB).
  - An imported 2048-frame saw round-trips exactly.
  - Fuzz with all warps.

## D: The design pass (the face rebuilt around the signal flow)

The window grows to **1280 × 800** (the face still scales to fit). Layout:

```
┌ HEADER: MORPHEUS mark · ◁ PRESET NAME ▷ ☰ SAVE · [SYNTH] [FX] [MATRIX] [STAGE] · ⚙ · meter ┐
│ SYNTH page                                                                                   │
│ ┌ OSC A ──────────┐ ┌──────── DREAMSCAPE ────────┐ ┌ FILTER ───────────────┐                  │
│ │ table ▾ · 2D wave│ │ A amber, B cool blue behind │ │ type · response curve │                  │
│ │ POS WARP▾ amt    │ │ voices ride ridges          │ │ CUT RES MORPH DRIVE   │                  │
│ │ UNI DET SPR LVL  │ │ X-Y pad (assignable)        │ │ KEY HPF · A B N SUB → │                  │
│ ├ OSC B ──────────┤ │ LOAD · TABLE · SCAN          │ ├ AMP / OUT ───────────┤                  │
│ │ same + OCT SEMI  │ └─────────────────────────────┘ │ DRIVE OUT · POLY▾ GLIDE│                 │
│ ├ SUB · NOISE ────┤                                  └───────────────────────┘                  │
│ └─────────────────┘                                                                            │
│ ┌ MOD: AMP MOD ENV3 LFO1-4 MIDI · editor · routes ┐ ┌ ORPHEO MACROS (8) ┐                      │
└───────────────────────────────────────────────────────────────────────────────────────────────┘
```

- **Header** (always visible):
  - the preset browser: name, ◁ ▷, a ☰ list with categories, SAVE;
  - page tabs and ⚙ global settings (voices, quality, bend range, MIDI);
  - the output meter.
- **SYNTH:**
  - **Left column:** OSC A, OSC B, then SUB + NOISE, each a compact panel with the same layout, so learning
    one teaches both. Each oscillator panel has:
    - a table dropdown;
    - a 2D current-frame screen showing the warped wave;
    - POSITION;
    - a WARP mode menu plus its amount;
    - UNISON / DETUNE / SPREAD;
    - LEVEL;
    - (B only) OCT / SEMI / FINE.
  - **Centre:** the Dreamscape. OSC B is drawn as a cooler landscape behind A, and the X-Y pad axes are
    assignable (default Position A / Cutoff). SCAN controls sit under it.
  - **Right column:** FILTER (type, response curve, the knobs, which of A/B/noise/sub feed it), then
    AMP/OUT (Prometheus drive, output, voice mode, glide).
- **Bottom:** the mod deck exactly as M2 built it (chips, editor, route pills), plus the macros.
- **MATRIX page:** all 16 slots as rows: source ▾, destination ▾, amount slider, a mute toggle, and the
  source's live value. The same data as the pills, for when you want the whole picture.
- **FX page** (built in M4): the chain as cards in order, dragged to reorder.
- **STAGE page** (M5): the Dreamscape full width plus 8 big macros.
- **Consistency rules:**
  - every knob shows its name and value;
  - every modulatable knob accepts a dropped source;
  - colours mean the same thing everywhere (amber = oscillator A, cool blue = B, orange = filter,
    each mod source its own);
  - double-click resets;
  - Shift gives fine control.
- **Verify:** `editorhost` snapshots of every page, plus LAB_EVAL scripts that:
  - pick a table on A and on B,
  - set each warp,
  - drag a source onto an OSC B knob,
  - switch pages,
  - and check the parameters reach the plugin.

## M4: The FX rack (our own plugins inside)

- **A chain on the synth output.** Up to 6 slots, reorderable, each with on/off + mix.
  - Engines are reused as-is:
    - Prometheus: `DriveEngine` with the 8 `kDriveVoices`;
    - Echo Nymph: `dsp/EchoEngine.h`, tempo-synced from the host;
    - Letheverb: `dsp/ReverbEngine.h`;
    - Charon: `dsp/PumpEngine.h`.
  - New: Juno Chorus I/II and Solina Ensemble (`dsp/Chorus.h`, BBD-style modulated delays).
  - Each FX exposes its main controls as parameters (append-only). The current Drive knob becomes the
    Prometheus slot.
- **Sidechain:** an aux input bus, set up the way Charon's `initialize`/`setBusArrangements` already do it
  (`plugin/pump/plugin.cpp`). It feeds Charon's ducking and a new **Sidechain** mod source (an envelope
  follower).
- **Face (FX page):** one card per slot with that plugin's mini screen (echo ring, reverb decay, pump
  wave), its wordmark, key knobs, on/off and mix; drag to reorder.
- **Tests:**
  - each FX at mix 0 is bit-exact dry;
  - the ORPHEO voicings are level-matched (±1 dB);
  - echo times match the tempo;
  - the sidechain follower's attack/release timing;
  - CPU per FX is logged.

## M5: Play it live

- **Expression:** verify first whether Live 12 sends MPE to VST3 plugins as note expression. Then
  per-note pitch/pressure/slide as mod sources (note expression, or channel-per-note MPE as the fallback).
- **STAGE page:** the Dreamscape full width, the 8 macros big and high-contrast, the X-Y pad assignable to
  any two destinations, and a panic button.
- **Presets inside the plugin**:
  - a factory bank (ORPHEO + the face's presets + the **26 vintage racks**, rebuilt from
    `~/Developer/vintage-synths/library.py` with the M1–M4 pieces);
  - user presets as JSON in `~/Music/Morpheus/Presets` (parameters + grown table), with categories and
    tags, browsable in the header;
  - presets then also work without the face open.
- **CPU:**
  - per-voice filter coefficients every 16 samples;
  - SIMD unison;
  - a voice-count parameter;
  - an Eco/HQ switch (warp oversampling);
  - `harness/bench_synth.cpp` with a target (16 voices × 7 unison × 2 osc + ladder + FX < 35% of one core
    at 48 kHz on this Mac).
- **Tests:** the benchmark; every factory preset loads, renders finitely and sits within ±3 dB of −18 LUFS.

## Verification (every step)

1. Build `.tools`, `XCODE_VERSION=16.0`, `cmake --build build/plugin`, then `ctest` (all suites).
2. Install to `~/Library/Audio/Plug-Ins/VST3`, ad-hoc codesign, run the validator (47/47).
3. Run the real face in `editorhost` with `LAB_EVAL` scripts and check with `LAB_SNAPSHOT`.
4. Demo renders; Neeff plays it in Live.
5. Commit on `wavetable` and update `plugin/synth/ROADMAP.md`.

## M6: Sound design station (research + plan)

**Where the others stand out** (what we take, not their look):

| Synth | What makes it a sound design station |
|---|---|
| Serum 2 | Drawable LFO/envelope curves with grid and snap; LFO as a one-shot envelope; per-route remap curves; a real wavetable editor (draw, FFT, morph, formula); spectral, granular and multisample oscillators; arpeggiator and clip sequencer; FX parameters as mod destinations |
| Vital | Drag any source to anything, remap curves on every route, text-to-wavetable, spectral warps, stereo LFOs, a free and very fast UI |
| Pigments | Sequencer, function and random generators, mod combinators, macro "sound bank" tweaking, one-click randomise per section |
| Phase Plant | Modular lanes, unlimited LFOs/envelopes, snapins: any FX anywhere |
| Massive X | Performers (sequenced modulation in bars), trackers (curves from note or velocity) |
| Omnisphere / Output | One gesture changes many things: the Orb, macro "states", morphing between snapshots |

**Morpheus, next level** (in order of payoff):

1. **FINISH** (done): one-click recipes that stack and undo. Motion on LFO 3/4, so presets keep LFO 1/2.
2. **Morph snapshots** (done): 4 whole-patch states on the X-Y pad corners, blended in the plugin by Morph X/Y (automatable), saved with the set (state v6). Empty corners = the knobs as they are.
3. **Remap curves per route**: amount + curve (exp, log, S, steps) per matrix slot (Serum/Vital).
4. **Dice**: randomise a section (osc, filter, mod, FX) with locks; "mutate" by a small amount.
5. **Step modulator / arp**: a 16-step lane as a source (Massive X performers, Pigments sequencer), plus a tempo arp.
6. **FX as destinations**: echo mix, reverb mix/decay, drive, pump depth on the matrix.
7. **Table editor**: draw a frame, harmonics view, morph two grown tables (we already grow tables from sound: Serum can't).
8. **Presets inside the plugin** (from M5) so FINISH + snapshots save with the patch everywhere.

## Where we stopped (save point)

M4 done; M5: STAGE page in. M6 started: FINISH panel (12 recipes) in the deck, a new Morpheus opens on
ORPHEO · Dream Pad (with Juno chorus + ORPHEO space), the preset name is saved with the set (state v5).
Morph snapshots done (Morph On/X/Y params; corners in the SCAN strip and on STAGE; the Dreamscape becomes the morph pad).
Charon's pump clock fixed for a stopped transport. Next: remap curves, Dice, presets inside the plugin.
