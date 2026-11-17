// Drive Lab parameters (one table: ids, names, units, ranges, mapping).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "../../dsp/DriveVoices.h"

namespace echolab {
namespace drive {

enum ParamId : int {
    kDrive = 0, kStyle, kPunish, kTone, kLowCut, kHighCut, kMix, kAutoGain, kOutput, kBypass,
    kPump, kPumpRate, kFilter, kResonance, kNumParams
};
enum class Map { Linear, Log, LogOffLow, LogOffHigh, List };
struct ParamDef { int id; const char* name; const char* units; double min, max, def; Map map; };

inline const char* const kRateNames[] = {"1/2", "1/4", "1/8"};

inline const ParamDef kParams[kNumParams] = {
    {kDrive, "Drive", "", 0, 10, 4, Map::Linear},
    {kStyle, "Style", "", 0, kNumDriveVoices - 1, 0, Map::List},
    {kPunish, "Punish", "", 0, 1, 0, Map::List},
    {kTone, "Tone", "dB", -12, 12, 0, Map::Linear},
    {kLowCut, "Low Cut", "Hz", 20, 1000, 0, Map::LogOffLow},
    {kHighCut, "High Cut", "Hz", 1000, 20000, 0, Map::LogOffHigh},
    {kMix, "Mix", "%", 0, 100, 100, Map::Linear},
    {kAutoGain, "Auto Gain", "", 0, 1, 1, Map::List},
    {kOutput, "Output", "dB", -24, 12, 0, Map::Linear},
    {kBypass, "Bypass", "", 0, 1, 0, Map::List},
    {kPump, "Pump", "%", 0, 100, 0, Map::Linear},
    {kPumpRate, "Pump Rate", "", 0, 2, 1, Map::List},
    {kFilter, "Filter", "Hz", 80, 20000, 20000, Map::LogOffHigh},
    {kResonance, "Resonance", "%", 0, 100, 20, Map::Linear},
};

// host shows the musical controls first
inline const int kDisplayOrder[kNumParams] = {
    kDrive, kStyle, kPunish, kTone, kPump, kPumpRate, kFilter, kResonance, kMix,
    kLowCut, kHighCut, kAutoGain, kOutput, kBypass,
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

// The plain values the host sees (VST3 normalizedParamToPlain / plainParamToNormalized).
// They must rise with the knob: Live clamps plain values into [plain(0), plain(1)], so an "Off"
// stored as 0 at the top of a knob collapses that range and every setting snaps to the bottom.
// For Off-at-top knobs the log curve simply continues past max into the Off zone; the DSP side
// (toPlain/toNorm) keeps its 0 = off convention.
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
    if (p.id == kStyle) return kDriveVoices[std::clamp((int)v, 0, kNumDriveVoices - 1)].name;
    if (p.id == kPumpRate) return kRateNames[std::clamp((int)v, 0, 2)];
    if (p.id == kPunish || p.id == kAutoGain || p.id == kBypass) return v > 0.5 ? "On" : "Off";
    if ((p.map == Map::LogOffLow || p.map == Map::LogOffHigh) && v <= 0) return "Off";
    if (p.id == kDrive) { std::snprintf(buf, sizeof buf, "%.1f", v); return buf; }
    if (p.units[0] == 'H' && v >= 1000) std::snprintf(buf, sizeof buf, "%.2f kHz", v / 1000);
    else if (p.units[0] == 'd') std::snprintf(buf, sizeof buf, "%+.1f dB", v);
    else std::snprintf(buf, sizeof buf, v < 100 ? "%.1f %s" : "%.0f %s", v, p.units);
    return buf;
}

}  // namespace drive
}  // namespace echolab
