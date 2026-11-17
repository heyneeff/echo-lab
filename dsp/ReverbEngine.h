#pragma once
// Lethe: algorithmic reverb.
//
//  in ─ predelay ─┬─ early reflections (tapped, panned) ───────────────────────────┐
//                 └─ input diffusion (4 allpasses) ─ (+) ─► 8-line FDN ─┬─ taps ─(+)─ wet EQ ─ duck ─ mix ─ out
//                                                    ▲                  │
//                                                    └─ Hadamard mix ◄──┴─ per-line 3-band decay (lo/mid/hi RT60)
//
// Each FDN line has its own decay gain computed from the RT60 the user asked for, split into three
// bands (bass multiplier, mid, damping above the crossover), so the tail darkens like real air.
// Slow, independent modulation of each line's length keeps long tails from ringing metallic.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace echolab {
namespace verb {

constexpr double kPi = 3.14159265358979323846;
constexpr int kLines = 8;

struct OnePoleLP {
    double a = 0, z = 0;
    void set(double hz, double sr) { a = std::exp(-2 * kPi * std::min(hz, sr * 0.45) / sr); }
    double tick(double x) { z = x + a * (z - x); return z; }
    void reset() { z = 0; }
};

struct Biquad {  // RBJ high/low pass, used on the wet signal
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void flat() { b0 = 1; b1 = b2 = a1 = a2 = 0; }
    void set(bool high, double f, double q, double sr) {
        double w = 2 * kPi * std::min(f, sr * 0.45) / sr, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
        if (high) { b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = b0; } else { b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0; }
        a1 = -2 * c / a0; a2 = (1 - al) / a0;
    }
    double tick(double x) { double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    void reset() { z1 = z2 = 0; }
};

struct Delay {  // power-of-two ring with Hermite fractional reads
    std::vector<double> buf; int mask = 0, w = 0;
    void init(int minSize) { int n = 1; while (n < minSize) n <<= 1; buf.assign(n, 0.0); mask = n - 1; w = 0; }
    void push(double x) { buf[w] = x; w = (w + 1) & mask; }
    double read(double d) const {  // d samples behind the newest sample
        double pos = (double)w - 1 - d; int i = (int)std::floor(pos); double t = pos - i;
        double y0 = buf[(i - 1) & mask], y1 = buf[i & mask], y2 = buf[(i + 1) & mask], y3 = buf[(i + 2) & mask];
        double c1 = 0.5 * (y2 - y0), c2 = y0 - 2.5 * y1 + 2 * y2 - 0.5 * y3, c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + y1;
    }
    double tap(int d) const { return buf[(w - 1 - d) & mask]; }
    void reset() { std::fill(buf.begin(), buf.end(), 0.0); w = 0; }
};

struct Allpass {  // Schroeder allpass diffuser
    Delay d; int len = 1; double g = 0.6;
    double tick(double x) { double v = d.tap(len - 1); double u = x + g * v; d.push(u); return v - g * u; }
};

// A style shapes the space: how big the room is relative to Size, how loud and how spread its walls
// (early reflections) are, how quickly it diffuses, how it moves, and how its tail colours.
struct Style {
    const char* name;
    double sizeMul = 1.0;      // scales the FDN line lengths
    double erLevel = 0.5;      // early reflections
    double erSpread = 1.0;     // reflection timing scale
    double diffusion = 0.7;    // input allpass gain
    double modDepthMs = 0.5;   // tail motion
    double dampMul = 1.0;      // scales the damping crossover
    double hfRatio = 0.6;      // RT60 above the crossover, relative to mid
    double bassRatio = 1.1;    // RT60 below 250 Hz, relative to mid
    double wetLPHz = 18000;    // overall wet bandwidth
    double outDb = 0;          // level match between styles (measured: equal tail energy)
};

inline const std::array<Style, 8> kVerbStyles = {{
    {"Hall", 1.0, 0.45, 1.0, 0.70, 0.55, 1.0, 0.55, 1.15, 16000, 0.00},
    {"Plate", 0.55, 0.0, 0.5, 0.82, 0.35, 1.8, 0.85, 0.9, 19000, -3.45},
    {"Room", 0.35, 0.8, 0.45, 0.62, 0.25, 0.8, 0.5, 1.0, 14000, -3.99},
    {"Chamber", 0.6, 0.6, 0.7, 0.75, 0.4, 0.9, 0.6, 1.2, 12000, -1.84},
    {"Cathedral", 1.7, 0.35, 1.8, 0.7, 0.7, 0.7, 0.45, 1.35, 11000, 4.27},
    {"Underworld", 2.0, 0.15, 2.0, 0.78, 1.4, 0.35, 0.3, 1.6, 6500, 8.63},
    {"ORPHEO (soon)", 1.0, 0.45, 1.0, 0.70, 0.55, 1.0, 0.55, 1.15, 16000},
    {"Spare (soon)", 1.0, 0.45, 1.0, 0.70, 0.55, 1.0, 0.55, 1.15, 16000},
}};

struct Params {
    double size = 1.0;          // 0.25..2 (room scale)
    double decayS = 2.5;        // mid-band RT60, seconds
    double predelayMs = 10;
    double dampingHz = 6000;    // crossover above which the tail decays faster
    double bass = 1.0;          // multiplier on the style's bass ratio
    double diffusion = 1.0;     // 0..1 (scales the style's diffusion)
    double modulation = 1.0;    // 0..2 (scales the style's motion)
    double early = 1.0;         // 0..2 (scales the style's reflections)
    double lowCutHz = 0, highCutHz = 0;  // 0 = off
    double width = 1.0, mix = 0.3, outputDb = 0;
    double duck = 0, duckReleaseMs = 300;
    bool freeze = false;
};

class ReverbEngine {
public:
    void prepare(double sampleRate) {
        sr = sampleRate;
        pre.init((int)(0.30 * sr) + 8);
        for (auto& l : line) l.init((int)(0.45 * sr) + 64);  // longest line: 80 ms * 2 size * 2 style + mod
        for (auto& a : apLoop) a.d.init((int)(0.06 * sr) + 8);
        static const double apMs[4] = {4.77, 3.59, 12.73, 9.31};
        for (int i = 0; i < 4; ++i) for (int c = 0; c < 2; ++c) { ap[c][i].d.init((int)(apMs[i] * 0.001 * sr) + 4); ap[c][i].len = (int)(apMs[i] * 0.001 * sr); }
        reset();
        update(params, style);
    }
    void reset() {
        pre.reset();
        for (auto& l : line) l.reset();
        for (auto& c : ap) for (auto& a : c) a.d.reset();
        for (auto& a : apLoop) a.d.reset();
        for (auto& f : lo) f.reset(); for (auto& f : hi) f.reset();
        for (auto& b : wetLo) b.reset(); for (auto& b : wetHi) b.reset(); for (auto& b : wetBand) b.reset();
        env = 0;
    }

