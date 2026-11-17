// Checks for the modulation system (dsp/Modulation.h, WavetableSynth.h): LFO clocks, routes,
// pitch bend, glide, mono/legato, aftertouch, and a fuzz over random routings.
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "WavetableSynth.h"

using namespace echolab::wt;
constexpr double SR = 48000;

static double freqOf(const std::vector<float>& y, size_t from, size_t to) {  // rising zero crossings
    double first = -1, last = -1; int n = 0;
    for (size_t i = from + 1; i < to; ++i) if (y[i - 1] < 0 && y[i] >= 0) { const double t = i - 1 + y[i - 1] / (y[i - 1] - y[i]); if (first < 0) first = t; last = t; ++n; }
    return n > 1 ? (n - 1) * SR / (last - first) : 0;
}
static Params sinePatch() {  // frame 0 of ORPHEO is a pure sine: pitch is easy to measure
    Params p; p.position = 0; p.cutoffHz = 20000; p.resonance = 0; p.ampA = 0.001; p.ampS = 1; p.velocity = 0; p.unison = 1;
    return p;
}
static std::vector<float> render(Synth& s, int samples) {
    std::vector<float> l(samples), r(samples);
    for (int i = 0; i < samples; i += 64) s.process(&l[i], &r[i], std::min(64, samples - i));
    return l;
}
static double posOf(Synth& s) { for (auto& v : s.views) if (v.note.load() >= 0) return v.pos.load(); return -1; }
static double modOf(Synth& s) { for (auto& v : s.views) if (v.note.load() >= 0) return v.mod.load(); return -1; }

