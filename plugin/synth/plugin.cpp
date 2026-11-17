#include "plugin.h"
#include "b64.h"
#include "WavetableFile.h"

#include <cctype>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/vst/vstparameters.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace echolab {
namespace wtsynth {

namespace {

class TableParameter : public Parameter {
public:
    explicit TableParameter(const ParamDef& d)
        : Parameter(USTRING(d.name), d.id, USTRING(d.units), toNorm(d, d.def), stepCount(d),
                    d.map == Map::List ? (ParameterInfo::kCanAutomate | ParameterInfo::kIsList) : ParameterInfo::kCanAutomate),
          def(d) {}
    void toString(ParamValue v, String128 s) const SMTG_OVERRIDE { UString(s, 128).fromAscii(toText(def, v).c_str()); }
    bool fromString(const TChar* string, ParamValue& v) const SMTG_OVERRIDE {
        char buf[128] = {0};
        UString(const_cast<TChar*>(string), 128).toAscii(buf, 128);
        bool ok; double n = toNormFromText(def, buf, ok);
        if (ok) v = n;
        return ok;
    }
    ParamValue toPlain(ParamValue v) const SMTG_OVERRIDE { return hostPlain(def, v); }
    ParamValue toNormalized(ParamValue v) const SMTG_OVERRIDE { return hostNorm(def, v); }

private:
    ParamDef def;
};

constexpr uint32 kStateVersion = 7;  // 2: + grown wavetable, 3: + its length (SCAN), 4: mod switches sized for growth, 5: + preset name, 6: + morph snapshots, 7: Table switches sized for growth (128)
constexpr int kSavedHarmonics = 512;   // per frame, complex, as floats

std::string upper(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }



}  // namespace

tresult PLUGIN_API SynthPlugin::initialize(FUnknown* context) {
    tresult r = SingleComponentEffect::initialize(context);
    if (r != kResultOk) return r;
    addEventInput(STR16("MIDI In"), 16);
    addAudioInput(STR16("Sidechain"), SpeakerArr::kStereo, BusTypes::kAux, 0);  // route a kick here (Charon, SIDECHAIN source)
    addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);
    for (int id : kDisplayOrder) {
        parameters.addParameter(new TableParameter(kParams[id]));
        norm[id] = toNorm(kParams[id], kParams[id].def);
    }
    applyInitPatch();
    if (!libThread.joinable()) libThread = std::thread([this] { libLoop(); });
    return kResultOk;
}

// A new Morpheus opens on ORPHEO · Dream Pad (the face's first preset, finished with Juno chorus and the
// ORPHEO space). The parameters' own defaults stay neutral, so double-click still resets a knob to Init.
void SynthPlugin::applyInitPatch() {
    struct V { const char* name; double plain; };
    static const V patch[] = {
        {"Position", 30}, {"Position Env", 35}, {"Unison", 5}, {"Detune", 18}, {"Spread", 80}, {"Sub", 20},
        {"Cutoff", 2500}, {"Resonance", 25}, {"Filter Env", 35}, {"Attack", 400}, {"Decay", 1500}, {"Sustain", 70},
        {"Release", 1800}, {"Mod Attack", 600}, {"Mod Decay", 3000}, {"Velocity", 40}, {"Drive", 1.5}, {"Output", 0},
        {"LFO1 Rate", .15}, {"LFO1 Mode", 1}, {"LFO2 Shape", 6}, {"LFO2 Rate", .4},
        {"Chorus On", 1}, {"Chorus Mode", 0}, {"Chorus Mix", 45},
        {"Reverb On", 1}, {"Reverb Style", 6}, {"Reverb Decay", 3.5}, {"Reverb Size", 120}, {"Reverb Mix", 28}};
    // DREAM sweeps the table, BLOOM opens the filter, MOTION widens, GRIT drives; LFOs breathe; Drift detunes a little
    static const double routes[][3] = {{15, 1, 45}, {16, 3, 40}, {16, 4, 15}, {17, 7, 45}, {17, 8, 30}, {18, 6, 60}, {4, 1, 12}, {5, 3, 10}, {14, 2, .3}};
    auto set = [&](const std::string& name, double plain) {
        for (const auto& d : kParams)
            if (name == d.name) { norm[d.id] = toNorm(d, plain); setParamNormalized(d.id, norm[d.id]); return; }
    };
    for (const auto& v : patch) set(v.name, v.plain);
    for (int k = 0; k < (int)(sizeof routes / sizeof routes[0]); ++k) {
        const std::string m = "Mod " + std::to_string(k + 1);
        set(m + " Source", routes[k][0]); set(m + " Dest", routes[k][1]); set(m + " Amount", routes[k][2]);
    }
    applyParams();
}

tresult PLUGIN_API SynthPlugin::setBusArrangements(SpeakerArrangement* in, int32 ni, SpeakerArrangement* out, int32 no) {
    if (no == 1 && out[0] == SpeakerArr::kStereo && (ni == 0 || (ni == 1 && (in[0] == SpeakerArr::kStereo || in[0] == SpeakerArr::kMono))))
        return SingleComponentEffect::setBusArrangements(in, ni, out, no);
    return kResultFalse;
}

tresult PLUGIN_API SynthPlugin::canProcessSampleSize(int32 s) { return s == kSample32 ? kResultTrue : kResultFalse; }

tresult PLUGIN_API SynthPlugin::setupProcessing(ProcessSetup& setup) {
    sampleRate = setup.sampleRate;
    return SingleComponentEffect::setupProcessing(setup);
}

