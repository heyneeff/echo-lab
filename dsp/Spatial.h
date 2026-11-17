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
#include <cstdint>
#include <vector>

namespace echolab {
namespace space {

constexpr double kPi = 3.14159265358979323846;

enum Orbit { kStill = 0, kCircle, kRise, kFall, kSwoop, kFigure8, kDrift, kBreath, kSpiral, kPingPong, kPendulum, kOverhead, kApproach, kTumble, kNumOrbits };
inline const char* const kOrbitNames[kNumOrbits] = {"Still", "Circle", "Rise", "Fall", "Swoop", "Figure 8", "Drift", "Breath",
                                                    "Spiral", "Ping-Pong", "Pendulum", "Overhead", "Approach", "Tumble"};
constexpr int kOrbitSlots = 32;  // the Orbit switch is sized for growth: new paths never move a saved one
inline const char* const kRateNames[] = {"1/8 Bar", "1/4 Bar", "1/2 Bar", "1 Bar", "2 Bars", "4 Bars", "8 Bars", "16 Bars"};
inline const double kRateBeats[] = {0.5, 1, 2, 4, 8, 16, 32, 64};
constexpr int kNumRates = 8;
inline double hash01(long long k) { uint64_t x = (uint64_t)(k * 2654435761LL + 0x9E3779B97F4A7C15ULL); x ^= x >> 31; x *= 0xBF58476D1CE4E5B9ULL; x ^= x >> 29; return (x >> 11) * (1.0 / 9007199254740992.0); }

// The sound's place in a cube around the listener (who sits at the centre, facing +z):
// x -1 left .. +1 right, y -1 below .. +1 above, z -1 behind .. +1 in front. Distance grows toward the corners.
struct Params {
    double x = 0, y = 0, z = 0.5;
    bool mirror = false;   // also play it at the opposite point (-x, -y, -z)
    int orbit = kStill;
    int rate = 2;          // index into kRateBeats
    double size = 0.7;     // how far an orbit travels, 0..1
    double room = 0.5;     // how much space the distance brings, 0..1
    double amount = 1.5;   // how strong every cue is: 0 = barely, 1 = natural, 2 = extreme
    bool headphones = false;
};
// cube position <-> the hearing cues' coordinates (around, height, distance)
inline void toPolar(double x, double y, double z, double& az, double& el, double& d) {
    az = std::atan2(x, z); el = std::clamp(y, -1.0, 1.0); d = std::clamp(std::sqrt(x * x + y * y + z * z) / 1.7320508, 0.0, 1.0);
}
inline void toCube(double az, double el, double d, double& x, double& y, double& z) {
    y = el; const double r = d * 1.7320508, rh = std::sqrt(std::max(0.0, r * r - y * y));
    x = std::clamp(rh * std::sin(az), -1.0, 1.0); z = std::clamp(rh * std::cos(az), -1.0, 1.0);
}

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
        w = rw = 0; airZ = 0; rdamp.fill(0);
        prox = Biquad{}; src[0] = Src{}; src[1] = Src{};
        first = true; count = 0;
    }
    void update(const Params& np) { p = np; }