int main() {
    int fails = 0;
    auto check = [&](bool ok, const char* what, double v) { std::printf("%s %-56s %9.4f\n", ok ? "ok  " : "FAIL", what, v); fails += !ok; };

    // LFO clocks
    {
        LfoState s; s.start(1); int wraps = 0; double prev = 0;
        for (int i = 0; i < (int)SR; ++i) { lfoStep(s, kLfoSawUp, 5.0 / SR, false); if (s.phase < prev) ++wraps; prev = s.phase; }
        check(wraps == 5, "LFO 5 Hz: cycles in one second", wraps);
        LfoState o; o.start(1); for (int i = 0; i < (int)SR; ++i) lfoStep(o, kLfoSine, 3.0 / SR, true);
        check(o.done && std::abs(lfoStep(o, kLfoSawUp, 0, true) - 1) < 1e-9, "LFO once: stops at the end of its cycle", o.phase);
    }
    // a synced LFO at 1/4 and 120 BPM is 2 Hz: route it to Position and read the playhead
    {
        Synth s; s.prepare(SR); Params p = sinePatch(); p.position = 0.5;
        p.lfo[0].shape = kLfoSawUp; p.lfo[0].sync = true; p.lfo[0].note = 8; p.lfo[0].mode = kLfoRetrig;
        p.slots[0] = {kSrcLfo1, kDstPosition, 0.25}; s.update(p); s.setTransport(120, 0, false);
        s.noteOn(57, 1); render(s, (int)(SR * 0.125));   // a quarter of a 2 Hz cycle: saw at -0.5
        check(std::abs(posOf(s) - (0.5 + 0.25 * -0.5)) < 0.01, "synced LFO 1/4 @120: a quarter cycle in 125 ms", posOf(s));
    }
    // a free + synced LFO follows the song position (beat 1.5 at 1/4 -> half way)
    {
        Synth s; s.prepare(SR); Params p = sinePatch(); p.position = 0.5;
        p.lfo[0].shape = kLfoSawUp; p.lfo[0].sync = true; p.lfo[0].note = 8; p.lfo[0].mode = kLfoFree;
        p.slots[0] = {kSrcLfo1, kDstPosition, 0.25}; s.update(p); s.setTransport(120, 1.5, true);
        s.noteOn(57, 1); render(s, 64);
        check(std::abs(posOf(s) - 0.5) < 0.01, "free synced LFO locks to the song position", posOf(s));
    }
    // routes: Macro 1 -> Pitch +100% (12 semitones); macro at 50% = a tritone up
    {
        Synth s; s.prepare(SR); Params p = sinePatch(); p.macro[0] = 0.5; p.slots[3] = {kSrcMacro1, kDstPitch, 1.0}; s.update(p);
        s.noteOn(57, 1); auto y = render(s, (int)SR / 2);
        const double f = freqOf(y, y.size() / 2, y.size()), want = 220 * std::pow(2.0, 6 / 12.0);
        check(std::abs(f / want - 1) < 0.002, "Macro 1 -> Pitch at 50%: a tritone up (Hz)", f);
    }
    // mod wheel -> Position
    {
        Synth s; s.prepare(SR); Params p = sinePatch(); p.position = 0.1; p.modWheel = 1; p.slots[0] = {kSrcModWheel, kDstPosition, 0.6}; s.update(p);
        s.noteOn(57, 1); render(s, (int)SR / 4);
        check(std::abs(posOf(s) - 0.7) < 0.01, "Mod wheel -> Position +60%", posOf(s));
    }
    // pitch bend: full up at a range of 7 = a fifth
    {
        Synth s; s.prepare(SR); Params p = sinePatch(); p.pitchBend = 1; p.bendRange = 7; s.update(p);
        s.noteOn(57, 1); auto y = render(s, (int)SR / 2);
        const double f = freqOf(y, y.size() / 2, y.size());
        check(std::abs(f / (220 * std::pow(2.0, 7 / 12.0)) - 1) < 0.002, "pitch bend +1, range 7: a fifth up (Hz)", f);
    }
    // glide: mono, 100 ms, A3 -> A4: half way-ish early, there by 300 ms
    {
        Synth s; s.prepare(SR); Params p = sinePatch(); p.voiceMode = kMono; p.glide = 0.1; s.update(p);
        s.noteOn(57, 1); render(s, (int)SR / 4); s.noteOn(69, 1);
        auto early = render(s, (int)(SR * 0.03)); auto late = render(s, (int)(SR * 0.4));
        const double fe = freqOf(early, 0, early.size()), fl = freqOf(late, late.size() / 2, late.size());
        check(fe > 240 && fe < 430, "glide 100 ms: still travelling after 30 ms (Hz)", fe);
        check(std::abs(fl / 440 - 1) < 0.003, "glide 100 ms: arrived (Hz)", fl);
    }
    // legato slides without retriggering; mono retriggers (watch the mod envelope, which decays to 0)
    for (int mode : {kLegato, kMono}) {
        Synth s; s.prepare(SR); Params p = sinePatch(); p.voiceMode = mode; p.modA = 0.001; p.modD = 0.05; s.update(p);
        s.noteOn(57, 1); render(s, (int)SR / 2); s.noteOn(60, 1); render(s, 256);
        const double m = modOf(s);
        if (mode == kLegato) check(m < 0.05, "legato: overlapping note does not retrigger (mod env)", m);
        else check(m > 0.5, "mono: every note retriggers (mod env)", m);
    }
    // poly aftertouch -> Level: the pressed note gets louder
    {
        Synth s; s.prepare(SR); Params p = sinePatch(); p.slots[0] = {kSrcAftertouch, kDstLevel, 1.0}; s.update(p);
        s.noteOn(57, 1); auto a = render(s, (int)SR / 4); s.polyPressure(57, 1.0); auto b = render(s, (int)SR / 4);
        double ra = 0, rb = 0; for (size_t i = a.size() / 2; i < a.size(); ++i) { ra += a[i] * a[i]; rb += b[i] * b[i]; }
        check(std::abs(20 * std::log10(std::sqrt(rb / ra)) - 6.02) < 0.3, "poly aftertouch -> Level +100%: +6 dB", 20 * std::log10(std::sqrt(rb / ra)));
    }
    // fuzz: random routings, shapes, modes and amounts; always finite and bounded
    {
        std::mt19937 rng(11); std::uniform_real_distribution<double> U(0, 1);
        bool fin = true; double pk = 0;
        for (int trial = 0; trial < 40; ++trial) {
            Synth s; s.prepare(SR); Params p; p.unison = 1 + (int)(U(rng) * 7); p.filterType = (int)(U(rng) * kNumFilterTypes); p.resonance = U(rng);
            p.voiceMode = (int)(U(rng) * 3); p.glide = U(rng) * 0.3; p.scanMode = (int)(U(rng) * 4);
            for (auto& l : p.lfo) { l.shape = (int)(U(rng) * kNumLfoShapes); l.hz = 0.1 + 30 * U(rng); l.sync = U(rng) < 0.5; l.note = (int)(U(rng) * kLfoNotes); l.mode = (int)(U(rng) * 3); }
            for (auto& sl : p.slots) { sl.src = (int)(U(rng) * kNumSources); sl.dst = (int)(U(rng) * kNumDests); sl.amt = U(rng) * 2 - 1; }
            for (auto& m : p.macro) m = U(rng);
            p.modWheel = U(rng); p.pitchBend = U(rng) * 2 - 1; p.aftertouch = U(rng);
            s.update(p); s.setTransport(90 + 60 * U(rng), 0, U(rng) < 0.5);
            for (int nn : {48, 55, 60, 67}) s.noteOn(nn, U(rng));
            std::vector<float> l(9600), r(9600);
            for (int i = 0; i < 9600; i += 64) s.process(&l[i], &r[i], 64);
            for (int nn : {48, 55, 60, 67}) s.noteOff(nn);
            for (int i = 0; i < 9600; i += 64) s.process(&l[i], &r[i], 64);
            for (float v : l) { fin &= std::isfinite(v); pk = std::max(pk, (double)std::abs(v)); }
        }
        check(fin && pk < 6, "fuzz: 40 random patches finite and bounded (peak)", pk);
    }
    std::printf("mod: %s\n", fails ? "FAIL" : "ok");
    // sustain pedal: a released key keeps sounding while the pedal is down, and lets go when it comes up
    {
        Synth s; s.setTable(&factoryTable(0)); s.prepare(SR); Params p = sinePatch(); p.ampR = 0.05; s.update(p);
        auto rms = [](const std::vector<float>& y) { double a = 0; for (size_t i = y.size() / 2; i < y.size(); ++i) a += y[i] * y[i]; return std::sqrt(a / (y.size() / 2)); };
        s.setPedal(true); s.noteOn(60, 1); render(s, 4800); s.noteOff(60);
        const double held = rms(render(s, 24000));
        s.setPedal(false); render(s, 24000);
        const double after = rms(render(s, 4800));
        check(held > 0.1 && after < 1e-4, "sustain pedal: holds after key-up, releases on pedal-up", held);
    }
    return fails ? 1 : 0;
}
