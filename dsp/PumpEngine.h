#pragma once
// Charon: pump / duck.
//
//   PUMP       a tempo-locked volume envelope (the "whole track breathes" trick): sharp dip on the beat,
//              optional hold, then a shaped recovery. Rate 1/1 .. 1/16 incl. triplets, phase offset.
//   SIDECHAIN  real ducking from the sidechain input (route a kick in): fast follower, threshold,
//              depth; release shapes the swell back.
//   BOTH       the deeper of the two at any moment.
// The duck can also close a resonant low-pass (the filtered-disco pump). Gain changes are smoothed
// (~1 ms) so the dip never clicks.
#include <algorithm>
#include <array>
#include <cmath>

namespace echolab {
namespace pump {

constexpr double kPi = 3.14159265358979323846;
inline const char* const kRateNames[] = {"1/1", "1/2", "1/4", "1/8", "1/16", "1/2T", "1/4T", "1/8T"};
inline const double kRateBeats[] = {4, 2, 1, 0.5, 0.25, 4.0 / 3, 2.0 / 3, 1.0 / 3};
constexpr int kNumRates = 8;

struct Params {
    int mode = 0;               // 0 pump, 1 sidechain, 2 both
    double depth = 0.6;         // 0..1
    int rate = 2;               // index into kRateBeats
    double attack = 0.05;       // fraction of the cycle spent diving (0 = instant)
    double hold = 0.0;          // fraction of the cycle held at the bottom
    double curve = 0.0;         // -1..1: negative = slow early recovery, positive = fast early
    double phase = 0.0;         // 0..1 offset of the dip within the cycle
    double scThreshDb = -30, scAttackMs = 2, scReleaseMs = 180;
    double filterHz = 0;        // 0 = off; cutoff reached at full duck
    double filterAmt = 0;       // 0..1 how far the filter follows the duck
    double resonance = 0.2;     // 0..1
    double mix = 1.0, outputDb = 0;
};

class PumpEngine {
public:
    void prepare(double sampleRate) { sr = sampleRate; reset(); update(params); }
    void reset() { env = 0; gSmooth = 1; ic1.fill(0); ic2.fill(0); }
    void update(const Params& p) {
        params = p;
        scAtt = std::exp(-1.0 / (std::max(p.scAttackMs, 0.05) * 0.001 * sr));
        scRel = std::exp(-1.0 / (std::max(p.scReleaseMs, 5.0) * 0.001 * sr));
        smooth = std::exp(-1.0 / (0.0008 * sr));
        outG = std::pow(10.0, p.outputDb / 20.0);
    }

    // the pump envelope's depth (0..1, 1 = fully ducked) at a phase in its cycle
    double pumpShape(double ph) const {
        ph = ph - params.phase; ph -= std::floor(ph);
        const double a = std::clamp(params.attack, 0.0, 0.5), h = std::clamp(params.hold, 0.0, 0.5);
        if (ph < a) return a > 0 ? ph / a : 1.0;                       // diving
        if (ph < a + h) return 1.0;                                      // held at the bottom
        double r = (ph - a - h) / std::max(1e-6, 1.0 - a - h);          // recovering 0..1
        double c = std::pow(2.0, params.curve * 2.5);                   // curve -1..1 -> exponent 0.18..5.7
        return std::pow(1.0 - r, c);
    }

    // process a block; beat = musical position (quarter notes) at the block start, beatsPerSample
    void process(float* L, float* R, const float* scL, const float* scR, int n, double beat, double beatsPerSample) {
        const double per = kRateBeats[std::clamp(params.rate, 0, kNumRates - 1)];
        const double thr = std::pow(10.0, params.scThreshDb / 20.0);
        const double dryG = 1.0 - params.mix;
        const bool filt = params.filterHz > 0 && params.filterAmt > 0;
        for (int i = 0; i < n; ++i) {
            double duck = 0;
            if (params.mode != 1) { double b = beat + i * beatsPerSample; double ph = b / per; ph -= std::floor(ph); duck = pumpShape(ph); lastPhase = ph; }
            if (params.mode != 0 && scL) {
                double x = std::abs(scL[i]); if (scR) x = std::max(x, (double)std::abs(scR[i]));
                env = x > env ? x + scAtt * (env - x) : x + scRel * (env - x);
                double over = env > thr ? std::clamp(20.0 * std::log10(env / thr) / 18.0, 0.0, 1.0) : 0.0;  // 18 dB over = full
                scDuck = over;
                duck = params.mode == 2 ? std::max(duck, over) : over;
            }
            const double target = 1.0 - params.depth * duck;
            gSmooth = target + smooth * (gSmooth - target);
            const double g = gSmooth, d = (1.0 - g) / std::max(params.depth, 1e-6);  // 0..1 how ducked we are
            lastGain = g;
            double fg = 0, k = 2, a1 = 0, a2 = 0, a3 = 0;
            if (filt) {  // TPT state-variable low-pass that closes with the duck
                double hz = 20000.0 * std::pow(params.filterHz / 20000.0, std::clamp(d * params.filterAmt, 0.0, 1.0));
                fg = std::tan(kPi * std::min(hz, sr * 0.45) / sr); k = 2.0 - 1.85 * params.resonance;
                a1 = 1.0 / (1.0 + fg * (fg + k)); a2 = fg * a1; a3 = fg * a2;
            }
            for (int c = 0; c < 2; ++c) {
                float* buf = c == 0 ? L : R;
                if (!buf) continue;
                double x = buf[i], y = x;
                if (filt) {
                    double v3 = y - ic2[c], v1 = a1 * ic1[c] + a2 * v3, v2 = ic2[c] + a2 * ic1[c] + a3 * v3;
                    ic1[c] = 2 * v1 - ic1[c]; ic2[c] = 2 * v2 - ic2[c]; y = v2;
                }
                y *= g;
                buf[i] = (float)((x * dryG + y * params.mix) * outG);
            }
        }
    }

    Params params;
    double lastGain = 1, lastPhase = 0, scDuck = 0;

private:
    double sr = 44100, env = 0, gSmooth = 1, scAtt = 0, scRel = 0, smooth = 0, outG = 1;
    std::array<double, 2> ic1{}, ic2{};
};

}  // namespace pump
}  // namespace echolab
