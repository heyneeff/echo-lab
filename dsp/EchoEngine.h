// EchoEngine: the echo-lab DSP core. Plain C++17, no framework dependencies.
//
// Signal flow per channel:
//   in ─┬──────────────────────────────────────────────────────── dry ──┐
//       └─(+)→ record-head saturation → [delay line] → read head ─┬─ playback tone ─ duck ─ mix → out
//          ↑                                  (modulated, smoothed)│
//          └── noise ← loop filters (style + user cuts) ← feedback gain
//
// Everything that makes a style sound like itself lives in the feedback loop, so each repeat
// is saturated, filtered and wobbled again: tape and BBD echoes darken and grit up over time.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace echolab {

constexpr double kPi = 3.14159265358979323846;

// ----------------------------------------------------------------------------- building blocks

// TPT one-pole (lowpass/highpass), stable under modulation.
struct OnePole {
    double g = 0, s = 0;
    void setCutoff(double hz, double sr) {
        hz = std::clamp(hz, 5.0, sr * 0.49);
        double w = std::tan(kPi * hz / sr);
        g = w / (1.0 + w);
    }
    double lp(double x) { double v = (x - s) * g; double y = v + s; s = y + v; return y; }
    double hp(double x) { return x - lp(x); }
    void reset() { s = 0; }
};

// TPT state-variable filter (12 dB/oct), resonance via q.
struct SVF {
    double g = 0, k = 1.414, a1 = 0, a2 = 0, a3 = 0, ic1 = 0, ic2 = 0;
    void set(double hz, double q, double sr) {
        hz = std::clamp(hz, 10.0, sr * 0.49);
        g = std::tan(kPi * hz / sr);
        k = 1.0 / std::max(q, 0.1);
        a1 = 1.0 / (1.0 + g * (g + k)); a2 = g * a1; a3 = g * a2;
    }
    // returns {low, band, high}
    std::array<double, 3> tick(double x) {
        double v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
        return {v2, v1, x - k * v1 - v2};
    }
    void reset() { ic1 = ic2 = 0; }
};

// Saturation with first-order antiderivative anti-aliasing (ADAA), so drive doesn't alias.
// Shape: tanh with asymmetry (bias) for even harmonics (tape), or a softer cubic (BBD).
struct Saturator {
    double drive = 1.0, bias = 0.0, xPrev = 0.0, outGain = 1.0, knee = 0.5;
    int type = 0;  // 0 = tanh (tape), 1 = cubic soft clip (BBD/transistor), 2 = clean, 3 = knee
    // knee curve: linear below the threshold k, then a tanh shoulder up to 1
    static double fk(double x, double k) {
        double a = std::abs(x);
        if (a <= k) return x;
        double r = 1.0 - k;
        return std::copysign(k + r * std::tanh((a - k) / r), x);
    }
    static double Fk(double x, double k) {
        double a = std::abs(x);
        if (a <= k) return 0.5 * x * x;
        double r = 1.0 - k;
        return k * a + r * r * std::log(std::cosh((a - k) / r)) - 0.5 * k * k;
    }
    double fx(double x) const { return type == 3 ? fk(x, knee) : f(x, type); }
    double Fx(double x) const { return type == 3 ? Fk(x, knee) : F(x, type); }
    static double F(double x, int t) {      // antiderivative of the shaper
        if (t == 0) return std::log(std::cosh(x));
        if (t == 1) {
            // ∫ f: inside |x|<1 f = x - x^3/3; outside f = ±2/3 (continuous at ±1: 5/12)
            if (x >= 1) return 2.0 * x / 3.0 - 0.25;
            if (x <= -1) return -2.0 * x / 3.0 - 0.25;
            return 0.5 * x * x - x * x * x * x / 12.0;
        }
        return 0.5 * x * x;
    }
    static double f(double x, int t) {
        if (t == 0) return std::tanh(x);
        if (t == 1) return x >= 1 ? 2.0 / 3.0 : x <= -1 ? -2.0 / 3.0 : x - x * x * x / 3.0;
        return x;
    }
    double process(double in) {
        if (type == 2) return in;
        double x = in * drive + bias;
        double y;
        if (drive < 2.0) {
            // gentle drive: aliasing is negligible, and ADAA's half-sample averaging would act as a
            // treble cut that compounds on every repeat, so use the plain curve
            y = fx(x);
        } else {
            double d = x - xPrev;
            if (std::abs(d) < 1e-6) y = fx(0.5 * (x + xPrev));
            else y = (Fx(x) - Fx(xPrev)) / d;
        }
        xPrev = x;
        return (y - fx(bias)) * outGain / std::max(drive, 1e-3);
    }
    void reset() { xPrev = bias; }
};