tresult PLUGIN_API SynthPlugin::setActive(TBool state) {
    if (state) {
        synth.prepare(sampleRate);
        rack.prepare(sampleRate);
        scFollow.prepare(sampleRate, 2, 150);
        applyParams();
        rack.reset();
    }
    return SingleComponentEffect::setActive(state);
}

// Morph: corners A (top left), B (top right), C (bottom left), D (bottom right), bilinear in Morph X/Y.
// Continuous parameters blend in normalized space (so log knobs morph evenly); switches and routes' source /
// destination come from the nearest corner. An empty corner stands for the knobs as they are.
bool SynthPlugin::blendInto(const double* base, double* out) const {
    bool has[4], any = false;
    for (int c = 0; c < 4; ++c) any |= has[c] = hasSnap[c].load(std::memory_order_acquire);
    if (base[kMorphOn] < 0.5 || !any) { for (int i = 0; i < kNumParams; ++i) out[i] = base[i]; return false; }
    const double x = base[kMorphX], y = base[kMorphY];
    const double w[4] = {(1 - x) * y, x * y, (1 - x) * (1 - y), x * (1 - y)};
    int best = 0;
    for (int c = 1; c < 4; ++c) if (w[c] > w[best] + 1e-9) best = c;
    for (int id = 0; id < kNumParams; ++id) {
        if (morphExcluded(id)) { out[id] = base[id]; continue; }
        if (kParams[id].map == Map::List) { out[id] = has[best] ? snap[best][id] : base[id]; continue; }
        double v = 0;
        for (int c = 0; c < 4; ++c) v += w[c] * (has[c] ? snap[c][id] : base[id]);
        out[id] = v;
    }
    return true;
}

void SynthPlugin::computeEffective() { morphActive = blendInto(norm.data(), eff.data()); }

void SynthPlugin::applyParams() {
    computeEffective();
    auto plain = [&](int id) { return toPlain(kParams[id], eff[id]); };
    wt::Params p;
    p.position = plain(kPosition) / 100;
    p.unison = (int)plain(kUnison);
    p.detuneCents = plain(kDetune);
    p.spread = plain(kSpread) / 100;
    p.sub = plain(kSub) / 100;
    p.cutoffHz = plain(kCutoff);
    p.resonance = plain(kResonance) / 100;
    p.filterEnv = plain(kFilterEnv) / 100;
    p.posEnv = plain(kPosEnv) / 100;
    p.ampA = plain(kAttack) / 1000;
    p.ampD = plain(kDecay) / 1000;
    p.ampS = plain(kSustain) / 100;
    p.ampR = plain(kRelease) / 1000;
    p.modA = plain(kModAttack) / 1000;
    p.modD = plain(kModDecay) / 1000;
    p.velocity = plain(kVelocity) / 100;
    p.outputDb = plain(kOutput);
    p.scanMode = (int)plain(kScan);
    p.scanSpeed = plain(kScanSpeed);
    p.filterType = (int)plain(kFilterType);
    p.filterMorph = plain(kFilterMorph) / 100;
    p.filterDriveDb = plain(kFilterDrive);
    p.hpf = (int)plain(kHpf);
    p.keytrack = plain(kKeytrack) / 100;
    for (int s = 0; s < wt::kModSlots; ++s)
        p.slots[s] = {(int)plain(kModBase + 3 * s), (int)plain(kModBase + 3 * s + 1), plain(kModBase + 3 * s + 2) / 100};
    for (int q = 0; q < wt::kLfos; ++q) {
        auto& l = p.lfo[q];
        l.shape = (int)plain(kLfoBase + 5 * q);
        l.hz = plain(kLfoBase + 5 * q + 1);
        l.sync = plain(kLfoBase + 5 * q + 2) > 0.5;
        l.note = (int)plain(kLfoBase + 5 * q + 3);
        l.mode = (int)plain(kLfoBase + 5 * q + 4);
    }
    p.env3A = plain(kEnv3A) / 1000;
    p.env3D = plain(kEnv3D) / 1000;
    p.env3S = plain(kEnv3S) / 100;
    p.env3R = plain(kEnv3R) / 1000;
    for (int q = 0; q < 8; ++q) p.macro[q] = plain(kMacro1 + q) / 100;
    p.modWheel = plain(kModWheel) / 100;
    p.pitchBend = plain(kPitchBend) / 100;
    p.aftertouch = plain(kAftertouch) / 100;
    p.bendRange = plain(kBendRange);
    p.voiceMode = (int)plain(kVoiceMode);
    p.glide = plain(kGlide) / 1000;
    p.levelA = plain(kOscALevel) / 100;
    p.levelB = plain(kOscBLevel) / 100;
    p.positionB = plain(kPositionB) / 100;
    p.octB = (int)plain(kOctB);
    p.semiB = (int)plain(kSemiB);
    p.fineB = plain(kFineB);
    p.warpA = (int)plain(kWarpA);
    p.warpAmtA = plain(kWarpAmtA) / 100;
    p.warpB = (int)plain(kWarpB);
    p.warpAmtB = plain(kWarpAmtB) / 100;
    p.noise = plain(kNoise) / 100;
    p.noiseColor = (int)plain(kNoiseColor);
    synth.update(p);
    if (auto* t_ = pickTable((int)plain(kTable))) synth.setTable(t_);
    if (auto* t_ = pickTable((int)plain(kTableB))) synth.setTableB(t_);
    fx::FxParams fp;
    fp.order = (int)plain(kFxOrder);
    fp.drive = plain(kDrive);
    fp.driveStyle = (int)plain(kDriveStyle);
    fp.driveMix = plain(kDriveMix) / 100;
    fp.chorusOn = plain(kChorusOn) > 0.5;
    fp.chorusMode = (int)plain(kChorusMode);
    fp.chorusMix = plain(kChorusMix) / 100;
    fp.echoOn = plain(kEchoOn) > 0.5;
    fp.echoStyle = (int)plain(kEchoStyle);
    fp.echoMode = (int)plain(kEchoMode);
    fp.echoSync = plain(kEchoSync) > 0.5;
    fp.noteL = (int)plain(kEchoNoteL);
    fp.noteR = (int)plain(kEchoNoteR);
    fp.timeL = plain(kEchoTimeL);
    fp.timeR = plain(kEchoTimeR);
    fp.echoFb = plain(kEchoFeedback) / 100;
    fp.echoMix = plain(kEchoMix) / 100;
    fp.verbOn = plain(kVerbOn) > 0.5;
    fp.verbStyle = (int)plain(kVerbStyle);
    fp.verbDecay = plain(kVerbDecay);
    fp.verbSize = plain(kVerbSize) / 100;
    fp.verbMix = plain(kVerbMix) / 100;
    fp.pumpOn = plain(kPumpOn) > 0.5;
    fp.pumpMode = (int)plain(kPumpMode);
    fp.pumpRate = (int)plain(kPumpRate);
    fp.pumpDepth = plain(kPumpDepth) / 100;
    fp.pumpMix = plain(kPumpMix) / 100;
    rack.update(fp, hostTempo);
}

