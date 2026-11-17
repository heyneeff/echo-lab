// Morpheus FX rack: our own plugins inside the synth, in any order.
//   Prometheus (DriveEngine) · Chorus (Juno I / II / I+II, Solina Ensemble) · Echo Nymph (EchoEngine)
//   · Letheverb (ReverbEngine) · Charon (PumpEngine, with the sidechain input)
// The engines are the same code as the standalone plugins. An FX that is off is skipped (bit-exact dry).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "DriveVoices.h"
#include "EchoEngine.h"
#include "PumpEngine.h"
#include "ReverbEngine.h"
#include "Styles.h"

namespace echolab {
namespace fx {

constexpr double kPi = 3.14159265358979323846;

// ------------------------------------------------------------------ chorus (BBD style)
// Juno-60 chorus: one bucket-brigade line, the left output reads it with a triangle LFO and the right
// with the inverted LFO (delay ~1.7-5.3 ms). I = 0.513 Hz, II = 0.863 Hz, I+II = 9.75 Hz and shallow.
// Ensemble: Solina-style three taps 120 degrees apart, a slow and a fast LFO each, for strings.
enum ChorusMode { kChorusI = 0, kChorusII, kChorusI_II, kEnsemble };
inline const char* const kChorusNames[] = {"Juno I", "Juno II", "Juno I+II", "Ensemble"};

class ChorusEngine {
public:
    void prepare(double sampleRate) {
        sr = sampleRate;
        int n = 1;
        while (n < (int)(0.03 * sr) + 8) n <<= 1;
        buf.assign(n, 0.0);
        mask = n - 1;
        reset();
    }
    void reset() { std::fill(buf.begin(), buf.end(), 0.0); w = 0; ph = 0; phFast = 0; lp[0] = lp[1] = 0; }
    void set(int m, double mixAmt) { mode = m; mix = std::clamp(mixAmt, 0.0, 1.0); }
    void process(float* L, float* R, int n) {
        const double rate = mode == kChorusI ? 0.513 : mode == kChorusII ? 0.863 : mode == kChorusI_II ? 9.75 : 0.6;
        const double centre = (mode == kEnsemble ? 6.0 : 3.5) * 0.001 * sr;
        const double depth = (mode == kChorusI_II ? 0.35 : mode == kEnsemble ? 2.5 : 1.85) * 0.001 * sr;
        const double lpK = 1 - std::exp(-2 * kPi * 9000 / sr);  // the BBD's darker top
        for (int i = 0; i < n; ++i) {
            const double dry = 0.5 * ((double)L[i] + R[i]);
            buf[w] = dry;
            ph += rate / sr; if (ph >= 1) ph -= 1;
            double wl, wr;
            if (mode == kEnsemble) {
                phFast += 6.0 / sr; if (phFast >= 1) phFast -= 1;
                double t[3];
                for (int k = 0; k < 3; ++k) {
                    const double s = std::sin(2 * kPi * (ph + k / 3.0)), f = std::sin(2 * kPi * (phFast + k / 3.0));
                    t[k] = read(centre + depth * s + 0.35 * 0.001 * sr * f);
                }
                wl = 0.5 * (t[0] + t[1]); wr = 0.5 * (t[1] + t[2]);
            } else {
                const double tri = ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph;  // -1..1
                wl = read(centre + depth * tri); wr = read(centre - depth * tri);
            }
            lp[0] += (wl - lp[0]) * lpK; lp[1] += (wr - lp[1]) * lpK;
            wl = std::tanh(lp[0] * 1.2) / 1.2; wr = std::tanh(lp[1] * 1.2) / 1.2;
            L[i] = (float)(L[i] * (1 - 0.5 * mix) + wl * mix * 0.8);
            R[i] = (float)(R[i] * (1 - 0.5 * mix) + wr * mix * 0.8);
            w = (w + 1) & mask;
        }
    }

private:
    double read(double d) const {  // d samples behind the newest, linear
        const double pos = w - d;
        const int i0 = (int)std::floor(pos);
        const double t = pos - i0;
        const double a = buf[i0 & mask], b = buf[(i0 + 1) & mask];
        return a + (b - a) * t;
    }
    std::vector<double> buf;
    int mask = 0, w = 0, mode = kChorusI;
    double sr = 48000, ph = 0, phFast = 0, mix = 0.5, lp[2]{};
};

// ------------------------------------------------------------------ the rack
enum FxId { kFxDrive = 0, kFxChorus, kFxEcho, kFxReverb, kFxPump, kNumFx };
inline const char* const kFxNames[kNumFx] = {"Prometheus", "Chorus", "Echo Nymph", "Letheverb", "Charon"};

// Echo Nymph's note values (tempo sync)
inline const char* const kEchoNoteNames[] = {"1/32", "1/16T", "1/16", "1/16D", "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2", "1/2D", "1/1"};
inline const double kEchoNoteBeats[] = {0.125, 1.0 / 6, 0.25, 0.375, 1.0 / 3, 0.5, 0.75, 2.0 / 3, 1.0, 1.5, 2.0, 3.0, 4.0};
constexpr int kEchoNotes = 13;

// The chain order as one number: a permutation of the five FX, 0..119 (0 = the order above)
inline std::array<int, kNumFx> orderFromIndex(int idx) {
    std::vector<int> pool = {0, 1, 2, 3, 4};
    std::array<int, kNumFx> out{};
    int f = 24;  // 4!
    idx = std::clamp(idx, 0, 119);
    for (int k = 0; k < kNumFx; ++k) {
        const int q = idx / f;
        idx %= f;
        out[k] = pool[q];
        pool.erase(pool.begin() + q);
        if (k < kNumFx - 1) f /= (kNumFx - 1 - k);
    }
    return out;
}
inline int indexFromOrder(const std::array<int, kNumFx>& o) {
    std::vector<int> pool = {0, 1, 2, 3, 4};
    int idx = 0, f = 24;
    for (int k = 0; k < kNumFx; ++k) {
        const int q = (int)(std::find(pool.begin(), pool.end(), o[k]) - pool.begin());
        idx += q * f;
        pool.erase(pool.begin() + q);
        if (k < kNumFx - 1) f /= (kNumFx - 1 - k);
    }
    return idx;
}

struct FxParams {
    int order = 0;  // permutation index
    // Prometheus
    double drive = 0, driveMix = 1;  // drive 0 = off
    int driveStyle = 6;              // ORPHEO
    // Chorus
    bool chorusOn = false; int chorusMode = kChorusI; double chorusMix = 0.5;
    // Echo Nymph
    bool echoOn = false, echoSync = true; int echoStyle = 7, echoMode = 1, noteL = 3, noteR = 5;
    double timeL = 188, timeR = 250, echoFb = 0.25, echoMix = 0.28;
    // Letheverb
    bool verbOn = false; int verbStyle = 6; double verbDecay = 2.8, verbSize = 1.0, verbMix = 0.25;
    // Charon
    bool pumpOn = false; int pumpMode = 0, pumpRate = 2; double pumpDepth = 0.45, pumpMix = 1;
};

// sidechain envelope follower (fast attack, slower release), for Charon and the SIDECHAIN mod source
struct Follower {
    double env = 0, a = 0, r = 0;
    void prepare(double sr, double attackMs = 2, double releaseMs = 150) { a = 1 - std::exp(-1.0 / (attackMs * 0.001 * sr)); r = 1 - std::exp(-1.0 / (releaseMs * 0.001 * sr)); }
    double run(const float* L, const float* R, int n) {
        for (int i = 0; i < n; ++i) {
            const double x = std::max(std::abs((double)L[i]), R ? std::abs((double)R[i]) : 0.0);
            env += (x - env) * (x > env ? a : r);
        }
        return env;
    }
};

class FxRack {
public:
    void prepare(double sampleRate) {
        sr = sampleRate;
        drive.prepare(sr);
        chorus.prepare(sr);
        echo.prepare(sr, 4.0);
        verb.prepare(sr);
        pump.prepare(sr);
        styleDrive = styleEcho = -1;
        update(p, tempo);
        reset();
    }
    void reset() { drive.reset(); chorus.reset(); echo.reset(); verb.reset(); pump.reset(); }

