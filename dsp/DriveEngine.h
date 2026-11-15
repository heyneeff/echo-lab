// DriveEngine: echo-lab's saturation core (for the drive plugin, and later Echo Lab's record head).
//
//   in -> pre-emphasis (shelf) -> drive gain -> [4x oversampled: asymmetric soft clipper w/ ceiling]
//      -> de-emphasis (inverse shelf) -> tone (low cut / tilt / high cut) -> polarity -> output gain
//
// Pre/de-emphasis around the clipper is the tape trick: highs are pushed into the clipper harder,
// then restored, so distortion has "memory" (hysteresis) and thick, frequency-dependent grit.
// A "voice" = one circuit character (measured profile). Plain C++17, no dependencies.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace echolab {

// RBJ biquad (shelves, peaks, HP/LP), direct form II transposed.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    enum Type { LowShelf, HighShelf, Peak, HighPass, LowPass };
    void set(Type t, double f, double q, double gainDb, double sr) {
        f = std::clamp(f, 5.0, sr * 0.49);
        double A = std::pow(10.0, gainDb / 40.0), w = 2 * 3.14159265358979 * f / sr;
        double cs = std::cos(w), sn = std::sin(w), al = sn / (2 * q), sq = 2 * std::sqrt(A) * al;
        double B0, B1, B2, A0, A1, A2;
        switch (t) {
            case LowShelf:
                B0 = A * ((A + 1) - (A - 1) * cs + sq); B1 = 2 * A * ((A - 1) - (A + 1) * cs);
                B2 = A * ((A + 1) - (A - 1) * cs - sq); A0 = (A + 1) + (A - 1) * cs + sq;
                A1 = -2 * ((A - 1) + (A + 1) * cs); A2 = (A + 1) + (A - 1) * cs - sq; break;
            case HighShelf:
                B0 = A * ((A + 1) + (A - 1) * cs + sq); B1 = -2 * A * ((A - 1) + (A + 1) * cs);
                B2 = A * ((A + 1) + (A - 1) * cs - sq); A0 = (A + 1) - (A - 1) * cs + sq;
                A1 = 2 * ((A - 1) - (A + 1) * cs); A2 = (A + 1) - (A - 1) * cs - sq; break;
            case Peak:
                B0 = 1 + al * A; B1 = -2 * cs; B2 = 1 - al * A; A0 = 1 + al / A; A1 = -2 * cs; A2 = 1 - al / A; break;
            case HighPass:
                B0 = (1 + cs) / 2; B1 = -(1 + cs); B2 = (1 + cs) / 2; A0 = 1 + al; A1 = -2 * cs; A2 = 1 - al; break;
            default:
                B0 = (1 - cs) / 2; B1 = 1 - cs; B2 = (1 - cs) / 2; A0 = 1 + al; A1 = -2 * cs; A2 = 1 - al; break;
        }
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }
    void flat() { b0 = 1; b1 = b2 = a1 = a2 = 0; }
    double tick(double x) { double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    void reset() { z1 = z2 = 0; }
};