// a built-in table, or the grown one (ORPHEO if nothing has been grown yet)
// nullptr = a library table that is still being built (keep playing the current one; it arrives in ~0.1 s)
const wt::Wavetable* SynthPlugin::pickTable(int ti) const {
    if (ti >= kTableLibrary) {
        if (ti >= kTableLibrary + wt::forge::count() || ti >= kTableSlots) return &wt::factoryTable(0);
        if (const auto* t = lib[ti].ptr.load(std::memory_order_acquire)) return t;
        lib[ti].want.store(true, std::memory_order_release);
        return nullptr;
    }
    const wt::Wavetable* grown = ti == kTableGrown ? customPtr.load(std::memory_order_acquire) : nullptr;
    return grown ? grown : &wt::factoryTable(ti == kTableGrown ? 0 : ti);
}

void SynthPlugin::libLoop() {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    std::vector<std::pair<std::unique_ptr<wt::Wavetable>, double>> retiredLib;  // kept 3 s after eviction
    while (!libQuit.load()) {
        const double now = std::chrono::duration<double>(clock::now() - t0).count();
        const int a = (int)toPlain(kParams[kTable], getParamNormalized(kTable)), b = (int)toPlain(kParams[kTableB], getParamNormalized(kTableB));
        for (int i : {a, b}) if (i >= kTableLibrary && i < kTableSlots) { lib[i].lastUse = now; if (!lib[i].ptr.load()) lib[i].want = true; }
        // build what was asked for
        for (int i = kTableLibrary; i < kTableLibrary + wt::forge::count() && !libQuit.load(); ++i) {
            if (!lib[i].want.load(std::memory_order_acquire) || lib[i].ptr.load()) { lib[i].want = false; continue; }
            auto t = std::make_unique<wt::Wavetable>();
            t->build(wt::forge::recipes()[i - kTableLibrary].make());
            lib[i].table = std::move(t);
            lib[i].lastUse = now;
            lib[i].ptr.store(lib[i].table.get(), std::memory_order_release);
            lib[i].want = false;
            // an atomic pointer swap, safe from here: the table switches even when no audio is running
            if ((int)toPlain(kParams[kTable], getParamNormalized(kTable)) == i) synth.setTable(lib[i].table.get());
            if ((int)toPlain(kParams[kTableB], getParamNormalized(kTableB)) == i) synth.setTableB(lib[i].table.get());
            tableDirty = true;
        }
        // keep at most kLibKeep built: evict the least recently used one that nothing plays
        for (;;) {
            int built = 0, victim = -1;
            for (int i = kTableLibrary; i < kTableSlots; ++i) {
                if (!lib[i].ptr.load()) continue;
                ++built;
                const auto* p = lib[i].ptr.load();
                const bool playing = i == a || i == b || p == &synth.wavetable() || p == &synth.wavetableB();
                if (!playing && (victim < 0 || lib[i].lastUse < lib[victim].lastUse)) victim = i;
            }
            if (built <= kLibKeep || victim < 0) break;
            lib[victim].ptr.store(nullptr, std::memory_order_release);
            retiredLib.emplace_back(std::move(lib[victim].table), now);
        }
        retiredLib.erase(std::remove_if(retiredLib.begin(), retiredLib.end(), [&](auto& r) {  // never free a table that is still playing
            return now - r.second > 3 && r.first.get() != &synth.wavetable() && r.first.get() != &synth.wavetableB(); }), retiredLib.end());
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
}

void SynthPlugin::render(float* L, float* R, int from, int to) {
    if (to <= from) return;
    synth.process(L + from, R + from, to - from);
}

tresult PLUGIN_API SynthPlugin::process(ProcessData& data) {
    bool changed = false;
    if (data.inputParameterChanges) {
        int32 n = data.inputParameterChanges->getParameterCount();
        for (int32 i = 0; i < n; ++i) {
            IParamValueQueue* q = data.inputParameterChanges->getParameterData(i);
            if (!q) continue;
            int32 pts = q->getPointCount(), off;
            ParamValue v;
            ParamID id = q->getParameterId();
            if (pts > 0 && id < (ParamID)kNumParams && q->getPoint(pts - 1, off, v) == kResultTrue) { norm[id] = v; changed = true; }
        }
    }
    if (tableDirty.exchange(false)) changed = true;
    if (panicReq.exchange(false)) { synth.reset(); rack.reset(); }
    if (auto* ctx = data.processContext) {  // tempo + song position for synced LFOs
        const bool tempoOk = (ctx->state & ProcessContext::kTempoValid) != 0;
        const bool posOk = (ctx->state & ProcessContext::kProjectTimeMusicValid) != 0;
        synth.setTransport(tempoOk ? ctx->tempo : 120, posOk ? ctx->projectTimeMusic : 0, posOk && (ctx->state & ProcessContext::kPlaying));
        if (tempoOk) { hostTempo = ctx->tempo; rack.setTempo(hostTempo); }
        // follow the song only while it plays: a stopped transport keeps reporting the same position, which froze
        // Charon's pump clock (no ducking while you audition a sound). Stopped, the FX clock runs on at the tempo.
        if (posOk && (ctx->state & ProcessContext::kPlaying)) fxBeat = ctx->projectTimeMusic;
    }
    if (changed) applyParams();
    if (data.numOutputs == 0 || data.numSamples <= 0) return kResultOk;

    AudioBusBuffers& out = data.outputs[0];
    float* L = out.channelBuffers32[0];
    float* R = out.numChannels > 1 ? out.channelBuffers32[1] : L;
    const int n = data.numSamples;
    std::memset(L, 0, sizeof(float) * n);
    if (R != L) std::memset(R, 0, sizeof(float) * n);
    // sidechain input (aux bus): its envelope is a mod source, and Charon can duck to it
    const float *scL = nullptr, *scR = nullptr;
    if (data.numInputs > 0 && data.inputs[0].numChannels > 0 && !(data.inputs[0].silenceFlags & 1)) {
        scL = data.inputs[0].channelBuffers32[0];
        scR = data.inputs[0].numChannels > 1 ? data.inputs[0].channelBuffers32[1] : scL;
    }
    synth.setSidechain(scL ? scFollow.run(scL, scR, n) : scFollow.silence(n));

    // notes, sample-accurate: render up to each event, then apply it
    int pos = 0;
    if (IEventList* ev = data.inputEvents) {
        const int32 count = ev->getEventCount();
        for (int32 i = 0; i < count; ++i) {
            Event e;
            if (ev->getEvent(i, e) != kResultOk) continue;
            const int at = std::clamp((int)e.sampleOffset, pos, n);
            render(L, R, pos, at);
            pos = at;
            if (e.type == Event::kNoteOnEvent) {
                if (e.noteOn.velocity > 0) synth.noteOn(e.noteOn.pitch, e.noteOn.velocity);
                else synth.noteOff(e.noteOn.pitch);
            } else if (e.type == Event::kNoteOffEvent) {
                synth.noteOff(e.noteOff.pitch);
            } else if (e.type == Event::kPolyPressureEvent) {
                synth.polyPressure(e.polyPressure.pitch, e.polyPressure.pressure);
            }
        }
    }
    render(L, R, pos, n);
    {
        const double bps = hostTempo / 60.0 / sampleRate;
        rack.process(L, R, scL, scR, n, fxBeat, bps);
        fxBeat += n * bps;
    }
    float pk = 0;
    for (int i = 0; i < n; ++i) {
        L[i] = wt::Synth::safety(L[i]);
        if (R != L) R[i] = wt::Synth::safety(R[i]);
        pk = std::max(pk, std::max(std::abs(L[i]), std::abs(R[i])));
    }
    if (pk > mOut.load(std::memory_order_relaxed)) mOut.store(pk, std::memory_order_relaxed);
    out.silenceFlags = synth.activeVoices() == 0 && !rack.anyOn() ? 3 : 0;
    return kResultOk;
}

tresult PLUGIN_API SynthPlugin::queryInterface(const TUID iid, void** obj) {
    QUERY_INTERFACE(iid, obj, IMidiMapping::iid, IMidiMapping)
    return SingleComponentEffect::queryInterface(iid, obj);
}

tresult PLUGIN_API SynthPlugin::getMidiControllerAssignment(int32 busIndex, int16, CtrlNumber cc, ParamID& id) {
    if (busIndex != 0) return kResultFalse;
    switch (cc) {
        case kCtrlModWheel: id = kModWheel; return kResultTrue;
        case kAfterTouch: id = kAftertouch; return kResultTrue;
        case kPitchBend: id = kPitchBend; return kResultTrue;
    }
    return kResultFalse;
}

SynthPlugin::~SynthPlugin() {
    libQuit = true;
    if (libThread.joinable()) libThread.join();
    if (worker.joinable()) worker.join();
    synth.setTable(nullptr);
}

void SynthPlugin::onEditorMessage(const std::string& type, const std::string& name, const std::string& data, double value) {
    if (type == "audio" && !data.empty() && value > 1000) {  // {name: file name, data: base64 float32 mono, value: sample rate}
        if (growing.exchange(true)) return;  // one at a time
        if (worker.joinable()) worker.join();
        worker = std::thread([this, audio = decodeFloats(data), sr = value, name]() mutable {
            growTable(std::move(audio), sr, name);
            growing = false;
        });
    } else if (type == "wavetable" && !data.empty() && value >= 16) {  // {name, data: base64 float32 cycles, value: cycle length}
        if (growing.exchange(true)) return;
        if (worker.joinable()) worker.join();
        worker = std::thread([this, audio = decodeFloats(data), cycle = (int)value, name]() mutable {
            auto spectra = wt::cyclesToSpectra(audio.data(), (int)audio.size(), cycle);
            if (spectra.size() >= 2) {
                char buf[64];
                std::snprintf(buf, sizeof buf, "WAVETABLE %d FRAMES", (int)spectra.size());
                const auto dot = name.find_last_of('.');
                installTable(std::move(spectra), upper(dot == std::string::npos ? name : name.substr(0, dot)).substr(0, 28), buf, audio.size() / 48000.0);
            }
            growing = false;
        });
    } else if (type == "export") {  // write the table OSC A is playing as a .wav wavetable
        const char* home = std::getenv("HOME");
        const std::string dir = std::string(home ? home : "/tmp") + "/Music/Morpheus/Tables";
        std::string nm = name.empty() ? "MORPHEUS TABLE" : name;
        for (auto& c : nm) if (!std::isalnum((unsigned char)c) && c != ' ' && c != '-') c = '_';
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const bool ok = !ec && wt::writeWavetableWav(dir + "/" + nm + ".wav", wt::tableToCycles(synth.wavetable()));
        std::lock_guard<std::mutex> lock(tableMx);
        exportNote = ok ? "SAVED " + nm + ".WAV IN MUSIC/MORPHEUS/TABLES" : "COULDN'T SAVE THE TABLE";
    } else if (type == "factory") {
        useFactoryTable();
    } else if (type == "panic") {
        panicReq = true;
    } else if (type == "snap" && value >= 0 && value < 4) {  // capture what you hear into a corner
        const int c = (int)value;
        std::array<double, kNumParams> base, heard;
        for (int id = 0; id < kNumParams; ++id) base[id] = getParamNormalized(id);
        blendInto(base.data(), heard.data());
        snap[c] = heard;
        hasSnap[c].store(true, std::memory_order_release);
        tableDirty = true;  // re-blend at the next block
    } else if (type == "snapclear" && value >= 0 && value < 4) {
        hasSnap[(int)value].store(false, std::memory_order_release);
        tableDirty = true;
    } else if (type == "snapload" && value >= 0 && value < 4 && hasSnap[(int)value].load()) {  // a corner into the knobs (morph off)
        const int c = (int)value;
        for (int id = 0; id < kNumParams; ++id) {
            if (morphExcluded(id)) continue;
            const double nv = snap[c][id];
            beginEdit(id); performEdit(id, nv); setParamNormalized(id, nv); endEdit(id);
        }
        beginEdit(kMorphOn); performEdit(kMorphOn, 0); setParamNormalized(kMorphOn, 0); endEdit(kMorphOn);
        if (auto* t_ = pickTable((int)toPlain(kParams[kTable], snap[c][kTable]))) synth.setTable(t_);
        if (auto* t_ = pickTable((int)toPlain(kParams[kTableB], snap[c][kTableB]))) synth.setTableB(t_);
    } else if (type == "usertable" && !name.empty()) {  // a .wav from ~/Music/Morpheus/Tables, by name
        if (name.find('/') != std::string::npos || name.find("..") != std::string::npos) return;
        const char* home = std::getenv("HOME");
        const std::string path = std::string(home ? home : "/tmp") + "/Music/Morpheus/Tables/" + name + ".wav";
        if (worker.joinable()) worker.join();
        worker = std::thread([this, path, name] {
            std::vector<float> mono;
            int cycle = 0;
            if (!wt::readWavetableWav(path, mono, cycle)) { std::lock_guard<std::mutex> lock(tableMx); exportNote = "COULDN'T READ THAT TABLE"; return; }
            auto spectra = wt::cyclesToSpectra(mono.data(), (int)mono.size(), cycle);
            if (spectra.size() < 2) return;
            char buf[64];
            std::snprintf(buf, sizeof buf, "YOUR TABLE %d FRAMES", (int)spectra.size());
            std::string up = name;
            for (auto& c : up) c = (char)std::toupper((unsigned char)c);
            installTable(std::move(spectra), up.substr(0, 28), buf, 0);
        });
    } else if (type == "pname") {  // the face picked a preset: remember its name with the set
        std::string nm;
        for (char c : name) if ((unsigned char)c >= 32 && c != '"' && c != '\\') nm += c;
        std::lock_guard<std::mutex> lock(tableMx);
        presetName = nm.substr(0, 64);
    }
}

void SynthPlugin::growTable(std::vector<float> audio, double sr, std::string name) {
    wt::ResynthInfo info;
    auto spectra = wt::resynthesize(audio.data(), (int)audio.size(), sr, info);
    char buf[64];
    if (info.pitched) std::snprintf(buf, sizeof buf, "PITCHED %.0f HZ", info.f0);
    else std::snprintf(buf, sizeof buf, "RAW CYCLES");
    const auto dot = name.find_last_of('.');
    installTable(std::move(spectra), upper(dot == std::string::npos ? name : name.substr(0, dot)).substr(0, 28), buf, info.seconds);
}

void SynthPlugin::installTable(std::vector<wt::Spectrum> spectra, std::string name, std::string info, double seconds) {
    for (std::string* t : {&name, &info})  // shown in the face as JSON text: keep it plain
        for (auto& c : *t) if (!std::isalnum((unsigned char)c) && c != ' ' && c != '-' && c != '.') c = ' ';
    for (auto& s : spectra) for (int h = kSavedHarmonics + 1; h <= wt::kMaxHarm; ++h) s[h] = 0;  // what the state can hold
    auto t = std::make_unique<wt::Wavetable>();
    t->build(spectra);
    t->seconds = seconds;
    std::lock_guard<std::mutex> lock(tableMx);
    retired = std::move(custom);  // the audio thread may still be finishing a block with it
    custom = std::move(t);
    customPtr.store(custom.get(), std::memory_order_release);
    customSpectra = std::move(spectra);
    customSeconds = seconds;
    tableName = std::move(name);
    tableInfo = std::move(info);
    tableDirty = true;  // the audio thread re-picks the table at its next block
    if ((int)toPlain(kParams[kTable], getParamNormalized(kTable)) == kTableGrown) synth.setTable(custom.get());
    if ((int)toPlain(kParams[kTableB], getParamNormalized(kTableB)) == kTableGrown) synth.setTableB(custom.get());
}

void SynthPlugin::useFactoryTable() {
    std::lock_guard<std::mutex> lock(tableMx);
    customPtr.store(nullptr, std::memory_order_release);
    retired = std::move(custom);
    customSpectra.clear();
    customSeconds = 0;
    tableName = "ORPHEO";
    tableInfo = "FACTORY";
    tableDirty = true;
    if (auto* t_ = pickTable((int)toPlain(kParams[kTable], getParamNormalized(kTable)))) synth.setTable(t_);
    if (auto* t_ = pickTable((int)toPlain(kParams[kTableB], getParamNormalized(kTableB)))) synth.setTableB(t_);
}

IPlugView* PLUGIN_API SynthPlugin::createView(FIDString name) {
    if (!FIDStringsEqual(name, ViewType::kEditor)) return nullptr;
    sendTable = true;  // a fresh page needs the wavetable once
    WebEditor::Hooks h;
    h.unit = "morpheus";
    h.page = "morpheus";
    h.feed = [this] { return feedJson(); };
    h.setParam = [this](const std::string& n, double v) { setFromEditor(n, v); };
    h.message = [this](const std::string& t, const std::string& n, const std::string& d, double v) { onEditorMessage(t, n, d, v); };
    return new WebEditor(std::move(h), 1280, 800);
}

void SynthPlugin::setFromEditor(const std::string& name, double plain) {
    for (int id = 0; id < kNumParams; ++id) {
        if (name != kParams[id].name) continue;
        double nv = toNorm(kParams[id], plain);
        beginEdit(id); performEdit(id, nv); setParamNormalized(id, nv); endEdit(id);
        // switching tables is an atomic pointer swap, safe from here: don't wait for the audio thread
        // (a sleeping device in Live, or a host without audio, would otherwise never switch)
        if (id == kTable) if (auto* t_ = pickTable((int)toPlain(kParams[kTable], nv))) synth.setTable(t_);
        if (id == kTableB) if (auto* t_ = pickTable((int)toPlain(kParams[kTableB], nv))) synth.setTableB(t_);
        return;
    }
}

std::string SynthPlugin::feedJson() {
    std::string s = "{\"p\":{";
    char buf[160];
    std::array<double, kNumParams> base, blend;  // morphing: show the blend (what plays), computed from the knobs here
    for (int id = 0; id < kNumParams; ++id) base[id] = getParamNormalized(id);
    blendInto(base.data(), blend.data());
    for (int id = 0; id < kNumParams; ++id) {
        const double nv = blend[id];
        std::snprintf(buf, sizeof buf, "%s\"%s\":%.6g", id ? "," : "", kParams[id].name, toPlain(kParams[id], nv));
        s += buf;
    }
    s += "},\"v\":[";
    bool first = true;
    for (auto& v : synth.views) {
        const int note = v.note.load(std::memory_order_relaxed);
        if (note < 0) continue;
        std::snprintf(buf, sizeof buf, "%s[%d,%.4f,%.4f,%.4f,%.4f]", first ? "" : ",", note, v.pos.load(std::memory_order_relaxed),
                      v.amp.load(std::memory_order_relaxed), v.mod.load(std::memory_order_relaxed), v.posB.load(std::memory_order_relaxed));
        s += buf;
        first = false;
    }
    std::snprintf(buf, sizeof buf, "],\"out\":%.5g,\"busy\":%d,\"lfo\":[", mOut.exchange(0), growing.load() ? 1 : 0);
    s += buf;
    for (int q = 0; q < wt::kLfos; ++q) {
        std::snprintf(buf, sizeof buf, "%s[%.4f,%.4f]", q ? "," : "", synth.lfoView[q].load(std::memory_order_relaxed), synth.lfoPhaseView[q].load(std::memory_order_relaxed));
        s += buf;
    }
    s += "]";
    const wt::Wavetable* shown = &synth.wavetable();
    {
        std::lock_guard<std::mutex> lock(tableMx);
        std::string tn = tableName, ti = tableInfo;
        if (shown != customPtr.load()) {
            for (int i = 0; i < wt::kFactoryTables; ++i)
                if (shown == &wt::factoryTable(i)) { tn = wt::kFactoryNames[i]; std::snprintf(buf, sizeof buf, "BUILT-IN %d FRAMES", shown->frames); ti = buf; }
            for (int i = 0; i < wt::forge::count(); ++i)
                if (shown == lib[kTableLibrary + i].ptr.load()) { tn = wt::forge::recipes()[i].name; std::snprintf(buf, sizeof buf, "%s %d FRAMES", wt::forge::recipes()[i].family, shown->frames); ti = buf; }
        }
        s += ",\"tn\":\"" + tn + "\",\"ti\":\"" + ti + "\",\"grown\":\"" + (custom ? tableName : std::string()) + "\"";
        if (!exportNote.empty()) { s += ",\"note\":\"" + exportNote + "\""; exportNote.clear(); }
        // the library (once per page), whether a table is still being built, and the tables in your folder
        if (sendTable || libFeed++ % 40 == 0) {  // also resent each second: a page may not be listening yet at first
            s += ",\"lib\":[";
            for (int i = 0; i < wt::forge::count(); ++i) { s += i ? "," : ""; s += "[\""; s += wt::forge::recipes()[i].name; s += "\",\""; s += wt::forge::recipes()[i].family; s += "\"]"; }
            s += "]";
        }
        bool building = false;
        for (int i = kTableLibrary; i < kTableSlots; ++i) building |= lib[i].want.load() && !lib[i].ptr.load();
        s += building ? ",\"tb\":1" : ",\"tb\":0";
        if (++userScan % 80 == 1) {  // ~every 2 s: ~/Music/Morpheus/Tables
            userTables.clear();
            const char* home = std::getenv("HOME");
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(std::string(home ? home : "/tmp") + "/Music/Morpheus/Tables", ec)) {
                const auto p = e.path();
                std::string ext = p.extension().string();
                for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
                if (ext != ".wav") continue;
                std::string nm = p.stem().string();
                for (auto& c : nm) if (c == '"' || c == '\\' || (unsigned char)c < 32 || (unsigned char)c > 126) c = '_';
                userTables.push_back(nm);
                if (userTables.size() >= 200) break;
            }
            std::sort(userTables.begin(), userTables.end());
        }
        s += ",\"ut\":[";
        for (size_t i = 0; i < userTables.size(); ++i) { s += i ? ",\"" : "\""; s += userTables[i]; s += "\""; }
        s += "]";
        s += ",\"sn\":[";
        for (int c = 0; c < 4; ++c) { s += c ? "," : ""; s += hasSnap[c].load() ? "1" : "0"; }
        s += "],\"pn\":\"";
        for (size_t i = 0; i < presetName.size();) {  // non-ASCII as \uXXXX, so the bridge's string encoding can't garble it
            const unsigned char c = (unsigned char)presetName[i];
            uint32_t cp = c; int len = 1;
            if (c >= 0xF0) { cp = c & 7; len = 4; } else if (c >= 0xE0) { cp = c & 15; len = 3; } else if (c >= 0xC0) { cp = c & 31; len = 2; }
            for (int k = 1; k < len && i + k < presetName.size(); ++k) cp = (cp << 6) | ((unsigned char)presetName[i + k] & 63);
            i += len;
            if (cp < 128) s += (char)cp;
            else if (cp < 0x10000) { char u[8]; std::snprintf(u, sizeof u, "\\u%04x", (unsigned)cp); s += u; }
        }
        s += "\"";
    }
    // tables for the face: frames x 128 points, from a smooth (64-harmonic) level; sent when they change
    auto tableJson = [&](const char* key, const wt::Wavetable& t) {
        s += ",\""; s += key; s += "\":[";
        for (int f = 0; f < t.frames; ++f) {
            const float* d = t.at(f, 4);
            s += f ? ",[" : "[";
            for (int k = 0; k < 128; ++k) { std::snprintf(buf, sizeof buf, "%s%.3f", k ? "," : "", d[k * wt::kTableSize / 128]); s += buf; }
            s += "]";
        }
        s += "]";
    };
    const wt::Wavetable* shownB = &synth.wavetableB();
    const bool fresh = sendTable.exchange(false);
    if (fresh || shown != lastShown) { lastShown = shown; tableJson("wt", *shown); }
    if (fresh || shownB != lastShownB) {
        lastShownB = shownB; tableJson("wtB", *shownB);
        std::string nb = "ORPHEO";
        if (shownB == customPtr.load()) { std::lock_guard<std::mutex> lock(tableMx); nb = tableName; }
        else for (int i = 0; i < wt::kFactoryTables; ++i) if (shownB == &wt::factoryTable(i)) nb = wt::kFactoryNames[i];
        s += ",\"tnB\":\"" + nb + "\"";
    }
    s += "}";
    return s;
}

