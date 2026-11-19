// Wavetable synth parameters (one table: ids, names, units, ranges, mapping). Ids are stable: append only.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../../dsp/Modulation.h"
#include "../../dsp/WavetableSynth.h"
#include "../../dsp/FxRack.h"
#include "../../dsp/TableForge.h"
#include "../../dsp/Spatial.h"

namespace echolab {
namespace wtsynth {

enum ParamId : int {
    kPosition = 0, kUnison, kDetune, kSpread, kSub, kCutoff, kResonance, kFilterEnv, kPosEnv,
    kAttack, kDecay, kSustain, kRelease, kModAttack, kModDecay, kVelocity, kDrive, kOutput,
    kTable, kScan, kScanSpeed,  // v0.3
    kFilterType, kFilterMorph, kFilterDrive, kHpf, kKeytrack,  // v0.4
    // v0.5: modulation
    kMod1Src, kMod1Dst, kMod1Amt, kMod2Src, kMod2Dst, kMod2Amt,
    kMod3Src, kMod3Dst, kMod3Amt, kMod4Src, kMod4Dst, kMod4Amt,
    kMod5Src, kMod5Dst, kMod5Amt, kMod6Src, kMod6Dst, kMod6Amt,
    kMod7Src, kMod7Dst, kMod7Amt, kMod8Src, kMod8Dst, kMod8Amt,
    kMod9Src, kMod9Dst, kMod9Amt, kMod10Src, kMod10Dst, kMod10Amt,
    kMod11Src, kMod11Dst, kMod11Amt, kMod12Src, kMod12Dst, kMod12Amt,
    kMod13Src, kMod13Dst, kMod13Amt, kMod14Src, kMod14Dst, kMod14Amt,
    kMod15Src, kMod15Dst, kMod15Amt, kMod16Src, kMod16Dst, kMod16Amt,
    kEnv3A, kEnv3D, kEnv3S, kEnv3R, kLfo1Shape, kLfo1Rate,
    kLfo1Sync, kLfo1Note, kLfo1Mode, kLfo2Shape, kLfo2Rate, kLfo2Sync,
    kLfo2Note, kLfo2Mode, kLfo3Shape, kLfo3Rate, kLfo3Sync, kLfo3Note,
    kLfo3Mode, kLfo4Shape, kLfo4Rate, kLfo4Sync, kLfo4Note, kLfo4Mode,
    kMacro1, kMacro2, kMacro3, kMacro4, kMacro5, kMacro6,
    kMacro7, kMacro8, kModWheel, kPitchBend, kAftertouch, kBendRange,
    kVoiceMode, kGlide,
    // v0.6: oscillator B, warp, noise
    kOscALevel, kTableB, kPositionB, kOctB, kSemiB, kFineB, kOscBLevel, kWarpA, kWarpAmtA, kWarpB, kWarpAmtB, kNoise, kNoiseColor,
    // v0.7: the FX rack
    kDriveStyle, kDriveMix, kChorusOn, kChorusMode, kChorusMix,
    kEchoOn, kEchoStyle, kEchoMode, kEchoSync, kEchoNoteL, kEchoNoteR, kEchoTimeL, kEchoTimeR, kEchoFeedback, kEchoMix,
    kVerbOn, kVerbStyle, kVerbDecay, kVerbSize, kVerbMix, kPumpOn, kPumpMode, kPumpRate, kPumpDepth, kPumpMix, kFxOrder,
    // v0.8: morph snapshots (4 whole-patch states on the X-Y pad corners)
    kMorphOn, kMorphX, kMorphY,
    // v0.9: SPACE (the whole sound around, above, below, near, far)
    kSpaceOn, kSpaceX, kSpaceY, kSpaceZ, kSpaceOrbit, kSpaceRate, kSpaceSize, kSpaceRoom, kSpaceHeadphones, kSpaceMirror, kSpaceAmount,
    kSustainPedal,
    // v0.10: output EQ
    kEqLow, kEqMid, kEqMidFreq, kEqHigh,
    kNumParams
};
constexpr int kModBase = kMod1Src;  // slot s (0-based): source kModBase + 3s, destination +1, amount +2
constexpr int kLfoBase = kLfo1Shape; // LFO q (0-based): shape kLfoBase + 5q, rate +1, sync +2, note +3, mode +4
inline const char* const kVoiceModeNames[] = {"Poly", "Mono", "Legato"};
inline const char* const kNoiseColorNames[] = {"White", "Pink", "Brown", "Vinyl"};
inline const char* const kFilterTypeNames[] = {"SVF", "Ladder", "Diode", "OTA", "Comb", "Formant"};
inline const char* const kHpfNames[] = {"Boost", "Flat", "Low", "High"};
inline const char* const kTableNames[] = {"ORPHEO", "MOOG", "PWM", "SYNC", "JUNO", "GROWN"};
constexpr int kTableGrown = 5;
// The Table switches are sized for growth (like the mod switches): 0-4 built-in, 5 GROWN, 6.. the TableForge library.
constexpr int kTableSlots = 128, kTableLibrary = 6;
inline std::string tableName(int i) {
    if (i >= 0 && i <= kTableGrown) return kTableNames[i];
    const int k = i - kTableLibrary;
    return k >= 0 && k < wt::forge::count() ? wt::forge::recipes()[k].name : "ORPHEO";
}
inline const char* const kScanNames[] = {"Off", "Once", "Loop", "Ping-Pong"};
enum class Map { Linear, Log, LogOffLow, LogOffHigh, List };
struct ParamDef { int id; const char* name; const char* units; double min, max, def; Map map; };

inline const ParamDef kParams[kNumParams] = {
    {kPosition, "Position", "%", 0, 100, 40, Map::Linear},
    {kUnison, "Unison", "", 1, 7, 1, Map::List},
    {kDetune, "Detune", "ct", 0, 100, 15, Map::Linear},
    {kSpread, "Spread", "%", 0, 100, 60, Map::Linear},
    {kSub, "Sub", "%", 0, 100, 0, Map::Linear},
    {kCutoff, "Cutoff", "Hz", 20, 20000, 20000, Map::Log},
    {kResonance, "Resonance", "%", 0, 100, 10, Map::Linear},
    {kFilterEnv, "Filter Env", "%", -100, 100, 0, Map::Linear},
    {kPosEnv, "Position Env", "%", -100, 100, 0, Map::Linear},
    {kAttack, "Attack", "ms", 0.5, 10000, 3, Map::Log},
    {kDecay, "Decay", "ms", 5, 10000, 300, Map::Log},
    {kSustain, "Sustain", "%", 0, 100, 80, Map::Linear},
    {kRelease, "Release", "ms", 5, 10000, 300, Map::Log},
    {kModAttack, "Mod Attack", "ms", 0.5, 10000, 3, Map::Log},
    {kModDecay, "Mod Decay", "ms", 5, 10000, 600, Map::Log},
    {kVelocity, "Velocity", "%", 0, 100, 60, Map::Linear},
    {kDrive, "Drive", "", 0, 10, 0, Map::Linear},
    {kOutput, "Output", "dB", -24, 12, 0, Map::Linear},
    {kTable, "Table", "", 0, kTableSlots - 1, 0, Map::List},
    {kScan, "Scan", "", 0, 3, 0, Map::List},
    {kScanSpeed, "Scan Speed", "x", 0.25, 4, 1, Map::Log},
    {kFilterType, "Filter Type", "", 0, 5, 0, Map::List},
    {kFilterMorph, "Filter Morph", "%", 0, 100, 0, Map::Linear},
    {kFilterDrive, "Filter Drive", "dB", 0, 24, 0, Map::Linear},
    {kHpf, "HPF", "", 0, 3, 1, Map::List},
    {kKeytrack, "Keytrack", "%", 0, 100, 0, Map::Linear},
    {kMod1Src, "Mod 1 Source", "", 0, wt::kNumSources - 1, 0, Map::List},
    {kMod1Dst, "Mod 1 Dest", "", 0, wt::kNumDests - 1, 0, Map::List},
    {kMod1Amt, "Mod 1 Amount", "%", -100, 100, 0, Map::Linear},
    {kMod2Src, "Mod 2 Source", "", 0, wt::kNumSources - 1, 0, Map::List},
    {kMod2Dst, "Mod 2 Dest", "", 0, wt::kNumDests - 1, 0, Map::List},
    {kMod2Amt, "Mod 2 Amount", "%", -100, 100, 0, Map::Linear},
    {kMod3Src, "Mod 3 Source", "", 0, wt::kNumSources - 1, 0, Map::List},
    {kMod3Dst, "Mod 3 Dest", "", 0, wt::kNumDests - 1, 0, Map::List},
    {kMod3Amt, "Mod 3 Amount", "%", -100, 100, 0, Map::Linear},
    {kMod4Src, "Mod 4 Source", "", 0, wt::kNumSources - 1, 0, Map::List},
    {kMod4Dst, "Mod 4 Dest", "", 0, wt::kNumDests - 1, 0, Map::List},
    {kMod4Amt, "Mod 4 Amount", "%", -100, 100, 0, Map::Linear},
    {kMod5Src, "Mod 5 Source", "", 0, wt::kNumSources - 1, 0, Map::List},
    {kMod5Dst, "Mod 5 Dest", "", 0, wt::kNumDests - 1, 0, Map::List},
    {kMod5Amt, "Mod 5 Amount", "%", -100, 100, 0, Map::Linear},
    {kMod6Src, "Mod 6 Source", "", 0, wt::kNumSources - 1, 0, Map::List},
    {kMod6Dst, "Mod 6 Dest", "", 0, wt::kNumDests - 1, 0, Map::List},
    {kMod6Amt, "Mod 6 Amount", "%", -100, 100, 0, Map::Linear},
    {kMod7Src, "Mod 7 Source", "", 0, wt::kNumSources - 1, 0, Map::List},
    {kMod7Dst, "Mod 7 Dest", "", 0, wt::kNumDests - 1, 0, Map::List},
    {kMod7Amt, "Mod 7 Amount", "%", -100, 100, 0, Map::Linear},
    {kMod8Src, "Mod 8 Source", "", 0, wt::kNumSources - 1, 0, Map::List},
    {kMod8Dst, "Mod 8 Dest", "", 0, wt::kNumDests - 1, 0, Map::List},
    {kMod8Amt, "Mod 8 Amount", "%", -100, 100, 0, Map::Linear},
    {kMod9Src, "Mod 9 Source", "", 0, wt::kNumSources - 1, 0, Map::List},
    {kMod9Dst, "Mod 9 Dest", "", 0, wt::kNumDests - 1, 0, Map::List},
    {kMod9Amt, "Mod 9 Amount", "%", -100, 100, 0, Map::Linear},
    {kMod10Src, "Mod 10 Source", "", 0, wt::kNumSources - 1, 0, Map::List},
    {kMod10Dst, "Mod 10 Dest", "", 0, wt::kNumDests - 1, 0, Map::List},
    {kMod10Amt, "Mod 10 Amount", "%", -100, 100, 0, Map::Linear},
    {kMod11Src, "Mod 11 Source", "", 0, wt::kNumSources - 1, 0, Map::List},
    {kMod11Dst, "Mod 11 Dest", "", 0, wt::kNumDests - 1, 0, Map::List},
    {kMod11Amt, "Mod 11 Amount", "%", -100, 100, 0, Map::Linear},
    {kMod12Src, "Mod 12 Source", "", 0, wt::kNumSources - 1, 0, Map::List},
    {kMod12Dst, "Mod 12 Dest", "", 0, wt::kNumDests - 1, 0, Map::List},
    {kMod12Amt, "Mod 12 Amount", "%", -100, 100, 0, Map::Linear},
    {kMod13Src, "Mod 13 Source", "", 0, wt::kNumSources - 1, 0, Map::List},
    {kMod13Dst, "Mod 13 Dest", "", 0, wt::kNumDests - 1, 0, Map::List},
    {kMod13Amt, "Mod 13 Amount", "%", -100, 100, 0, Map::Linear},
    {kMod14Src, "Mod 14 Source", "", 0, wt::kNumSources - 1, 0, Map::List},
    {kMod14Dst, "Mod 14 Dest", "", 0, wt::kNumDests - 1, 0, Map::List},
    {kMod14Amt, "Mod 14 Amount", "%", -100, 100, 0, Map::Linear},
    {kMod15Src, "Mod 15 Source", "", 0, wt::kNumSources - 1, 0, Map::List},
    {kMod15Dst, "Mod 15 Dest", "", 0, wt::kNumDests - 1, 0, Map::List},
    {kMod15Amt, "Mod 15 Amount", "%", -100, 100, 0, Map::Linear},
    {kMod16Src, "Mod 16 Source", "", 0, wt::kNumSources - 1, 0, Map::List},
    {kMod16Dst, "Mod 16 Dest", "", 0, wt::kNumDests - 1, 0, Map::List},
    {kMod16Amt, "Mod 16 Amount", "%", -100, 100, 0, Map::Linear},
    {kEnv3A, "Env3 Attack", "ms", 0.5, 10000, 10, Map::Log},
    {kEnv3D, "Env3 Decay", "ms", 5, 10000, 500, Map::Log},
    {kEnv3S, "Env3 Sustain", "%", 0, 100, 0, Map::Linear},
    {kEnv3R, "Env3 Release", "ms", 5, 10000, 300, Map::Log},
    {kLfo1Shape, "LFO1 Shape", "", 0, wt::kNumLfoShapes - 1, 0, Map::List},
    {kLfo1Rate, "LFO1 Rate", "Hz", 0.01, 40, 2, Map::Log},
    {kLfo1Sync, "LFO1 Sync", "", 0, 1, 0, Map::List},
    {kLfo1Note, "LFO1 Note", "", 0, wt::kLfoNotes - 1, 8, Map::List},
    {kLfo1Mode, "LFO1 Mode", "", 0, 2, 0, Map::List},
    {kLfo2Shape, "LFO2 Shape", "", 0, wt::kNumLfoShapes - 1, 0, Map::List},
    {kLfo2Rate, "LFO2 Rate", "Hz", 0.01, 40, 2, Map::Log},
    {kLfo2Sync, "LFO2 Sync", "", 0, 1, 0, Map::List},
    {kLfo2Note, "LFO2 Note", "", 0, wt::kLfoNotes - 1, 8, Map::List},
    {kLfo2Mode, "LFO2 Mode", "", 0, 2, 0, Map::List},
    {kLfo3Shape, "LFO3 Shape", "", 0, wt::kNumLfoShapes - 1, 0, Map::List},
    {kLfo3Rate, "LFO3 Rate", "Hz", 0.01, 40, 2, Map::Log},
    {kLfo3Sync, "LFO3 Sync", "", 0, 1, 0, Map::List},
    {kLfo3Note, "LFO3 Note", "", 0, wt::kLfoNotes - 1, 8, Map::List},
    {kLfo3Mode, "LFO3 Mode", "", 0, 2, 0, Map::List},
    {kLfo4Shape, "LFO4 Shape", "", 0, wt::kNumLfoShapes - 1, 0, Map::List},
    {kLfo4Rate, "LFO4 Rate", "Hz", 0.01, 40, 2, Map::Log},
    {kLfo4Sync, "LFO4 Sync", "", 0, 1, 0, Map::List},
    {kLfo4Note, "LFO4 Note", "", 0, wt::kLfoNotes - 1, 8, Map::List},
    {kLfo4Mode, "LFO4 Mode", "", 0, 2, 0, Map::List},
    {kMacro1, "Macro 1", "%", 0, 100, 0, Map::Linear},
    {kMacro2, "Macro 2", "%", 0, 100, 0, Map::Linear},
    {kMacro3, "Macro 3", "%", 0, 100, 0, Map::Linear},
    {kMacro4, "Macro 4", "%", 0, 100, 0, Map::Linear},
    {kMacro5, "Macro 5", "%", 0, 100, 0, Map::Linear},
    {kMacro6, "Macro 6", "%", 0, 100, 0, Map::Linear},
    {kMacro7, "Macro 7", "%", 0, 100, 0, Map::Linear},
    {kMacro8, "Macro 8", "%", 0, 100, 0, Map::Linear},
    {kModWheel, "Mod Wheel", "%", 0, 100, 0, Map::Linear},
    {kPitchBend, "Pitch Bend", "%", -100, 100, 0, Map::Linear},
    {kAftertouch, "Aftertouch", "%", 0, 100, 0, Map::Linear},
    {kBendRange, "Bend Range", "st", 0, 24, 2, Map::List},
    {kVoiceMode, "Voice Mode", "", 0, 2, 0, Map::List},
    {kGlide, "Glide", "ms", 1, 3000, 0, Map::LogOffLow},
    {kOscALevel, "Osc A Level", "%", 0, 100, 100, Map::Linear},
    {kTableB, "Table B", "", 0, kTableSlots - 1, 0, Map::List},
    {kPositionB, "Position B", "%", 0, 100, 40, Map::Linear},
    {kOctB, "Octave B", "", -3, 3, 0, Map::List},
    {kSemiB, "Semi B", "st", -12, 12, 0, Map::List},
    {kFineB, "Fine B", "ct", -100, 100, 0, Map::Linear},
    {kOscBLevel, "Osc B Level", "%", 0, 100, 0, Map::Linear},
    {kWarpA, "Warp A", "", 0, wt::kNumWarps - 1, 0, Map::List},
    {kWarpAmtA, "Warp A Amount", "%", 0, 100, 0, Map::Linear},
    {kWarpB, "Warp B", "", 0, wt::kNumWarps - 1, 0, Map::List},
    {kWarpAmtB, "Warp B Amount", "%", 0, 100, 0, Map::Linear},
    {kNoise, "Noise", "%", 0, 100, 0, Map::Linear},
    {kNoiseColor, "Noise Color", "", 0, 3, 0, Map::List},
    {kDriveStyle, "Drive Style", "", 0, 7, 6, Map::List},
    {kDriveMix, "Drive Mix", "%", 0, 100, 100, Map::Linear},
    {kChorusOn, "Chorus On", "", 0, 1, 0, Map::List},
    {kChorusMode, "Chorus Mode", "", 0, 3, 0, Map::List},
    {kChorusMix, "Chorus Mix", "%", 0, 100, 50, Map::Linear},
    {kEchoOn, "Echo On", "", 0, 1, 0, Map::List},
    {kEchoStyle, "Echo Style", "", 0, 7, 7, Map::List},
    {kEchoMode, "Echo Mode", "", 0, 2, 1, Map::List},
    {kEchoSync, "Echo Sync", "", 0, 1, 1, Map::List},
    {kEchoNoteL, "Echo Note L", "", 0, 12, 3, Map::List},
    {kEchoNoteR, "Echo Note R", "", 0, 12, 5, Map::List},
    {kEchoTimeL, "Echo Time L", "ms", 5, 2000, 188, Map::Log},
    {kEchoTimeR, "Echo Time R", "ms", 5, 2000, 250, Map::Log},
    {kEchoFeedback, "Echo Feedback", "%", 0, 110, 25, Map::Linear},
    {kEchoMix, "Echo Mix", "%", 0, 100, 28, Map::Linear},
    {kVerbOn, "Reverb On", "", 0, 1, 0, Map::List},
    {kVerbStyle, "Reverb Style", "", 0, 7, 6, Map::List},
    {kVerbDecay, "Reverb Decay", "s", 0.2, 30, 2.8, Map::Log},
    {kVerbSize, "Reverb Size", "%", 25, 200, 100, Map::Linear},
    {kVerbMix, "Reverb Mix", "%", 0, 100, 25, Map::Linear},
    {kPumpOn, "Pump On", "", 0, 1, 0, Map::List},
    {kPumpMode, "Pump Mode", "", 0, 2, 0, Map::List},
    {kPumpRate, "Pump Rate", "", 0, 7, 2, Map::List},
    {kPumpDepth, "Pump Depth", "%", 0, 100, 45, Map::Linear},
    {kPumpMix, "Pump Mix", "%", 0, 100, 100, Map::Linear},
    {kFxOrder, "FX Order", "", 0, 119, 0, Map::List},
    {kMorphOn, "Morph On", "", 0, 1, 0, Map::List},
    {kMorphX, "Morph X", "%", 0, 100, 50, Map::Linear},
    {kMorphY, "Morph Y", "%", 0, 100, 50, Map::Linear},
    {kSpaceOn, "Space On", "", 0, 1, 0, Map::List},
    {kSpaceX, "Space X", "%", -100, 100, 0, Map::Linear},     // left .. right
    {kSpaceY, "Space Y", "%", -100, 100, 0, Map::Linear},     // below .. above
    {kSpaceZ, "Space Z", "%", -100, 100, 50, Map::Linear},    // behind .. in front
    {kSpaceOrbit, "Space Orbit", "", 0, space::kOrbitSlots - 1, 0, Map::List},
    {kSpaceRate, "Space Rate", "", 0, space::kNumRates - 1, 4, Map::List},
    {kSpaceSize, "Space Size", "%", 0, 100, 70, Map::Linear},
    {kSpaceRoom, "Space Room", "%", 0, 100, 50, Map::Linear},
    {kSpaceHeadphones, "Space Listen", "", 0, 1, 0, Map::List},
    {kSpaceMirror, "Space Mirror", "", 0, 1, 0, Map::List},
    {kSpaceAmount, "Space Amount", "%", 0, 100, 75, Map::Linear},   // 50 = natural, 100 = extreme
    {kSustainPedal, "Sustain Pedal", "%", 0, 100, 0, Map::Linear},  // CC 64; down from 50 %
    {kEqLow, "EQ Low", "dB", -15, 15, 0, Map::Linear},        // shelf at 110 Hz
    {kEqMid, "EQ Mid", "dB", -15, 15, 0, Map::Linear},        // bell, swept by EQ Mid Freq
    {kEqMidFreq, "EQ Mid Freq", "Hz", 150, 8000, 1000, Map::Log},
    {kEqHigh, "EQ High", "dB", -15, 15, 0, Map::Linear},      // shelf at 7 kHz
};
inline const int kDisplayOrder[kNumParams] = {
    kTable, kPosition, kScan, kScanSpeed, kPosEnv, kUnison, kDetune, kSpread, kSub,
    kFilterType, kCutoff, kResonance, kFilterMorph, kFilterDrive, kKeytrack, kHpf, kFilterEnv,
    kAttack, kDecay, kSustain, kRelease, kModAttack, kModDecay, kVelocity, kDrive, kOutput,
    kMod1Src, kMod1Dst, kMod1Amt, kMod2Src, kMod2Dst, kMod2Amt,
    kMod3Src, kMod3Dst, kMod3Amt, kMod4Src, kMod4Dst, kMod4Amt,
    kMod5Src, kMod5Dst, kMod5Amt, kMod6Src, kMod6Dst, kMod6Amt,
    kMod7Src, kMod7Dst, kMod7Amt, kMod8Src, kMod8Dst, kMod8Amt,
    kMod9Src, kMod9Dst, kMod9Amt, kMod10Src, kMod10Dst, kMod10Amt,
    kMod11Src, kMod11Dst, kMod11Amt, kMod12Src, kMod12Dst, kMod12Amt,
    kMod13Src, kMod13Dst, kMod13Amt, kMod14Src, kMod14Dst, kMod14Amt,
    kMod15Src, kMod15Dst, kMod15Amt, kMod16Src, kMod16Dst, kMod16Amt,
    kEnv3A, kEnv3D, kEnv3S, kEnv3R, kLfo1Shape, kLfo1Rate,
    kLfo1Sync, kLfo1Note, kLfo1Mode, kLfo2Shape, kLfo2Rate, kLfo2Sync,
    kLfo2Note, kLfo2Mode, kLfo3Shape, kLfo3Rate, kLfo3Sync, kLfo3Note,
    kLfo3Mode, kLfo4Shape, kLfo4Rate, kLfo4Sync, kLfo4Note, kLfo4Mode,
    kMacro1, kMacro2, kMacro3, kMacro4, kMacro5, kMacro6,
    kMacro7, kMacro8, kModWheel, kPitchBend, kAftertouch, kBendRange,
    kVoiceMode, kGlide,
    kOscALevel, kTableB, kPositionB, kOctB, kSemiB, kFineB, kOscBLevel, kWarpA, kWarpAmtA, kWarpB, kWarpAmtB, kNoise, kNoiseColor,
    kDriveStyle, kDriveMix, kChorusOn, kChorusMode, kChorusMix,
    kEchoOn, kEchoStyle, kEchoMode, kEchoSync, kEchoNoteL, kEchoNoteR, kEchoTimeL, kEchoTimeR, kEchoFeedback, kEchoMix,
    kVerbOn, kVerbStyle, kVerbDecay, kVerbSize, kVerbMix, kPumpOn, kPumpMode, kPumpRate, kPumpDepth, kPumpMix, kFxOrder,
    kMorphOn, kMorphX, kMorphY,
    kSpaceOn, kSpaceX, kSpaceY, kSpaceZ, kSpaceOrbit, kSpaceRate, kSpaceSize, kSpaceRoom, kSpaceHeadphones, kSpaceMirror, kSpaceAmount,
    kSustainPedal,
    kEqLow, kEqMid, kEqMidFreq, kEqHigh,
};

inline int stepCount(const ParamDef& p) { return p.map == Map::List ? (int)(p.max - p.min) : 0; }

inline double toPlain(const ParamDef& p, double n) {
    n = std::clamp(n, 0.0, 1.0);
    switch (p.map) {
        case Map::Linear: return p.min + n * (p.max - p.min);
        case Map::Log: return p.min * std::pow(p.max / p.min, n);
        case Map::LogOffLow: return n < 0.02 ? 0.0 : p.min * std::pow(p.max / p.min, (n - 0.02) / 0.98);
        case Map::LogOffHigh: return n > 0.98 ? 0.0 : p.min * std::pow(p.max / p.min, n / 0.98);
        case Map::List: return std::round(p.min + n * (p.max - p.min));
    }
    return 0;
}

inline double toNorm(const ParamDef& p, double v) {
    switch (p.map) {
        case Map::Linear: return std::clamp((v - p.min) / (p.max - p.min), 0.0, 1.0);
        case Map::Log: return std::clamp(std::log(v / p.min) / std::log(p.max / p.min), 0.0, 1.0);
        case Map::LogOffLow:
            return v <= 0 ? 0.0 : 0.02 + 0.98 * std::clamp(std::log(v / p.min) / std::log(p.max / p.min), 0.0, 1.0);
        case Map::LogOffHigh:
            return (v <= 0 || v >= p.max) ? 1.0 : 0.98 * std::clamp(std::log(v / p.min) / std::log(p.max / p.min), 0.0, 1.0);
        case Map::List: return p.max > p.min ? std::clamp((v - p.min) / (p.max - p.min), 0.0, 1.0) : 0.0;
    }
    return 0;
}

// The plain values the host sees; they must rise with the knob (Live clamps plain values into
// [plain(0), plain(1)]). See harness/test_params.cpp.
inline double hostPlain(const ParamDef& p, double n) {
    if (p.map == Map::LogOffHigh) return p.min * std::pow(p.max / p.min, std::clamp(n, 0.0, 1.0) / 0.98);
    return toPlain(p, n);
}
inline double hostNorm(const ParamDef& p, double v) {
    if (p.map == Map::LogOffHigh)
        return v <= 0 ? 1.0 : std::clamp(0.98 * std::log(v / p.min) / std::log(p.max / p.min), 0.0, 1.0);
    return toNorm(p, v);
}

inline std::string toText(const ParamDef& p, double n) {
    double v = toPlain(p, n);
    char buf[64];
    if (p.id == kTable) return tableName((int)v);
    if (p.id == kScan) return kScanNames[std::clamp((int)v, 0, 3)];
    if (p.id == kFilterType) return kFilterTypeNames[std::clamp((int)v, 0, 5)];
    if (p.id >= kModBase && p.id < kModBase + 3 * wt::kModSlots && (p.id - kModBase) % 3 == 0) return (int)v < wt::kNumSources ? wt::kSourceNames[std::max(0, (int)v)] : "None";
    if (p.id >= kModBase && p.id < kModBase + 3 * wt::kModSlots && (p.id - kModBase) % 3 == 1) return (int)v < wt::kNumDests ? wt::kDestNames[std::max(0, (int)v)] : "None";
    if (p.id >= kLfoBase && p.id < kLfoBase + 5 * wt::kLfos) {
        switch ((p.id - kLfoBase) % 5) {
            case 0: return wt::kLfoShapeNames[std::clamp((int)v, 0, wt::kNumLfoShapes - 1)];
            case 2: return v > 0.5 ? "On" : "Off";
            case 3: return wt::kLfoNoteNames[std::clamp((int)v, 0, wt::kLfoNotes - 1)];
            case 4: return wt::kLfoModeNames[std::clamp((int)v, 0, 2)];
        }
    }
    if (p.id == kVoiceMode) return kVoiceModeNames[std::clamp((int)v, 0, 2)];
    if (p.id == kTableB) return tableName((int)v);
    if (p.id == kWarpA || p.id == kWarpB) return wt::kWarpNames[std::clamp((int)v, 0, wt::kNumWarps - 1)];
    if (p.id == kNoiseColor) return kNoiseColorNames[std::clamp((int)v, 0, 3)];
    if (p.id == kChorusOn || p.id == kEchoOn || p.id == kVerbOn || p.id == kPumpOn || p.id == kEchoSync || p.id == kMorphOn || p.id == kSpaceOn || p.id == kSpaceMirror) return v > 0.5 ? "On" : "Off";
    if (p.id == kSpaceOrbit) return (int)v < space::kNumOrbits ? space::kOrbitNames[std::max(0, (int)v)] : "Still";
    if (p.id == kSpaceRate) return space::kRateNames[std::clamp((int)v, 0, space::kNumRates - 1)];
    if (p.id == kSpaceHeadphones) return v > 0.5 ? "Headphones" : "Speakers";
    if (p.id == kDriveStyle) return kDriveVoices[std::clamp((int)v, 0, 7)].name;
    if (p.id == kChorusMode) return fx::kChorusNames[std::clamp((int)v, 0, 3)];
    if (p.id == kEchoStyle) return kStyles[std::clamp((int)v, 0, 7)].name;
    if (p.id == kEchoMode) { static const char* m[] = {"Single", "Dual", "Ping-Pong"}; return m[std::clamp((int)v, 0, 2)]; }
    if (p.id == kEchoNoteL || p.id == kEchoNoteR) return fx::kEchoNoteNames[std::clamp((int)v, 0, 12)];
    if (p.id == kVerbStyle) return verb::kVerbStyles[std::clamp((int)v, 0, 7)].name;
    if (p.id == kPumpMode) { static const char* m[] = {"Pump", "Sidechain", "Both"}; return m[std::clamp((int)v, 0, 2)]; }
    if (p.id == kPumpRate) return pump::kRateNames[std::clamp((int)v, 0, 7)];
    if (p.id == kFxOrder) { const auto o = fx::orderFromIndex((int)v); std::string t; for (int k = 0; k < 5; ++k) { t += k ? " > " : ""; t += fx::kFxNames[o[k]]; } return t; }
    if (p.id == kOctB) { std::snprintf(buf, sizeof buf, "%+.0f oct", v); return buf; }
    if (p.id == kSemiB) { std::snprintf(buf, sizeof buf, "%+.0f st", v); return buf; }
    if (p.id == kBendRange) { std::snprintf(buf, sizeof buf, "%.0f st", v); return buf; }
    if (p.id == kHpf) return kHpfNames[std::clamp((int)v, 0, 3)];
    if (p.id == kScanSpeed) { std::snprintf(buf, sizeof buf, "%.2fx", v); return buf; }
    if (p.id == kUnison) { std::snprintf(buf, sizeof buf, v < 1.5 ? "%.0f voice" : "%.0f voices", v); return buf; }
    if ((p.map == Map::LogOffLow || p.map == Map::LogOffHigh) && v <= 0) return "Off";
    if (p.id == kDrive) { if (v < 0.005) return "Off"; std::snprintf(buf, sizeof buf, "%.1f", v); return buf; }
    if (p.units[0] == 'H' && v >= 1000) std::snprintf(buf, sizeof buf, "%.2f kHz", v / 1000);
    else if (p.units[0] == 'H' && v < 1) std::snprintf(buf, sizeof buf, "%.2f Hz", v);
    else if (p.units[0] == 'm' && v >= 1000) std::snprintf(buf, sizeof buf, "%.2f s", v / 1000);
    else if (p.units[0] == 'd') std::snprintf(buf, sizeof buf, "%+.1f dB", v);
    else if (p.min < 0 && p.units[0] == '%') std::snprintf(buf, sizeof buf, "%+.0f %%", v);
    else std::snprintf(buf, sizeof buf, std::abs(v) < 10 ? "%.1f %s" : "%.0f %s", v, p.units);
    return buf;
}

inline double toNormFromText(const ParamDef& p, const char* s, bool& ok) {
    ok = true;
    if (p.map == Map::List) {
        for (int i = 0; i <= (int)(p.max - p.min); ++i) if (toText(p, toNorm(p, p.min + i)) == s) return toNorm(p, p.min + i);
        double x = std::atof(s);
        if (x >= p.min && x <= p.max) return toNorm(p, x);
        ok = false; return 0;
    }
    if (!std::strncmp(s, "Off", 3) || !std::strncmp(s, "off", 3)) return p.map == Map::LogOffHigh ? 1.0 : 0.0;
    double x = std::atof(s);
    if (std::strstr(s, "k")) x *= 1000;
    if (p.units[0] == 'm' && std::strstr(s, " s")) x *= 1000;
    return toNorm(p, x);
}

}  // namespace wtsynth
}  // namespace echolab
