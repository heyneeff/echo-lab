// Morpheus filter models (per voice, stereo). All cutoff-tuned filters are zero-delay-feedback
// (TPT) designs: the resonance lands exactly on the cutoff and self-oscillation is in tune.
//
//   SVF      12 dB state-variable; Morph sweeps LP -> BP -> HP -> Notch (Morph 0 = the original LP)
//   LADDER   Moog 24 dB transistor ladder: input + feedback saturate (tanh), resonance self-oscillates
//            into a sine at 100%, and the bass thins as resonance rises (the Minimoog trait)
//   DIODE    TB-303-style 18 dB: three poles, asymmetric (diode) saturation, squelchy resonance
//   OTA      Juno/Jupiter IR3109-style 24 dB: clean input, the resonance itself saturates, no bass loss
//   COMB     tuned to the cutoff (one period of delay); Resonance = feedback, Morph < 50% positive,
//            >= 50% negative feedback (hollow, an octave down)
//   FORMANT  three resonant band-passes at vowel formants; Morph sweeps A -> E -> I -> O -> U,
//            Cutoff shifts the formants, Resonance sharpens them
//
// Drive pushes the signal into each model's nonlinearity and takes the same gain back out, so it
// changes the character, not the level.
#pragma once
#include <algorithm>
#include <cmath>

namespace echolab {
namespace wt {

enum FilterType { kFilterSvf = 0, kFilterLadder, kFilterDiode, kFilterOta, kFilterComb, kFilterFormant, kNumFilterTypes };

struct FilterSettings {
    int type = kFilterSvf;
    double fc = 20000;      // Hz (already including envelope + keytrack)
    double res = 0.1;       // 0..1
    double morph = 0;       // 0..1
    double driveDb = 0;     // 0..24
};

constexpr int kCombLen = 4096;  // longest comb delay: 20 Hz at 48 kHz fits; 96 kHz clamps at ~23 Hz

struct VoiceFilter {
    double s[4][2]{};        // integrator states (ladders, SVF uses s[0], s[1])
    double f[3][2][2]{};     // formant band-passes: [band][ic1/ic2][channel]
    double combLp[2]{};
    double dc[2][2]{};       // DC blocker (diode): last input, last output
    float* comb = nullptr;   // 2 * kCombLen samples, owned by the synth
    int combPos = 0;

    void reset() {
        for (auto& a : s) a[0] = a[1] = 0;
        for (auto& b : f) for (auto& c : b) c[0] = c[1] = 0;
        combLp[0] = combLp[1] = 0;
        for (auto& d : dc) d[0] = d[1] = 0;
        combPos = 0;
        if (comb) std::fill(comb, comb + 2 * kCombLen, 0.f);
    }

    static double sat(double x) { return std::tanh(x); }
    static double diodeSat(double x) { return std::tanh(x + 0.12) - 0.11942729853438588; }  // tanh(0.12): no DC at rest

    // one TPT one-pole stage: returns the low-pass output, updates the state
    static double stage(double x, double G, double& st) { const double v = (x - st) * G, y = v + st; st = y + v; return y; }

