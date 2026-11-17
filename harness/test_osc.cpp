// Checks for M3: oscillator B, warp modes, noise colours, wavetable file round trip.
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

#include "WavetableFile.h"

using namespace echolab::wt;
constexpr double SR = 48000;

static std::vector<float> render(Synth& s, int note, int samples) {
    std::vector<float> l(samples), r(samples);
    s.noteOn(note, 1.0);
    for (int i = 0; i < samples; i += 64) s.process(&l[i], &r[i], std::min(64, samples - i));
    return l;
}
static double freqOf(const std::vector<float>& y) {
    double first = -1, last = -1; int n = 0;
    for (size_t i = y.size() / 2 + 1; i < y.size(); ++i) if (y[i - 1] < 0 && y[i] >= 0) { const double t = i - 1 + y[i - 1] / (y[i - 1] - y[i]); if (first < 0) first = t; last = t; ++n; }
    return n > 1 ? (n - 1) * SR / (last - first) : 0;
}
static std::vector<std::complex<double>> spectrum(const std::vector<float>& x) {  // last 32768 samples, Blackman-Harris
    const int N = 32768; std::vector<std::complex<double>> a(N);
    for (int i = 0; i < N; ++i) { const double t = 2 * kPi * i / (N - 1);
        a[i] = x[x.size() - N + i] * (0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t)); }
    fft(a); return a;
}
static double aliasDb(const std::vector<float>& x, double f0) {
    auto a = spectrum(x); const int N = (int)a.size(); double harm = 0, other = 0;
    for (int k = (int)(20.0 * N / SR); k < N / 2; ++k) { const double f = k * SR / N, h = f / f0, d = std::abs(h - std::round(h)) * f0;  // from 20 Hz
        (d < 6 * SR / N && std::round(h) >= 1 ? harm : other) += std::norm(a[k]); }
    return 10 * std::log10(other / harm + 1e-30);
}
static Params base() { Params p; p.cutoffHz = 20000; p.resonance = 0; p.ampA = 0.001; p.ampS = 1; p.velocity = 0; p.unison = 1; return p; }

