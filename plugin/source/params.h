// Parameter table: one source of truth for ids, names, units, ranges and the
// normalized <-> plain mapping (used by both the audio processing and the host display).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "../../dsp/Styles.h"

namespace echolab {

enum ParamId : int {
    kTime = 0, kTimeR, kFeedback, kMix, kStyle, kMode, kSaturation, kLowCut, kHighCut,
    kWobble, kDuck, kDuckRelease, kWidth, kOutput, kBypass, kNumParams
};

enum class Map { Linear, Log, LogOffLow, LogOffHigh, List };

struct ParamDef {
    int id;
    const char* name;
    const char* units;
    double min, max, def;  // plain values
    Map map;
};

inline const char* const kModeNames[] = {"Single", "Dual", "Ping-Pong"};

inline const ParamDef kParams[kNumParams] = {
    {kTime, "Time", "ms", 10, 2000, 250, Map::Log},
    {kTimeR, "Time R", "ms", 10, 2000, 375, Map::Log},
    {kFeedback, "Feedback", "%", 0, 110, 40, Map::Linear},
    {kMix, "Mix", "%", 0, 100, 30, Map::Linear},
    {kStyle, "Style", "", 0, kNumStyles - 1, 0, Map::List},
    {kMode, "Mode", "", 0, 2, 0, Map::List},
    {kSaturation, "Saturation", "dB", -12, 24, 0, Map::Linear},
    {kLowCut, "Low Cut", "Hz", 20, 1000, 0, Map::LogOffLow},      // 0 = off
    {kHighCut, "High Cut", "Hz", 1000, 20000, 0, Map::LogOffHigh}, // 0 = off
    {kWobble, "Wobble", "%", 0, 200, 100, Map::Linear},
    {kDuck, "Duck", "%", 0, 100, 0, Map::Linear},
    {kDuckRelease, "Duck Release", "ms", 20, 2000, 250, Map::Log},
    {kWidth, "Width", "%", 0, 100, 100, Map::Linear},
    {kOutput, "Output", "dB", -24, 12, 0, Map::Linear},
    {kBypass, "Bypass", "", 0, 1, 0, Map::List},
};

inline int stepCount(const ParamDef& p) { return p.map == Map::List ? (int)(p.max - p.min) : 0; }

// normalized [0,1] -> plain
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

// plain -> normalized
inline double toNorm(const ParamDef& p, double v) {
    switch (p.map) {
        case Map::Linear: return std::clamp((v - p.min) / (p.max - p.min), 0.0, 1.0);
        case Map::Log: return std::clamp(std::log(v / p.min) / std::log(p.max / p.min), 0.0, 1.0);
        case Map::LogOffLow:
            return v <= 0 ? 0.0 : 0.02 + 0.98 * std::clamp(std::log(v / p.min) / std::log(p.max / p.min), 0.0, 1.0);
        case Map::LogOffHigh:
            return v <= 0 ? 1.0 : 0.98 * std::clamp(std::log(v / p.min) / std::log(p.max / p.min), 0.0, 1.0);
        case Map::List: return p.max > p.min ? std::clamp((v - p.min) / (p.max - p.min), 0.0, 1.0) : 0.0;
    }
    return 0;
}

inline std::string toText(const ParamDef& p, double n) {
    double v = toPlain(p, n);
    char buf[64];
    if (p.id == kStyle) return kStyles[std::clamp((int)v, 0, kNumStyles - 1)].name;
    if (p.id == kMode) return kModeNames[std::clamp((int)v, 0, 2)];
    if (p.id == kBypass) return v > 0.5 ? "On" : "Off";
    if ((p.map == Map::LogOffLow || p.map == Map::LogOffHigh) && v <= 0) return "Off";
    if (p.units[0] == 'H' && v >= 1000) std::snprintf(buf, sizeof buf, "%.2f kHz", v / 1000);
    else if (p.units[0] == 'm' && v >= 1000) std::snprintf(buf, sizeof buf, "%.2f s", v / 1000);
    else if (p.units[0] == 'd') std::snprintf(buf, sizeof buf, "%+.1f dB", v);
    else std::snprintf(buf, sizeof buf, v < 100 ? "%.1f %s" : "%.0f %s", v, p.units);
    return buf;
}

}  // namespace echolab