tresult PLUGIN_API SynthPlugin::setState(IBStream* state) {
    IBStreamer s(state, kLittleEndian);
    uint32 version = 0, count = 0;
    if (!s.readInt32u(version) || !s.readInt32u(count)) return kResultFalse;
    for (uint32 i = 0; i < count; ++i) {
        int32 id; double v;
        if (!s.readInt32(id) || !s.readDouble(v)) return kResultFalse;
        if (id >= 0 && id < kNumParams) { norm[id] = v; setParamNormalized(id, v); }
    }
    // Before v4 the Source/Dest switches were as long as their lists (sources 23; destinations 13 in the
    // M2 build = 112 parameters, 18 in the M3 build = 125): re-express them on today's fixed-length switches.
    if (version < 4 && (count == 112 || count == 125)) {
        const double oldSrc = 22, oldDst = count == 112 ? 12 : 17;
        for (int k = 0; k < wt::kModSlots; ++k) {
            const int si = kModBase + 3 * k, di = si + 1;
            norm[si] = std::round(norm[si] * oldSrc) / (wt::kSourceSlots - 1);
            norm[di] = std::round(norm[di] * oldDst) / (wt::kDestSlots - 1);
            setParamNormalized(si, norm[si]);
            setParamNormalized(di, norm[di]);
        }
    }
    // Before v7 the Table switches had 6 entries (0..5): re-express them on the 128-slot switches
    auto oldTable = [](double n) { return std::round(n * 5) / (kTableSlots - 1); };
    if (version < 7) for (int id : {(int)kTable, (int)kTableB}) { norm[id] = oldTable(norm[id]); setParamNormalized(id, norm[id]); }
    applyParams();
    int32 hasTable = 0;
    if (version >= 2 && s.readInt32(hasTable) && hasTable) {
        int32 nameLen = 0, frames = 0, harm = 0;
        if (!s.readInt32(nameLen) || nameLen < 0 || nameLen > 256) return kResultFalse;
        std::string name(nameLen, ' ');
        for (auto& c : name) { int8 ch; if (!s.readInt8(ch)) return kResultFalse; c = (char)ch; }
        if (!s.readInt32(frames) || !s.readInt32(harm) || frames != wt::kFrames || harm < 1 || harm > wt::kMaxHarm) return kResultFalse;
        std::vector<wt::Spectrum> spectra(frames);
        for (auto& sp : spectra) {
            sp.fill(0.0);
            for (int h = 1; h <= harm; ++h) { float re, im; if (!s.readFloat(re) || !s.readFloat(im)) return kResultFalse; sp[h] = {re, im}; }
        }
        std::string info;
        int32 infoLen = 0;
        if (s.readInt32(infoLen) && infoLen > 0 && infoLen < 256) { info.resize(infoLen); for (auto& c : info) { int8 ch; if (!s.readInt8(ch)) break; c = (char)ch; } }
        double seconds = 0;
        if (version >= 3) s.readDouble(seconds);
        installTable(std::move(spectra), name, info.empty() ? "GROWN" : info, seconds);
        if (version == 2) {  // v2 had no Table parameter: a saved grown table was what played
            norm[kTable] = toNorm(kParams[kTable], kTableGrown);
            setParamNormalized(kTable, norm[kTable]);
            applyParams();
        }
    } else if (version >= 2) {
        useFactoryTable();
    }
    int32 pnLen = 0;
    std::string pn;
    if (version >= 5 && s.readInt32(pnLen) && pnLen >= 0 && pnLen <= 256) { pn.resize(pnLen); for (auto& c : pn) { int8 ch; if (!s.readInt8(ch)) break; c = (char)ch; } }
    { std::lock_guard<std::mutex> lock(tableMx); presetName = pn; }  // older sets: no name (the face shows a dash)
    int32 mask = 0, cnt = 0;
    for (auto& h : hasSnap) h.store(false);
    if (version >= 6 && s.readInt32(mask) && s.readInt32(cnt) && cnt > 0 && cnt <= 4096) {
        for (int c = 0; c < 4; ++c) {
            if (!(mask & (1 << c))) continue;
            snap[c] = norm;  // parameters newer than the saved set keep the knobs' values
            for (int32 i = 0; i < cnt; ++i) { double v; if (!s.readDouble(v)) return kResultFalse; if (i < kNumParams) snap[c][i] = v; }
            if (version < 7) { snap[c][kTable] = oldTable(snap[c][kTable]); snap[c][kTableB] = oldTable(snap[c][kTableB]); }
            hasSnap[c].store(true);
        }
    }
    applyParams();
    return kResultOk;
}