int main() {
    int fails = 0;
    auto check = [&](bool ok, const char* what, double v) { std::printf("%s %-58s %9.3f\n", ok ? "ok  " : "FAIL", what, v); fails += !ok; };

    // B off: nothing about B changes the sound
    {
        auto run = [&](int octB, int tableB) { Synth s; s.prepare(SR); s.setTableB(&factoryTable(tableB)); Params p = base(); p.octB = octB; p.positionB = 0.9; s.update(p); return render(s, 57, 9600); };
        auto a = run(0, 0), b = run(2, 3); double d = 0; for (size_t i = 0; i < a.size(); ++i) d = std::max(d, (double)std::abs(a[i] - b[i]));
        check(d == 0, "OSC B at level 0 changes nothing (max diff)", d);
    }
    // B tuning: octave +1, semi +7, fine +50 ct on a pure sine
    {
        Synth s; s.prepare(SR); Params p = base(); p.levelA = 0; p.levelB = 1; p.positionB = 0; p.octB = 1; p.semiB = 7; p.fineB = 50; s.update(p);
        const double f = freqOf(render(s, 57, 24000)), want = 220 * std::exp2(1 + 7 / 12.0 + 50 / 1200.0);
        check(std::abs(f / want - 1) < 0.001, "OSC B tuning: +1 oct +7 st +50 ct (Hz)", f);
    }
    // warps: aliasing (energy between the harmonics, from 20 Hz). Warped oscillators run at 2x.
    // Smooth waves are spotless; the worst case is a saw's edge sitting on the warp's bend at the wrap,
    // measured at C4 and C7 (limits = what this design achieves, so a regression shows).
    {
        auto alias = [&](int table, double position, int mode, int note) {
            Synth s; s.prepare(SR); s.setTable(&factoryTable(table)); s.setTableB(&factoryTable(0));
            Params p = base(); p.position = position; p.warpA = mode; p.warpAmtA = 0.5; p.positionB = 0;  // B: a sine at the same pitch, silent
            s.update(p);
            return aliasDb(render(s, note, 48000), 440 * std::exp2((note - 69) / 12.0));
        };
        struct { int mode; double smoothLim, sawC4, sawC7; } W[] = {{kWarpOff, -80, -80, -80}, {kWarpBendUp, -60, -46, -38}, {kWarpBendDown, -60, -46, -38},
            {kWarpMirror, -60, -42, -32}, {kWarpAsym, -60, -50, -42}, {kWarpSync, -45, -45, -35}, {kWarpFm, -80, -80, -55}, {kWarpRing, -80, -80, -80}};
        for (auto w : W) {
            char b[96];
            const double sm = alias(1, 0.0, w.mode, 96);  // MOOG frame 1: the triangle (corners, no jump)
            std::snprintf(b, sizeof b, "warp %-6s triangle at C7: aliasing (dB)", kWarpNames[w.mode]); check(sm < w.smoothLim, b, sm);
            const double c4 = alias(1, 0.4, w.mode, 60), c7 = alias(1, 0.4, w.mode, 96);
            std::snprintf(b, sizeof b, "warp %-6s saw (worst case) at C4 (dB)", kWarpNames[w.mode]); check(c4 < w.sawC4, b, c4);
            std::snprintf(b, sizeof b, "warp %-6s saw (worst case) at C7 (dB)", kWarpNames[w.mode]); check(c7 < w.sawC7, b, c7);
        }
    }
    // noise colours: level per Hz at 1 kHz vs 8 kHz (3 octaves): white 0, pink -9, brown -18 dB
    {
        const double want[] = {0, -9, -18};
        for (int c = 0; c < 3; ++c) {
            Synth s; s.prepare(SR); Params p = base(); p.levelA = 0; p.noise = 1; p.noiseColor = c; s.update(p);
            auto x = render(s, 60, 48000 * 2); auto a = spectrum(x); const int N = (int)a.size();
            auto band = [&](double f) { double e = 0; int n = 0; for (int k = (int)(f * 0.8 * N / SR); k <= (int)(f * 1.25 * N / SR); ++k) { e += std::norm(a[k]); ++n; } return e / n; };
            const double slope = 10 * std::log10(band(8000) / band(1000));
            const char* nm[] = {"white", "pink", "brown"};
            char b[96]; std::snprintf(b, sizeof b, "noise %s: 8 kHz vs 1 kHz per Hz (dB, want %.0f)", nm[c], want[c]);
            check(std::abs(slope - want[c]) < 2.5, b, slope);
        }
        Synth s; s.prepare(SR); Params p = base(); p.levelA = 0; p.noise = 1; p.noiseColor = 3; s.update(p);
        auto x = render(s, 60, 48000); double pk = 0; for (float v : x) pk = std::max(pk, (double)std::abs(v));
        check(pk > 0.05 && pk < 1, "noise vinyl: crackles, bounded (peak)", pk);
    }
    // wavetable file round trip: band-limited saw cycles -> spectra -> table -> cycles
    {
        const int frames = 8; std::vector<float> cyc(frames * 2048);
        for (int k = 0; k < frames; ++k) for (int j = 0; j < 2048; ++j) { double v = 0; for (int h = 1; h <= 40 + 40 * k; ++h) v += std::sin(2 * kPi * h * j / 2048.0) / h; cyc[k * 2048 + j] = (float)v; }
        auto sp = cyclesToSpectra(cyc.data(), (int)cyc.size(), 2048);
        Wavetable t; t.build(sp); auto back = tableToCycles(t);
        double err = 0;
        for (int k = 0; k < frames; ++k) {  // compare shapes (the table normalises each frame's peak)
            double pk = 0, pb = 0; for (int j = 0; j < 2048; ++j) { pk = std::max(pk, (double)std::abs(cyc[k * 2048 + j])); pb = std::max(pb, (double)std::abs(back[k * 2048 + j])); }
            for (int j = 0; j < 2048; ++j) err = std::max(err, std::abs(back[k * 2048 + j] / pb - cyc[k * 2048 + j] / pk)); }
        check(sp.size() == frames && err < 1e-4, "wavetable .wav: 8 frames round-trip (max error)", err);
        const bool wrote = writeWavetableWav("/tmp/morpheus_test_table.wav", back);
        FILE* f = std::fopen("/tmp/morpheus_test_table.wav", "rb"); char hdr[64] = {0}; size_t n = f ? std::fread(hdr, 1, 64, f) : 0; if (f) std::fclose(f);
        bool clm = false; for (size_t i = 0; i + 4 <= n; ++i) clm |= hdr[i] == 'c' && hdr[i + 1] == 'l' && hdr[i + 2] == 'm';
        check(wrote && clm, "wavetable .wav written with a clm chunk", clm);
        std::remove("/tmp/morpheus_test_table.wav");
    }
    // fuzz: every warp on both oscillators, B tables, noise colours, routes to the new destinations
    {
        std::mt19937 rng(5); std::uniform_real_distribution<double> U(0, 1); bool fin = true; double pk = 0;
        for (int trial = 0; trial < 40; ++trial) {
            Synth s; s.prepare(SR); s.setTable(&factoryTable((int)(U(rng) * 5))); s.setTableB(&factoryTable((int)(U(rng) * 5)));
            Params p = base(); p.unison = 1 + (int)(U(rng) * 7); p.levelA = U(rng); p.levelB = U(rng); p.octB = (int)(U(rng) * 7) - 3; p.semiB = (int)(U(rng) * 25) - 12;
            p.warpA = (int)(U(rng) * kNumWarps); p.warpB = (int)(U(rng) * kNumWarps); p.warpAmtA = U(rng); p.warpAmtB = U(rng); p.noise = U(rng); p.noiseColor = (int)(U(rng) * 4);
            p.filterType = (int)(U(rng) * kNumFilterTypes); p.resonance = U(rng);
            for (auto& sl : p.slots) { sl.src = (int)(U(rng) * kNumSources); sl.dst = (int)(U(rng) * kNumDests); sl.amt = U(rng) * 2 - 1; }
            s.update(p);
            for (int nn : {36, 60, 84, 100}) s.noteOn(nn, U(rng));
            std::vector<float> l(9600), r(9600); for (int i = 0; i < 9600; i += 64) s.process(&l[i], &r[i], 64);
            for (float v : l) { fin &= std::isfinite(v); pk = std::max(pk, (double)std::abs(v)); }
        }
        check(fin && pk < 6, "fuzz: 40 patches, all warps/noise/B, finite + bounded", pk);
    }
    std::printf("osc: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
