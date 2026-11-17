// Checks for dsp/FxRack.h: off = bit-exact, mix 0 = dry, echo tempo sync, chain order, chorus, sidechain, CPU.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "FxRack.h"

using namespace echolab::fx;
constexpr double SR = 48000;

static void run(FxRack& r, std::vector<float>& L, std::vector<float>& R, const std::vector<float>* sc = nullptr) {
    const double bps = 120.0 / 60 / SR;
    for (size_t i = 0; i < L.size(); i += 256) {
        const int n = (int)std::min<size_t>(256, L.size() - i);
        r.process(&L[i], &R[i], sc ? &(*sc)[i] : nullptr, sc ? &(*sc)[i] : nullptr, n, i * bps, bps);
    }
}
static std::vector<float> music(int n, int seed = 1) {  // a chord with some noise
    std::mt19937 rng(seed); std::normal_distribution<float> g(0, 0.02f); std::vector<float> x(n);
    for (int i = 0; i < n; ++i) x[i] = (float)(0.2 * (std::sin(2 * kPi * 220 * i / SR) + std::sin(2 * kPi * 277 * i / SR) + std::sin(2 * kPi * 330 * i / SR)) + g(rng));
    return x;
}

int main() {
    int fails = 0;
    auto check = [&](bool ok, const char* what, double v) { std::printf("%s %-56s %10.4f\n", ok ? "ok  " : "FAIL", what, v); fails += !ok; };

    // everything off: bit-exact
    {
        FxRack r; r.prepare(SR); r.update(FxParams{}, 120);
        auto L = music(48000), R = L, L0 = L; run(r, L, R);
        double d = 0; for (size_t i = 0; i < L.size(); ++i) d = std::max(d, (double)std::abs(L[i] - L0[i]));
        check(d == 0 && !r.anyOn(), "all FX off: bit-exact dry (max diff)", d);
    }
    // each FX on at mix 0 stays dry (Prometheus aligns its dry path to its oversampler, so it is checked at mix 1 for level only)
    for (int id : {kFxChorus, kFxEcho, kFxReverb, kFxPump}) {
        FxParams p; p.chorusOn = id == kFxChorus; p.echoOn = id == kFxEcho; p.verbOn = id == kFxReverb; p.pumpOn = id == kFxPump;
        p.chorusMix = p.echoMix = p.verbMix = p.pumpMix = 0;
        FxRack r; r.prepare(SR); r.update(p, 120);
        auto L = music(48000), R = L, L0 = L; run(r, L, R);
        double d = 0; for (size_t i = 0; i < L.size(); ++i) d = std::max(d, (double)std::abs(L[i] - L0[i]));
        char b[96]; std::snprintf(b, sizeof b, "%s on at mix 0 = dry (max diff)", kFxNames[id]);
        check(d < 1e-5, b, d);
    }
    // echo synced: 1/4 at 120 BPM = 500 ms; feedback 0, so one repeat
    {
        FxParams p; p.echoOn = true; p.echoSync = true; p.noteL = p.noteR = 8; p.echoMode = 0; p.echoFb = 0; p.echoMix = 1;
        // a tape echo glides to a new time, so let it settle first; its wow moves a repeat by a couple of ms
        auto hit = [&](FxRack& r, int len, int expect) {
            std::vector<float> s(48000 * 8, 0.f), s2 = s; run(r, s, s2);
            std::vector<float> L(len, 0.f), R(len, 0.f); L[100] = R[100] = 1.f; run(r, L, R);
            int at = 0; double pk = 0; for (int i = 1000; i < len; ++i) if (std::abs(L[i]) > pk) { pk = std::abs(L[i]); at = i; }
            return at - 100 - expect;
        };
        FxRack r; r.prepare(SR); r.update(p, 120);
        const int e1 = hit(r, 48000, 24000);
        check(std::abs(e1) < 120, "echo 1/4 @ 120 BPM lands at 500 ms (samples off, wow +-2.5 ms)", e1);
        r.setTempo(90); const int e2 = hit(r, 72000, 32000);
        check(std::abs(e2) < 120, "echo follows the tempo: 1/4 @ 90 BPM at 667 ms (samples off)", e2);
    }
    // chain order: every permutation round-trips, and order changes the sound
    {
        bool ok = true; for (int i = 0; i < 120; ++i) ok &= indexFromOrder(orderFromIndex(i)) == i;
        check(ok, "chain order: all 120 orders encode/decode", ok);
        auto render = [&](int order) { FxParams p; p.order = order; p.drive = 7; p.verbOn = true; p.verbMix = 0.5; FxRack r; r.prepare(SR); r.update(p, 120);
            auto L = music(48000), R = L; run(r, L, R); return L; };
        const int driveThenVerb = indexFromOrder({0, 1, 2, 3, 4}), verbThenDrive = indexFromOrder({3, 1, 2, 0, 4});
        auto a = render(driveThenVerb), b = render(verbThenDrive); double d = 0; for (size_t i = 0; i < a.size(); ++i) d += std::abs(a[i] - b[i]);
        check(d / a.size() > 1e-3, "reverb->drive sounds different from drive->reverb", d / a.size());
    }
    // chorus: stereo, bounded, each mode
    for (int m = 0; m < 4; ++m) {
        FxParams p; p.chorusOn = true; p.chorusMode = m; p.chorusMix = 1; FxRack r; r.prepare(SR); r.update(p, 120);
        auto L = music(48000), R = L; run(r, L, R);
        double d = 0, pk = 0; for (size_t i = 0; i < L.size(); ++i) { d += std::abs(L[i] - R[i]); pk = std::max(pk, (double)std::abs(L[i])); }
        char b[96]; std::snprintf(b, sizeof b, "chorus %s: stereo (mean |L-R|), bounded", kChorusNames[m]);
        check(d / L.size() > 1e-3 && pk < 1.5, b, d / L.size());
    }
    // sidechain follower: a kick (step) reaches 63% within its 2 ms attack, falls within its release
    {
        Follower f; f.prepare(SR, 2, 150); std::vector<float> one(96, 1.f), zero(7200, 0.f);
        const double up = f.run(one.data(), nullptr, 96); const double down = f.run(zero.data(), nullptr, 7200);
        check(up > 0.63 && up < 0.99, "sidechain follower: attack (level after 2 ms)", up);
        check(down < 0.37 * up && down > 0.2 * up, "sidechain follower: release (after 150 ms)", down / up);
    }
    // CPU: each FX alone on 10 s of stereo
    {
        const char* nm[] = {"Prometheus", "Chorus", "Echo Nymph", "Letheverb", "Charon"};
        for (int id = 0; id < kNumFx; ++id) {
            FxParams p; p.drive = id == kFxDrive ? 6 : 0; p.chorusOn = id == kFxChorus; p.echoOn = id == kFxEcho; p.verbOn = id == kFxReverb; p.pumpOn = id == kFxPump;
            FxRack r; r.prepare(SR); r.update(p, 120); auto L = music((int)SR * 10), R = L;
            auto t0 = std::chrono::steady_clock::now(); run(r, L, R); const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("     CPU %-12s %.2f%% of one core\n", nm[id], 100 * s / 10);
        }
    }
    std::printf("fx: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