    // beat = musical position (quarter notes) at the block start
    void process(float* L, float* R, int n, double beat, double beatsPerSample) {
        const double smooth = 1 - std::exp(-1.0 / (0.03 * sr));  // 30 ms glide on every move
        const double hp = p.headphones ? 1.0 : 0.75, A = std::clamp(p.amount, 0.0, 2.0);
        const double cyc = kRateBeats[std::clamp(p.rate, 0, kNumRates - 1)];
        const bool flyer = p.orbit == kSwoop || p.orbit == kPendulum || p.orbit == kOverhead || p.orbit == kApproach;
        const double dopplerScale = (flyer ? 1.0 : 0.2) * A;
        double baseAz, baseEl, baseD;
        toPolar(p.x, p.y, p.z, baseAz, baseEl, baseD);
        const int nsrc = p.mirror ? 2 : 1;
        const double srcGain = p.mirror ? 0.72 : 1.0;
        for (int i = 0; i < n; ++i) {
            // ---- where the sound is now: the base position plus the orbit
            double az = baseAz, el = baseEl, d = baseD;
            if (p.orbit != kStill) {
                const double b = beat + i * beatsPerSample, u = b / cyc - std::floor(b / cyc), s = p.size, tw = 2 * kPi * u;
                switch (p.orbit) {
                    case kCircle: az += 2 * kPi * u; d = std::max(d, 0.35); break;
                    case kRise: el = std::clamp(el + s * (2 * u - 1) * 1.6, -1.0, 1.0); az += 0.6 * s * std::sin(tw); break;
                    case kFall: el = std::clamp(el + s * (1 - 2 * u) * 1.6, -1.0, 1.0); az -= 0.6 * s * std::sin(tw); break;
                    case kSwoop: az += s * kPi * (u - 0.5) * 1.6; d = std::clamp(d + s * 0.85 * (std::cos(tw) * 0.5 + 0.5) - 0.2 * s, 0.0, 1.0); break;
                    case kFigure8: az += s * 1.6 * std::sin(tw); el = std::clamp(el + s * 0.7 * std::sin(2 * tw), -1.0, 1.0); break;
                    case kDrift: az += s * 2.1 * (0.6 * std::sin(tw) + 0.4 * std::sin(3.13 * tw + 1)); el = std::clamp(el + s * 0.5 * std::sin(2.31 * tw + 2), -1.0, 1.0);
                                 d = std::clamp(d + s * 0.25 * std::sin(1.73 * tw), 0.0, 1.0); break;
                    case kBreath: d = std::clamp(d + s * 0.6 * (0.5 - 0.5 * std::cos(tw)), 0.0, 1.0); el = std::clamp(el + s * 0.15 * std::sin(tw), -1.0, 1.0); break;
                    case kSpiral: az += 2 * kPi * u; el = std::clamp(el + s * (2 * u - 1) * 1.6, -1.0, 1.0); d = std::max(d, 0.35); break;
                    case kPingPong: az += s * 1.5 * std::tanh(6 * std::sin(tw)) / std::tanh(6.0); break;  // hard left, hard right
                    case kPendulum: az += s * 1.4 * std::sin(tw); d = std::clamp(d - s * 0.3 * std::cos(2 * tw) + 0.1, 0.0, 1.0); break;
                    case kOverhead: el = std::clamp(s * std::sin(kPi * u) * 1.2, -1.0, 1.0); az += u < 0.5 ? 0 : kPi; d = std::max(d, 0.3); break;  // front, over your head, behind
                    case kApproach: { const double t = u * u * (3 - 2 * u); d = std::clamp(1 - s * t, 0.0, 1.0); break; }  // from far away right up to you
                    case kTumble: {  // a new place every 1/8 of the cycle, gliding between them
                        const double q = u * 8; const long long k = (long long)std::floor(b / cyc * 8); const double f = q - std::floor(q), sm = f * f * (3 - 2 * f);
                        auto pick = [&](long long j, int c) { return hash01(j * 3 + c) * 2 - 1; };
                        az += s * kPi * (pick(k, 0) + (pick(k + 1, 0) - pick(k, 0)) * sm);
                        el = std::clamp(el + s * (pick(k, 1) + (pick(k + 1, 1) - pick(k, 1)) * sm), -1.0, 1.0);
                        d = std::clamp(d + s * 0.4 * (pick(k, 2) + (pick(k + 1, 2) - pick(k, 2)) * sm), 0.0, 1.0); break; }
                }
            }
            if (first) { sAz = az; sEl = el; sDist = d; first = false; }
            double da = az - sAz; da -= 2 * kPi * std::floor((da + kPi) / (2 * kPi));  // glide the angle the short way round
            sAz += da * smooth; sEl += (el - sEl) * smooth; sDist += (d - sDist) * smooth;
            if ((count++ & 31) == 0) coeffs(hp, nsrc, A);

            // ---- the source: mono, its width shrinks with distance
            const double mid = 0.5 * ((double)L[i] + R[i]), side = 0.5 * ((double)L[i] - R[i]);
            double x = prox.run(mid);                  // proximity bass
            airZ += (x - airZ) * airK; x = airZ;       // air absorption
            buf[w] = x;
            const double base = 2 + dopplerScale * (0.5 + 19.5 * sDist * sDist) / 343.0 * sr;
            double outL = 0, outR = 0;
            for (int k = 0; k < nsrc; ++k) {  // k = 1: the mirror, at the opposite point
                const double a = sAz + k * kPi, sinA = std::sin(a), cosA = std::cos(a);
                const double lat = std::asin(std::clamp(sinA, -1.0, 1.0));
                const double itd = 0.000255 * (std::abs(lat) + std::abs(sinA)) * sr;  // Woodworth, in samples
                double ear[2] = {read(base + (sinA > 0 ? itd : 0)), read(base + (sinA < 0 ? itd : 0))};  // the far ear hears later
                const double shade = std::abs(sinA);
                Src& S = src[k];
                for (int c = 0; c < 2; ++c) {
                    const bool far = (c == 0) == (sinA > 0);
                    double y = ear[c];
                    S.shZ[c] += (y - S.shZ[c]) * (far ? shK : nearK);  // head shadow: the far ear loses highs and level
                    y = S.shZ[c] * (far ? 1 - std::min(0.85, 0.36 * A) * shade : 1 + 0.12 * A * shade);
                    if (cosA < 0) { S.backZ[c] += (y - S.backZ[c]) * backK; y += (S.backZ[c] - y) * (-cosA) * std::min(0.9, 0.4 * A); }  // behind: darker
                    ear[c] = S.tilt[c].run(S.notch[c].run(y));  // height
                }
                outL += ear[0] * srcGain; outR += ear[1] * srcGain;
            }
            const double g = 1 / (1 + (1.4 + 1.6 * A) * sDist), keepSide = p.mirror ? 0.0 : 0.8 * (1 - sDist);
            outL = (outL + side * keepSide) * g; outR = (outR - side * keepSide) * g;
            // ---- the room: farther = more of it (a small 4-line feedback network, Householder mix)
            const double send = mid * p.room * (0.15 + 0.85 * sDist) * 0.6;
            double o[4];
            for (int k = 0; k < 4; ++k) { o[k] = room[k][(rw - rlen[k]) & rmask[k]]; rdamp[k] += (o[k] - rdamp[k]) * rdK; o[k] = rdamp[k]; }
            const double sum = 0.5 * (o[0] + o[1] + o[2] + o[3]);
            for (int k = 0; k < 4; ++k) room[k][rw & rmask[k]] = send + 0.78 * (o[k] - sum);
            const double wet = p.room * (0.1 + 0.9 * sDist) * (0.6 + 0.4 * A);
            outL += wet * (o[0] - o[2]) * 0.7;
            outR += wet * (o[1] - o[3]) * 0.7;
            L[i] = (float)outL;
            R[i] = (float)outR;
            w = (w + 1) & mask;
            ++rw;
        }
        double vx, vy, vz;
        toCube(sAz, sEl, sDist, vx, vy, vz);
        viewX.store((float)vx, std::memory_order_relaxed); viewY.store((float)vy, std::memory_order_relaxed); viewZ.store((float)vz, std::memory_order_relaxed);
        viewAz.store((float)(std::remainder(sAz, 2 * kPi) * 180 / kPi), std::memory_order_relaxed);
    }

