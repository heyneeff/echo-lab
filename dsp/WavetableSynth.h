// Wavetable synth engine: band-limited morphing wavetables (five built-in, or grown from sound),
// SCAN playheads, unison, sub, SVF low-pass, amp ADSR + mod envelope (filter + table position).
// Header-only, no allocation while playing.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "Filters.h"
#include "Modulation.h"

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
    double seconds = 0;       // length of the sound it was grown from (0 = a built-in table)
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

// Spectrum of any single-cycle shape given in time (t in [0,1)). Sampled 64x finer than the top
// harmonic we keep, so even shapes with jumps (saw, pulse, sync) come out exact to ~-96 dB.
template <class F>
inline Spectrum spectrumOf(F wave) {
    constexpr int N = 65536;
    std::vector<std::complex<double>> a(N);
    for (int i = 0; i < N; ++i) a[i] = wave((i + 0.5) / N);
    fft(a);
    Spectrum s;
    s.fill(0.0);
    for (int h = 1; h <= kMaxHarm; ++h) s[h] = std::conj(a[h]) * (2.0 / N);
    return s;
}

namespace shape {
inline double saw(double t) { return 1 - 2 * t; }
inline double square(double t) { return t < 0.5 ? 1 : -1; }
inline double pulse(double t, double duty) { return t < duty ? 1 : -1; }
inline double tri(double t) { return 1 - 4 * std::abs(t - 0.5); }
inline double shark(double t) { return t < 0.75 ? -1 + 2 * t / 0.75 : 1 - 2 * (t - 0.75) / 0.25; }  // Minimoog "triangle-saw"
inline double sync(double t, double ratio) { const double x = t * ratio; return 1 - 2 * (x - std::floor(x)); }  // hard-synced saw
}  // namespace shape

// MOOG: the Minimoog's six waveforms, in panel order.
inline std::vector<Spectrum> moogSpectra() {
    return {spectrumOf(shape::tri), spectrumOf(shape::shark), spectrumOf(shape::saw), spectrumOf(shape::square),
            spectrumOf([](double t) { return shape::pulse(t, 0.3); }), spectrumOf([](double t) { return shape::pulse(t, 0.1); })};
}
// PWM: pulse width 50% -> 5%, so Position is pulse-width modulation.
inline std::vector<Spectrum> pwmSpectra() {
    std::vector<Spectrum> out;
    for (int k = 0; k < 32; ++k) { const double d = 0.5 - 0.45 * k / 31; out.push_back(spectrumOf([d](double t) { return shape::pulse(t, d); })); }
    return out;
}
// SYNC: a saw hard-synced at ratio 1 -> 8, so Position is the Prophet sync sweep.
inline std::vector<Spectrum> syncSpectra() {
    std::vector<Spectrum> out;
    for (int k = 0; k < 32; ++k) { const double r = 1 + 7.0 * k / 31; out.push_back(spectrumOf([r](double t) { return shape::sync(t, r); })); }
    return out;
}
// JUNO: the DCO, saw blending into a pulse that narrows (Juno PWM).
inline std::vector<Spectrum> junoSpectra() {
    std::vector<Spectrum> out;
    for (int k = 0; k < 16; ++k) {
        const double u = k / 15.0, d = 0.5 - 0.4 * u;
        out.push_back(spectrumOf([u, d](double t) { return (1 - 0.6 * u) * shape::saw(t) + (0.4 + 0.6 * u) * shape::pulse(t, d); }));
    }
    return out;
}

// The built-in tables, built once per process and shared by every Morpheus in the set.
constexpr int kFactoryTables = 5;
inline const char* const kFactoryNames[kFactoryTables] = {"ORPHEO", "MOOG", "PWM", "SYNC", "JUNO"};
inline const Wavetable& factoryTable(int i) {
    static const std::array<Wavetable, kFactoryTables> tables = [] {
        std::array<Wavetable, kFactoryTables> t;
        t[0].build(orpheoSpectra());
        t[1].build(moogSpectra());
        t[2].build(pwmSpectra());
        t[3].build(syncSpectra());
        t[4].build(junoSpectra());
        return t;
    }();
    return tables[std::clamp(i, 0, kFactoryTables - 1)];
}

