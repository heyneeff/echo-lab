// Morpheus SPACE: places the whole sound around, above, below, near or far, on ordinary stereo.
// The cues are the ones our hearing uses (all cheap: a delay line, one-poles, biquads, a 4-line room):
//   around   interaural time difference (Woodworth, <= 0.66 ms) + the head shadowing the far ear (level + highs)
//            and a darker tone behind the head
//   height   the pinna notch moving 6 -> 10 kHz and a high shelf (above = brighter, below = body-shadowed)
//            (headphones: full strength; speakers: about half, since speakers blur these cues)
//   distance level, air absorption (highs fall away), proximity bass up close, a narrower image, more room,
//            and the propagation delay, whose changes give a natural Doppler on fly-bys
// Orbits move the position along tempo-synced paths. Header-only, no allocation after prepare().
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

namespace echolab {
namespace space {

constexpr double kPi = 3.14159265358979323846;

enum Orbit { kStill = 0, kCircle, kRise, kFall, kSwoop, kFigure8, kDrift, kBreath, kNumOrbits };
inline const char* const kOrbitNames[kNumOrbits] = {"Still", "Circle", "Rise", "Fall", "Swoop", "Figure 8", "Drift", "Breath"};
inline const char* const kRateNames[] = {"1/2 Bar", "1 Bar", "2 Bars", "4 Bars", "8 Bars", "16 Bars"};
inline const double kRateBeats[] = {2, 4, 8, 16, 32, 64};
constexpr int kNumRates = 6;

struct Params {
    double around = 0;     // degrees, -180..180 (0 = in front, +90 = right)
    double height = 0;     // -1 (below) .. 1 (above)
    double distance = 0.2; // 0 (at your ear) .. 1 (far away)
    int orbit = kStill;
    int rate = 2;          // index into kRateBeats
    double size = 0.7;     // how far an orbit travels, 0..1
    double room = 0.5;     // how much space the distance brings, 0..1
    bool headphones = false;
};

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    double run(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    void peak(double sr, double f, double q, double db) {
        const double A = std::pow(10, db / 40), w = 2 * kPi * f / sr, al = std::sin(w) / (2 * q), c = std::cos(w), a0 = 1 + al / A;
        b0 = (1 + al * A) / a0; b1 = -2 * c / a0; b2 = (1 - al * A) / a0; a1 = -2 * c / a0; a2 = (1 - al / A) / a0;
    }
    void shelf(double sr, double f, double db, bool high) {  // RBJ shelf, S = 1
        const double A = std::pow(10, db / 40), w = 2 * kPi * f / sr, c = std::cos(w), s = std::sin(w), al = s / 2 * std::sqrt(2.0), sq = 2 * std::sqrt(A) * al;
        double a0;
        if (high) { a0 = (A + 1) - (A - 1) * c + sq; b0 = A * ((A + 1) + (A - 1) * c + sq); b1 = -2 * A * ((A - 1) + (A + 1) * c); b2 = A * ((A + 1) + (A - 1) * c - sq);
                    a1 = 2 * ((A - 1) - (A + 1) * c); a2 = (A + 1) - (A - 1) * c - sq; }
        else      { a0 = (A + 1) + (A - 1) * c + sq; b0 = A * ((A + 1) - (A - 1) * c + sq); b1 = 2 * A * ((A - 1) - (A + 1) * c); b2 = A * ((A + 1) - (A - 1) * c - sq);
                    a1 = -2 * ((A - 1) + (A + 1) * c); a2 = (A + 1) + (A - 1) * c - sq; }
        b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
    }
};

class SpatialEngine {
public:
    void prepare(double sampleRate) {
        sr = sampleRate;
        int n = 1;
        while (n < (int)(0.05 * sr) + 8) n <<= 1;
        buf.assign(n, 0.0);
        mask = n - 1;
        static const double ms[4] = {29.7, 37.1, 43.9, 53.3};  // the room: four lines, mutually prime-ish
        for (int k = 0; k < 4; ++k) { int m = 1; const int len = (int)(ms[k] * 0.001 * sr); while (m < len + 2) m <<= 1; room[k].assign(m, 0.0); rlen[k] = len; rmask[k] = m - 1; }
        reset();
    }
    void reset() {
        std::fill(buf.begin(), buf.end(), 0.0);
        for (auto& r : room) std::fill(r.begin(), r.end(), 0.0);
        w = rw = 0; airZ = 0; shZ[0] = shZ[1] = 0; backZ[0] = backZ[1] = 0; rdamp.fill(0);
        prox = Biquad{}; for (auto& b : notch) b = Biquad{}; for (auto& b : tilt) b = Biquad{};
        sAz = p.around * kPi / 180; sEl = p.height; sDist = p.distance; first = true; count = 0;
    }
    void update(const Params& np) { p = np; }

