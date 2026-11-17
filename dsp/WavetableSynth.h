// Wavetable synth engine (v0.1): band-limited morphing wavetables, unison, sub, SVF low-pass,
// amp ADSR + mod envelope (filter + table position). Header-only, no allocation while playing.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace echolab {
namespace wt {

constexpr double kPi = 3.14159265358979323846;

// ------------------------------------------------------------------ wavetable
// Each frame is stored as kLevels band-limited copies: level L keeps harmonics 1..(kMaxHarm >> L).
// The table is 4x oversampled against its top harmonic, so linear interpolation stays clean.
constexpr int kTableSize = 4096;
constexpr int kMaxHarm = 1024;
constexpr int kLevels = 11;  // 1024, 512, ... 1 harmonics
constexpr int kStride = kTableSize + 1;  // +1 guard sample for interpolation

inline void fft(std::vector<std::complex<double>>& a) {  // in-place radix-2, e^{+i} (unscaled inverse)
    const int n = (int)a.size();
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const std::complex<double> w(std::cos(2 * kPi / len), std::sin(2 * kPi / len));
        for (int i = 0; i < n; i += len) {
            std::complex<double> wk(1);
            for (int k = 0; k < len / 2; ++k, wk *= w) {
                auto u = a[i + k], v = a[i + k + len / 2] * wk;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
        }
    }
}

// A frame's spectrum: harmonic h (1..kMaxHarm, index 0 unused) contributes Re(c_h * e^{i 2pi h phase}),
// i.e. amplitude |c_h| and phase arg(c_h). A sine-phase partial of amplitude a is c = -i a.
using Spectrum = std::array<std::complex<double>, kMaxHarm + 1>;

struct Wavetable {
    int frames = 0;
    std::vector<float> data;  // [frame][level][kStride]
    const float* at(int f, int level) const { return &data[((size_t)f * kLevels + level) * kStride]; }

    void build(const std::vector<Spectrum>& spec) {
        frames = (int)spec.size();
        data.assign((size_t)frames * kLevels * kStride, 0.f);
        std::vector<std::complex<double>> buf(kTableSize);
        for (int f = 0; f < frames; ++f) {
            double gain = 1;
            for (int level = 0; level < kLevels; ++level) {
                const int top = kMaxHarm >> level;
                std::fill(buf.begin(), buf.end(), 0.0);
                for (int h = 1; h <= top; ++h) buf[h] = spec[f][h];
                fft(buf);
                if (level == 0) {  // one gain per frame (from the full-band copy), so levels match
                    double peak = 1e-9;
                    for (auto& c : buf) peak = std::max(peak, std::abs(c.real()));
                    gain = peak > 1e-6 ? 1.0 / peak : 0.0;  // a silent frame stays silent
                }
                float* d = &data[((size_t)f * kLevels + level) * kStride];
                for (int i = 0; i < kTableSize; ++i) d[i] = (float)(buf[i].real() * gain);
                d[kTableSize] = d[0];
            }
        }
    }
};

// The factory table, 64 frames: sine -> triangle -> saw -> square -> two moving vowel formants.
inline std::vector<Spectrum> orpheoSpectra() {
    auto sine = [](int h) { return h == 1 ? 1.0 : 0.0; };
    auto tri = [](int h) { return h % 2 ? ((h / 2) % 2 ? -1.0 : 1.0) / (double(h) * h) : 0.0; };
    auto saw = [](int h) { return 1.0 / h; };
    auto square = [](int h) { return h % 2 ? 1.0 / h : 0.0; };
    auto vowel = [](int h, double t) {  // saw through two formants that open as t rises
        const double f1 = 3 + 4 * t, f2 = 9 + 9 * t, lh = std::log2((double)h);
        const double g = std::exp(-std::pow((lh - std::log2(f1)) / 0.35, 2)) + 0.7 * std::exp(-std::pow((lh - std::log2(f2)) / 0.3, 2));
        return (0.08 + g) / std::sqrt((double)h);
    };
    std::vector<Spectrum> out(64);
    for (int f = 0; f < 64; ++f) {
        const int seg = f / 16;
        const double t = (f % 16) / 15.0;
        for (int h = 1; h <= kMaxHarm; ++h) {
            double a;
            switch (seg) {
                case 0: a = (1 - t) * sine(h) + t * tri(h); break;
                case 1: a = (1 - t) * tri(h) + t * saw(h); break;
                case 2: a = (1 - t) * saw(h) + t * square(h); break;
                default: a = (1 - t) * square(h) + t * vowel(h, t); break;
            }
            out[f][h] = std::complex<double>(0, -a);  // sine phase
        }
        out[f][0] = 0;
    }
    return out;
}

// ------------------------------------------------------------------ parameters
struct Params {
    double position = 0.4;        // 0..1 across the frames
    int unison = 1;               // 1..7
    double detuneCents = 12;      // total spread between the outer unison voices
    double spread = 0.6;          // stereo width of the unison 0..1
    double sub = 0;               // sine one octave down, 0..1
    double cutoffHz = 20000, resonance = 0.1;
    double filterEnv = 0;         // -1..1, +-6 octaves from the mod envelope
    double posEnv = 0;            // -1..1, how far the mod envelope pushes the position
    double ampA = 0.003, ampD = 0.3, ampS = 0.8, ampR = 0.3;  // seconds, sustain 0..1
    double modA = 0.003, modD = 0.6;
    double velocity = 0.6;        // how much velocity changes the level, 0..1
    double outputDb = 0;
};

// ------------------------------------------------------------------ envelope
struct Env {
    enum Stage { Idle, Attack, Decay, Sustain, Release } stage = Idle;
    double v = 0, aInc = 0, dCoef = 0, rCoef = 0, sus = 0;
    void set(double sr, double a, double d, double s, double r) {
        aInc = 1.0 / std::max(1.0, a * sr);
        dCoef = std::exp(-1.0 / std::max(1.0, d * sr / 5));  // ~-43 dB at the stated time
        rCoef = std::exp(-1.0 / std::max(1.0, r * sr / 5));
        sus = s;
    }
    void gate(bool on) { stage = on ? Attack : (stage == Idle ? Idle : Release); }
    double next() {
        switch (stage) {
            case Attack: v += aInc; if (v >= 1) { v = 1; stage = Decay; } break;
            case Decay: v = sus + (v - sus) * dCoef; if (v - sus < 1e-4) { v = sus; stage = Sustain; } break;
            case Sustain: v = sus; break;
            case Release: v *= rCoef; if (v < 1e-5) { v = 0; stage = Idle; } break;
            case Idle: break;
        }
        return v;
    }
};

// ------------------------------------------------------------------ voice
constexpr int kMaxUnison = 7;

struct Voice {
    int note = -1;
    double vel = 0;
    uint64_t age = 0;
    Env amp, mod;
    std::array<double, kMaxUnison> phase{};
    double subPhase = 0;
    double ic1[2]{}, ic2[2]{};  // SVF state per channel
    bool active() const { return amp.stage != Env::Idle; }
};

class Synth {
public:
    static constexpr int kVoices = 16;

