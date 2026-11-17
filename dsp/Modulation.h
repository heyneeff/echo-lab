// Morpheus modulation: sources, destinations, LFOs and the 16-slot matrix (evaluated per voice,
// per sample, in WavetableSynth.h). Header-only, no allocation.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace echolab {
namespace wt {

enum ModSource {
    kSrcNone = 0, kSrcAmpEnv, kSrcModEnv, kSrcEnv3, kSrcLfo1, kSrcLfo2, kSrcLfo3, kSrcLfo4,
    kSrcVelocity, kSrcNote, kSrcModWheel, kSrcPitchBend, kSrcAftertouch, kSrcRandom, kSrcDrift,
    kSrcMacro1, kSrcMacro2, kSrcMacro3, kSrcMacro4, kSrcMacro5, kSrcMacro6, kSrcMacro7, kSrcMacro8,
    kNumSources
};
inline const char* const kSourceNames[kNumSources] = {
    "None", "Amp Env", "Mod Env", "Env 3", "LFO 1", "LFO 2", "LFO 3", "LFO 4",
    "Velocity", "Note", "Mod Wheel", "Pitch Bend", "Aftertouch", "Random", "Drift",
    "Macro 1", "Macro 2", "Macro 3", "Macro 4", "Macro 5", "Macro 6", "Macro 7", "Macro 8"};

// What a full (+100%) route does to each destination
enum ModDest {
    kDstNone = 0,
    kDstPosition,     // +1 = the whole table
    kDstPitch,        // +12 semitones
    kDstCutoff,       // +6 octaves
    kDstResonance,    // +100%
    kDstMorph,        // +100%
    kDstFilterDrive,  // +24 dB
    kDstDetune,       // +100 cents
    kDstSpread,       // +100%
    kDstSub,          // +100%
    kDstLevel,        // +100% (0 = silent at -100%)
    kDstPan,          // full right
    kDstScanSpeed,    // x4 (two octaves of speed)
    kDstPositionB,    // +1 = the whole table (OSC B)
    kDstLevelB,       // +100%
    kDstWarpA,        // +100% warp amount
    kDstWarpB,
    kDstNoise,        // +100% noise level
    kNumDests
};
inline const char* const kDestNames[kNumDests] = {
    "None", "Position", "Pitch", "Cutoff", "Resonance", "Filter Morph", "Filter Drive",
    "Detune", "Spread", "Sub", "Level", "Pan", "Scan Speed", "Position B", "Level B", "Warp A", "Warp B", "Noise"};

constexpr int kModSlots = 16;
struct ModSlot { int src = kSrcNone, dst = kDstNone; double amt = 0; };  // amt -1..1

// ------------------------------------------------------------------ LFO
enum LfoShape { kLfoSine = 0, kLfoTriangle, kLfoSawUp, kLfoSawDown, kLfoSquare, kLfoSampleHold, kLfoSmooth, kNumLfoShapes };
inline const char* const kLfoShapeNames[kNumLfoShapes] = {"Sine", "Triangle", "Saw Up", "Saw Down", "Square", "S&H", "Smooth"};
enum LfoMode { kLfoRetrig = 0, kLfoFree, kLfoOnce };  // per note from 0 / one shared clock / per note, once
inline const char* const kLfoModeNames[] = {"Retrig", "Free", "Once"};
constexpr int kLfos = 4;
constexpr int kLfoNotes = 17;
inline const char* const kLfoNoteNames[kLfoNotes] = {"8/1", "4/1", "2/1", "1/1", "1/2D", "1/2", "1/2T", "1/4D", "1/4",
                                                     "1/4T", "1/8D", "1/8", "1/8T", "1/16D", "1/16", "1/16T", "1/32"};
inline const double kLfoNoteBeats[kLfoNotes] = {32, 16, 8, 4, 3, 2, 4.0 / 3, 1.5, 1, 2.0 / 3, 0.75, 0.5, 1.0 / 3, 0.375, 0.25, 1.0 / 6, 0.125};

struct LfoParams {
    int shape = kLfoSine;
    double hz = 2;        // free rate
    bool sync = false;    // rate from the host tempo instead
    int note = 8;         // index into kLfoNoteNames when synced (1/4)
    int mode = kLfoRetrig;
};

struct LfoState {
    double phase = 0;
    double a = 0, b = 0;   // random shapes: the previous and next value
    uint32_t seed = 1;
    bool done = false;
    void start(uint32_t s) { phase = 0; done = false; seed = s * 2654435761u + 1; a = rnd(); b = rnd(); }
    double rnd() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) * (2.0 / 16777216.0) - 1; }
};

// advance by inc cycles and return the value, -1..1
inline double lfoStep(LfoState& s, int shape, double inc, bool once) {
    if (!s.done) {
        s.phase += inc;
        if (s.phase >= 1) {
            if (once) { s.phase = 1; s.done = true; }
            else { s.phase -= std::floor(s.phase); s.a = s.b; s.b = s.rnd(); }
        }
    }
    const double p = s.phase;
    switch (shape) {
        case kLfoSine: return std::sin(2 * 3.14159265358979323846 * p);
        case kLfoTriangle: return p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4;
        case kLfoSawUp: return 2 * p - 1;
        case kLfoSawDown: return 1 - 2 * p;
        case kLfoSquare: return p < 0.5 ? 1 : -1;
        case kLfoSampleHold: return s.a;
        case kLfoSmooth: { const double t = p * p * (3 - 2 * p); return s.a + (s.b - s.a) * t; }
    }
    return 0;
}

}  // namespace wt
}  // namespace echolab