// ------------------------------------------------------------------ parameters
enum ScanMode { kScanOff = 0, kScanOnce, kScanLoop, kScanPingPong };
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
    // SCAN: each note's playhead travels from Position to the end of the table at the speed of the
    // sound the table was grown from (built-in tables: 2 s), times scanSpeed
    int scanMode = kScanOff;
    double scanSpeed = 1;
    // filter section (dsp/Filters.h); SVF with morph 0 and no drive is the original low-pass
    int filterType = kFilterSvf;
    double filterMorph = 0;       // 0..1
    double filterDriveDb = 0;     // 0..24
    double keytrack = 0;          // 0..1: cutoff follows the note (1 = an octave per octave, around C4)
    int hpf = 1;                  // 0 boost, 1 flat, 2 low, 3 high
    // modulation (dsp/Modulation.h)
    std::array<ModSlot, kModSlots> slots{};
    std::array<LfoParams, kLfos> lfo{};
    double env3A = 0.01, env3D = 0.5, env3S = 0, env3R = 0.3;
    std::array<double, 8> macro{};            // 0..1
    double modWheel = 0, pitchBend = 0, aftertouch = 0;  // wheel/AT 0..1, bend -1..1
    double bendRange = 2;                     // semitones
    int voiceMode = 0;                        // 0 poly, 1 mono, 2 legato (overlapping notes slide, no retrigger)
    double glide = 0;                         // seconds
};
enum VoiceMode { kPoly = 0, kMono, kLegato };

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
    Env amp, mod, env3;
    std::array<double, kMaxUnison> phase{};
    double subPhase = 0;
    double scan = 0;            // SCAN playhead 0..1 (from Position to the end)
    int scanDir = 1;
    VoiceFilter filt;
    std::array<LfoState, kLfos> lfo{};
    double rnd = 0;             // per-note random, -1..1
    double drift = 0, driftTarget = 0;  // slow per-voice wander, -1..1
    int driftCount = 0;
    double pressure = 0;        // poly aftertouch 0..1
    double pitch = 60;          // sounding pitch in semitones (glides toward note)
    double ratio = 1;           // last pitch ratio from modulation + bend + glide (for band-limiting)
    double modDetune = 0, modSpread = 0;  // last block's modulation of the per-block settings
    bool active() const { return amp.stage != Env::Idle; }
};

class Synth {
public:
    static constexpr int kVoices = 16;

    Synth() : combMem((size_t)kVoices * 2 * kCombLen, 0.f) { setTable(nullptr); wireCombs(); }

    // Play from another table (nullptr = the ORPHEO table). The caller keeps it alive until it
    // has been replaced and at least one more block has run.
    void setTable(const Wavetable* t) { current.store(t ? t : &factoryTable(0), std::memory_order_release); }

    void prepare(double sampleRate) { sr = sampleRate; reset(); hpf.set(p.hpf, sr); }
    void reset() {
        sm.pos = -1;
        for (auto& v : voices) v = Voice{};
        wireCombs();
        held.fill(false);
        depth = 0;
        lastPitch = -1;
        for (int k = 0; k < kLfos; ++k) lfoG[k].start(97 + k);
    }
    void update(const Params& np) {
        if (np.hpf != p.hpf) hpf.set(np.hpf, sr);
        p = np;
    }

