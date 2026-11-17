// Charon parameters (one table: ids, names, units, ranges, mapping). Ids are stable: append only.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../../dsp/PumpEngine.h"

namespace echolab {
namespace pumpfx {

enum ParamId : int {
    kMode = 0, kDepth, kRate, kAttack, kHold, kCurve, kPhase, kScThresh, kScAttack, kScRelease,
    kFilter, kFilterAmt, kResonance, kMix, kOutput, kBypass, kNumParams
};
enum class Map { Linear, Log, LogOffLow, LogOffHigh, List };
struct ParamDef { int id; const char* name; const char* units; double min, max, def; Map map; };
inline const char* const kModeNames[] = {"Pump", "Sidechain", "Both"};

inline const ParamDef kParams[kNumParams] = {
    {kMode, "Mode", "", 0, 2, 0, Map::List},
    {kDepth, "Depth", "%", 0, 100, 60, Map::Linear},
    {kRate, "Rate", "", 0, pump::kNumRates - 1, 2, Map::List},
    {kAttack, "Attack", "%", 0, 50, 5, Map::Linear},
    {kHold, "Hold", "%", 0, 50, 0, Map::Linear},
    {kCurve, "Curve", "%", -100, 100, 0, Map::Linear},
    {kPhase, "Phase", "%", 0, 100, 0, Map::Linear},
    {kScThresh, "SC Threshold", "dB", -60, 0, -30, Map::Linear},
    {kScAttack, "SC Attack", "ms", 0.1, 50, 2, Map::Log},
    {kScRelease, "SC Release", "ms", 10, 1000, 180, Map::Log},
    {kFilter, "Filter", "Hz", 200, 20000, 0, Map::LogOffHigh},
    {kFilterAmt, "Filter Amount", "%", 0, 100, 0, Map::Linear},
    {kResonance, "Resonance", "%", 0, 100, 20, Map::Linear},
    {kMix, "Mix", "%", 0, 100, 100, Map::Linear},
    {kOutput, "Output", "dB", -24, 12, 0, Map::Linear},
    {kBypass, "Bypass", "", 0, 1, 0, Map::List},
};
inline const int kDisplayOrder[kNumParams] = {
    kMode, kDepth, kRate, kCurve, kAttack, kHold, kPhase, kScThresh, kScAttack, kScRelease,
    kFilter, kFilterAmt, kResonance, kMix, kOutput, kBypass,
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
    if (p.id == kMode) return kModeNames[std::clamp((int)v, 0, 2)];
    if (p.id == kRate) return pump::kRateNames[std::clamp((int)v, 0, pump::kNumRates - 1)];
    if (p.id == kBypass) return v > 0.5 ? "On" : "Off";
    if ((p.map == Map::LogOffLow || p.map == Map::LogOffHigh) && v <= 0) return "Off";
    if (p.units[0] == 'H' && v >= 1000) std::snprintf(buf, sizeof buf, "%.2f kHz", v / 1000);
    else if (p.units[0] == 'd') std::snprintf(buf, sizeof buf, "%+.1f dB", v);
    else std::snprintf(buf, sizeof buf, std::abs(v) < 100 ? "%.1f %s" : "%.0f %s", v, p.units);
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

}  // namespace pumpfx
}  // namespace echolab
