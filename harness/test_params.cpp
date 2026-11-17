// Host-parameter contract test, built once per plugin (see CMakeLists.txt).
//
// Ableton Live clamps every plain value it sets on a VST3 parameter into
// [plain(0), plain(1)]. If a knob's plain value isn't monotonic (e.g. "Off" = 0 at the top of a
// 200..20000 Hz knob), that range collapses to [0, 200] and every setting snaps to the bottom.
// So for every continuous parameter: plain must never fall as the knob rises, and
// norm -> plain -> clamp -> norm must land on the same setting.
#include <cstdio>
#include <cmath>
#include <algorithm>
#include PARAMS_H
using namespace PLUGIN_NS;

int main() {
    int fails = 0;
    for (const auto& p : kParams) {
        if (p.map == Map::List) continue;
        const double p0 = hostPlain(p, 0.0), p1 = hostPlain(p, 1.0);
        const double lo = std::min(p0, p1), hi = std::max(p0, p1);
        double prev = -INFINITY;
        int bad = 0;
        for (int i = 0; i <= 1000; ++i) {
            const double n = i / 1000.0, v = hostPlain(p, n);
            const double back = hostNorm(p, std::clamp(v, lo, hi));
            const bool mono = v >= prev - 1e-9;
            const bool same = std::abs(back - n) < 1e-6 || toText(p, back) == toText(p, n);
            if ((!mono || !same) && bad++ < 3)
                std::printf("FAIL %-14s n=%.3f plain=%g -> live-clamped back=%.4f (%s, want %s)%s\n", p.name, n, v, back,
                            toText(p, back).c_str(), toText(p, n).c_str(), mono ? "" : " [plain falls]");
            prev = v;
        }
        fails += bad > 0;
    }
    std::printf("%s: %s\n", PLUGIN_LABEL, fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