// 4x oversampler: windowed-sinc polyphase FIR (up) and FIR (down). ~90 dB alias rejection.
class Oversampler4x {
public:
    static constexpr int kFactor = 4, kTaps = 64;
    Oversampler4x() {
        for (int i = 0; i < kTaps; ++i) {
            double n = i - (kTaps - 1) / 2.0, x = n / kFactor;
            double sinc = std::abs(x) < 1e-9 ? 1.0 : std::sin(3.14159265358979 * x * 0.92) / (3.14159265358979 * x);
            double w = 0.42 - 0.5 * std::cos(2 * 3.14159265358979 * i / (kTaps - 1)) +
                       0.08 * std::cos(4 * 3.14159265358979 * i / (kTaps - 1));  // Blackman
            h[i] = sinc * w * 0.92;
        }
        double s = 0; for (double v : h) s += v;
        for (double& v : h) v *= kFactor / s;  // unity DC gain after zero-stuffing
        reset();
    }
    void reset() { up.fill(0); dn.fill(0); upPos = dnPos = 0; }
    // in: one sample at base rate -> out[4] at 4x rate
    void upsample(double x, double* out) {
        for (int p = 0; p < kFactor; ++p) {
            push(up, upPos, p == 0 ? x : 0.0);
            out[p] = conv(up, upPos);
        }
    }
    // in[4] at 4x rate -> one sample at base rate
    double downsample(const double* in) {
        double y = 0;
        for (int p = 0; p < kFactor; ++p) { push(dn, dnPos, in[p]); if (p == kFactor - 1) y = conv(dn, dnPos) / kFactor; }
        return y;
    }
    static constexpr int latency() { return (kTaps - 1) / kFactor; }  // ~base-rate samples (up+down)

private:
    std::array<double, kTaps> h{}, up{}, dn{};
    int upPos = 0, dnPos = 0;
    static void push(std::array<double, kTaps>& b, int& pos, double x) { pos = (pos + kTaps - 1) % kTaps; b[pos] = x; }
    double conv(const std::array<double, kTaps>& b, int pos) const {
        double s = 0;
        for (int i = 0; i < kTaps; ++i) s += h[i] * b[(pos + i) % kTaps];
        return s;
    }
};

// One circuit character. Fitted from measurements (measure/fit_drive.py) or voiced by ear.
struct DriveVoice {
    const char* name = "Clean";
    double gainPerDriveDb = 3.3;  // dB of input gain per Drive step (0..10)
    double baseGainDb = -10.0;    // input gain at Drive 0
    double ceiling = 0.33;        // clipper output ceiling (linear, at the clipper)
    double knee = 0.6;            // 0 = very soft (tanh-like) .. 0.95 = nearly hard clip
    double asym = 0.1;            // positive/negative ceiling imbalance (even harmonics)
    double bias = 0.05;           // DC offset into the clipper (more even harmonics as level drops)
    double emphHz = 3000, emphDb = 0;   // pre-emphasis high shelf (de-emphasis = inverse)
    double lowHz = 10, highHz = 22000;  // voice band limits (post)
    double tiltDb = 0;                  // post tilt around 1 kHz (+ = brighter)
    bool invert = false;                // polarity
    double outDb = 0;
    double punishDb = 20;               // extra drive when Punish is on
};

struct DriveParams {
    double drive = 5;           // 0..10
    bool punish = false;
    double lowCutHz = 0, highCutHz = 0;  // 0 = off
    double toneDb = 0;          // -12..+12 tilt
    double mix = 1.0;
    double outputDb = 0;
    bool autoGain = true;       // keep loudness roughly constant as Drive changes
};

class DriveEngine {
public:
    void prepare(double sampleRate) { sr = sampleRate; setVoice(voice); }
    void reset() { for (auto& c : ch) c.reset(); }

    void setVoice(const DriveVoice& v) {
        voice = v;
        for (auto& c : ch) {
            c.pre.set(Biquad::HighShelf, v.emphHz, 0.7, v.emphDb, sr);
            c.de.set(Biquad::HighShelf, v.emphHz, 0.7, -v.emphDb, sr);
            c.vlo.set(Biquad::HighPass, std::max(v.lowHz, 5.0), 0.707, 0, sr);
            c.vhi.set(Biquad::LowPass, std::min(v.highHz, sr * 0.45), 0.707, 0, sr);
        }
        update(params);
    }