    void update(const FxParams& np, double bpm) {
        p = np;
        tempo = bpm > 20 ? bpm : 120;
        order = orderFromIndex(p.order);
        if (p.driveStyle != styleDrive) { styleDrive = std::clamp(p.driveStyle, 0, kNumDriveVoices - 1); drive.setVoice(kDriveVoices[styleDrive]); }
        DriveParams d;
        d.drive = p.drive;
        d.mix = p.driveMix;
        d.autoGain = true;
        drive.update(d);
        chorus.set(p.chorusMode, p.chorusMix);
        if (p.echoStyle != styleEcho) { styleEcho = std::clamp(p.echoStyle, 0, kNumStyles - 1); echo.setStyle(kStyles[styleEcho]); }
        Params e;
        e.timeMs[0] = p.echoSync ? kEchoNoteBeats[std::clamp(p.noteL, 0, kEchoNotes - 1)] * 60000.0 / tempo : p.timeL;
        e.timeMs[1] = p.echoSync ? kEchoNoteBeats[std::clamp(p.noteR, 0, kEchoNotes - 1)] * 60000.0 / tempo : p.timeR;
        e.timeMs[0] = std::clamp(e.timeMs[0], 5.0, 3900.0);
        e.timeMs[1] = std::clamp(e.timeMs[1], 5.0, 3900.0);
        e.feedback = p.echoFb;
        e.mix = p.echoMix;
        e.mode = p.echoMode;
        e.lowCutHz = 150;
        e.outputDb = -kStyles[styleEcho].outputDb;  // the style's whole-plugin trim would move the dry too: inside the rack dry stays at unity
        echo.updateParams(e);
        verb::Params v;
        v.decayS = p.verbDecay;
        v.size = p.verbSize;
        v.mix = p.verbMix;
        const auto& vs = verb::kVerbStyles[std::clamp(p.verbStyle, 0, 7)];
        v.outputDb = -vs.outDb;  // likewise: dry at unity
        verb.update(v, vs);
        pump::Params q;
        q.mode = p.pumpMode;
        q.rate = std::clamp(p.pumpRate, 0, pump::kNumRates - 1);
        q.depth = p.pumpDepth;
        q.mix = p.pumpMix;
        pump.update(q);
    }
    void setTempo(double bpm) { if (bpm > 20 && std::abs(bpm - tempo) > 1e-3) update(p, bpm); }
    bool anyOn() const { return p.drive > 0.005 || p.chorusOn || p.echoOn || p.verbOn || p.pumpOn; }

    void process(float* L, float* R, const float* scL, const float* scR, int n, double beat, double beatsPerSample) {
        for (int id : order) {
            switch (id) {
                case kFxDrive: if (p.drive > 0.005) drive.process(L, R, n); break;
                case kFxChorus: if (p.chorusOn) chorus.process(L, R, n); break;
                case kFxEcho: if (p.echoOn) echo.process(L, R, n); break;
                case kFxReverb: if (p.verbOn) verb.process(L, R, n); break;
                case kFxPump: if (p.pumpOn) pump.process(L, R, scL, scR, n, beat, beatsPerSample); break;
            }
        }
    }
    const std::array<int, kNumFx>& chain() const { return order; }
    double lastPumpGain() const { return pump.lastGain; }

    DriveEngine drive;
    ChorusEngine chorus;
    EchoEngine echo;
    verb::ReverbEngine verb;
    pump::PumpEngine pump;

private:
    FxParams p;
    std::array<int, kNumFx> order{0, 1, 2, 3, 4};
    double sr = 48000, tempo = 120;
    int styleDrive = -1, styleEcho = -1;
};

}  // namespace fx
}  // namespace echolab