    void noteOn(int note, double velocity) {
        const bool wasHeld = depth > 0;
        push(note);
        if (p.voiceMode != kPoly) {  // one voice; the others let go
            Voice& v = voices[0];
            for (int i = 1; i < kVoices; ++i) if (voices[i].active()) gateAll(voices[i], false);
            const bool slide = p.voiceMode == kLegato && wasHeld && v.active();
            if (!v.active()) startVoice(v, note, velocity, note);
            else if (p.glide <= 0 || (p.voiceMode == kLegato && !wasHeld)) v.pitch = note;  // jump (else glide on)
            v.note = note;
            v.age = ++clock;
            if (!slide) { v.vel = velocity; retrigger(v); }
            lastPitch = note;
            return;
        }
        Voice* v = nullptr;
        for (auto& c : voices) if (!c.active()) { v = &c; break; }
        if (!v) {  // steal: the oldest releasing voice, else the oldest
            for (auto& c : voices) if (c.amp.stage == Env::Release && (!v || c.age < v->age)) v = &c;
            if (!v) for (auto& c : voices) if (!v || c.age < v->age) v = &c;
        }
        const double from = p.glide > 0 && lastPitch >= 0 ? lastPitch : note;  // poly portamento
        if (!v->active()) startVoice(*v, note, velocity, from);  // a stolen voice keeps running phases: no click
        else { v->pitch = from; }
        v->note = note;
        v->vel = velocity;
        v->age = ++clock;
        retrigger(*v);
        lastPitch = note;
    }
    void noteOff(int note) {
        remove(note);
        if (p.voiceMode != kPoly) {
            Voice& v = voices[0];
            if (v.note != note || !v.active()) return;
            if (depth > 0) {  // fall back to the newest held note, without retriggering
                v.note = stack[depth - 1];
                if (p.glide <= 0) v.pitch = v.note;
                lastPitch = v.note;
            } else gateAll(v, false);
            return;
        }
        for (auto& v : voices)
            if (v.note == note && v.active() && v.amp.stage != Env::Release) gateAll(v, false);
    }
    void polyPressure(int note, double value) { for (auto& v : voices) if (v.note == note && v.active()) v.pressure = value; }
    void allOff() { for (auto& v : voices) gateAll(v, false); depth = 0; }
    void setTransport(double bpm, double beatPos, bool isPlaying) { if (bpm > 20) tempo = bpm; beat = beatPos; playing = isPlaying; }

    // what the face shows: each LFO's value and phase for the newest voice (or the shared clock)
    std::array<std::atomic<float>, kLfos> lfoView{}, lfoPhaseView{};

    // add n samples into L/R (caller clears)
    void process(float* L, float* R, int n) {
        for (int off = 0; off < n; off += kChunk) processChunk(L + off, R + off, std::min(kChunk, n - off));
        hpf.process(L, R, n);
    }

private:
    // Knob moves arrive once per host block; stepping the table position or the cutoff that coarsely
    // is audible as static. These glide sample by sample (~12 ms) instead.
    static constexpr int kChunk = 64;
    struct Smooth { double pos = -1, cut = 0, res = 0, sub = 0, out = 0, morph = 0, drive = 0, wheel = 0, bend = 0, at = 0; std::array<double, 8> mac{}; } sm;

