// Offline checks for the wavetable synth engine (I can't hear it, so: numbers).
//   - aliasing: high notes on the saw frame, energy off the harmonic series must stay tiny
//   - stability: every frame across the table, finite and bounded
//   - headroom: 4-note chord, 7-voice unison, peak stays under 0 dBFS
//   - release: voices end after note-off
// Also writes build/.../synth_demo.wav (arg 1 overrides the path) to listen to.
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

#include "DriveVoices.h"
#include "WavetableSynth.h"

using namespace echolab;
constexpr double SR = 48000;

static std::vector<float> renderNote(wt::Synth& s, int note, int samples, int offAt = -1) {
    std::vector<float> L(samples), R(samples);
    s.noteOn(note, 1.0);
    for (int i = 0; i < samples; i += 256) {
        if (offAt >= 0 && i >= offAt && i < offAt + 256) s.noteOff(note);
        s.process(&L[i], &R[i], std::min(256, samples - i));
    }
    return L;
}

// fraction of power (dB) that is not near a harmonic of f0
static double aliasDb(const std::vector<float>& x, double f0) {
    const int N = 32768;
    std::vector<std::complex<double>> a(N);
    const int start = (int)x.size() - N;
    for (int i = 0; i < N; ++i) {  // Blackman-Harris
        const double t = 2 * wt::kPi * i / (N - 1);
        const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
        a[i] = x[start + i] * w;
    }
    wt::fft(a);
    double harm = 0, other = 0;
    for (int k = 1; k < N / 2; ++k) {
        const double f = k * SR / N, p = std::norm(a[k]);
        const double h = f / f0, d = std::abs(h - std::round(h)) * f0;  // Hz from the nearest harmonic
        (d < 6 * SR / N && std::round(h) >= 1 ? harm : other) += p;
    }
    return 10 * std::log10(other / harm + 1e-30);
}

static void writeWav(const char* path, const std::vector<float>& L, const std::vector<float>& R) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    const uint32_t n = (uint32_t)L.size(), bytes = n * 4;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32((uint32_t)SR); u32((uint32_t)SR * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(bytes);
    for (uint32_t i = 0; i < n; ++i) {
        int16_t s[2] = {(int16_t)std::lround(std::clamp(L[i], -1.f, 1.f) * 32767), (int16_t)std::lround(std::clamp(R[i], -1.f, 1.f) * 32767)};
        std::fwrite(s, 2, 2, f);
    }
    std::fclose(f);
}

int main(int argc, char** argv) {
    int fails = 0;
    auto check = [&](bool ok, const char* what, double v) { std::printf("%s %-44s %8.2f\n", ok ? "ok  " : "FAIL", what, v); fails += !ok; };

    wt::Synth s;
    s.prepare(SR);
    wt::Params p;
    p.position = 31.0 / 63;  // saw
    p.cutoffHz = 20000; p.resonance = 0; p.velocity = 0; p.ampA = 0.001; p.ampS = 1;
    s.update(p);

    // aliasing: up the keyboard, including the top notes where only a few harmonics fit
    double worst = -999;
    for (int note : {60, 84, 96, 100, 104, 108, 112}) {
        s.reset();
        auto x = renderNote(s, note, 48000);
        const double a = aliasDb(x, 440.0 * std::pow(2.0, (note - 69) / 12.0));
        char buf[64]; std::snprintf(buf, sizeof buf, "alias, saw, note %d (dB re harmonics)", note);
        check(a < -60, buf, a);
        worst = std::max(worst, a);
    }

    // stability across the whole table
    double peakAll = 0; bool finite = true;
    for (int f = 0; f <= 63; f += 3) {
        p.position = f / 63.0; s.update(p); s.reset();
        for (float v : renderNote(s, 45, 4800)) { finite &= std::isfinite(v); peakAll = std::max(peakAll, (double)std::abs(v)); }
    }
    check(finite && peakAll < 1.0, "every frame finite, peak (single note)", peakAll);

    // headroom: big unison chord through the ORPHEO drive
    p.position = 0.5; p.unison = 7; p.detuneCents = 30; p.spread = 1; p.sub = 0.5; s.update(p); s.reset();
    DriveEngine drv; drv.prepare(SR);
    for (const auto& v : kDriveVoices) if (std::string(v.name) == "ORPHEO") drv.setVoice(v);
    DriveParams dp; dp.drive = 6; drv.update(dp);
    std::vector<float> L(96000), R(96000), L2(96000), R2(96000);
    for (int nn : {48, 55, 60, 64}) s.noteOn(nn, 1.0);
    for (int i = 0; i < 96000; i += 256) s.process(&L2[i], &R2[i], std::min(256, 96000 - i));
    double pk0 = 0, sq = 0;
    for (int i = 0; i < 96000; ++i) { pk0 = std::max({pk0, (double)std::abs(L2[i]), (double)std::abs(R2[i])}); sq += (double)L2[i] * L2[i]; }
    check(pk0 < 2.0, "chord x unison 7 + sub, before safety, peak", pk0);
    check(std::sqrt(sq / 96000) < 0.5, "  ... and its RMS (safety only shaves peaks)", std::sqrt(sq / 96000));
    double pks = 0; for (int i = 0; i < 96000; ++i) pks = std::max(pks, (double)std::abs(wt::Synth::safety(L2[i])));
    check(pks < 1.0, "  ... after the safety, peak", pks);
    check(wt::Synth::safety(0.5f) == 0.5f, "safety transparent at -6 dBFS", 0.5);
    s.reset();
    for (int nn : {48, 55, 60, 64}) s.noteOn(nn, 1.0);
    for (int i = 0; i < 96000; i += 256) { const int m = std::min(256, 96000 - i); s.process(&L[i], &R[i], m); drv.process(&L[i], &R[i], m); }
    double pk = 0; for (int i = 0; i < 96000; ++i) pk = std::max({pk, (double)std::abs(L[i]), (double)std::abs(R[i])});
    check(pk < 1.0, "chord x unison 7 + sub + drive, peak", pk);

    // release ends voices
    s.allOff();
    std::vector<float> tail(256 * 400), tr(256 * 400);
    for (int i = 0; i < (int)tail.size(); i += 256) s.process(&tail[i], &tr[i], 256);
    check(s.activeVoices() == 0, "voices left 2 s after release", s.activeVoices());

    // demo: a slow position sweep with the mod envelope on a little progression
    {
        wt::Synth d; d.prepare(SR);
        wt::Params q; q.unison = 5; q.detuneCents = 18; q.spread = 0.8; q.sub = 0.3; q.cutoffHz = 2500; q.resonance = 0.25;
        q.filterEnv = 0.35; q.posEnv = 0.35; q.modD = 0.9; q.ampA = 0.01; q.ampR = 0.6;
        const int chords[4][3] = {{57, 60, 64}, {53, 57, 60}, {48, 52, 55}, {55, 59, 62}};
        const int len = (int)(SR * 1.5), total = len * 4 + (int)SR;
        std::vector<float> dl(total), dr(total);
        for (int c = 0; c < 4; ++c) {
            q.position = c / 4.0 + 0.1; d.update(q);
            for (int k : chords[c]) d.noteOn(k, 0.8);
            for (int i = 0; i < len; i += 256) d.process(&dl[c * len + i], &dr[c * len + i], std::min(256, len - i));
            for (int k : chords[c]) d.noteOff(k);
        }
        for (int i = len * 4; i < total; i += 256) d.process(&dl[i], &dr[i], std::min(256, total - i));
        writeWav(argc > 1 ? argv[1] : "synth_demo.wav", dl, dr);
    }
    std::printf("synth: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