tresult PLUGIN_API SynthPlugin::getState(IBStream* state) {
    IBStreamer s(state, kLittleEndian);
    s.writeInt32u(kStateVersion);
    s.writeInt32u(kNumParams);
    for (int i = 0; i < kNumParams; ++i) { s.writeInt32(i); s.writeDouble(getParamNormalized(i)); }
    std::lock_guard<std::mutex> lock(tableMx);
    s.writeInt32(customSpectra.empty() ? 0 : 1);
    if (!customSpectra.empty()) {
        s.writeInt32((int32)tableName.size());
        for (char c : tableName) s.writeInt8((int8)c);
        s.writeInt32((int32)customSpectra.size());
        s.writeInt32(kSavedHarmonics);
        for (const auto& sp : customSpectra)
            for (int h = 1; h <= kSavedHarmonics; ++h) { s.writeFloat((float)sp[h].real()); s.writeFloat((float)sp[h].imag()); }
        s.writeInt32((int32)tableInfo.size());
        for (char c : tableInfo) s.writeInt8((int8)c);
        s.writeDouble(customSeconds);
    }
    s.writeInt32((int32)presetName.size());
    for (char c : presetName) s.writeInt8((int8)c);
    int32 mask = 0;
    for (int c = 0; c < 4; ++c) if (hasSnap[c].load()) mask |= 1 << c;
    s.writeInt32(mask);
    s.writeInt32(kNumParams);
    for (int c = 0; c < 4; ++c)
        if (mask & (1 << c)) for (int i = 0; i < kNumParams; ++i) s.writeDouble(snap[c][i]);
    return kResultOk;
}

}  // namespace wtsynth
}  // namespace echolab