    void processChunk(float* L, float* R, int n) {
        const Wavetable& table = *current.load(std::memory_order_acquire);
        const double k = 1 - std::exp(-1.0 / (0.012 * sr));
        const double tCut = std::log2(std::max(20.0, p.cutoffHz)), tOut = std::pow(10.0, p.outputDb / 20);
        if (sm.pos < 0) {  // first block: start on target
            sm = {p.position, tCut, p.resonance, p.sub, tOut, p.filterMorph, p.filterDriveDb};
            sm.wheel = p.modWheel; sm.bend = p.pitchBend; sm.at = p.aftertouch; sm.mac = p.macro;
        }
        double sPos[kChunk], sCut[kChunk], sRes[kChunk], sSub[kChunk], sOut[kChunk], sMorph[kChunk], sDrive[kChunk];
        double sWheel[kChunk], sBend[kChunk], sAt[kChunk], sMac[8][kChunk];
        for (int i = 0; i < n; ++i) {
            sm.pos += (p.position - sm.pos) * k; sPos[i] = sm.pos;
            sm.cut += (tCut - sm.cut) * k;       sCut[i] = sm.cut;
            sm.res += (p.resonance - sm.res) * k; sRes[i] = sm.res;
            sm.sub += (p.sub - sm.sub) * k;       sSub[i] = sm.sub;
            sm.out += (tOut - sm.out) * k;        sOut[i] = sm.out;
            sm.morph += (p.filterMorph - sm.morph) * k; sMorph[i] = sm.morph;
            sm.drive += (p.filterDriveDb - sm.drive) * k; sDrive[i] = sm.drive;
            sm.wheel += (p.modWheel - sm.wheel) * k; sWheel[i] = sm.wheel;
            sm.bend += (p.pitchBend - sm.bend) * k;  sBend[i] = sm.bend;
            sm.at += (p.aftertouch - sm.at) * k;     sAt[i] = sm.at;
            for (int q = 0; q < 8; ++q) { sm.mac[q] += (p.macro[q] - sm.mac[q]) * k; sMac[q][i] = sm.mac[q]; }
        }
        // the routes that do something this block
        ModSlot act[kModSlots];
        int nAct = 0;
        bool useLfo[kLfos] = {};
        for (const auto& sl : p.slots)
            if (sl.src > kSrcNone && sl.src < kNumSources && sl.dst > kDstNone && sl.dst < kNumDests && std::abs(sl.amt) > 1e-5) {
                act[nAct++] = sl;
                if (sl.src >= kSrcLfo1 && sl.src <= kSrcLfo4) useLfo[sl.src - kSrcLfo1] = true;
            }
        // LFO clocks: the free-running ones are shared by every voice (and follow the host's beat when synced)
        const double bps = tempo / 60.0 / sr;
        double lfoInc[kLfos], gLfo[kLfos][kChunk];
        for (int q = 0; q < kLfos; ++q) {
            const auto& lp = p.lfo[q];
            const double beats = kLfoNoteBeats[std::clamp(lp.note, 0, kLfoNotes - 1)];
            lfoInc[q] = lp.sync ? bps / beats : lp.hz / sr;
            if (lp.mode != kLfoFree) continue;
            for (int i = 0; i < n; ++i) {
                if (lp.sync && playing) {  // locked to the song position
                    const double x = (beat + i * bps) / beats, ph = x - std::floor(x);
                    if (ph < lfoG[q].phase) { lfoG[q].a = lfoG[q].b; lfoG[q].b = lfoG[q].rnd(); }
                    lfoG[q].phase = ph;
                    gLfo[q][i] = lfoStep(lfoG[q], lp.shape, 0, false);
                } else gLfo[q][i] = lfoStep(lfoG[q], lp.shape, lfoInc[q], false);
            }
            lfoView[q].store((float)gLfo[q][n - 1], std::memory_order_relaxed);
            lfoPhaseView[q].store((float)lfoG[q].phase, std::memory_order_relaxed);
        }
        beat += n * bps;
        const double glideK = p.glide > 1e-4 ? 1 - std::exp(-3.0 / (p.glide * sr)) : 1.0;
        const double driftK = 1 - std::exp(-1.0 / (0.35 * sr));
        uint64_t newest = 0;
        for (const auto& v : voices) if (v.active()) newest = std::max(newest, v.age);

        const int uni = std::clamp(p.unison, 1, kMaxUnison);
        const double uniGain = 1.0 / std::sqrt((double)uni);
        for (int vi = 0; vi < kVoices; ++vi) {
            auto& v = voices[vi];
            if (!v.active()) { views[vi].note.store(-1, std::memory_order_relaxed); continue; }
            v.amp.set(sr, p.ampA, p.ampD, p.ampS, p.ampR);
            v.mod.set(sr, p.modA, p.modD, 0, p.ampR);
            v.env3.set(sr, p.env3A, p.env3D, p.env3S, p.env3R);
            const double f0 = 440.0 * std::pow(2.0, (v.note - 69) / 12.0);
            // per-unison increments, pan gains and band-limit level (fixed for the block; detune and
            // spread take the modulation from the end of the previous block)
            const double detune = std::max(0.0, p.detuneCents + v.modDetune * 100);
            const double spread = std::clamp(p.spread + v.modSpread, 0.0, 1.0);
            double inc[kMaxUnison], gl[kMaxUnison], gr[kMaxUnison];
            int lev[kMaxUnison];
            for (int u = 0; u < uni; ++u) {
                const double x = uni == 1 ? 0.0 : (2.0 * u / (uni - 1) - 1);  // -1..1
                const double f = f0 * std::pow(2.0, x * detune / 2 / 1200);
                inc[u] = f / sr;
                lev[u] = levelFor(f * std::max(1.0, v.ratio) * 1.06);  // headroom for pitch moving within the block
                const double pan = x * spread * 0.5 + 0.5;  // 0 = left
                gl[u] = std::cos(pan * kPi / 2) * uniGain * 1.41421356;
                gr[u] = std::sin(pan * kPi / 2) * uniGain * 1.41421356;
            }
            const double subInc = f0 * 0.5 / sr;
            const double scanInc = p.scanSpeed / ((table.seconds > 0 ? table.seconds : 2.0) * sr);
            const double velGain = 1 - p.velocity + p.velocity * v.vel;
            const double keyOct = p.keytrack * (v.note - 60) / 12.0;
            FilterSettings fs;
            fs.type = p.filterType;
            double src[kNumSources] = {};
            double mods[kNumDests] = {};
            for (int i = 0; i < n; ++i) {
                const double a = v.amp.next(), m = v.mod.next(), e3 = v.env3.next();
                if (!v.active()) break;
                // ---- modulation sources -> destinations
                v.drift += (v.driftTarget - v.drift) * driftK;
                if (--v.driftCount <= 0) { v.driftTarget = rand01() * 2 - 1; v.driftCount = (int)(sr * (0.15 + 0.3 * rand01())); }
                std::fill(mods, mods + kNumDests, 0.0);
                if (nAct) {
                    src[kSrcAmpEnv] = a; src[kSrcModEnv] = m; src[kSrcEnv3] = e3;
                    for (int q = 0; q < kLfos; ++q)
                        if (useLfo[q]) src[kSrcLfo1 + q] = p.lfo[q].mode == kLfoFree ? gLfo[q][i] : lfoStep(v.lfo[q], p.lfo[q].shape, lfoInc[q], p.lfo[q].mode == kLfoOnce);
                    src[kSrcVelocity] = v.vel; src[kSrcNote] = (v.note - 60) / 60.0;
                    src[kSrcModWheel] = sWheel[i]; src[kSrcPitchBend] = sBend[i]; src[kSrcAftertouch] = std::max(sAt[i], v.pressure);
                    src[kSrcRandom] = v.rnd; src[kSrcDrift] = v.drift;
                    for (int q = 0; q < 8; ++q) src[kSrcMacro1 + q] = sMac[q][i];
                    for (int r = 0; r < nAct; ++r) mods[act[r].dst] += act[r].amt * src[act[r].src];
                }
                // ---- pitch: modulation, bend, glide
                v.pitch += (v.note - v.pitch) * glideK;
                const double semis = mods[kDstPitch] * 12 + sBend[i] * p.bendRange + (v.pitch - v.note);
                const double ratio = std::abs(semis) > 1e-9 ? std::exp2(semis / 12) : 1.0;
                v.ratio = ratio;
                // ---- table position: SCAN playhead, mod envelope, routes
                double base = sPos[i];
                if (p.scanMode != kScanOff) {
                    base += (1 - base) * v.scan;
                    v.scan += scanInc * v.scanDir * (mods[kDstScanSpeed] != 0 ? std::exp2(mods[kDstScanSpeed] * 2) : 1.0);
                    if (v.scan >= 1) {
                        if (p.scanMode == kScanOnce) v.scan = 1;
                        else if (p.scanMode == kScanLoop) v.scan -= 1;
                        else { v.scan = 2 - v.scan; v.scanDir = -1; }
                    } else if (v.scan <= 0 && v.scanDir < 0) { v.scan = -v.scan; v.scanDir = 1; }
                }
                const double pos = std::clamp(base + p.posEnv * m + mods[kDstPosition], 0.0, 1.0) * (table.frames - 1);
                const int f0i = std::min((int)pos, table.frames - 2);
                const double ff = pos - f0i;
                double sl = 0, sr_ = 0;
                for (int u = 0; u < uni; ++u) {
                    const double s = read(table, f0i, ff, lev[u], v.phase[u]);
                    sl += s * gl[u];
                    sr_ += s * gr[u];
                    v.phase[u] += inc[u] * ratio;
                    if (v.phase[u] >= 1) v.phase[u] -= std::floor(v.phase[u]);
                }
                const double subLvl = std::clamp(sSub[i] + mods[kDstSub], 0.0, 1.0);
                if (subLvl > 1e-4) {
                    const double s = std::sin(2 * kPi * v.subPhase) * subLvl;
                    sl += s; sr_ += s;
                }
                v.subPhase += subInc * ratio;
                if (v.subPhase >= 1) v.subPhase -= std::floor(v.subPhase);
                // ---- filter (dsp/Filters.h): mod envelope, keyboard, routes
                fs.fc = std::clamp(std::exp2(sCut[i] + p.filterEnv * 6 * m + keyOct + mods[kDstCutoff] * 6), 20.0, sr * 0.45);
                fs.res = std::clamp(sRes[i] + mods[kDstResonance], 0.0, 1.0);
                fs.morph = std::clamp(sMorph[i] + mods[kDstMorph], 0.0, 1.0);
                fs.driveDb = std::clamp(sDrive[i] + mods[kDstFilterDrive] * 24, 0.0, 24.0);
                // the plain SVF low-pass, wide open and clean, is transparent: skip it (as v0.1 did)
                const bool open = fs.type == kFilterSvf && fs.fc >= sr * 0.449 && fs.res <= 0.01 && fs.morph < 1e-4 && fs.driveDb < 0.01;
                if (!open) v.filt.process(sl, sr_, fs, sr);
                // ---- level and pan
                const double g = a * velGain * sOut[i] * 0.35 * std::max(0.0, 1 + mods[kDstLevel]);
                const double pan = std::clamp(mods[kDstPan], -1.0, 1.0);
                L[i] += (float)(sl * g * (pan > 0 ? 1 - pan : 1));
                R[i] += (float)(sr_ * g * (pan < 0 ? 1 + pan : 1));
                if (i == n - 1) {
                    v.modDetune = mods[kDstDetune];
                    v.modSpread = mods[kDstSpread];
                    views[vi].pos.store((float)(pos / (table.frames - 1)), std::memory_order_relaxed);
                    views[vi].amp.store((float)a, std::memory_order_relaxed);
                    views[vi].mod.store((float)m, std::memory_order_relaxed);
                    if (v.age == newest)
                        for (int q = 0; q < kLfos; ++q) if (p.lfo[q].mode != kLfoFree && useLfo[q]) {
                            lfoView[q].store((float)src[kSrcLfo1 + q], std::memory_order_relaxed);
                            lfoPhaseView[q].store((float)v.lfo[q].phase, std::memory_order_relaxed);
                        }
                }
            }
            views[vi].note.store(v.active() ? v.note : -1, std::memory_order_relaxed);
        }
    }

