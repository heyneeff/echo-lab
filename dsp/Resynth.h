// Resynth: grow a 64-frame wavetable from any sound.
//
//   pitched sound (voice, synth, bass, strings):
//     find the pitch (YIN), then at 64 points across the sound measure each harmonic's amplitude and
//     phase over four periods (tracking the local pitch, so vibrato and glides stay clean), and
//     time-align every frame so its fundamental starts in sine phase. Then every frame keeps its own
//     harmonic levels but shares one set of phases, so sweeping through the table never crackles.
//   unpitched sound (drums, noise, textures):
//     slice raw 2048-sample cycles at 64 points (the sound's own grain becomes the table).
//
// Silence at the ends is trimmed. Offline (not real-time): run it off the audio thread.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#include "WavetableSynth.h"

namespace echolab {
namespace wt {

constexpr int kFrames = 64;

struct ResynthInfo {
    bool pitched = false;
    double f0 = 0;             // Hz, when pitched
    double seconds = 0;        // length of the sound after trimming
};

// YIN (cumulative mean normalized difference) on x[0 .. W + tmax]. Returns the period in samples;
// clarity is 1 - the dip depth (1 = perfectly periodic).
inline double yinPeriod(const float* x, int W, int tmin, int tmax, double& clarity) {
    std::vector<double> d(tmax + 2, 0.0), c(tmax + 2, 1.0);
    for (int t = 1; t <= tmax + 1; ++t) {
        double s = 0;
        for (int j = 0; j < W; ++j) { const double v = x[j] - x[j + t]; s += v * v; }
        d[t] = s;
    }
    double run = 0;
    for (int t = 1; t <= tmax + 1; ++t) { run += d[t]; c[t] = run > 0 ? d[t] * t / run : 1; }
    int best = -1;
    for (int t = std::max(tmin, 2); t <= tmax; ++t)
        if (c[t] < 0.15) { while (t + 1 <= tmax && c[t + 1] < c[t]) ++t; best = t; break; }
    if (best < 0) { best = std::max(tmin, 2); for (int t = best; t <= tmax; ++t) if (c[t] < c[best]) best = t; }
    clarity = 1 - c[best];
    const double a = c[best - 1], b = c[best], e = c[best + 1], den = a - 2 * b + e;
    return best + (std::abs(den) > 1e-12 ? std::clamp(0.5 * (a - e) / den, -0.5, 0.5) : 0.0);
}

// How to grow: AUTO picks pitched or raw by itself; SPECTRAL keeps only the smoothed tone colour of each moment
// (glassy, blurred: pads); FORMANT lays each moment's vowel/body envelope over a bright saw (voices, talkboxes).
// smooth 0..1: how much the tone is evened out from frame to frame (0 = every detail, 1 = slow morphs).
enum GrowMode { kGrowAuto = 0, kGrowPitched, kGrowRaw, kGrowSpectral, kGrowFormant, kNumGrowModes };
struct GrowOptions { int mode = kGrowAuto; double smooth = 0.2; };

// one moment's magnitude spectrum (Hann-windowed 4096-point frames averaged over [c0, c1)), bins 0..2048
inline std::vector<double> momentSpectrum(const float* x, int n, int c0, int c1) {
    constexpr int N = 4096;
    std::vector<double> mag(N / 2 + 1, 0.0);
    std::vector<std::complex<double>> buf(N);
    int count = 0;
    for (int c = c0; c < std::max(c0 + 1, c1); c += N / 2) {
        for (int j = 0; j < N; ++j) { const int i = c + j; const double w = 0.5 - 0.5 * std::cos(2 * kPi * (j + 0.5) / N); buf[j] = (i >= 0 && i < n) ? x[i] * w : 0.0; }
        fft(buf);
        for (int b = 0; b <= N / 2; ++b) mag[b] += std::abs(buf[b]);
        ++count;
        if (count >= 8) break;
    }
    for (auto& m : mag) m /= std::max(1, count) * (N / 4.0);
    return mag;
}

inline std::vector<Spectrum> resynthesize(const float* x, int n, double sr, ResynthInfo& info, GrowOptions opt = {}) {
    std::vector<Spectrum> out(kFrames);
    for (auto& s : out) s.fill(0.0);
    info = {};
    if (n < 64) return out;

    // ---- trim: 10 ms blocks above -40 dB of the loudest block
    const int blk = std::max(1, (int)(sr * 0.01));
    std::vector<double> rms;
    for (int i = 0; i < n; i += blk) {
        double s = 0; const int m = std::min(blk, n - i);
        for (int j = 0; j < m; ++j) s += (double)x[i + j] * x[i + j];
        rms.push_back(std::sqrt(s / m));
    }
    const double top = *std::max_element(rms.begin(), rms.end());
    int b0 = 0, b1 = (int)rms.size() - 1;
    while (b0 < b1 && rms[b0] < top * 0.01) ++b0;
    while (b1 > b0 && rms[b1] < top * 0.01) --b1;
    int start = b0 * blk, end = std::min(n, (b1 + 1) * blk);
    if (end - start < 4096) { start = 0; end = n; }
    const int len = end - start;
    info.seconds = len / sr;

    // ---- pitch: YIN at 24 points; pitched when enough of them are clearly periodic
    const int tmin = std::max(2, (int)(sr / 1500)), tmax = (int)(sr / 40), W = tmax;
    std::vector<double> periods;
    if (len > W + tmax + 2) {
        const int span = len - (W + tmax + 2);
        for (int k = 0; k < 24; ++k) {
            const int at = start + (int)((double)span * k / 23);
            double cl; const double p = yinPeriod(x + at, W, tmin, tmax, cl);
            if (cl > 0.8) periods.push_back(p);
        }
    }
    if ((int)periods.size() >= (opt.mode == kGrowPitched ? 3 : 8)) {
        std::nth_element(periods.begin(), periods.begin() + periods.size() / 2, periods.end());
        info.pitched = true;
        info.f0 = sr / periods[periods.size() / 2];
    }
    if (opt.mode == kGrowRaw) info.pitched = false;

    if (opt.mode == kGrowSpectral || opt.mode == kGrowFormant) {
        // each frame: the averaged magnitude spectrum of its stretch of the sound
        const double binHz = sr / 4096;
        const double spacing = (double)len / kFrames;
        for (int k = 0; k < kFrames; ++k) {
            const int c0 = start + (int)(spacing * k) - 2048, c1 = start + (int)(spacing * (k + 1)) - 2048;
            const auto mag = momentSpectrum(x, n, c0, c1);
            if (opt.mode == kGrowSpectral) {
                // bins become harmonics directly (the sound's own spectrum, as one cycle): the smeared colour of that moment
                for (int h = 1; h <= std::min(kMaxHarm, 2047); ++h) out[k][h] = std::complex<double>(0, -mag[h * 2 < 2048 ? h * 2 : 2048]);
            } else {
                // the envelope (smoothed over ~1/3 octave), laid over a bright saw at 110 Hz
                auto env = [&](double hz) {
                    const double b = hz / binHz; if (b >= 2047) return 0.0;
                    const double w = std::max(1.0, b * 0.12); double s = 0, ws = 0;
                    for (int j = std::max(1, (int)(b - 2 * w)); j <= std::min(2047, (int)(b + 2 * w)); ++j) { const double g = std::exp(-0.5 * std::pow((j - b) / w, 2)); s += g * mag[j]; ws += g; }
                    return ws > 0 ? s / ws : 0.0;
                };
                for (int h = 1; h <= kMaxHarm; ++h) { const double hz = h * 110.0; if (hz > 0.45 * sr) break; out[k][h] = std::complex<double>(0, -env(hz) / std::sqrt((double)h)); }
            }
        }
        info.pitched = false;
    } else if (info.pitched) {
        const double P = sr / info.f0;
        // analysis window: at least four periods, and long enough to cover the stretch between
        // neighbouring frames (averages out breath and noise), capped so glides stay sharp
        const double spacing = (double)len / kFrames;
        const int L = (int)std::round(std::clamp(2 * spacing, 4 * P, 16 * P));
        const int margin = std::min(L / 2 + (int)(1.2 * P) + 2, std::max(1, len / 2 - 1));  // room for the window + pitch search
        const int a0 = start + margin, a1 = std::max(a0, end - margin);
        std::vector<double> w(L);
        double wsum = 0;
        for (int j = 0; j < L; ++j) { w[j] = 0.5 - 0.5 * std::cos(2 * kPi * (j + 0.5) / L); wsum += w[j]; }
        for (int k = 0; k < kFrames; ++k) {
            const int c = a0 + (int)((double)(a1 - a0) * k / (kFrames - 1));
            if (c - margin < 0 || c + margin > n) continue;
            // local pitch: search near the global period, keep the global one if unclear
            double Pl = P, cl;
            const int lo = std::max(2, (int)(P * 0.9)), hi = (int)std::ceil(P * 1.1), Wl = (int)std::ceil(2 * P);
            if (c - Wl / 2 >= 0 && c - Wl / 2 + Wl + hi + 2 <= n) {
                const double p = yinPeriod(x + c - Wl / 2, Wl, lo, hi, cl);
                if (cl > 0.7) Pl = p;
            }
            const double f0l = sr / Pl;
            const int H = std::min(kMaxHarm, (int)(0.5 * sr / f0l * 0.98));
            const float* seg = x + c - L / 2;
            for (int h = 1; h <= H; ++h) {
                const double om = 2 * kPi * h * f0l / sr;
                const std::complex<double> step(std::cos(om), -std::sin(om));
                std::complex<double> rot = std::polar(1.0, om * (L / 2.0)), acc = 0;  // phase measured at the window centre
                for (int j = 0; j < L; ++j) { acc += rot * (seg[j] * w[j]); rot *= step; }
                out[k][h] = acc * (2.0 / wsum);
            }
            // time-align: fundamental in sine phase, every harmonic shifted by the same time
            if (std::abs(out[k][1]) > 1e-12) {
                const double shift = -kPi / 2 - std::arg(out[k][1]);
                for (int h = 1; h <= H; ++h) out[k][h] *= std::polar(1.0, h * shift);
            }
        }
    } else {
        const int N = 2048;                               // raw cycles
        const int a0 = start, a1 = std::max(start, end - N);
        std::vector<std::complex<double>> buf(N);
        for (int k = 0; k < kFrames; ++k) {
            const int c = a0 + (int)((double)(a1 - a0) * k / (kFrames - 1));
            for (int j = 0; j < N; ++j) buf[j] = (c + j < n) ? x[c + j] : 0.0f;
            fft(buf);  // e^{+i}: for real input, conj gives the analysis spectrum
            for (int h = 1; h <= std::min(kMaxHarm, N / 2 - 1); ++h) out[k][h] = std::conj(buf[h]) * (2.0 / N);
        }
    }

    // a frame that came out silent (gap in the sound) borrows its neighbour
    auto energy = [&](int k) { double e = 0; for (int h = 1; h <= kMaxHarm; ++h) e += std::norm(out[k][h]); return e; };
    double emax = 0;
    for (int k = 0; k < kFrames; ++k) emax = std::max(emax, energy(k));
    for (int k = 1; k < kFrames; ++k) if (energy(k) < emax * 1e-8) out[k] = out[k - 1];
    for (int k = kFrames - 2; k >= 0; --k) if (energy(k) < emax * 1e-8) out[k] = out[k + 1];
    // Phase coherence. Each frame keeps its own harmonic *levels* (the tone colour), but every frame
    // takes its harmonic *phases* from one reference frame (the loudest). Otherwise vibrato, breath
    // and measurement noise give each frame slightly different phases, and sweeping Position spins
    // them around at random, which you hear as static. Levels are also smoothed lightly across
    // neighbouring frames so measurement noise doesn't jitter the tone.
    int ref = 0;
    for (int k = 1; k < kFrames; ++k) if (energy(k) > energy(ref)) ref = k;
    std::vector<std::array<double, kMaxHarm + 1>> mag(kFrames);
    for (int k = 0; k < kFrames; ++k) for (int h = 0; h <= kMaxHarm; ++h) mag[k][h] = std::abs(out[k][h]);
    std::array<std::complex<double>, kMaxHarm + 1> unit;
    for (int h = 0; h <= kMaxHarm; ++h) unit[h] = std::abs(out[ref][h]) > 1e-15 ? out[ref][h] / std::abs(out[ref][h]) : std::complex<double>(0, -1);
    // smoothness: 0 = no smoothing, 0.2 = the classic light 5-tap pass, 1 = six passes (slow, glassy morphs)
    const int passes = (int)std::lround(std::clamp(opt.smooth, 0.0, 1.0) * 6);
    for (int pass = 0; pass < passes; ++pass) {
        auto src = mag;
        for (int k = 0; k < kFrames; ++k) {
            const int i0 = std::max(0, k - 2), i1 = std::max(0, k - 1), i3 = std::min(kFrames - 1, k + 1), i4 = std::min(kFrames - 1, k + 2);
            for (int h = 1; h <= kMaxHarm; ++h) mag[k][h] = (src[i0][h] + 4 * src[i1][h] + 6 * src[k][h] + 4 * src[i3][h] + src[i4][h]) / 16;
        }
    }
    for (int k = 0; k < kFrames; ++k)
        for (int h = 1; h <= kMaxHarm; ++h) out[k][h] = unit[h] * mag[k][h];
    return out;
}

}  // namespace wt
}  // namespace echolab
