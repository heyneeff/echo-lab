#pragma once
#include <array>

#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include "params.h"
#include "../../dsp/DriveVoices.h"
#include "../../dsp/WavetableSynth.h"

namespace echolab {
namespace wtsynth {

class SynthPlugin : public Steinberg::Vst::SingleComponentEffect {
public:
    using tresult = Steinberg::tresult;
    static Steinberg::FUnknown* createInstance(void*) { return (Steinberg::Vst::IAudioProcessor*)new SynthPlugin; }
    tresult PLUGIN_API initialize(Steinberg::FUnknown* context) SMTG_OVERRIDE;
    tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns,
                                          Steinberg::Vst::SpeakerArrangement* outputs, Steinberg::int32 numOuts) SMTG_OVERRIDE;
    tresult PLUGIN_API canProcessSampleSize(Steinberg::int32 symbolicSampleSize) SMTG_OVERRIDE;
    tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup& setup) SMTG_OVERRIDE;
    tresult PLUGIN_API setActive(Steinberg::TBool state) SMTG_OVERRIDE;
    tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) SMTG_OVERRIDE;
    tresult PLUGIN_API setState(Steinberg::IBStream* state) SMTG_OVERRIDE;
    tresult PLUGIN_API getState(Steinberg::IBStream* state) SMTG_OVERRIDE;

private:
    void applyParams();
    void render(float* L, float* R, int from, int to);
    std::array<double, kNumParams> norm{};
    wt::Synth synth;
    DriveEngine drive;
    bool driveOn = false;
    double sampleRate = 44100;
};

}  // namespace wtsynth
}  // namespace echolab
