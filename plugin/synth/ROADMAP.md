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