    void setStyle(const Style& s) { style = s; update(params, style); }

    void update(const Params& p, const Style& s) {
        params = p; style = s;
        // FDN line lengths: mutually prime-ish, scaled by size and style
        static const double baseMs[kLines] = {29.71, 37.13, 41.11, 43.73, 53.03, 59.93, 67.73, 79.31};
        const double sc = std::clamp(p.size, 0.25, 2.0) * s.sizeMul;
        for (int i = 0; i < kLines; ++i) lenS[i] = std::max(8.0, baseMs[i] * 0.001 * sr * sc);
        // a short diffuser inside every loop: each pass smears further, so the tail goes dense fast
        static const double apLoopMs[kLines] = {3.13, 4.27, 5.39, 6.61, 7.93, 9.07, 10.33, 11.87};
        for (int i = 0; i < kLines; ++i) {
            apLoop[i].len = std::max(2, (int)(apLoopMs[i] * 0.001 * sr * std::min(sc, 1.5)));
            apLoop[i].g = std::clamp(0.3 + 0.35 * s.diffusion * p.diffusion, 0.0, 0.65);
        }
        // three-band decay per line: g = 10^(-3 * len / (RT60 * sr))
        const double rtMid = std::clamp(p.decayS, 0.1, 60.0);
        const double rtLo = rtMid * std::clamp(s.bassRatio * p.bass, 0.2, 3.0);
        const double rtHi = rtMid * std::clamp(s.hfRatio, 0.05, 1.2);
        const double xover = std::clamp(p.dampingHz * s.dampMul, 500.0, sr * 0.45);
        for (int i = 0; i < kLines; ++i) {
            const double secs = (lenS[i] + modAvg() + apLoop[i].len) / sr;
            gMid[i] = std::pow(10.0, -3.0 * secs / rtMid);
            gLo[i] = std::pow(10.0, -3.0 * secs / rtLo);
            gHi[i] = std::pow(10.0, -3.0 * secs / rtHi);
            lo[i].set(250.0, sr);
            hi[i].set(xover, sr);
        }
        for (int c = 0; c < 2; ++c) for (auto& a : ap[c]) a.g = std::clamp(s.diffusion * p.diffusion, 0.0, 0.85);
        modDepth = s.modDepthMs * 0.001 * sr * std::clamp(p.modulation, 0.0, 2.0);
        predelay = std::clamp(p.predelayMs, 0.0, 250.0) * 0.001 * sr;
        erGain = s.erLevel * std::clamp(p.early, 0.0, 2.0);
        static const double erMs[12] = {7.1, 11.3, 13.9, 19.7, 23.3, 29.9, 34.7, 41.3, 47.9, 53.1, 61.7, 71.9};
        for (int k = 0; k < 12; ++k) {
            erTap[k] = (int)(erMs[k] * 0.001 * sr * s.erSpread * std::clamp(p.size, 0.25, 2.0));
            erAmp[k] = std::pow(0.82, k) * (k % 3 == 0 ? 1.0 : 0.8);
        }
        for (int c = 0; c < 2; ++c) {
            if (p.lowCutHz > 0) wetLo[c].set(true, p.lowCutHz, 0.707, sr); else wetLo[c].flat();
            double hc = p.highCutHz > 0 ? std::min(p.highCutHz, s.wetLPHz) : s.wetLPHz;
            wetHi[c].set(false, hc, 0.707, sr);
            wetBand[c].flat();
        }
        duckAtt = std::exp(-1.0 / (0.004 * sr));
        duckRel = std::exp(-1.0 / (std::max(p.duckReleaseMs, 10.0) * 0.001 * sr));
        outG = std::pow(10.0, (p.outputDb + s.outDb) / 20.0);
        for (int i = 0; i < kLines; ++i) lfoInc[i] = 2 * kPi * (0.13 + 0.071 * i) / sr;
    }