// Slow random walk (for wow/drift): smoothed noise in [-1, 1].
struct Wander {
    uint32_t seed = 22222;
    double target = 0, value = 0, coef = 0, counter = 0, period = 1;
    double rnd() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 8388608.0 - 1.0; }
    void set(double rateHz, double sr) { period = sr / std::max(rateHz, 0.01); coef = 1.0 - std::exp(-2.0 * kPi * rateHz / sr); }
    double tick() {
        if ((counter -= 1) <= 0) { counter = period; target = rnd(); }
        value += (target - value) * coef;
        return value;
    }
};

// Envelope follower for ducking.
struct Follower {
    double env = 0, att = 0, rel = 0;
    void set(double attackMs, double releaseMs, double sr) {
        att = std::exp(-1.0 / (0.001 * attackMs * sr));
        rel = std::exp(-1.0 / (0.001 * releaseMs * sr));
    }
    double tick(double x) { x = std::abs(x); env = x > env ? att * env + (1 - att) * x : rel * env + (1 - rel) * x; return env; }
};

// ----------------------------------------------------------------------------- style

// A style = the character of the loop. Values come from measurement (measure/profiles).
struct Style {
    const char* name = "Clean";
    // loop filters (applied every repeat)
    double loopHPHz = 20, loopLPHz = 20000;  // one-pole slopes
    double loopLP2Hz = 0;                    // optional second LP pole (steeper darkening), 0 = off
    double loopTiltDb = 0;                   // extra per-repeat loss (dB) above shelfHz (high shelf)
    double shelfHz = 3000;
    double loopPeakHz = 500, loopPeakDb = 0, loopPeakQ = 0.7;  // mid bump/dip inside the loop
    // input bandwidth (BBD/radio styles narrow the echo itself)
    double wetHPHz = 0, wetLPHz = 0;         // 0 = off
    int wetLPPoles = 1;                      // 1..4 cascaded poles (steeper = darker machine)
    double wetBrickHz = 0;                   // steep 8th-order Butterworth cliff (converter-style), 0 = off
    // saturation in the loop
    int satType = 2;                         // 0 tanh, 1 cubic, 2 clean
    double satDrive = 1.0, satBias = 0.0, satKnee = 0.5;
    // modulation
    double wowCents = 0, wowRateHz = 0.5;    // slow, random-walk
    double flutterCents = 0, flutterRateHz = 6.0;
    // noise injected in the loop
    double noiseDb = -200, noiseLPHz = 8000;
    // time changes: true = tape varispeed (pitch glides), false = digital crossfade
    bool repitch = true;
    double timeGlideMs = 120;
    double outputDb = 0;                     // makeup so styles sit at similar level
};

// ----------------------------------------------------------------------------- engine

struct Params {
    double timeMs[2] = {250, 250};  // L, R
    double feedback = 0.4;          // 0 .. 1.1 (above 1 = runaway, saturation tames it)
    double mix = 0.5;               // 0 dry .. 1 wet
    double saturationDb = 0;        // extra drive into the loop saturator
    double lowCutHz = 0, highCutHz = 0;  // user loop cuts, 0 = off
    double wobble = 1.0;            // scales the style's wow/flutter (0 = steady, 2 = seasick)
    double duck = 0;                // 0..1 how far the echo dips under the dry signal
    double duckReleaseMs = 250;
    double width = 1.0;             // 0 mono .. 1 stereo
    int mode = 0;                   // 0 single (mono-summed in), 1 dual (L/R times), 2 ping-pong
    double inputDb = 0, outputDb = 0;
};

class EchoEngine {
public:
    void prepare(double sampleRate, double maxSeconds = 4.0) {
        sr = sampleRate;
        size = 1;
        while (size < (int)(maxSeconds * sr) + 8) size <<= 1;
        mask = size - 1;
        for (auto& c : ch) { c.buf.assign(size, 0.0); c.w = 0; }
        reset();
        setStyle(style);
    }