    // beat = musical position (quarter notes) at the block start
    void process(float* L, float* R, int n, double beat, double beatsPerSample) {
        const double smooth = 1 - std::exp(-1.0 / (0.03 * sr));  // 30 ms glide on every move
        const double hp = p.headphones ? 1.0 : 0.5;
        const double cyc = kRateBeats[std::clamp(p.rate, 0, kNumRates - 1)];
        const double dopplerScale = p.orbit == kSwoop ? 1.0 : 0.25;
        for (int i = 0; i < n; ++i) {
            // ---- where the sound is now: the base position plus the orbit
            double az = p.around * kPi / 180, el = p.height, d = p.distance;
            if (p.orbit != kStill) {
                const double b = beat + i * beatsPerSample, u = b / cyc - std::floor(b / cyc), s = p.size, tw = 2 * kPi * u;
                switch (p.orbit) {
                    case kCircle: az += 2 * kPi * u; break;
                    case kRise: el = std::clamp(el + s * (2 * u - 1) * 1.6, -1.0, 1.0); az += 0.6 * s * std::sin(tw); break;
                    case kFall: el = std::clamp(el + s * (1 - 2 * u) * 1.6, -1.0, 1.0); az -= 0.6 * s * std::sin(tw); break;
                    case kSwoop: az += s * kPi * (u - 0.5) * 1.6; d = std::clamp(d + s * 0.85 * (std::cos(tw) * 0.5 + 0.5) - 0.2 * s, 0.0, 1.0); break;
                    case kFigure8: az += s * 1.6 * std::sin(tw); el = std::clamp(el + s * 0.7 * std::sin(2 * tw), -1.0, 1.0); break;
                    case kDrift: az += s * 2.1 * (0.6 * std::sin(tw) + 0.4 * std::sin(3.13 * tw + 1)); el = std::clamp(el + s * 0.5 * std::sin(2.31 * tw + 2), -1.0, 1.0);
                                 d = std::clamp(d + s * 0.25 * std::sin(1.73 * tw), 0.0, 1.0); break;
                    case kBreath: d = std::clamp(d + s * 0.6 * (0.5 - 0.5 * std::cos(tw)), 0.0, 1.0); el = std::clamp(el + s * 0.15 * std::sin(tw), -1.0, 1.0); break;
                }
            }
            if (first) { sAz = az; sEl = el; sDist = d; first = false; }
            // glide the angle the short way round
            double da = az - sAz; da -= 2 * kPi * std::floor((da + kPi) / (2 * kPi));
            sAz += da * smooth; sEl += (el - sEl) * smooth; sDist += (d - sDist) * smooth;
            if ((count++ & 31) == 0) coeffs(hp);

            // ---- the source: mono at its position; its width shrinks with distance
            const double mid = 0.5 * ((double)L[i] + R[i]), side = 0.5 * ((double)L[i] - R[i]);
            double x = prox.run(mid);                  // proximity bass
            airZ += (x - airZ) * airK; x = airZ;       // air absorption
            buf[w] = x;
            const double sinA = std::sin(sAz), cosA = std::cos(sAz);
            const double lat = std::asin(std::clamp(sinA, -1.0, 1.0));            // lateral angle (front/back folded)
            const double itd = 0.000255 * (std::abs(lat) + std::abs(sinA)) * sr;   // Woodworth, in samples
            const double base = 2 + dopplerScale * (0.5 + 19.5 * sDist * sDist) / 343.0 * sr;
            double ear[2] = {read(base + (sinA > 0 ? itd : 0)), read(base + (sinA < 0 ? itd : 0))};  // the far ear hears later
            const double shade = std::abs(sinA);
            for (int c = 0; c < 2; ++c) {
                const bool far = (c == 0) == (sinA > 0);
                double y = ear[c];
                shZ[c] += (y - shZ[c]) * (far ? shK : nearK);  // head shadow: the far ear loses highs and level
                y = shZ[c] * (far ? 1 - 0.3 * shade : 1 + 0.12 * shade);
                if (cosA < 0) { backZ[c] += (y - backZ[c]) * backK; y += (backZ[c] - y) * (-cosA) * 0.45; }  // behind: darker
                y = tilt[c].run(notch[c].run(y));     // height
                ear[c] = y;
            }
            const double g = 1 / (1 + 2.2 * sDist), keepSide = 0.8 * (1 - sDist);
            double outL = (ear[0] + side * keepSide) * g, outR = (ear[1] - side * keepSide) * g;
            // ---- the room: farther = more of it (a small 4-line feedback network, Householder mix)
            const double send = mid * p.room * (0.15 + 0.85 * sDist) * 0.6;
            double o[4];
            for (int k = 0; k < 4; ++k) { o[k] = room[k][(rw - rlen[k]) & rmask[k]]; rdamp[k] += (o[k] - rdamp[k]) * rdK; o[k] = rdamp[k]; }
            const double sum = 0.5 * (o[0] + o[1] + o[2] + o[3]);
            for (int k = 0; k < 4; ++k) room[k][rw & rmask[k]] = send + 0.78 * (o[k] - sum);
            const double wet = p.room * (0.1 + 0.9 * sDist);
            outL += wet * (o[0] - o[2]) * 0.7;
            outR += wet * (o[1] - o[3]) * 0.7;
            L[i] = (float)outL;
            R[i] = (float)outR;
            w = (w + 1) & mask;
            ++rw;
        }
        viewAz.store((float)(std::remainder(sAz, 2 * kPi) * 180 / kPi), std::memory_order_relaxed);
        viewEl.store((float)sEl, std::memory_order_relaxed);
        viewDist.store((float)sDist, std::memory_order_relaxed);
    }

