// Checks for dsp/Resynth.h (growing wavetables from sound) on signals with known answers.
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "Resynth.h"
#include "../plugin/synth/b64.h"

using namespace echolab::wt;
constexpr double SR = 48000;

// band-limited saw at f (Hz) whose harmonics roll off above `bright(t)` (in harmonics), + optional vibrato
template <class B>
static std::vector<float> saw(double f, double secs, B bright, double vibCents = 0, double pad = 0) {
    const int n = (int)(secs * SR), p = (int)(pad * SR);
    std::vector<float> x(n + 2 * p, 0.f);
    double ph = 0;
    for (int i = 0; i < n; ++i) {
        const double t = (double)i / n, fi = f * std::pow(2.0, vibCents / 1200 * std::sin(2 * kPi * 5 * i / SR));
        ph += fi / SR;
        const double br = bright(t);
        double s = 0;
        for (int h = 1; h * fi < SR / 2; ++h) s += std::sin(2 * kPi * h * ph) / h / (1 + std::pow(h / br, 4));
        x[p + i] = (float)(0.3 * s);
    }
    return x;
}
static double centroid(const Spectrum& s) { double a = 0, b = 0; for (int h = 1; h <= kMaxHarm; ++h) { const double m = std::abs(s[h]); a += h * m; b += m; } return b > 0 ? a / b : 0; }

