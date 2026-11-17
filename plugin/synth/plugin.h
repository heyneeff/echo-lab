#pragma once
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <string>

#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include "params.h"
#include "WebEditor.h"
#include "../../dsp/DriveVoices.h"
#include "../../dsp/Resynth.h"

namespace echolab {
namespace wtsynth {

class SynthPlugin : public Steinberg::Vst::SingleComponentEffect {
public:
    using tresult = Steinberg::tresult;
    static Steinberg::FUnknown* createInstance(void*) { return (Steinberg::Vst::IAudioProcessor*)new SynthPlugin; }
    ~SynthPlugin() override;
    tresult PLUGIN_API initialize(Steinberg::FUnknown* context) SMTG_OVERRIDE;
    tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns,
                                          Steinberg::Vst::SpeakerArrangement* outputs, Steinberg::int32 numOuts) SMTG_OVERRIDE;
    tresult PLUGIN_API canProcessSampleSize(Steinberg::int32 symbolicSampleSize) SMTG_OVERRIDE;
    tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup& setup) SMTG_OVERRIDE;
    tresult PLUGIN_API setActive(Steinberg::TBool state) SMTG_OVERRIDE;
    tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) SMTG_OVERRIDE;
    tresult PLUGIN_API setState(Steinberg::IBStream* state) SMTG_OVERRIDE;
    tresult PLUGIN_API getState(Steinberg::IBStream* state) SMTG_OVERRIDE;
    Steinberg::IPlugView* PLUGIN_API createView(Steinberg::FIDString name) SMTG_OVERRIDE;

private:
    void applyParams();
    void render(float* L, float* R, int from, int to);
    std::array<double, kNumParams> norm{};
    wt::Synth synth;
    DriveEngine drive;
    bool driveOn = false;
    double sampleRate = 44100;
    // editor link
    std::atomic<float> mOut{0};
    std::atomic<bool> sendTable{true};
    std::string feedJson();
    // grown wavetables: analysed on a worker thread, swapped in atomically, saved in the state
    void onEditorMessage(const std::string& type, const std::string& name, const std::string& data, double value);
    void growTable(std::vector<float> audio, double sr, std::string name);
    void installTable(std::vector<wt::Spectrum> spectra, std::string name, std::string info);
    void useFactoryTable();
    std::mutex tableMx;                        // guards everything below (never taken on the audio thread)
    std::unique_ptr<wt::Wavetable> custom, retired;
    std::vector<wt::Spectrum> customSpectra;   // what the state saves
    std::string tableName = "ORPHEO", tableInfo = "FACTORY";
    std::thread worker;
    std::atomic<bool> growing{false};
    void setFromEditor(const std::string& name, double plain);
};

}  // namespace wtsynth
}  // namespace echolab