    void reset() {
        for (int i = 0; i < 2; ++i) {
            auto& c = ch[i];
            std::fill(c.buf.begin(), c.buf.end(), 0.0);
            c.hp.reset(); c.lp.reset(); c.lp2.reset(); c.ucLow.reset(); c.ucHigh.reset();
            c.wetHP.reset(); for (auto& f : c.wetLP) f.reset(); c.noiseLP.reset(); c.peak.reset();
            for (auto& b : c.brick) b.reset(); c.sat.reset(); c.shelf.reset();
            c.delaySmoothed = params.timeMs[i] * 0.001 * sr;
            c.fbSample = 0;
            c.modOffset = 0;
            c.wander.seed = 12345u + 777u * (uint32_t)i;
        }
        flutterPhase = 0;
        noiseSeed = 99991u;
    }

    void setStyle(const Style& s) {
        style = s;
        for (auto& c : ch) {
            c.hp.setCutoff(s.loopHPHz, sr);
            c.lp.setCutoff(s.loopLPHz, sr);
            if (s.loopLP2Hz > 0) c.lp2.setCutoff(s.loopLP2Hz, sr);
            c.shelf.setCutoff(s.shelfHz, sr);
            c.peak.set(s.loopPeakHz, s.loopPeakQ, sr);
            static constexpr double kButter8Q[4] = {0.5098, 0.6013, 0.9000, 2.5629};
            for (int b = 0; b < 4; ++b) c.brick[b].set(s.wetBrickHz > 0 ? s.wetBrickHz : sr * 0.45, kButter8Q[b], sr);
            c.wetHP.setCutoff(s.wetHPHz > 0 ? s.wetHPHz : 10, sr);
            for (auto& f : c.wetLP) f.setCutoff(s.wetLPHz > 0 ? s.wetLPHz : sr * 0.45, sr);
            c.noiseLP.setCutoff(s.noiseLPHz, sr);
            c.sat.type = s.satType;
            c.sat.bias = s.satBias;
            c.sat.knee = s.satKnee;
            c.wander.set(s.wowRateHz, sr);
        }
        updateParams(params);
    }

    void updateParams(const Params& p) {
        params = p;
        double extra = std::pow(10.0, p.saturationDb / 20.0);
        for (auto& c : ch) {
            c.sat.drive = style.satDrive * extra;
            c.sat.outGain = 1.0;
            c.ucLow.setCutoff(p.lowCutHz > 0 ? p.lowCutHz : 5, sr);
            c.ucHigh.setCutoff(p.highCutHz > 0 ? p.highCutHz : sr * 0.45, sr);
        }
        duckF.set(5.0, std::max(p.duckReleaseMs, 10.0), sr);
        glideCoef = 1.0 - std::exp(-1.0 / (0.001 * std::max(style.timeGlideMs, 1.0) * sr));
        modLeak = std::exp(-1.0 / (3.0 * sr));  // 3 s leak: far below the slowest wow
    }

