#pragma once
#include <array>
#include <atomic>
#include <string>

#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include "../../dsp/DriveEngine.h"
#include "params.h"
#include "WebEditor.h"

namespace echolab {
namespace drive {

class DriveLabPlugin : public Steinberg::Vst::SingleComponentEffect {
public:
    using tresult = Steinberg::tresult;
    static Steinberg::FUnknown* createInstance(void*) {
        return (Steinberg::Vst::IAudioProcessor*)new DriveLabPlugin;
    }
    tresult PLUGIN_API initialize(Steinberg::FUnknown* context) SMTG_OVERRIDE;
    tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns,
                                          Steinberg::Vst::SpeakerArrangement* outputs,
                                          Steinberg::int32 numOuts) SMTG_OVERRIDE;
    tresult PLUGIN_API canProcessSampleSize(Steinberg::int32 symbolicSampleSize) SMTG_OVERRIDE;
    tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup& setup) SMTG_OVERRIDE;
    tresult PLUGIN_API setActive(Steinberg::TBool state) SMTG_OVERRIDE;
    tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) SMTG_OVERRIDE;
    tresult PLUGIN_API setState(Steinberg::IBStream* state) SMTG_OVERRIDE;
    tresult PLUGIN_API getState(Steinberg::IBStream* state) SMTG_OVERRIDE;
    Steinberg::uint32 PLUGIN_API getLatencySamples() SMTG_OVERRIDE { return Oversampler4x::latency(); }
    Steinberg::IPlugView* PLUGIN_API createView(Steinberg::FIDString name) SMTG_OVERRIDE;

private:
    void applyParams();
    std::array<double, kNumParams> norm{};
    DriveEngine engine;
    int currentStyle = -1;
    double sampleRate = 44100;
    // pump + filter (after the drive)
    double pumpDepth = 0, freeBeat = 0;
    int pumpRate = 1;
    double filterHz = 0, filterRes = 0.2;
    std::array<double, 2> ic1{}, ic2{};  // SVF state per channel
    // editor link
    static constexpr int kScope = 1024;
    std::array<float, kScope> scope{};      // last input samples (left), ring
    std::atomic<int> scopePos{0};
    std::atomic<float> mIn{0}, mOut{0}, mPump{1};
    std::string feedJson();
    void setFromEditor(const std::string& name, double plain);
};

}  // namespace drive
}  // namespace echolab
