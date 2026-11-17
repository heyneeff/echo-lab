// TableForge: Morpheus's own wavetable library, computed from math (nothing sampled, nothing copied).
// Each recipe returns the spectra of its frames; the plugin builds a table only when it is picked.
// Families: ANALOG, DIGITAL, SPECTRAL, VOCAL, ORPHEO. Recipes are append-only: a saved Table value is an index.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "WavetableSynth.h"

namespace echolab {
namespace wt {
namespace forge {

constexpr int kFr = 32;  // frames per forged table (Position sweeps the recipe's one idea)

// frames from a time-domain shape g(phase 0..1, u 0..1 along the table)
template <class G>
inline std::vector<Spectrum> shapes(G g, int frames = kFr) {
    std::vector<Spectrum> out;
    out.reserve(frames);
    for (int f = 0; f < frames; ++f) {
        const double u = frames > 1 ? (double)f / (frames - 1) : 0;
        out.push_back(spectrumOf([&](double x) { return g(x, u); }));
    }
    return out;
}
// frames from harmonic amplitudes a(h, u), sine phase (alt = alternate signs, which keeps peaks lower)
template <class A>
inline std::vector<Spectrum> amps(A a, int frames = kFr, bool alt = false) {
    std::vector<Spectrum> out(frames);
    for (int f = 0; f < frames; ++f) {
        const double u = frames > 1 ? (double)f / (frames - 1) : 0;
        out[f].fill(0.0);
        for (int h = 1; h <= kMaxHarm; ++h) {
            const double v = a(h, u);
            if (v != 0) out[f][h] = std::complex<double>(0, -(alt && (h & 2) ? -v : v));
        }
    }
    return out;
}

constexpr double kTau = 6.283185307179586;
inline double lerp(double a, double b, double t) { return a + (b - a) * t; }
inline double gauss(double x, double w) { return std::exp(-(x * x) / (w * w)); }
inline double fold(double x) { x = 0.25 * x + 0.25; x -= std::floor(x); return 4 * std::abs(x - 0.5) - 1; }  // triangle folder, -1..1

// vowel formants (Hz): F1, F2, F3 and their relative levels; A E I O U
struct Vowel { double f[3], g[3]; };
inline const Vowel kMale[5] = {{{730, 1090, 2440}, {1, .5, .25}}, {{530, 1840, 2480}, {1, .45, .3}}, {{270, 2290, 3010}, {1, .3, .25}},
                               {{570, 840, 2410}, {1, .55, .2}}, {{300, 870, 2240}, {1, .35, .15}}};
inline const Vowel kFemale[5] = {{{850, 1220, 2810}, {1, .5, .25}}, {{610, 2330, 2990}, {1, .45, .3}}, {{310, 2790, 3310}, {1, .3, .25}},
                                 {{590, 920, 2710}, {1, .55, .2}}, {{370, 950, 2670}, {1, .35, .15}}};
// a glottal-ish source (falls ~12 dB/oct) through three formants, f0 = the pitch the formants are placed for
inline double vowelAmp(int h, double u, const Vowel* set, int n, double f0, double width = 0.18) {
    const double x = u * (n - 1);
    const int i = std::min((int)x, n - 2);
    const double t = x - i;
    const double fh = h * f0, lf = std::log2(fh);
    double g = 0.02;
    for (int k = 0; k < 3; ++k) {
        const double F = lerp(set[i].f[k], set[i + 1].f[k], t), G = lerp(set[i].g[k], set[i + 1].g[k], t);
        g += G * gauss(lf - std::log2(F), width);
    }
    return g / std::pow((double)h, 0.9);
}
inline bool isPrime(int n) { if (n < 2) return false; for (int d = 2; d * d <= n; ++d) if (n % d == 0) return false; return true; }
inline double hashRand(uint32_t x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return (x & 0xffffff) / 16777216.0; }

struct Recipe { const char* name; const char* family; std::vector<Spectrum> (*make)(); };

inline const std::vector<Recipe>& recipes() {
    static const std::vector<Recipe> r = {
        // ---------------------------------------------------------------- ANALOG
        {"SAW TO SQUARE", "ANALOG", [] { return shapes([](double t, double u) { return lerp(shape::saw(t), shape::square(t), u); }); }},
        {"SHARK FIN", "ANALOG", [] { return shapes([](double t, double u) {  // triangle whose rise grows until it is a saw
             const double w = lerp(0.5, 0.985, u); return t < w ? -1 + 2 * t / w : 1 - 2 * (t - w) / (1 - w); }); }},
        {"WARM SAW", "ANALOG", [] { return amps([](int h, double u) {  // a saw through a gently opening one-pole
             const double fc = 1.5 * std::pow(160.0, u); return 1.0 / h / std::sqrt(1 + (h / fc) * (h / fc)); }); }},
        {"TRI FOLD", "ANALOG", [] { return shapes([](double t, double u) { return fold(shape::tri(t) * (1 + 5 * u)); }); }},
        {"SQUARE SYNC", "ANALOG", [] { return shapes([](double t, double u) { const double x = t * (1 + 5 * u); return x - std::floor(x) < 0.5 ? 1.0 : -1.0; }); }},
        {"OCTAVE SAW", "ANALOG", [] { return shapes([](double t, double u) {  // a saw growing its octave-down sub
             return (shape::saw(t) * (1 - 0.5 * u) + u * shape::saw(std::fmod(2 * t, 1.0)) * 0.6); }); }},
        {"DIODE LADDER", "ANALOG", [] { return amps([](int h, double u) {  // a saw through a resonant 4-pole that sweeps up
             const double fc = 2 * std::pow(40.0, u), x = h / fc, q = 3.0;
             const double mag = 1 / std::sqrt(std::pow(1 - x * x, 2) + std::pow(x / q, 2));
             return mag / std::pow(1 + x * x, 1.5) / h; }); }},
        // ---------------------------------------------------------------- DIGITAL
        {"FM 1:1", "DIGITAL", [] { return shapes([](double t, double u) { return std::sin(kTau * t + 8 * u * std::sin(kTau * t)); }); }},
        {"FM 1:2", "DIGITAL", [] { return shapes([](double t, double u) { return std::sin(kTau * t + 6 * u * std::sin(2 * kTau * t)); }); }},
        {"FM 2:1", "DIGITAL", [] { return shapes([](double t, double u) { return std::sin(2 * kTau * t + 6 * u * std::sin(kTau * t)); }); }},
        {"FM 1:3", "DIGITAL", [] { return shapes([](double t, double u) { return std::sin(kTau * t + 5 * u * std::sin(3 * kTau * t)); }); }},
        {"FM FEEDBACK", "DIGITAL", [] { return shapes([](double t, double u) {  // y = sin(wt + b y), solved by iteration
             const double b = 1.6 * u; double y = 0; for (int k = 0; k < 9; ++k) y = std::sin(kTau * t + b * y); return y; }); }},
        {"PHASE DISTORT", "DIGITAL", [] { return shapes([](double t, double u) {  // CZ: the phase bends at a knee that moves
             const double d = lerp(0.5, 0.02, u); const double p = t < d ? 0.5 * t / d : 0.5 + 0.5 * (t - d) / (1 - d); return -std::cos(kTau * p); }); }},
        {"CZ RESONANCE", "DIGITAL", [] { return shapes([](double t, double u) { return (1 - t) * std::sin(kTau * t * (1 + 11 * u)); }); }},
        {"BITCRUSH", "DIGITAL", [] { return shapes([](double t, double u) {
             const double lv = std::round(lerp(48, 2, std::pow(u, 0.6))); return std::round(std::sin(kTau * t) * lv) / lv; }); }},
        {"SAMPLE CRUSH", "DIGITAL", [] { return shapes([](double t, double u) {
             const double n = std::round(lerp(96, 4, std::pow(u, 0.5))); return shape::saw(std::floor(t * n) / n); }); }},
        {"SINE FOLD", "DIGITAL", [] { return shapes([](double t, double u) { return std::sin(std::sin(kTau * t) * (1 + 7 * u) * 1.5707963); }); }},
        {"OVERDRIVE", "DIGITAL", [] { return shapes([](double t, double u) { const double g = 1 + 24 * u * u; return std::tanh(g * std::sin(kTau * t)) / std::tanh(g); }); }},
        // ---------------------------------------------------------------- SPECTRAL
        {"ODD TO EVEN", "SPECTRAL", [] { return amps([](int h, double u) { return (h == 1 ? 1.0 : (h % 2 ? 1 - u : u)) / h; }); }},
        {"HARMONIC SWEEP", "SPECTRAL", [] { return amps([](int h, double u) {  // one bright band climbing the series
             return 0.25 / h + gauss(std::log2((double)h) - 6 * u, 0.35); }, kFr, true); }},
        {"MOVING COMB", "SPECTRAL", [] { return amps([](int h, double u) { return (0.5 + 0.5 * std::cos(kTau * h * u * 0.25)) / h; }); }},
        {"DRAWBARS", "SPECTRAL", [] { return amps([](int h, double u) {  // organ registrations, flute to full
             static const int hs[] = {1, 2, 3, 4, 6, 8, 10, 12, 16};
             for (int k = 0; k < 9; ++k) if (h == hs[k]) { const double on = std::clamp(u * 9 - k + 1, 0.0, 1.0); return (k == 0 ? 1.0 : 0.8) * on; }
             return 0.0; }); }},
        {"GLASS", "SPECTRAL", [] { return amps([](int h, double u) {  // square-number partials: hollow, bell-like
             const int r = (int)std::lround(std::sqrt((double)h)); return r * r == h ? std::pow((double)r, -lerp(2.0, 0.6, u)) : 0.0; }); }},
        {"SPECTRAL TILT", "SPECTRAL", [] { return amps([](int h, double u) { return std::pow((double)h, -lerp(2.0, 0.35, u)); }, kFr, true); }},
        {"PRIMES", "SPECTRAL", [] { return amps([](int h, double u) { const int top = 3 + (int)(u * 120); return h == 1 ? 1.0 : (isPrime(h) && h <= top ? 0.9 / std::sqrt((double)h) : 0.0); }); }},
        {"GOLDEN", "SPECTRAL", [] { return amps([](int h, double u) {  // Fibonacci partials
             int a = 1, b = 2, k = 0; while (a < h) { const int c = a + b; a = b; b = c; ++k; }
             return a == h || h == 1 ? std::pow(0.82, k) * (k <= 2 + u * 12 ? 1.0 : 0.0) : 0.0; }); }},
        {"DIGITAL DUST", "SPECTRAL", [] { return amps([](int h, double u) {  // seeded random partials, more of them along the table
             const int top = 4 + (int)(std::pow(u, 1.5) * 200); return h <= top ? hashRand(h * 7919u + 17) / std::sqrt((double)h) : 0.0; }, kFr, true); }},
        // ---------------------------------------------------------------- VOCAL
        {"VOWELS LOW", "VOCAL", [] { return amps([](int h, double u) { return vowelAmp(h, u, kMale, 5, 110); }); }},
        {"VOWELS HIGH", "VOCAL", [] { return amps([](int h, double u) { return vowelAmp(h, u, kFemale, 5, 220); }); }},
        {"CHOIR", "VOCAL", [] { return amps([](int h, double u) {  // ooh -> aah, soft and breathy
             static const Vowel s[2] = {{{300, 870, 2240}, {1, .35, .15}}, {{730, 1090, 2440}, {1, .5, .25}}};
             return vowelAmp(h, u, s, 2, 165, 0.26) + 0.01 / h; }); }},
        {"TALKBOX", "VOCAL", [] { return amps([](int h, double u) {  // a diphthong: yah - yow - yee
             static const Vowel s[3] = {{{730, 1090, 2440}, {1, .5, .25}}, {{570, 840, 2410}, {1, .55, .2}}, {{270, 2290, 3010}, {1, .3, .25}}};
             return vowelAmp(h, u, s, 3, 130, 0.14) * std::pow((double)h, 0.35); }); }},
        // ---------------------------------------------------------------- ORPHEO
        {"LYRE", "ORPHEO", [] { return amps([](int h, double u) {  // a plucked string: the pluck point slides toward the bridge
             const double p = lerp(0.5, 0.03, u); return std::abs(std::sin(3.14159265 * h * p)) / std::pow((double)h, 1.6); }); }},
        {"UNDERWORLD", "ORPHEO", [] { return amps([](int h, double u) {  // dark, with odd grit rising from below
             return (h <= 3 ? 1.0 / h : 0.0) + (h % 2 ? u * 0.6 / std::pow((double)h, 0.8) * gauss(std::log2((double)h) - 3.5, 1.2) : 0.0); }); }},
        {"ELYSIUM", "ORPHEO", [] { return amps([](int h, double u) {  // a sine fundamental with an airy upper choir fading in
             return h == 1 ? 1.0 : u * 0.35 * gauss(std::log2((double)h) - 4.5, 0.7) / std::sqrt((double)h); }, kFr, true); }},
        {"STYX", "ORPHEO", [] { return shapes([](double t, double u) {  // FM 1:2 that folds over itself
             return fold(std::sin(kTau * t + 3 * u * std::sin(2 * kTau * t)) * (1 + 3 * u)); }); }},
        {"NOCTURNE", "ORPHEO", [] { return amps([](int h, double u) {  // a soft square that closes into an "o"
             const double sq = h % 2 ? 1.0 / (double(h) * h) * 3 : 0.0;
             return lerp(sq, vowelAmp(h, 0.0, kMale + 3, 2, 110) * 2, u); }); }},
    };
    return r;
}
inline int count() { return (int)recipes().size(); }

}  // namespace forge
}  // namespace wt
}  // namespace echolab
