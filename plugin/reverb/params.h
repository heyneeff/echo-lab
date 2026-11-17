// Lethe Reverb parameters (one table: ids, names, units, ranges, mapping). Ids are stable: append only.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../../dsp/ReverbEngine.h"

namespace echolab {
namespace reverb {

enum ParamId : int {
    kSize = 0, kDecay, kPredelay, kStyle, kMix, kDamping, kBass, kDiffusion, kModulation, kEarly,
    kLowCut, kHighCut, kWidth, kDuck, kDuckRelease, kFreeze, kOutput, kBypass, kNumParams
};
enum class Map { Linear, Log, LogOffLow, LogOffHigh, List };
struct ParamDef { int id; const char* name; const char* units; double min, max, def; Map map; };
constexpr int kNumVerbStyles = (int)verb::kVerbStyles.size();

inline const ParamDef kParams[kNumParams] = {
    {kSize, "Size", "%", 25, 200, 100, Map::Linear},
    {kDecay, "Decay", "s", 0.2, 30, 2.5, Map::Log},
    {kPredelay, "Pre-Delay", "ms", 0, 250, 10, Map::Linear},
    {kStyle, "Style", "", 0, kNumVerbStyles - 1, 0, Map::List},
    {kMix, "Mix", "%", 0, 100, 30, Map::Linear},
    {kDamping, "Damping", "Hz", 1000, 20000, 6000, Map::Log},
    {kBass, "Bass", "%", 50, 200, 100, Map::Linear},
    {kDiffusion, "Diffusion", "%", 0, 100, 100, Map::Linear},
    {kModulation, "Modulation", "%", 0, 200, 100, Map::Linear},
    {kEarly, "Early", "%", 0, 200, 100, Map::Linear},
    {kLowCut, "Low Cut", "Hz", 20, 1000, 0, Map::LogOffLow},
    {kHighCut, "High Cut", "Hz", 1000, 20000, 0, Map::LogOffHigh},
    {kWidth, "Width", "%", 0, 100, 100, Map::Linear},
    {kDuck, "Duck", "%", 0, 100, 0, Map::Linear},
    {kDuckRelease, "Duck Release", "ms", 20, 2000, 300, Map::Log},
    {kFreeze, "Freeze", "", 0, 1, 0, Map::List},
    {kOutput, "Output", "dB", -24, 12, 0, Map::Linear},
    {kBypass, "Bypass", "", 0, 1, 0, Map::List},
};

inline const int kDisplayOrder[kNumParams] = {
    kSize, kDecay, kStyle, kMix, kPredelay, kDamping, kBass, kDiffusion, kModulation, kEarly,
    kDuck, kDuckRelease, kFreeze, kLowCut, kHighCut, kWidth, kOutput, kBypass,
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

inline std::string toText(const ParamDef& p, double n) {
    double v = toPlain(p, n);
    char buf[64];
    if (p.id == kStyle) return verb::kVerbStyles[std::clamp((int)v, 0, kNumVerbStyles - 1)].name;
    if (p.id == kFreeze || p.id == kBypass) return v > 0.5 ? "On" : "Off";
    if ((p.map == Map::LogOffLow || p.map == Map::LogOffHigh) && v <= 0) return "Off";
    if (p.id == kDecay) { std::snprintf(buf, sizeof buf, v < 10 ? "%.2f s" : "%.1f s", v); return buf; }
    if (p.units[0] == 'H' && v >= 1000) std::snprintf(buf, sizeof buf, "%.2f kHz", v / 1000);
    else if (p.units[0] == 'd') std::snprintf(buf, sizeof buf, "%+.1f dB", v);
    else std::snprintf(buf, sizeof buf, v < 100 ? "%.1f %s" : "%.0f %s", v, p.units);
    return buf;
}

inline double toNormFromText(const ParamDef& p, const char* s, bool& ok) {
    ok = true;
    if (p.map == Map::List) {
        for (int i = 0; i <= (int)(p.max - p.min); ++i) if (toText(p, toNorm(p, p.min + i)) == s) return toNorm(p, p.min + i);
        ok = false; return 0;
    }
    if (!std::strncmp(s, "Off", 3)) return p.map == Map::LogOffHigh ? 1.0 : 0.0;
    double x = std::atof(s);
    if (std::strstr(s, "k")) x *= 1000;
    return toNorm(p, x);
}

}  // namespace reverb
}  // namespace echolab