    // ---- notes
    void startVoice(Voice& v, int note, double velocity, double fromPitch) {
        v.amp.v = 0; v.mod.v = 0; v.env3.v = 0;
        for (int u = 0; u < kMaxUnison; ++u) v.phase[u] = u == 0 ? 0.0 : rand01();
        v.subPhase = 0;
        v.filt.reset();
        v.pitch = fromPitch;
        v.ratio = 1;
        v.pressure = 0;
        v.modDetune = v.modSpread = 0;
        v.drift = v.driftTarget = rand01() * 2 - 1;
        v.driftCount = 0;
        v.note = note;
        v.vel = velocity;
    }
    void retrigger(Voice& v) {
        v.scan = 0;
        v.scanDir = 1;
        v.rnd = rand01() * 2 - 1;
        for (int q = 0; q < kLfos; ++q) v.lfo[q].start((uint32_t)(clock * 7 + q));
        v.amp.gate(true);
        v.mod.gate(true);
        v.env3.gate(true);
        held[v.note & 127] = true;
    }
    static void gateAll(Voice& v, bool on) { v.amp.gate(on); v.mod.gate(on); v.env3.gate(on); }
    void push(int note) { remove(note); if (depth < (int)stack.size()) stack[depth++] = note; held[note & 127] = true; }
    void remove(int note) {
        held[note & 127] = false;
        int w = 0;
        for (int r = 0; r < depth; ++r) if (stack[r] != note) stack[w++] = stack[r];
        depth = w;
    }

public:

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
    // Linear within a frame (the table is 4x oversampled) and between frames. (A Catmull-Rom curve
    // across four frames was measured: no cleaner sweeps, twice the reads.)
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

    std::atomic<const Wavetable*> current{nullptr};
    std::vector<float> combMem;  // comb-filter delay lines, 2 x kCombLen per voice (allocated once)
    HpfStep hpf;
    std::array<LfoState, kLfos> lfoG{};  // free-running LFO clocks (shared)
    std::array<int, 32> stack{};         // held notes, oldest first (mono/legato fall back to the newest)
    int depth = 0;
    double lastPitch = -1;
    double tempo = 120, beat = 0;
    bool playing = false;
    void wireCombs() { for (int i = 0; i < kVoices; ++i) voices[i].filt.comb = &combMem[(size_t)i * 2 * kCombLen]; }
    Params p;
    double sr = 48000;
    std::array<Voice, kVoices> voices{};
    std::array<bool, 128> held{};
    uint64_t clock = 0, seed = 0x9E3779B97F4A7C15ULL;
};

}  // namespace wt
}  // namespace echolab