    std::atomic<float> viewAz{0}, viewX{0}, viewY{0}, viewZ{0.5f};  // for the face (where the sound is, in the cube)

private:
    void coeffs(double hp, int nsrc, double A) {
        const double shade = std::abs(std::sin(sAz));
        shK = 1 - std::exp(-2 * kPi * std::min(0.45 * sr, 20000 * std::pow(0.07, shade * std::min(1.0, A / 1.4))) / sr);  // down to ~1.4 kHz
        nearK = 1 - std::exp(-2 * kPi * std::min(0.45 * sr, 20000.0) / sr);
        backK = 1 - std::exp(-2 * kPi * 4500 / sr);
        airK = 1 - std::exp(-2 * kPi * std::min(0.45 * sr, 20000 * std::pow(0.12 / (1 + 0.4 * A), sDist)) / sr);
        rdK = 1 - std::exp(-2 * kPi * 5000 / sr);
        prox.shelf(sr, 180, 4.0 * std::max(0.0, 1 - sDist / 0.2), false);
        for (int k = 0; k < nsrc; ++k) {
            const double el = k ? -sEl : sEl;  // the mirror is below when the sound is above
            const double f = std::min(0.45 * sr, 6000 + 4500 * el);  // the pinna notch climbs with height
            for (int c = 0; c < 2; ++c) {
                src[k].notch[c].peak(sr, f, 2.2, -8 * A * hp * (0.5 + 0.5 * std::abs(el)));
                src[k].tilt[c].peak(sr, std::min(0.45 * sr, 8000.0), 0.9, 7.5 * A * hp * el);  // Blauert's 'above' band
            }
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
    struct Src { double shZ[2]{}, backZ[2]{}; Biquad notch[2], tilt[2]; };  // one per position (the mirror is the second)
    Src src[2];
    double airZ = 0, shK = 1, nearK = 1, backK = 1, airK = 1, rdK = 1;
    Biquad prox;
};

}  // namespace space
}  // namespace echolab