    // Process interleaved-free stereo buffers in place.
    void process(float* left, float* right, int n) {
        const double inG = std::pow(10.0, params.inputDb / 20.0);
        const double outG = std::pow(10.0, (params.outputDb + style.outputDb) / 20.0);
        const double shelfLoss = std::pow(10.0, -style.loopTiltDb / 20.0);
        const double noiseAmp = std::pow(10.0, style.noiseDb / 20.0);
        const double fl = 2 * kPi * style.flutterRateHz / sr;
        const double centsToRatio = std::log(2.0) / 1200.0;
        for (int i = 0; i < n; ++i) {
            double inL = left[i] * inG, inR = right ? right[i] * inG : inL;
            double dryL = left[i], dryR = right ? right[i] : left[i];
            if (params.mode == 0) { inL = inR = 0.5 * (inL + inR); }
            // ducking: the dry signal's envelope pushes the wet down
            double env = duckF.tick(0.5 * (std::abs(inL) + std::abs(inR)));
            double duckGain = 1.0 / (1.0 + params.duck * 12.0 * env);

            flutterPhase += fl;
            if (flutterPhase > 2 * kPi) flutterPhase -= 2 * kPi;
            double flut = std::sin(flutterPhase);

            double wetOut[2];
            for (int k = 0; k < 2; ++k) {
                auto& c = ch[k];
                // modulated delay length (in samples)
                double target = params.timeMs[params.mode == 1 ? k : 0] * 0.001 * sr;
                c.delaySmoothed += (target - c.delaySmoothed) * glideCoef;
                // wow/flutter as TAPE SPEED variation: the read head runs at ratio r, so the delay
                // drifts by (1 - r) samples per sample. Pitch deviation = wob cents, independent of
                // the echo time (like a real machine). A slow leak keeps the offset bounded.
                double wob = params.wobble * (style.wowCents * c.wander.tick() + style.flutterCents * flut);
                c.modOffset += 1.0 - std::exp(wob * centsToRatio);
                c.modOffset *= modLeak;
                double d = std::clamp(c.delaySmoothed + c.modOffset, 2.0, (double)size - 4);
                double y = readCubic(c, d);
                // loop: feedback gain -> filters -> noise (saturation happens at the record head)
                double fb = y * params.feedback;
                fb = c.hp.hp(fb);
                fb = c.lp.lp(fb);
                if (style.loopLP2Hz > 0) fb = c.lp2.lp(fb);
                if (style.loopPeakDb != 0) {  // bell: x + (A-1)·normalized bandpass
                    double A = std::pow(10.0, style.loopPeakDb / 20.0);
                    fb += (A - 1.0) * c.peak.k * c.peak.tick(fb)[1];
                }
                if (style.loopTiltDb != 0) { double lo = c.shelf.lp(fb); fb = lo + (fb - lo) * shelfLoss; }
                if (params.lowCutHz > 0) fb = c.ucLow.hp(fb);
                if (params.highCutHz > 0) fb = c.ucHigh.lp(fb);
                if (noiseAmp > 1e-9) fb += c.noiseLP.lp(noise()) * noiseAmp;
                c.fbSample = fb;
                // wet tone (BBD / radio bandwidth)
                double w = y;
                if (style.wetHPHz > 0) w = c.wetHP.hp(w);
                if (style.wetLPHz > 0)
                    for (int p = 0; p < std::clamp(style.wetLPPoles, 1, 4); ++p) w = c.wetLP[p].lp(w);
                if (style.wetBrickHz > 0)
                    for (auto& b : c.brick) w = b.tick(w)[0];
                wetOut[k] = w;
            }
            // write inputs + feedback (ping-pong crosses the loop)
            double fbL = ch[0].fbSample, fbR = ch[1].fbSample;
            if (params.mode == 2) std::swap(fbL, fbR);
            // record head: new input + repeats are saturated together, like tape
            ch[0].buf[ch[0].w] = ch[0].sat.process(inL + fbL);
            ch[1].buf[ch[1].w] = ch[1].sat.process((params.mode == 2 ? 0.0 : inR) + fbR);
            for (auto& c : ch) c.w = (c.w + 1) & mask;

            // width (mid/side on the wet), mix, duck
            double m = 0.5 * (wetOut[0] + wetOut[1]), s = 0.5 * (wetOut[0] - wetOut[1]) * params.width;
            double wl = (m + s) * duckGain, wr = (m - s) * duckGain;
            double dryG = std::cos(params.mix * 0.5 * kPi), wetG = std::sin(params.mix * 0.5 * kPi);
            left[i] = (float)((dryL * dryG + wl * wetG) * outG);
            if (right) right[i] = (float)((dryR * dryG + wr * wetG) * outG);
        }
    }

    Params params;
    Style style;

private:
    struct Channel {
        std::vector<double> buf;
        int w = 0;
        double delaySmoothed = 0, fbSample = 0, modOffset = 0;
        OnePole hp, lp, lp2, shelf, ucLow, ucHigh, wetHP, noiseLP;
        SVF peak;
        std::array<SVF, 4> brick;
        std::array<OnePole, 4> wetLP;
        Saturator sat;
        Wander wander;
    };

    double readCubic(const Channel& c, double delay) const {
        double pos = (double)c.w - delay;
        int i = (int)std::floor(pos);
        double t = pos - i;
        double y0 = c.buf[(i - 1) & mask], y1 = c.buf[i & mask], y2 = c.buf[(i + 1) & mask], y3 = c.buf[(i + 2) & mask];
        // 4-point Hermite
        double c0 = y1, c1 = 0.5 * (y2 - y0), c2 = y0 - 2.5 * y1 + 2 * y2 - 0.5 * y3, c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + c0;
    }

    double noise() { noiseSeed = noiseSeed * 1664525u + 1013904223u; return (noiseSeed >> 8) / 8388608.0 - 1.0; }

    double sr = 44100;
    int size = 1, mask = 0;
    std::array<Channel, 2> ch;
    Follower duckF;
    double glideCoef = 0.001, flutterPhase = 0, modLeak = 0.99999;
    uint32_t noiseSeed = 99991u;
};

}  // namespace echolab
