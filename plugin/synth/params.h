// Wavetable synth parameters (one table: ids, names, units, ranges, mapping). Ids are stable: append only.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace echolab {
namespace wtsynth {

enum ParamId : int {
    kPosition = 0, kUnison, kDetune, kSpread, kSub, kCutoff, kResonance, kFilterEnv, kPosEnv,
    kAttack, kDecay, kSustain, kRelease, kModAttack, kModDecay, kVelocity, kDrive, kOutput,
    kTable, kScan, kScanSpeed,  // v0.3
    kNumParams
};
inline const char* const kTableNames[] = {"ORPHEO", "MOOG", "PWM", "SYNC", "JUNO", "GROWN"};
constexpr int kTableGrown = 5;
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
    {kTable, "Table", "", 0, 5, 0, Map::List},
    {kScan, "Scan", "", 0, 3, 0, Map::List},
    {kScanSpeed, "Scan Speed", "x", 0.25, 4, 1, Map::Log},
};
inline const int kDisplayOrder[kNumParams] = {
    kTable, kPosition, kScan, kScanSpeed, kPosEnv, kUnison, kDetune, kSpread, kSub, kCutoff, kResonance, kFilterEnv,
    kAttack, kDecay, kSustain, kRelease, kModAttack, kModDecay, kVelocity, kDrive, kOutput,
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
    if (p.id == kTable) return kTableNames[std::clamp((int)v, 0, 5)];
    if (p.id == kScan) return kScanNames[std::clamp((int)v, 0, 3)];
    if (p.id == kScanSpeed) { std::snprintf(buf, sizeof buf, "%.2fx", v); return buf; }
    if (p.id == kUnison) { std::snprintf(buf, sizeof buf, v < 1.5 ? "%.0f voice" : "%.0f voices", v); return buf; }
    if ((p.map == Map::LogOffLow || p.map == Map::LogOffHigh) && v <= 0) return "Off";
    if (p.id == kDrive) { if (v < 0.005) return "Off"; std::snprintf(buf, sizeof buf, "%.1f", v); return buf; }
    if (p.units[0] == 'H' && v >= 1000) std::snprintf(buf, sizeof buf, "%.2f kHz", v / 1000);
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
