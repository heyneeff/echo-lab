// Morpheus output stage: a 3-band EQ (low shelf, sweepable mid bell, high shelf) and a clean peak limiter.
// The limiter lets the synth sit louder without chords crunching into the safety clipper: instant attack,
// smooth release, ceiling just under 0 dBFS. Below the ceiling it does nothing at all.
#pragma once
#include <algorithm>
#include <cmath>

namespace echolab {
namespace out {

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[2] = {0, 0}, z2[2] = {0, 0};
    void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }
    inline float run(int c, float xf) {
        const double x = xf, y = b0 * x + z1[c];
        z1[c] = b1 * x - a1 * y + z2[c];
        z2[c] = b2 * x - a2 * y;
        return (float)y;
    }
    // RBJ cookbook. kind 0 = low shelf, 1 = bell, 2 = high shelf
    void set(int kind, double sr, double f, double db, double q) {
        const double A = std::pow(10.0, db / 40), w = 2 * 3.14159265358979 * std::min(f, sr * 0.45) / sr;
        const double cs = std::cos(w), sn = std::sin(w);
        double B0, B1, B2, A0, A1, A2;
        if (kind == 1) {
            const double al = sn / (2 * q);
            B0 = 1 + al * A; B1 = -2 * cs; B2 = 1 - al * A; A0 = 1 + al / A; A1 = -2 * cs; A2 = 1 - al / A;
        } else {
            const double al = sn / 2 * std::sqrt(2.0), sA = 2 * std::sqrt(A) * al;  // shelf slope 1
            if (kind == 0) {
                B0 = A * ((A + 1) - (A - 1) * cs + sA); B1 = 2 * A * ((A - 1) - (A + 1) * cs); B2 = A * ((A + 1) - (A - 1) * cs - sA);
                A0 = (A + 1) + (A - 1) * cs + sA; A1 = -2 * ((A - 1) + (A + 1) * cs); A2 = (A + 1) + (A - 1) * cs - sA;
            } else {
                B0 = A * ((A + 1) + (A - 1) * cs + sA); B1 = -2 * A * ((A - 1) + (A + 1) * cs); B2 = A * ((A + 1) + (A - 1) * cs - sA);
                A0 = (A + 1) - (A - 1) * cs + sA; A1 = 2 * ((A - 1) - (A + 1) * cs); A2 = (A + 1) - (A - 1) * cs - sA;
            }
        }
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }
};

struct Params {
    double lowDb = 0, midDb = 0, midHz = 1000, highDb = 0;  // dB, +-15
};

class Stage {
public:
    static constexpr double kLowHz = 110, kHighHz = 7000, kMidQ = 0.8;
    static constexpr float kCeiling = 0.94f;  // about -0.5 dBFS

    void prepare(double sampleRate) {
        sr = sampleRate;
        relK = 1 - std::exp(-1.0 / (0.12 * sr));  // 120 ms release
        reset();
        cur = tgt;
        design();
    }
    void reset() { lo.reset(); mid.reset(); hi.reset(); gain = 1; }
    void update(const Params& p) { tgt = p; }

    void process(float* L, float* R, int n) {
        // glide the settings once per block (a knob turn never zips)
        const double k = std::min(1.0, n / (0.03 * sr));
        cur.lowDb += (tgt.lowDb - cur.lowDb) * k;
        cur.midDb += (tgt.midDb - cur.midDb) * k;
        cur.highDb += (tgt.highDb - cur.highDb) * k;
        cur.midHz *= std::pow(tgt.midHz / cur.midHz, k);
        design();
        const bool eqOn = std::abs(cur.lowDb) > 0.01 || std::abs(cur.midDb) > 0.01 || std::abs(cur.highDb) > 0.01;
        for (int i = 0; i < n; ++i) {
            float l = L[i], r = R[i];
            if (eqOn) {
                if (std::abs(cur.lowDb) > 0.01) { l = lo.run(0, l); r = lo.run(1, r); }
                if (std::abs(cur.midDb) > 0.01) { l = mid.run(0, l); r = mid.run(1, r); }
                if (std::abs(cur.highDb) > 0.01) { l = hi.run(0, l); r = hi.run(1, r); }
            }
            // limiter: linked stereo, instant attack, smooth release
            const float pk = std::max(std::abs(l), std::abs(r));
            gain += (1.0 - gain) * relK;
            if (pk * gain > kCeiling) gain = kCeiling / pk;
            L[i] = (float)(l * gain);
            R[i] = (float)(r * gain);
        }
        if (!std::isfinite(gain)) gain = 1;
    }
    double reduction() const { return gain; }

private:
    void design() {
        lo.set(0, sr, kLowHz, cur.lowDb, 0.707);
        mid.set(1, sr, cur.midHz, cur.midDb, kMidQ);
        hi.set(2, sr, kHighHz, cur.highDb, 0.707);
    }
    double sr = 48000, relK = 0.001, gain = 1;
    Params tgt, cur;
    Biquad lo, mid, hi;
};

}  // namespace out
}  // namespace echolab
