// Checks for dsp/Spatial.h: the cues point the right way (around, height, distance), orbits move, it stays finite, CPU.
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

#include "Spatial.h"

using namespace echolab::space;
constexpr double SR = 48000;

static std::vector<float> noise(int n, int seed = 3) { std::mt19937 r(seed); std::normal_distribution<float> g(0, 0.2f); std::vector<float> x(n); for (auto& v : x) v = g(r); return x; }
static void run(SpatialEngine& e, std::vector<float>& L, std::vector<float>& R) {
    const double bps = 120.0 / 60 / SR;
    for (size_t i = 0; i < L.size(); i += 256) e.process(&L[i], &R[i], (int)std::min<size_t>(256, L.size() - i), i * bps, bps);
}
static double rms(const std::vector<float>& x, size_t from = 24000) { double s = 0; for (size_t i = from; i < x.size(); ++i) s += x[i] * x[i]; return std::sqrt(s / (x.size() - from)); }
static double band(const std::vector<float>& x, double f0, double f1) {  // energy between f0 and f1 (DFT over a window)
    const int N = 8192, off = 30000; double e = 0;
    for (int k = (int)(f0 * N / SR); k <= (int)(f1 * N / SR); ++k) { std::complex<double> a = 0; for (int j = 0; j < N; ++j) a += (double)x[off + j] * std::polar(1.0, -2 * kPi * k * j / N); e += std::norm(a); }
    return e;
}
static std::pair<std::vector<float>, std::vector<float>> place(Params p, int seed = 3) {
    SpatialEngine e; e.prepare(SR); e.update(p); auto L = noise(48000, seed), R = L; run(e, L, R); return {L, R};
}

int main() {
    int fails = 0;
    auto check = [&](bool ok, const char* what, double v) { std::printf("%s %-58s %10.4f\n", ok ? "ok  " : "FAIL", what, v); fails += !ok; };
    Params base; base.room = 0; base.x = 0; base.y = 0; base.z = 0.25;

    { // around: +90 = right louder, and the left ear hears it later (~0.66 ms)
        Params p = base; p.x = 1; p.z = 0; auto [L, R] = place(p);
        const double ild = 20 * std::log10(rms(R) / rms(L));
        int best = 0; double bc = -1;
        for (int lag = -60; lag <= 60; ++lag) { double c = 0; for (int i = 30000; i < 40000; ++i) c += (double)R[i] * L[i + lag]; if (c > bc) { bc = c; best = lag; } }
        check(ild > 3, "around +90: right ear louder (dB)", ild);
        check(best >= 25 && best <= 40, "around +90: left ear later (samples, ~31)", best);
        Params q = base; q.x = -1; q.z = 0; auto [L2, R2] = place(q);
        check(rms(L2) > rms(R2) * 1.4, "around -90: left ear louder (ratio)", rms(L2) / rms(R2));
    }
    { // height: above is brighter around 8-12 kHz than below
        Params up = base, dn = base; up.y = 1; dn.y = -1; up.headphones = dn.headphones = true;
        auto [Lu, Ru] = place(up); auto [Ld, Rd] = place(dn);
        const double r = 10 * std::log10(band(Lu, 7000, 9500) / band(Ld, 7000, 9500));
        check(r > 4, "height: above brighter than below at 7-9.5 kHz (dB)", r);
    }
    { // distance: far is quieter and darker than near
        Params nr = base, fr = base; nr.z = 0.1; fr.x = 1; fr.y = 1; fr.z = 1;  // a far corner
        auto [Ln, Rn] = place(nr); auto [Lf, Rf] = place(fr);
        check(rms(Lf) < rms(Ln) * 0.5, "distance: far quieter (ratio)", rms(Lf) / rms(Ln));
        const double tn = band(Ln, 6000, 10000) / band(Ln, 200, 1000), tf = band(Lf, 6000, 10000) / band(Lf, 200, 1000);
        check(tf < tn * 0.3, "distance: far darker (high/low ratio far/near)", tf / tn);
    }
    { // orbit: Circle goes all the way round in 2 bars at 120 BPM (4 s)
        Params p = base; p.orbit = kCircle; p.rate = 2; SpatialEngine e; e.prepare(SR); e.update(p);
        std::vector<float> L(256, 0.1f), R = L; double mn = 999, mx = -999; const double bps = 120.0 / 60 / SR;
        for (int b = 0; b < (int)(4.2 * SR / 256); ++b) { auto l = L, r = R; e.process(l.data(), r.data(), 256, b * 256 * bps, bps); mn = std::min(mn, (double)e.viewAz); mx = std::max(mx, (double)e.viewAz); }
        check(mx - mn > 300, "orbit circle: the angle sweeps the whole way (deg)", mx - mn);
    }
    { // mirror: a sound hard right plus its mirror (hard left) comes out balanced
        Params p = base; p.x = 1; p.z = 0; p.mirror = true; auto [L, R] = place(p);
        const double bal = 20 * std::log10(rms(R) / rms(L));
        check(std::abs(bal) < 1.5, "mirror: right + its opposite = balanced (dB)", bal);
    }
    { // fuzz: every orbit, extreme settings, finite and bounded
        double peak = 0; bool finite = true;
        for (int o = 0; o < kNumOrbits; ++o) for (int hp = 0; hp < 2; ++hp) {
            Params p; p.orbit = o; p.size = 1; p.room = 1; p.z = o % 2 ? 1 : 0; p.x = o % 2 ? 1 : 0; p.y = o % 3 - 1.0; p.headphones = hp; p.mirror = o % 2;
            auto [L, R] = place(p, o + 7);
            for (size_t i = 0; i < L.size(); ++i) { finite &= std::isfinite(L[i]) && std::isfinite(R[i]); peak = std::max(peak, (double)std::max(std::abs(L[i]), std::abs(R[i]))); }
        }
        check(finite && peak < 2.5, "fuzz: all orbits finite and bounded (peak)", peak);
    }
    { // CPU
        Params p = base; p.orbit = kDrift; p.room = 0.6; SpatialEngine e; e.prepare(SR); e.update(p);
        auto L = noise(48000 * 10), R = L; const auto t0 = std::chrono::steady_clock::now(); run(e, L, R);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("     CPU SPACE %.2f%% of one core\n", sec / 10 * 100);
    }
    std::printf("space: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