    void process(double& l, double& r, const FilterSettings& p, double sr) {
        const double fc = std::clamp(p.fc, 20.0, sr * 0.45);
        const double drive = std::pow(10.0, p.driveDb / 20);
        double* ch[2] = {&l, &r};
        switch (p.type) {
            case kFilterSvf: {
                const double g = std::tan(kPiF * fc / sr), k = 2 - 1.9 * p.res;
                const double a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
                const double m = std::clamp(p.morph, 0.0, 1.0) * 3;
                for (int c = 0; c < 2; ++c) {
                    double v0 = *ch[c];
                    if (drive > 1.001) v0 = sat(v0 * drive) / drive;
                    const double v3 = v0 - s[1][c];
                    const double v1 = a1 * s[0][c] + a2 * v3, v2 = s[1][c] + a2 * s[0][c] + a3 * v3;
                    s[0][c] = 2 * v1 - s[0][c];
                    s[1][c] = 2 * v2 - s[1][c];
                    const double lp = v2, bp = k * v1, hp = v0 - k * v1 - v2, notch = lp + hp;
                    double y;
                    if (m < 1) y = lp + (bp - lp) * m;
                    else if (m < 2) y = bp + (hp - bp) * (m - 1);
                    else y = hp + (notch - hp) * (m - 2);
                    *ch[c] = y;
                }
                break;
            }
            case kFilterLadder:
            case kFilterOta: {
                const bool moog = p.type == kFilterLadder;
                const double g = std::tan(kPiF * fc / sr), G = g / (1 + g), G4 = G * G * G * G;
                const double k = p.res * (moog ? 4.2 : 4.3);
                const double comp = moog ? 1 + 0.5 * k : 1 + 0.75 * k;  // bass loss with resonance: the Moog's is famous, the OTA's slight
                for (int c = 0; c < 2; ++c) {
                    const double x = *ch[c] * drive;
                    const double S = G * G * G * (1 - G) * s[0][c] + G * G * (1 - G) * s[1][c] + G * (1 - G) * s[2][c] + (1 - G) * s[3][c];
                    const double y4 = (G4 * x + S) / (1 + k * G4);  // linear zero-delay solution
                    const double u = moog ? sat(x - k * y4) : x - k * sat(y4);
                    double y = stage(u, G, s[0][c]);
                    y = stage(y, G, s[1][c]);
                    y = stage(y, G, s[2][c]);
                    y = stage(y, G, s[3][c]);
                    *ch[c] = y * comp / drive;
                }
                break;
            }
            case kFilterDiode: {
                // three poles; where the stages sit depends on resonance, so the filter's character
                // frequency (its resonant peak, or its -3 dB point when there is no peak) is the cutoff.
                // Table computed from the analog prototype (stage cutoff / fc at resonance 0, 0.1 .. 1;
                // the 0.1 step smoothed across the no-peak -> peak changeover).
                static const double T[11] = {1.96146, 1.62, 1.15972, 0.9385, 0.82595, 0.75445, 0.70361, 0.6649, 0.63411, 0.60874, 0.58735};
                const double ri = std::clamp(p.res, 0.0, 1.0) * 10;
                const int i0 = std::min(9, (int)ri);
                const double tune = T[i0] * std::pow(T[i0 + 1] / T[i0], ri - i0);
                const double g = std::tan(kPiF * fc / sr) * tune, G = g / (1 + g), G3 = G * G * G;
                const double k = p.res * 7.6, comp = 1 + 0.45 * k;
                for (int c = 0; c < 2; ++c) {
                    const double x = *ch[c] * drive;
                    const double S = G * G * (1 - G) * s[0][c] + G * (1 - G) * s[1][c] + (1 - G) * s[2][c];
                    const double y3 = (G3 * x + S) / (1 + k * G3);
                    const double u = diodeSat(x - k * y3);
                    double y = stage(u, G, s[0][c]);
                    y = stage(y, G, s[1][c]);
                    y = stage(y, G, s[2][c]) * comp / drive;
                    // the diode's lopsided saturation adds a little DC with the signal: block it (~8 Hz)
                    const double hp = y - dc[c][0] + 0.999 * dc[c][1];
                    dc[c][0] = y;
                    dc[c][1] = hp;
                    *ch[c] = hp;
                }
                break;
            }
            case kFilterComb: {
                if (!comb) break;
                const double D = std::clamp(sr / fc, 2.0, (double)kCombLen - 3);
                const double fb = (p.morph < 0.5 ? 1 : -1) * p.res * 0.97;
                const double damp = 0.35;  // a little high-frequency loss in the loop, like a real string
                for (int c = 0; c < 2; ++c) {
                    float* buf = comb + c * kCombLen;
                    double rp = combPos - D;
                    if (rp < 0) rp += kCombLen;
                    const int i0 = (int)rp, i1 = (i0 + 1) % kCombLen;
                    const double fr = rp - i0, d = buf[i0] + (buf[i1] - buf[i0]) * fr;
                    combLp[c] += (d - combLp[c]) * (1 - damp);
                    const double x = *ch[c] * drive;
                    const double y = sat(x + fb * combLp[c]);
                    buf[combPos] = (float)y;
                    *ch[c] = y * std::sqrt(1 - std::abs(fb) * 0.9) / drive;
                }
                combPos = (combPos + 1) % kCombLen;
                break;
            }
            case kFilterFormant: {
                // vowel formants (Hz) and their levels, morphing A E I O U
                static const double F[5][3] = {{800, 1150, 2900}, {530, 1840, 2480}, {270, 2290, 3010}, {450, 800, 2830}, {325, 700, 2530}};
                static const double A[3] = {1.0, 0.5, 0.28};
                const double m = std::clamp(p.morph, 0.0, 1.0) * 4;
                const int v0i = std::min(3, (int)m);
                const double t = m - v0i;
                const double shift = std::pow(2.0, std::clamp(std::log2(fc / 1000.0), -2.0, 2.0) * 0.5);
                const double q = 3 + p.res * 17, k = 1 / q;
                double gB[3], a1[3], a2[3], a3[3];
                for (int b = 0; b < 3; ++b) {
                    const double fb = std::clamp((F[v0i][b] + (F[v0i + 1][b] - F[v0i][b]) * t) * shift, 40.0, sr * 0.45);
                    gB[b] = std::tan(kPiF * fb / sr);
                    a1[b] = 1 / (1 + gB[b] * (gB[b] + k)); a2[b] = gB[b] * a1[b]; a3[b] = gB[b] * a2[b];
                }
                for (int c = 0; c < 2; ++c) {
                    double x = *ch[c];
                    if (drive > 1.001) x = sat(x * drive) / drive;
                    double y = 0;
                    for (int b = 0; b < 3; ++b) {
                        double& i1 = f[b][0][c];
                        double& i2 = f[b][1][c];
                        const double v3 = x - i2, v1 = a1[b] * i1 + a2[b] * v3, v2 = i2 + a2[b] * i1 + a3[b] * v3;
                        i1 = 2 * v1 - i1;
                        i2 = 2 * v2 - i2;
                        y += A[b] * v1 * k * 2.2;  // band-pass, peak-normalised
                    }
                    *ch[c] = y;
                }
                break;
            }
        }
    }