    Synth() { factory.build(orpheoSpectra()); }

    // Play from another table (nullptr = the factory table). The caller keeps it alive until it
    // has been replaced and at least one more block has run.
    void setTable(const Wavetable* t) { current.store(t ? t : &factory, std::memory_order_release); }

    void prepare(double sampleRate) { sr = sampleRate; reset(); }
    void reset() {
        for (auto& v : voices) v = Voice{};
        held.fill(false);
    }
    void update(const Params& np) { p = np; }

    void noteOn(int note, double velocity) {
        Voice* v = nullptr;
        for (auto& c : voices) if (!c.active()) { v = &c; break; }
        if (!v) {  // steal: the oldest releasing voice, else the oldest
            for (auto& c : voices) if (c.amp.stage == Env::Release && (!v || c.age < v->age)) v = &c;
            if (!v) for (auto& c : voices) if (!v || c.age < v->age) v = &c;
        }
        const bool fresh = !v->active();
        v->note = note;
        v->vel = velocity;
        v->age = ++clock;
        if (fresh) {  // keep running phases on a stolen voice: no click
            v->amp.v = 0;
            v->mod.v = 0;
            for (int u = 0; u < kMaxUnison; ++u) v->phase[u] = u == 0 ? 0.0 : rand01();
            v->subPhase = 0;
            for (int c = 0; c < 2; ++c) v->ic1[c] = v->ic2[c] = 0;
        }
        v->amp.gate(true);
        v->mod.gate(true);
        held[note & 127] = true;
    }
    void noteOff(int note) {
        held[note & 127] = false;
        for (auto& v : voices)
            if (v.note == note && v.active() && v.amp.stage != Env::Release) { v.amp.gate(false); v.mod.gate(false); }
    }
    void allOff() { for (auto& v : voices) { v.amp.gate(false); v.mod.gate(false); } }

