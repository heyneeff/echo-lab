#include "plugin.h"
#include "b64.h"
#include "WavetableFile.h"

#include <cctype>
#include <cstdlib>
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

constexpr uint32 kStateVersion = 4;  // 2: + grown wavetable, 3: + its length (SCAN), 4: mod switches sized for growth
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
    return kResultOk;
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

void SynthPlugin::applyParams() {
    auto plain = [&](int id) { return toPlain(kParams[id], norm[id]); };
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
    synth.setTable(pickTable((int)plain(kTable)));
    synth.setTableB(pickTable((int)plain(kTableB)));
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
const wt::Wavetable* SynthPlugin::pickTable(int ti) const {
    const wt::Wavetable* grown = ti == kTableGrown ? customPtr.load(std::memory_order_acquire) : nullptr;
    return grown ? grown : &wt::factoryTable(ti == kTableGrown ? 0 : ti);
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
    if (auto* ctx = data.processContext) {  // tempo + song position for synced LFOs
        const bool tempoOk = (ctx->state & ProcessContext::kTempoValid) != 0;
        const bool posOk = (ctx->state & ProcessContext::kProjectTimeMusicValid) != 0;
        synth.setTransport(tempoOk ? ctx->tempo : 120, posOk ? ctx->projectTimeMusic : 0, posOk && (ctx->state & ProcessContext::kPlaying));
        if (tempoOk) { hostTempo = ctx->tempo; rack.setTempo(hostTempo); }
        fxBeat = posOk ? ctx->projectTimeMusic : fxBeat;
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
    synth.setTable(pickTable((int)toPlain(kParams[kTable], getParamNormalized(kTable))));
    synth.setTableB(pickTable((int)toPlain(kParams[kTableB], getParamNormalized(kTableB))));
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
        if (id == kTable) synth.setTable(pickTable((int)toPlain(kParams[kTable], nv)));
        if (id == kTableB) synth.setTableB(pickTable((int)toPlain(kParams[kTableB], nv)));
        return;
    }
}

std::string SynthPlugin::feedJson() {
    std::string s = "{\"p\":{";
    char buf[160];
    for (int id = 0; id < kNumParams; ++id) {
        std::snprintf(buf, sizeof buf, "%s\"%s\":%.6g", id ? "," : "", kParams[id].name, toPlain(kParams[id], getParamNormalized(id)));
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
        }
        s += ",\"tn\":\"" + tn + "\",\"ti\":\"" + ti + "\",\"grown\":\"" + (custom ? tableName : std::string()) + "\"";
        if (!exportNote.empty()) { s += ",\"note\":\"" + exportNote + "\""; exportNote.clear(); }
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
    return kResultOk;
}

}  // namespace wtsynth
}  // namespace echolab