    void process(float* L, float* R, int n) {
        const double dryG = std::cos(params.mix * 0.5 * kPi), wetG = std::sin(params.mix * 0.5 * kPi);
        const bool freeze = params.freeze;
        for (int i = 0; i < n; ++i) {
            const double xl = L[i], xr = R ? R[i] : L[i];
            const double mono = 0.5 * (xl + xr);
            // ducking follower on the dry input
            double a = std::abs(mono);
            env = a > env ? a + duckAtt * (env - a) : a + duckRel * (env - a);
            const double duckGain = 1.0 / (1.0 + params.duck * 10.0 * env);

            pre.push(freeze ? 0.0 : mono);
            const double pd = pre.read(std::max(predelay, 1.0));
            // early reflections straight from the pre-delay line, alternating sides
            double erL = 0, erR = 0;
            if (erGain > 0 && !freeze)
                for (int k = 0; k < 12; ++k) {
                    double v = pre.read(std::max(predelay, 1.0) + erTap[k]) * erAmp[k];
                    if (k & 1) erR += v; else erL += v;
                }
            // diffused input (two decorrelated chains)
            double dl = pd, dr = pd;
            for (int k = 0; k < 4; ++k) { dl = ap[0][k].tick(dl); dr = ap[1][k].tick(-dr); }

            // read the 8 lines (modulated), apply 3-band decay
            double y[kLines];
            for (int k = 0; k < kLines; ++k) {
                lfo[k] += lfoInc[k]; if (lfo[k] > 2 * kPi) lfo[k] -= 2 * kPi;
                double d = lenS[k] + modDepth * (1.0 + std::sin(lfo[k]));
                double v = apLoop[k].tick(line[k].read(d));
                if (freeze) { y[k] = v * 0.99997; continue; }  // a hair of loss: interpolation must never pump energy in
                double l = lo[k].tick(v), h = v - hi[k].tick(v), m = v - l - h;
                y[k] = gLo[k] * l + gMid[k] * m + gHi[k] * h;
            }
            // Hadamard 8x8 (fast, lossless): three butterfly stages, scaled 1/sqrt(8)
            double h8[kLines];
            for (int k = 0; k < kLines; ++k) h8[k] = y[k];
            for (int s = 1; s < kLines; s <<= 1)
                for (int k = 0; k < kLines; k += 2 * s)
                    for (int j = k; j < k + s; ++j) { double u = h8[j], v = h8[j + s]; h8[j] = u + v; h8[j + s] = u - v; }
            const double norm = 1.0 / std::sqrt((double)kLines);
            for (int k = 0; k < kLines; ++k) {
                double in = (k & 1) ? dr : dl;
                line[k].push(h8[k] * norm + (freeze ? 0.0 : in * 0.5));
            }
            // outputs: two orthogonal sign patterns over the lines
            static const int sL[kLines] = {1, -1, 1, 1, -1, 1, -1, -1}, sR[kLines] = {1, 1, -1, 1, 1, -1, -1, 1};
            double lateL = 0, lateR = 0;
            for (int k = 0; k < kLines; ++k) { lateL += sL[k] * y[k]; lateR += sR[k] * y[k]; }
            lateL *= norm; lateR *= norm;
            double wl = lateL + erL * erGain * 0.35, wr = lateR + erR * erGain * 0.35;
            wl = wetHi[0].tick(wetLo[0].tick(wl)); wr = wetHi[1].tick(wetLo[1].tick(wr));
            // width (mid/side) and duck
            double m = 0.5 * (wl + wr), s = 0.5 * (wl - wr) * params.width;
            wl = (m + s) * duckGain; wr = (m - s) * duckGain;
            // meters for the editor
            meter.in += mono * mono; meter.wet += 0.5 * (wl * wl + wr * wr);
            meter.er += 0.5 * (erL * erL + erR * erR) * erGain * erGain * 0.12; meter.duckGain += duckGain; meter.n++;
            L[i] = (float)((xl * dryG + wl * wetG) * outG);
            if (R) R[i] = (float)((xr * dryG + wr * wetG) * outG);
        }
    }

    // smallest line length (for display / maths)
    double lineMs(int k) const { return lenS[k] * 1000.0 / sr; }

    Params params;
    Style style = kVerbStyles[0];
    struct Meter { double in = 0, wet = 0, er = 0, duckGain = 0; int n = 0; } meter;

private:
    double modAvg() const { return modDepth; }
    double sr = 44100;
    Delay pre;
    std::array<Delay, kLines> line;
    std::array<std::array<Allpass, 4>, 2> ap;
    std::array<Allpass, kLines> apLoop;
    std::array<OnePoleLP, kLines> lo, hi;
    std::array<double, kLines> lenS{}, gLo{}, gMid{}, gHi{}, lfo{}, lfoInc{};
    std::array<Biquad, 2> wetLo, wetHi, wetBand;
    std::array<int, 12> erTap{};
    std::array<double, 12> erAmp{};
    double predelay = 0, erGain = 0, modDepth = 0, env = 0, duckAtt = 0, duckRel = 0, outG = 1;
};

}  // namespace verb
}  // namespace echolab
