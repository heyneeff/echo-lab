#pragma once
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <string>

#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"

#include "params.h"
#include "WebEditor.h"
#include "../../dsp/DriveVoices.h"
#include "../../dsp/Resynth.h"
#include "../../dsp/FxRack.h"

namespace echolab {
namespace wtsynth {

class SynthPlugin : public Steinberg::Vst::SingleComponentEffect, public Steinberg::Vst::IMidiMapping {
public:
    // mod wheel, channel aftertouch and pitch bend arrive as the parameters of the same names
    Steinberg::tresult PLUGIN_API getMidiControllerAssignment(Steinberg::int32 busIndex, Steinberg::int16 channel,
                                                              Steinberg::Vst::CtrlNumber midiControllerNumber,
                                                              Steinberg::Vst::ParamID& id) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) SMTG_OVERRIDE;
    Steinberg::uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return SingleComponentEffect::addRef(); }
    Steinberg::uint32 PLUGIN_API release() SMTG_OVERRIDE { return SingleComponentEffect::release(); }
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
    void applyInitPatch();
    void render(float* L, float* R, int from, int to);
    std::array<double, kNumParams> norm{};
    wt::Synth synth;
    fx::FxRack rack;
    fx::Follower scFollow;
    double hostTempo = 120, fxBeat = 0;
    double sampleRate = 44100;
    // editor link
    std::atomic<float> mOut{0};
    std::atomic<bool> sendTable{true};
    std::atomic<bool> panicReq{false};
    // MIDI: VST3 never hands a plugin raw CCs, so every CC 0-127 comes in on a hidden proxy parameter (kCCBase + cc)
    // and is routed here: the default table below, plus whatever you teach it with LEARN MIDI. Saved with the set.
    static constexpr int kCCBase = 100000;
    std::array<std::atomic<int>, 128> ccMap{};   // cc -> parameter id, -1 = nothing
    std::atomic<int> learnTarget{-1}, learnedCC{-1};
    void defaultCCs();
    // LOCK: a locked parameter is left alone by the morph (and by Dice in the face)
    std::array<std::atomic<bool>, kNumParams> locked{};
    space::SpatialEngine spatial;  // SPACE: after the FX rack, before the safety limiter
    bool spaceOn = false;
    // morph snapshots: 4 whole-patch states (normalized values) on the X-Y pad corners A B / C D, blended by Morph X/Y.
    // eff = what the engine plays (the blend while morphing, else the knobs); the face shows eff.
    std::array<std::array<double, kNumParams>, 4> snap{};
    std::array<std::atomic<bool>, 4> hasSnap{};
    std::array<double, kNumParams> eff{};
    std::atomic<bool> morphActive{false};
    void computeEffective();
    bool blendInto(const double* base, double* out) const;  // the morph blend of base (normalized); false = morph not active
    static bool morphExcluded(int id) { return id == kMorphOn || id == kMorphX || id == kMorphY || id == kModWheel || id == kPitchBend || id == kAftertouch || id == kSustainPedal; }  // STAGE's panic button: every voice and FX tail stops at the next block
    std::string feedJson();
    // grown wavetables: analysed on a worker thread, swapped in atomically, saved in the state
    void onEditorMessage(const std::string& type, const std::string& name, const std::string& data, double value);
    void growTable(std::vector<float> audio, double sr, std::string name);
    void installTable(std::vector<wt::Spectrum> spectra, std::string name, std::string info, double seconds);
    void useFactoryTable();
    const wt::Wavetable* pickTable(int tableParam) const;  // any thread: the table a Table value means
    std::mutex tableMx;                        // guards everything below (never taken on the audio thread)
    std::unique_ptr<wt::Wavetable> custom, retired;
    std::vector<wt::Spectrum> customSpectra;   // what the state saves
    std::string tableName = "ORPHEO", tableInfo = "FACTORY";
    std::thread worker;
    // TableForge library: a table is built on this worker when it is picked (never on the audio thread). The audio
    // thread only asks (want) and keeps playing the previous table until the new one is ready. At most kLibKeep stay built.
    struct LibSlot { std::unique_ptr<wt::Wavetable> table; std::atomic<const wt::Wavetable*> ptr{nullptr}; std::atomic<bool> want{false}; double lastUse = 0; };
    static constexpr int kLibKeep = 6;
    mutable std::array<LibSlot, kTableSlots> lib;
    std::thread libThread;
    std::atomic<bool> libQuit{false};
    void libLoop();
    std::atomic<bool> growing{false};
    double customSeconds = 0;
    std::atomic<const wt::Wavetable*> customPtr{nullptr};  // what the audio thread may play as GROWN
    std::atomic<bool> tableDirty{false};
    std::string presetName = "ORPHEO \u00B7 Dream Pad";  // shown in the face's browser, saved in the set (guarded by tableMx)
    std::vector<std::string> userTables;  // ~/Music/Morpheus/Tables (feed thread only)
    int userScan = 0, libFeed = 0;
    std::string exportNote;  // one-shot message for the face after exporting (guarded by tableMx)                    // a grown table arrived: re-pick the table
    const wt::Wavetable* lastShown = nullptr, *lastShownB = nullptr;  // editor thread: which tables the face has
    void setFromEditor(const std::string& name, double plain);
};

}  // namespace wtsynth
}  // namespace echolab