    void update(const DriveParams& p) {
        params = p;
        for (auto& c : ch) {
            if (p.lowCutHz > 0) c.ulo.set(Biquad::HighPass, p.lowCutHz, 0.707, 0, sr); else c.ulo.flat();
            if (p.highCutHz > 0) c.uhi.set(Biquad::LowPass, p.highCutHz, 0.707, 0, sr); else c.uhi.flat();
            double tilt = voice.tiltDb + p.toneDb;
            if (std::abs(tilt) > 0.05) {
                c.tiltLo.set(Biquad::LowShelf, 1000, 0.5, -tilt / 2, sr);
                c.tiltHi.set(Biquad::HighShelf, 1000, 0.5, tilt / 2, sr);
            } else { c.tiltLo.flat(); c.tiltHi.flat(); }
        }
        inGain = std::pow(10.0, (voice.baseGainDb + voice.gainPerDriveDb * p.drive +
                                 (p.punish ? voice.punishDb : 0)) / 20.0);
        // auto gain: undo roughly the drive's gain change (small signals stay at similar level)
        double comp = p.autoGain ? -(voice.baseGainDb + voice.gainPerDriveDb * p.drive) * 0.75 : 0.0;
        outGain = std::pow(10.0, (voice.outDb + p.outputDb + comp) / 20.0) * (voice.invert ? -1 : 1);
    }

    // The clipper: soft knee into an asymmetric ceiling (+ceil*(1+asym), -ceil*(1-asym)).
    double clip(double x) const {
        x += voice.bias;
        double c = x >= 0 ? voice.ceiling * (1 + voice.asym) : voice.ceiling * (1 - voice.asym);
        double a = std::abs(x), k = std::clamp(voice.knee, 0.0, 0.97) * c;
        double y = a <= k ? a : k + (c - k) * std::tanh((a - k) / std::max(c - k, 1e-6));
        y = std::copysign(y, x);
        double b = voice.bias, cb = b >= 0 ? voice.ceiling * (1 + voice.asym) : voice.ceiling * (1 - voice.asym);
        double kb = std::clamp(voice.knee, 0.0, 0.97) * cb, ab = std::abs(b);
        double yb = ab <= kb ? ab : kb + (cb - kb) * std::tanh((ab - kb) / std::max(cb - kb, 1e-6));
        return y - std::copysign(yb, b);  // remove the bias offset (no DC out)
    }

    void process(float* L, float* R, int n) {
        for (int i = 0; i < n; ++i) {
            for (int k = 0; k < 2; ++k) {
                float* buf = k == 0 ? L : R;
                if (!buf) continue;
                auto& c = ch[k];
                double dry = buf[i];
                double x = c.pre.tick(dry) * inGain;
                double up[4], dn[4];
                c.os.upsample(x, up);
                for (int j = 0; j < 4; ++j) dn[j] = clip(up[j]);
                double y = c.os.downsample(dn);
                y = c.de.tick(y);
                y = c.vlo.tick(y); y = c.vhi.tick(y);
                y = c.tiltLo.tick(y); y = c.tiltHi.tick(y);
                y = c.ulo.tick(y); y = c.uhi.tick(y);
                y *= outGain;
                // dry is delayed by the oversampler latency so Mix doesn't comb-filter
                c.dly[c.dp] = dry; c.dp = (c.dp + 1) % kDly;
                double dd = c.dly[(c.dp + kDly - Oversampler4x::latency()) % kDly];
                buf[i] = (float)(dd * (1 - params.mix) + y * params.mix);
            }
        }
    }

    DriveVoice voice;
    DriveParams params;

private:
    static constexpr int kDly = 64;
    struct Ch {
        Biquad pre, de, vlo, vhi, tiltLo, tiltHi, ulo, uhi;
        Oversampler4x os;
        std::array<double, kDly> dly{};
        int dp = 0;
        void reset() { pre.reset(); de.reset(); vlo.reset(); vhi.reset(); tiltLo.reset(); tiltHi.reset();
                       ulo.reset(); uhi.reset(); os.reset(); dly.fill(0); dp = 0; }
    };
    std::array<Ch, 2> ch;
    double sr = 44100, inGain = 1, outGain = 1;
};

}  // namespace echolab
