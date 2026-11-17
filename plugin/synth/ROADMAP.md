# Morpheus — what we want to build

The god of dreams, who takes any shape: a wavetable synth built around morphing.
Same family as Echo Nymph, Prometheus, Letheverb and Charon Pump (black chassis, vector display,
ORPHEO rail, gothic wordmark in `ui/brand/morpheus-wordmark.png`).

## Done

- **v0.1 voice** (`6ced8c4`): band-limited 64-frame ORPHEO table (sine → triangle → saw → square →
  vowels), unison 1–7 with detune + stereo spread, sub, SVF low-pass, amp ADSR, mod envelope on
  filter and table position, 16 voices sample-accurate, Prometheus ORPHEO drive, output safety.
  Tested offline: aliasing below −88 dB to the top of the keyboard (`harness/test_synth.cpp`).

- **v0.2 face** (`ui/prototype/morpheus.html`, 1180×760): the Dreamscape (one ridge per frame,
  every held note rides its own ridge as its envelope moves it; drag = X-Y pad, Position across,
  Cutoff up/down), the VOICE rail, and the mod deck. Opens in a browser as a playable preview
  (computer keyboard + Web Audio).

- **M1 · Motion and the ladder** (`741190e`, filters after): SCAN (Once/Loop/Ping-Pong at the
  sound's own speed); built-in tables MOOG (the Minimoog's six waves), PWM, SYNC (Prophet sync
  sweep), JUNO; filter models SVF morph (LP→BP→HP→Notch), LADDER (Moog 24 dB: self-oscillates in
  tune as a sine, bass loss with resonance), DIODE (303, 18 dB, peak on the cutoff at any
  resonance), OTA (Juno/Jupiter 24 dB), COMB, FORMANT (A-E-I-O-U); Filter Drive, Keytrack, the
  Juno-106 4-step HPF. Measured in `harness/test_filters.cpp`. Face: VOICE · FILTER tabs.

- **M2 · The modulation system:** a 16-slot matrix evaluated per voice, per sample
  (`dsp/Modulation.h`). Sources: Amp/Mod/Env 3, LFO 1-4 (7 shapes; free or tempo-synced; retrig,
  free-running or one-shot; free+synced locks to the song position), velocity, note, mod wheel,
  pitch bend, aftertouch (channel + per-note), per-note random, Drift, 8 ORPHEO macros
  (DREAM BLOOM MOTION GRIT SPACE WIDTH TONE FLUX). Destinations: position, pitch, cutoff, resonance,
  morph, filter drive, detune, spread, sub, level, pan, scan speed. Poly/Mono/Legato + glide (303
  slide). Face: source chips with editors, drag a source's dot onto any knob, rings in the source's
  colour, route pills (drag = amount, × = remove, + ROUTE for Pitch/Pan/Level). Tests: `test_mod`.

- **M3a · Oscillator B, noise, warp** (`39affab`): OSC B (own table, position, oct/semi/fine, level),
  noise (white/pink/brown/vinyl), warp on both oscillators (Sync, Bend±, Mirror, Asym, Quantize, FM,
  Ring; 2x oversampled where it matters, DC-blocked), wavetable .wav import/export. `test_osc`.
- **D · The design pass** (1280×800): header (preset browser, SYNTH / MATRIX / FX / STAGE pages, meter);
  SYNTH page laid out along the signal: OSC A · OSC B · SUB/NOISE → the Dreamscape (B drawn behind A) +
  UNISON/SCAN strip → FILTER → AMP/OUT; modulation + macros along the bottom; MATRIX page with all 16
  slots. Plan for the rest: `plugin/synth/PLAN.md`.

- **M4 · The FX rack:** Prometheus (8 styles), Chorus (Juno I/II/I+II, Solina Ensemble), Echo Nymph
  (tempo-synced), Letheverb, Charon, in any of 120 orders; a sidechain input feeding Charon and a
  SIDECHAIN mod source; the FX page (cards in chain order, on/off, ◀ ▶ to reorder). `test_fx`.

## Face layout (where new things go)

- **Dreamscape (hero):** always the table. Later: OSC B as a second, cooler-coloured landscape;
  grown-from-sound tables; the stage view makes it full width.
- **Rail:** gains tabs when it fills up: VOICE (oscillators) · FILTER · FX (Echo Nymph, Letheverb,
  Prometheus built in).
- **Mod deck (bottom):** every modulation source is a card that owns its routes (MOD ENV →
  Position, → Cutoff today). Each knob a source moves shows a ring in that source's colour.
  LFOs, MPE lanes and the 8 ORPHEO macros join as cards; drag-a-card-onto-a-knob assigning
  comes with them.

## Next (our plan)

1. **Grow wavetables from sound.** Drop in any audio (voice, a Crate sample, a stem) and it is
   resynthesized into frames you can sweep.
2. **Second oscillator, noise, ladder filter** alongside the SVF.
3. **ORPHEO macros:** one knob that does something musical, not technical.
4. **Echo Nymph and Letheverb built in** as the FX section.

## From the vintage synth library (~/Developer/vintage-synths)

- **Filter models**, first the **Moog ladder** (4-stage 24 dB, saturating, self-oscillates to a
  sine at full Emphasis, thins the bass as resonance rises; Minimoog/Taurus, with Overload drive
  in front). Then the TB-303 18 dB diode, the Juno/Jupiter IR3109 24 dB, the OB/SEM 12 dB
  LP↔BP↔HP morph (today's SVF grows into it), and the Juno-106's 4-step HPF incl. its bass boost.
- **Vintage tables:** the Minimoog's six waveforms (tri, shark-tooth, saw, square, wide and
  narrow pulse) as one table; a pulse-width table (Position = PWM); a hard-sync table
  (Position = the Prophet sync sweep).
- **Player behaviour:** 303 accent + slide (mono/legato + glide); CS-80 velocity/aftertouch →
  brilliance + level (prepares MPE); Prophet Poly-Mod as a mod-deck route; a **Vintage** knob
  (per-voice pitch/cutoff drift, OB/Prophet slop).
- **FX cards:** Juno Chorus I/II (BBD), Solina string ensemble.
- **Vintage bank:** the 26 racks rebuilt as Morpheus presets once the above exists.

## Synthesis & engine

- **Physical-modeling oscillators:** waveguide, Karplus-Strong and modal resonators, for plucked
  strings, metallic percussion and bowed textures, synthesized rather than sampled.
- **Zero-delay feedback loops:** audio-rate routing from the end of the filter or FX back into the
  oscillators' FM / warp inputs, for self-oscillating analog and experimental tones.
- **Per-note expression (MPE + polyphonic aftertouch):** every modulation source gets its own
  per-note lane, so MPE controllers feel native.

## Workflow & modulation

- **Sidechain audio input:** a kick or vocal routed in drives envelope followers, filter cutoff
  or a vocoder inside the synth (Charon already has the aux-bus routing to reuse).
- **CPU controls:** per-oscillator oversampling caps and multi-core voice rendering, so big
  stacked patches stay playable live.

## Live performance & hardware

- **Stage view:** a simplified, high-contrast mode showing only the eight macros and the X-Y
  pads, for fast, safe tweaking on stage.
- **Controller auto-mapping:** popular knob/grid controllers map automatically, with values sent
  back to the hardware, so there is no MIDI-learn setup.

## Skipped

- AI-assisted timbre transfer / morphing between recordings.