    static constexpr double kPiF = 3.14159265358979323846;
};

// Juno-106 style high-pass, after the voices: 0 = bass boost, 1 = flat, 2 = low HP, 3 = high HP
struct HpfStep {
    int step = 1;
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z[2][2]{};
    void set(int st, double sr) {
        step = st;
        if (st == 1) { b0 = 1; b1 = b2 = a1 = a2 = 0; return; }
        const double pi = 3.14159265358979323846;
        if (st == 0) {  // low shelf +6 dB at 90 Hz
            const double A = std::pow(10.0, 6.0 / 40), w = 2 * pi * 90 / sr, cs = std::cos(w), sn = std::sin(w), al = sn / 2 * std::sqrt(2.0);
            const double sq = 2 * std::sqrt(A) * al, a0 = (A + 1) + (A - 1) * cs + sq;
            b0 = A * ((A + 1) - (A - 1) * cs + sq) / a0; b1 = 2 * A * ((A - 1) - (A + 1) * cs) / a0; b2 = A * ((A + 1) - (A - 1) * cs - sq) / a0;
            a1 = -2 * ((A - 1) + (A + 1) * cs) / a0; a2 = ((A + 1) + (A - 1) * cs - sq) / a0;
            return;
        }
        const double f = st == 2 ? 240.0 : 720.0, w = 2 * pi * f / sr, cs = std::cos(w), al = std::sin(w) / (2 * 0.707), a0 = 1 + al;
        b0 = (1 + cs) / 2 / a0; b1 = -(1 + cs) / a0; b2 = (1 + cs) / 2 / a0; a1 = -2 * cs / a0; a2 = (1 - al) / a0;
    }
    void process(float* L, float* R, int n) {
        if (step == 1) return;
        float* ch[2] = {L, R};
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i) {
                const double x = ch[c][i], y = b0 * x + z[c][0];
                z[c][0] = b1 * x - a1 * y + z[c][1];
                z[c][1] = b2 * x - a2 * y;
                ch[c][i] = (float)y;
            }
    }
};

}  // namespace wt
}  // namespace echolab
