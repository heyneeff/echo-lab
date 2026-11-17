// Measurements for dsp/Filters.h: tuning, slopes, self-oscillation, stability, comb, formant, HPF.
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

#include "WavetableSynth.h"

using namespace echolab::wt;
constexpr double SR = 48000;

static std::vector<float> combMem(2 * kCombLen);
static VoiceFilter fresh() { VoiceFilter f; f.comb = combMem.data(); f.reset(); return f; }

// run a filter over x (mono, both channels the same), return the left output
static std::vector<double> run(FilterSettings fs, const std::vector<double>& x) {
    auto f = fresh(); std::vector<double> y(x.size());
    for (size_t i = 0; i < x.size(); ++i) { double l = x[i], r = x[i]; f.process(l, r, fs, SR); y[i] = l; }
    return y;
}
static double rms(const std::vector<double>& y, size_t from) { double s = 0; for (size_t i = from; i < y.size(); ++i) s += y[i] * y[i]; return std::sqrt(s / (y.size() - from)); }
static double gainDb(FilterSettings fs, double f) {  // steady-state sine gain
    std::vector<double> x((int)SR / 2); for (size_t i = 0; i < x.size(); ++i) x[i] = 0.01 * std::sin(2 * kPi * f * i / SR);
    auto y = run(fs, x); return 20 * std::log10(rms(y, x.size() / 2) / (0.01 / std::sqrt(2.0)));
}
static double freqOf(const std::vector<double>& y, size_t from) {  // from rising zero crossings (interpolated)
    double first = -1, last = -1; int n = 0;
    for (size_t i = from + 1; i < y.size(); ++i) if (y[i - 1] < 0 && y[i] >= 0) { const double t = i - 1 + y[i - 1] / (y[i - 1] - y[i]); if (first < 0) first = t; last = t; ++n; }
    return n > 1 ? (n - 1) * SR / (last - first) : 0;
}
static double harmonicDb(const std::vector<double>& y, size_t from, double f0, int h) {  // level of harmonic h re the fundamental
    auto at = [&](double f) { std::complex<double> a = 0; for (size_t i = from; i < y.size(); ++i) a += y[i] * std::polar(1.0, -2 * kPi * f * i / SR); return std::abs(a); };
    return 20 * std::log10(at(h * f0) / at(f0));
}

