#pragma once
#include <array>
#include <atomic>
#include <string>

#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include "../../dsp/EchoEngine.h"
#include "params.h"
#include "WebEditor.h"

namespace echolab {

class EchoLabPlugin : public Steinberg::Vst::SingleComponentEffect {
public:
    using tresult = Steinberg::tresult;
    static Steinberg::FUnknown* createInstance(void*) {
        return (Steinberg::Vst::IAudioProcessor*)new EchoLabPlugin;
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
    Steinberg::uint32 PLUGIN_API getLatencySamples() SMTG_OVERRIDE { return 0; }
    Steinberg::uint32 PLUGIN_API getTailSamples() SMTG_OVERRIDE { return Steinberg::Vst::kInfiniteTail; }
    Steinberg::IPlugView* PLUGIN_API createView(Steinberg::FIDString name) SMTG_OVERRIDE;

private:
    void applyParams();
    std::array<double, kNumParams> norm{};  // current normalized values (audio thread)
    EchoEngine engine;
    int currentStyle = -1;
    double sampleRate = 44100;
    // editor link: meters written by the audio thread (peak-held), read and reset by the editor
    std::atomic<float> mIn{0}, mWet{0}, mWr{0}, mWf{0}, mDuck{1};
    std::string feedJson();
    void setFromEditor(const std::string& name, double plain);
};

}  // namespace echolab