int main() {
    int fails = 0;
    auto check = [&](bool ok, const char* what, double v) { std::printf("%s %-50s %9.3f\n", ok ? "ok  " : "FAIL", what, v); fails += !ok; };
    ResynthInfo info;

    // 1. plain saw: pitch, 1/h harmonic series, and trimming of silence around it
    {
        auto x = saw(220, 1.0, [](double) { return 1e9; }, 0, 0.5);
        auto sp = resynthesize(x.data(), (int)x.size(), SR, info);
        check(info.pitched, "saw: detected as pitched", info.pitched);
        check(std::abs(info.f0 / 220 - 1) < 0.005, "saw: f0 (Hz)", info.f0);
        check(std::abs(info.seconds - 1.0) < 0.05, "saw: silence trimmed, seconds", info.seconds);
        double worst = 0;
        for (int k : {5, 32, 58}) for (int h = 2; h <= 20; ++h)
            worst = std::max(worst, std::abs(20 * std::log10(std::abs(sp[k][h]) / std::abs(sp[k][1]) * h)));
        check(worst < 1.0, "saw: harmonics 2-20 follow 1/h (worst dB off)", worst);
        double ph = 0;
        for (int k = 0; k < kFrames; ++k) ph = std::max(ph, std::abs(std::remainder(std::arg(sp[k][1]) + kPi / 2, 2 * kPi)));
        check(ph < 1e-6, "saw: every frame's fundamental in sine phase (rad)", ph);
    }
    // 2. a saw that brightens: the table should brighten front to back
    {
        auto x = saw(110, 2.0, [](double t) { return 2 + 30 * t; });
        auto sp = resynthesize(x.data(), (int)x.size(), SR, info);
        const double c0 = centroid(sp[0]), c1 = centroid(sp[kFrames - 1]);
        int rises = 0; for (int k = 1; k < kFrames; ++k) rises += centroid(sp[k]) >= centroid(sp[k - 1]) - 1e-9;
        check(c1 > 3 * c0, "brightening: last/first frame centroid", c1 / c0);
        check(rises >= kFrames - 3, "brightening: frames getting brighter in order", rises);
    }
    // 3. vibrato: still locks, harmonic 10 stays where it should
    {
        auto x = saw(330, 1.5, [](double) { return 1e9; }, 25);
        auto sp = resynthesize(x.data(), (int)x.size(), SR, info);
        check(info.pitched && std::abs(info.f0 / 330 - 1) < 0.02, "vibrato +-25 ct: f0 (Hz)", info.f0);
        double worst = 0;
        for (int k = 0; k < kFrames; k += 7) worst = std::max(worst, std::abs(20 * std::log10(std::abs(sp[k][10]) / std::abs(sp[k][1]) * 10)));
        check(worst < 2.0, "vibrato: harmonic 10 level (worst dB off)", worst);
    }
    // 4. noise: unpitched, raw cycles, nothing silent or broken
    {
        std::mt19937 rng(7); std::normal_distribution<float> g(0, 0.2f);
        std::vector<float> x((int)SR); for (auto& v : x) v = g(rng);
        auto sp = resynthesize(x.data(), (int)x.size(), SR, info);
        check(!info.pitched, "noise: detected as unpitched", info.pitched);
        bool ok = true; for (auto& s : sp) { double e = 0; for (int h = 1; h <= kMaxHarm; ++h) { ok &= std::isfinite(s[h].real()); e += std::norm(s[h]); } ok &= e > 0; }
        check(ok, "noise: every frame finite and non-silent", ok);
    }
    // 5. round trip: grow a table from the saw and play it
    {
        auto x = saw(220, 1.0, [](double t) { return 3 + 20 * t; });
        Wavetable t; t.build(resynthesize(x.data(), (int)x.size(), SR, info)); t.seconds = info.seconds;
        Synth s; s.prepare(SR); s.setTable(&t);
        Params p; p.position = 0.8; s.update(p);
        std::vector<float> L(48000), R(48000);
        s.noteOn(57, 1.0);
        for (int i = 0; i < 48000; i += 256) s.process(&L[i], &R[i], 256);
        double pk = 0; bool fin = true; for (float v : L) { fin &= std::isfinite(v); pk = std::max(pk, (double)std::abs(v)); }
        check(fin && pk > 0.05 && pk < 1.0, "round trip: grown table plays, peak", pk);
        s.setTable(nullptr);
    }
    // 7. sweeping a grown table must not crackle: a breathy vowel with vibrato (noisy frame to frame),
    //    played while Position sweeps the table; energy between the harmonics vs the same note held still
    {
        std::mt19937 rng(3); std::normal_distribution<float> g(0, 1);
        const int n = (int)(2.5 * SR); std::vector<float> x(n); double ph = 0;
        for (int i = 0; i < n; ++i) {
            const double t = (double)i / n, f = 196 * std::pow(2.0, 15.0 / 1200 * std::sin(2 * kPi * 5.5 * i / SR));
            ph += f / SR; double s = 0;
            for (int h = 1; h * f < 8000; ++h) { const double fh = h * f, f1 = 600 + 300 * t, f2 = 1100 + 900 * t;
                s += (std::exp(-std::pow((fh - f1) / 160, 2)) + 0.7 * std::exp(-std::pow((fh - f2) / 220, 2)) + 0.03) * std::sin(2 * kPi * h * ph); }
            x[i] = (float)(0.2 * s + 0.01 * g(rng));
        }
        Wavetable t; t.build(resynthesize(x.data(), n, SR, info));
        auto play = [&](bool sweep, bool factory = false) {
            Synth s; s.prepare(SR); if (!factory) s.setTable(&t);
            Params p; p.cutoffHz = 20000; p.ampA = 0.001; p.ampS = 1; p.velocity = 0; p.position = 0.5; s.update(p);
            const int N = 32768 + 4800; std::vector<float> l(N), r(N);
            s.noteOn(45, 1.0);
            for (int i = 0; i < N; i += 256) { if (sweep) { p.position = 0.1 + 0.8 * (double)i / N; s.update(p); } s.process(&l[i], &r[i], std::min(256, N - i)); }
            return l;
        };
        auto between = [&](const std::vector<float>& y) {  // energy away from 110 Hz harmonics, dB re harmonics
            const int N = 32768; std::vector<std::complex<double>> a(N);
            for (int i = 0; i < N; ++i) { const double u = 2 * kPi * i / (N - 1);
                a[i] = y[y.size() - N + i] * (0.35875 - 0.48829 * std::cos(u) + 0.14128 * std::cos(2 * u) - 0.01168 * std::cos(3 * u)); }
            fft(a); double hm = 0, ot = 0;
            for (int k = 1; k < N / 2; ++k) { const double f = k * SR / N, hh = f / 110, d = std::abs(hh - std::round(hh)) * 110;
                (d < 6 * SR / N && std::round(hh) >= 1 ? hm : ot) += std::norm(a[k]); }
            return 10 * std::log10(ot / hm + 1e-30);
        };
        const double held = between(play(false)), swept = between(play(true)), fac = between(play(true, true));
        std::printf("     between-harmonic energy: grown held %.1f dB, grown swept %.1f dB, factory swept %.1f dB\n", held, swept, fac);
        check(swept < fac + 3, "grown table sweeps as cleanly as the factory one (dB over)", swept - fac);
    }
    // 6. the face -> plugin hand-off: base64 of little-endian float32 (reference made with Python struct/base64)
    {
        auto v = echolab::decodeFloats("AACAPwAAAL8AAIA+0A9JQG8Sg7o=");
        const float want[] = {1.0f, -0.5f, 0.25f, 3.14159f, -1e-3f};
        bool ok = v.size() == 5; for (int i = 0; ok && i < 5; ++i) ok = v[i] == want[i];
        check(ok, "base64 float32 decode matches Python", (double)v.size());
    }
    std::printf("resynth: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