    // add n samples into L/R (caller clears)
    void process(float* L, float* R, int n) {
        const Wavetable& table = *current.load(std::memory_order_acquire);
        const double out = std::pow(10.0, p.outputDb / 20);
        const int uni = std::clamp(p.unison, 1, kMaxUnison);
        const double uniGain = 1.0 / std::sqrt((double)uni);
        for (int vi = 0; vi < kVoices; ++vi) {
            auto& v = voices[vi];
            if (!v.active()) { views[vi].note.store(-1, std::memory_order_relaxed); continue; }
            v.amp.set(sr, p.ampA, p.ampD, p.ampS, p.ampR);
            v.mod.set(sr, p.modA, p.modD, 0, p.ampR);
            const double f0 = 440.0 * std::pow(2.0, (v.note - 69) / 12.0);
            // per-unison increments, pan gains and band-limit level (fixed for the block)
            double inc[kMaxUnison], gl[kMaxUnison], gr[kMaxUnison];
            int lev[kMaxUnison];
            for (int u = 0; u < uni; ++u) {
                const double x = uni == 1 ? 0.0 : (2.0 * u / (uni - 1) - 1);  // -1..1
                const double f = f0 * std::pow(2.0, x * p.detuneCents / 2 / 1200);
                inc[u] = f / sr;
                lev[u] = levelFor(f);
                const double pan = x * p.spread * 0.5 + 0.5;  // 0 = left
                gl[u] = std::cos(pan * kPi / 2) * uniGain * 1.41421356;
                gr[u] = std::sin(pan * kPi / 2) * uniGain * 1.41421356;
            }
            const double subInc = f0 * 0.5 / sr;
            const double velGain = 1 - p.velocity + p.velocity * v.vel;
            for (int i = 0; i < n; ++i) {
                const double a = v.amp.next(), m = v.mod.next();
                if (!v.active()) break;
                // table position with the mod envelope, frames interpolated
                const double pos = std::clamp(p.position + p.posEnv * m, 0.0, 1.0) * (table.frames - 1);
                const int f0i = std::min((int)pos, table.frames - 2);
                const double ff = pos - f0i;
                double sl = 0, sr_ = 0;
                for (int u = 0; u < uni; ++u) {
                    const double s = read(table, f0i, ff, lev[u], v.phase[u]);
                    sl += s * gl[u];
                    sr_ += s * gr[u];
                    v.phase[u] += inc[u];
                    if (v.phase[u] >= 1) v.phase[u] -= 1;
                }
                if (p.sub > 0) {
                    const double s = std::sin(2 * kPi * v.subPhase) * p.sub;
                    sl += s; sr_ += s;
                    v.subPhase += subInc;
                    if (v.subPhase >= 1) v.subPhase -= 1;
                }
                // SVF low-pass (TPT), cutoff follows the mod envelope
                const double fc = std::clamp(p.cutoffHz * std::pow(2.0, p.filterEnv * 6 * m), 20.0, sr * 0.45);
                if (fc < sr * 0.449 || p.resonance > 0.01) {
                    const double g = std::tan(kPi * fc / sr), k = 2 - 1.9 * p.resonance;
                    const double a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
                    double* ch[2] = {&sl, &sr_};
                    for (int c = 0; c < 2; ++c) {
                        const double v0 = *ch[c], v3 = v0 - v.ic2[c];
                        const double v1 = a1 * v.ic1[c] + a2 * v3, v2 = v.ic2[c] + a2 * v.ic1[c] + a3 * v3;
                        v.ic1[c] = 2 * v1 - v.ic1[c];
                        v.ic2[c] = 2 * v2 - v.ic2[c];
                        *ch[c] = v2;
                    }
                }
                const double g = a * velGain * out * 0.35;
                L[i] += (float)(sl * g);
                R[i] += (float)(sr_ * g);
                if (i == n - 1) {
                    views[vi].pos.store((float)(pos / (table.frames - 1)), std::memory_order_relaxed);
                    views[vi].amp.store((float)a, std::memory_order_relaxed);
                    views[vi].mod.store((float)m, std::memory_order_relaxed);
                }
            }
            views[vi].note.store(v.active() ? v.note : -1, std::memory_order_relaxed);
        }
    }

    // Master safety: transparent below kKnee, then rounds off smoothly towards 1.0 (never hard clips).
    static constexpr double kKnee = 0.8;
    static float safety(float x) {
        const double a = std::abs(x);
        if (a <= kKnee) return x;
        const double r = 1 - kKnee;
        return (float)std::copysign(kKnee + r * std::tanh((a - kKnee) / r), x);
    }
    void process(float* L, float* R, int n, bool withSafety) {
        process(L, R, n);
        if (withSafety) for (int i = 0; i < n; ++i) { L[i] = safety(L[i]); R[i] = safety(R[i]); }
    }

    // per-voice view for the editor: note (-1 = idle), effective table position, amp and mod levels
    struct View { std::atomic<int> note{-1}; std::atomic<float> pos{0}, amp{0}, mod{0}; };
    std::array<View, kVoices> views;

    int activeVoices() const { int c = 0; for (auto& v : voices) c += v.active(); return c; }
    const Wavetable& wavetable() const { return *current.load(std::memory_order_acquire); }

private:
    int levelFor(double f) const {  // fewest-cut level whose top harmonic stays under Nyquist
        const double maxH = (sr * 0.5) / f;
        int level = 0;
        while (level < kLevels - 1 && (kMaxHarm >> level) > maxH) ++level;
        return level;
    }
    static double read(const Wavetable& table, int f, double ff, int level, double ph) {
        const double x = ph * kTableSize;
        const int i = (int)x;
        const double t = x - i;
        const float* a = table.at(f, level);
        const float* b = table.at(f + 1, level);
        const double sa = a[i] + (a[i + 1] - a[i]) * t, sb = b[i] + (b[i + 1] - b[i]) * t;
        return sa + (sb - sa) * ff;
    }
    double rand01() { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; return (seed >> 11) * (1.0 / 9007199254740992.0); }

    Wavetable factory;
    std::atomic<const Wavetable*> current{&factory};
    Params p;
    double sr = 48000;
    std::array<Voice, kVoices> voices{};
    std::array<bool, 128> held{};
    uint64_t clock = 0, seed = 0x9E3779B97F4A7C15ULL;
};

}  // namespace wt
}  // namespace echolab