    std::atomic<float> viewAz{0}, viewEl{0}, viewDist{0.2f};  // for the face

private:
    void coeffs(double hp) {
        const double shade = std::abs(std::sin(sAz));
        shK = 1 - std::exp(-2 * kPi * std::min(0.45 * sr, 20000 - 17000 * shade) / sr);
        nearK = 1 - std::exp(-2 * kPi * std::min(0.45 * sr, 20000.0) / sr);
        backK = 1 - std::exp(-2 * kPi * 4500 / sr);
        airK = 1 - std::exp(-2 * kPi * std::min(0.45 * sr, 20000 * std::pow(0.12, sDist)) / sr);
        rdK = 1 - std::exp(-2 * kPi * 5000 / sr);
        prox.shelf(sr, 180, 4.0 * std::max(0.0, 1 - sDist / 0.2), false);
        const double f = std::min(0.45 * sr, 6000 + 4500 * sEl);  // the pinna notch climbs with height
        for (int c = 0; c < 2; ++c) {
            notch[c].peak(sr, f, 2.2, -10 * hp * (0.5 + 0.5 * std::abs(sEl)));
            tilt[c].peak(sr, std::min(0.45 * sr, 8000.0), 0.9, 8 * hp * sEl);  // Blauert's 'above' band: ~8 kHz up = above, cut = below
        }
    }
    double read(double d) const {  // d samples behind the newest, linear
        const double pos = w - d;
        const int i0 = (int)std::floor(pos);
        const double t = pos - i0;
        return buf[i0 & mask] + (buf[(i0 + 1) & mask] - buf[i0 & mask]) * t;
    }
    Params p;
    double sr = 48000, sAz = 0, sEl = 0, sDist = 0.2;
    bool first = true;
    unsigned count = 0;
    std::vector<double> buf;
    int mask = 0, w = 0;
    std::array<std::vector<double>, 4> room;
    std::array<int, 4> rlen{}, rmask{};
    std::array<double, 4> rdamp{};
    unsigned rw = 0;
    double airZ = 0, shZ[2]{}, backZ[2]{}, shK = 1, nearK = 1, backK = 1, airK = 1, rdK = 1;
    Biquad prox, notch[2], tilt[2];
};

}  // namespace space
}  // namespace echolab