int main() {
    int fails = 0;
    auto check = [&](bool ok, const char* what, double v) { std::printf("%s %-54s %9.3f\n", ok ? "ok  " : "FAIL", what, v); fails += !ok; };
    const char* names[] = {"SVF", "LADDER", "DIODE", "OTA", "COMB", "FORMANT"};

    // self-oscillation: the ladder and the OTA sing a sine at the cutoff, in tune
    for (int type : {kFilterLadder, kFilterOta}) for (double fc : {220.0, 1000.0, 4000.0}) {
        FilterSettings fs; fs.type = type; fs.fc = fc; fs.res = 1.0;
        std::vector<double> x((int)SR); x[0] = 1e-3;  // a tiny kick, then silence
        auto y = run(fs, x);
        const double f = freqOf(y, x.size() / 2), err = 100 * std::abs(f / fc - 1), lvl = rms(y, x.size() / 2);
        char b[96]; std::snprintf(b, sizeof b, "%s self-oscillates in tune at %.0f Hz (%% off)", names[type], fc);
        check(err < 2 && lvl > 0.05 && lvl < 2, b, err);
        if (fc == 1000.0) { std::snprintf(b, sizeof b, "%s oscillation is near-sine: 3rd harmonic (dB)", names[type]); const double h3 = harmonicDb(y, x.size() / 2, f, 3); check(h3 < -20, b, h3); }
    }
    // slopes, measured an octave apart well above the cutoff (resonance off, linear level)
    const double want[] = {-12, -24, -18, -24};
    for (int type : {kFilterSvf, kFilterLadder, kFilterDiode, kFilterOta}) {
        FilterSettings fs; fs.type = type; fs.fc = 100; fs.res = 0;  // 4-5 octaves up, far from Nyquist
        const double slope = gainDb(fs, 3200) - gainDb(fs, 1600);
        char b[96]; std::snprintf(b, sizeof b, "%s slope per octave (dB, want %.0f)", names[type], want[type]);
        check(std::abs(slope - want[type]) < 2, b, slope);
    }
    // the diode's resonant peak lands on the cutoff, at any resonance that has a peak
    for (double res : {0.4, 0.8, 0.95}) {
        FilterSettings fs; fs.type = kFilterDiode; fs.fc = 1000; fs.res = res;
        double best = 0, bestF = 0; for (double f = 600; f <= 1700; f *= 1.01) { const double g = gainDb(fs, f); if (g > best || bestF == 0) { best = g; bestF = f; } }
        char b[96]; std::snprintf(b, sizeof b, "DIODE resonant peak at the cutoff, res %.0f%% (Hz)", res * 100);
        check(std::abs(bestF / 1000 - 1) < 0.04, b, bestF);
    }
    {   // and with no resonance, its -3 dB point is the cutoff (not an octave darker)
        FilterSettings fs; fs.type = kFilterDiode; fs.fc = 1000; fs.res = 0;
        const double g0 = gainDb(fs, 50), g1 = gainDb(fs, 1000);
        check(std::abs(g1 - g0 + 3) < 1, "DIODE -3 dB at the cutoff with no resonance (dB)", g1 - g0);
    }
    // the ladder thins the bass as resonance rises (but not to nothing); the OTA much less
    {
        FilterSettings fs; fs.type = kFilterLadder; fs.fc = 2000;
        fs.res = 0; const double g0 = gainDb(fs, 60); fs.res = 0.9; const double g9 = gainDb(fs, 60);
        check(g9 < g0 - 2 && g9 > g0 - 9, "LADDER bass at 90% resonance vs 0% (dB)", g9 - g0);
        fs.type = kFilterOta; fs.res = 0; const double o0 = gainDb(fs, 60); fs.res = 0.9; const double o9 = gainDb(fs, 60);
        check(o9 - o0 > -2.5 && o9 - o0 > g9 - g0 + 1.5, "OTA loses much less bass than the ladder (dB)", o9 - o0);
    }
    // stability: full resonance, full drive, a loud saw, cutoff swept 20 Hz -> 20 kHz and back
    for (int type = 0; type < kNumFilterTypes; ++type) {
        auto f = fresh(); double pk = 0; bool fin = true;
        for (int i = 0; i < (int)SR * 2; ++i) {
            const double t = (double)i / SR, u = t < 1 ? t : 2 - t;
            FilterSettings fs; fs.type = type; fs.res = 1; fs.driveDb = 24; fs.morph = std::fmod(t, 1.0); fs.fc = 20 * std::pow(1000.0, u);
            double l = 2 * std::fmod(110 * t, 1.0) - 1, r = l; f.process(l, r, fs, SR);
            fin &= std::isfinite(l); pk = std::max(pk, std::abs(l));
        }
        char b[96]; std::snprintf(b, sizeof b, "%s stable at max resonance + drive (peak)", names[type]);
        check(fin && pk < 8, b, pk);
    }
    // comb: rings at the cutoff's period
    {
        FilterSettings fs; fs.type = kFilterComb; fs.fc = 200; fs.res = 0.9;
        std::vector<double> x((int)SR / 4); x[0] = 0.5; auto y = run(fs, x);
        const int D = (int)(SR / 200); double best = 0; int at = 0;
        for (int lag = 60; lag < 600; ++lag) { double c = 0; for (size_t i = 0; i + lag < y.size(); ++i) c += y[i] * y[i + lag]; if (c > best) { best = c; at = lag; } }
        check(std::abs(at - D) <= 2, "COMB rings at one period of the cutoff (samples)", at);
    }
    // formant: vowel A puts the most energy near its first formant (800 Hz)
    {
        FilterSettings fs; fs.type = kFilterFormant; fs.fc = 1000; fs.res = 0.5; fs.morph = 0;
        double best = -999, bestF = 0; for (double f = 200; f <= 4000; f *= 1.03) { const double g = gainDb(fs, f); if (g > best) { best = g; bestF = f; } }
        check(bestF > 650 && bestF < 1000, "FORMANT vowel A: strongest band (Hz)", bestF);
    }
    // Juno HPF steps
    {
        auto hp = [&](int step, double f) { HpfStep h; h.set(step, SR); const int n = (int)SR / 2; std::vector<float> l(n), r(n);
            for (int i = 0; i < n; ++i) l[i] = r[i] = (float)(0.1 * std::sin(2 * kPi * f * i / SR)); h.process(l.data(), r.data(), n);
            double s = 0; for (int i = n / 2; i < n; ++i) s += (double)l[i] * l[i]; return 20 * std::log10(std::sqrt(s / (n / 2)) / (0.1 / std::sqrt(2.0))); };
        check(hp(3, 100) < -15, "HPF high step: 100 Hz cut (dB)", hp(3, 100));
        check(std::abs(hp(3, 5000)) < 1, "HPF high step: 5 kHz untouched (dB)", hp(3, 5000));
        check(hp(0, 50) > 3 && hp(0, 50) < 7, "HPF boost step: +dB at 50 Hz", hp(0, 50));
        check(std::abs(hp(1, 50)) < 0.01, "HPF flat step is flat (dB)", hp(1, 50));
    }
    std::printf("filters: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
