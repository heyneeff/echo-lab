// Checks for dsp/TableForge.h: every recipe builds a finite, audible, normalized table; names unique; build time.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>

#include "TableForge.h"
#include "WavetableFile.h"
#include <filesystem>

using namespace echolab::wt;

int main() {
    int fails = 0;
    std::set<std::string> names;
    double worst = 0;
    for (const auto& r : forge::recipes()) {
        const auto t0 = std::chrono::steady_clock::now();
        Wavetable t;
        t.build(r.make());
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        worst = std::max(worst, ms);
        bool finite = true;
        double minPeak = 9, maxPeak = 0;
        for (int f = 0; f < t.frames; ++f) {
            const float* d = t.at(f, 0);
            double pk = 0;
            for (int i = 0; i < kTableSize; ++i) { finite &= std::isfinite(d[i]); pk = std::max(pk, (double)std::abs(d[i])); }
            minPeak = std::min(minPeak, pk); maxPeak = std::max(maxPeak, pk);
        }
        const bool ok = finite && t.frames >= 2 && minPeak > 0.5 && maxPeak < 1.01 && names.insert(r.name).second;
        std::printf("%s %-16s %-9s %2d frames  peak %.2f..%.2f  %6.1f ms\n", ok ? "ok  " : "FAIL", r.name, r.family, t.frames, minPeak, maxPeak, ms);
        fails += !ok;
    }
    std::printf("%d recipes, slowest build %.0f ms\n", forge::count(), worst);
    // your-tables folder path: a table written by SAVE TABLE reads back as the same cycles
    {
        Wavetable t; t.build(forge::recipes()[0].make());
        const auto cyc = tableToCycles(t);
        const std::string path = (std::filesystem::temp_directory_path() / "morpheus_forge_test.wav").string();
        std::vector<float> back; int cycle = 0;
        const bool ok = writeWavetableWav(path, cyc) && readWavetableWav(path, back, cycle) && cycle == 2048 && back.size() == cyc.size();
        double d = 0; if (ok) for (size_t i = 0; i < cyc.size(); ++i) d = std::max(d, (double)std::abs(back[i] - cyc[i]));
        std::filesystem::remove(path);
        std::printf("%s wavetable .wav read-back (max diff)  %g\n", ok && d < 1e-7 ? "ok  " : "FAIL", d);
        fails += !(ok && d < 1e-7);
    }
    return fails ? 1 : 0;
}
