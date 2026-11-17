#include "plugin.h"
#include "b64.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
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

constexpr uint32 kStateVersion = 2;  // 2: + grown wavetable
constexpr int kSavedHarmonics = 512;   // per frame, complex, as floats

std::string upper(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }

const DriveVoice& orpheoDrive() {
    for (const auto& v : kDriveVoices) if (!std::strcmp(v.name, "ORPHEO")) return v;
    return kDriveVoices[0];
}

}  // namespace

tresult PLUGIN_API SynthPlugin::initialize(FUnknown* context) {
    tresult r = SingleComponentEffect::initialize(context);
    if (r != kResultOk) return r;
    addEventInput(STR16("MIDI In"), 16);
    addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);
    for (int id : kDisplayOrder) {
        parameters.addParameter(new TableParameter(kParams[id]));
        norm[id] = toNorm(kParams[id], kParams[id].def);
    }
    return kResultOk;
}

tresult PLUGIN_API SynthPlugin::setBusArrangements(SpeakerArrangement* in, int32 ni, SpeakerArrangement* out, int32 no) {
    if (ni == 0 && no == 1 && out[0] == SpeakerArr::kStereo) return SingleComponentEffect::setBusArrangements(in, ni, out, no);
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
        drive.prepare(sampleRate);
        drive.setVoice(orpheoDrive());
        applyParams();
        drive.reset();
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
    synth.update(p);
    DriveParams d;
    d.drive = plain(kDrive);
    d.autoGain = true;
    driveOn = d.drive > 0.005;
    drive.update(d);
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
    if (changed) applyParams();
    if (data.numOutputs == 0 || data.numSamples <= 0) return kResultOk;

    AudioBusBuffers& out = data.outputs[0];
    float* L = out.channelBuffers32[0];
    float* R = out.numChannels > 1 ? out.channelBuffers32[1] : L;
    const int n = data.numSamples;
    std::memset(L, 0, sizeof(float) * n);
    if (R != L) std::memset(R, 0, sizeof(float) * n);

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
            }
        }
    }
    render(L, R, pos, n);
    if (driveOn) drive.process(L, R != L ? R : nullptr, n);
    float pk = 0;
    for (int i = 0; i < n; ++i) {
        L[i] = wt::Synth::safety(L[i]);
        if (R != L) R[i] = wt::Synth::safety(R[i]);
        pk = std::max(pk, std::max(std::abs(L[i]), std::abs(R[i])));
    }
    if (pk > mOut.load(std::memory_order_relaxed)) mOut.store(pk, std::memory_order_relaxed);
    out.silenceFlags = synth.activeVoices() == 0 && !driveOn ? 3 : 0;
    return kResultOk;
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
    installTable(std::move(spectra), upper(dot == std::string::npos ? name : name.substr(0, dot)).substr(0, 28), buf);
}

void SynthPlugin::installTable(std::vector<wt::Spectrum> spectra, std::string name, std::string info) {
    for (std::string* t : {&name, &info})  // shown in the face as JSON text: keep it plain
        for (auto& c : *t) if (!std::isalnum((unsigned char)c) && c != ' ' && c != '-' && c != '.') c = ' ';
    for (auto& s : spectra) for (int h = kSavedHarmonics + 1; h <= wt::kMaxHarm; ++h) s[h] = 0;  // what the state can hold
    auto t = std::make_unique<wt::Wavetable>();
    t->build(spectra);
    std::lock_guard<std::mutex> lock(tableMx);
    retired = std::move(custom);  // the audio thread may still be finishing a block with it
    custom = std::move(t);
    synth.setTable(custom.get());
    customSpectra = std::move(spectra);
    tableName = std::move(name);
    tableInfo = std::move(info);
    sendTable = true;
}

void SynthPlugin::useFactoryTable() {
    std::lock_guard<std::mutex> lock(tableMx);
    synth.setTable(nullptr);
    retired = std::move(custom);
    customSpectra.clear();
    tableName = "ORPHEO";
    tableInfo = "FACTORY";
    sendTable = true;
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
    return new WebEditor(std::move(h), 1180, 760);
}

void SynthPlugin::setFromEditor(const std::string& name, double plain) {
    for (int id = 0; id < kNumParams; ++id) {
        if (name != kParams[id].name) continue;
        double nv = toNorm(kParams[id], plain);
        beginEdit(id); performEdit(id, nv); setParamNormalized(id, nv); endEdit(id);
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
        std::snprintf(buf, sizeof buf, "%s[%d,%.4f,%.4f,%.4f]", first ? "" : ",", note, v.pos.load(std::memory_order_relaxed),
                      v.amp.load(std::memory_order_relaxed), v.mod.load(std::memory_order_relaxed));
        s += buf;
        first = false;
    }
    std::snprintf(buf, sizeof buf, "],\"out\":%.5g,\"busy\":%d", mOut.exchange(0), growing.load() ? 1 : 0);
    s += buf;
    {
        std::lock_guard<std::mutex> lock(tableMx);
        s += ",\"tn\":\"" + tableName + "\",\"ti\":\"" + tableInfo + "\"";
    }
    if (sendTable.exchange(false)) {  // 64 frames x 128 points, from a smooth (64-harmonic) level
        const auto& t = synth.wavetable();
        s += ",\"wt\":[";
        for (int f = 0; f < t.frames; ++f) {
            const float* d = t.at(f, 4);
            s += f ? ",[" : "[";
            for (int k = 0; k < 128; ++k) { std::snprintf(buf, sizeof buf, "%s%.3f", k ? "," : "", d[k * wt::kTableSize / 128]); s += buf; }
            s += "]";
        }
        s += "]";
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
        installTable(std::move(spectra), name, info.empty() ? "GROWN" : info);
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
    }
    return kResultOk;
}

}  // namespace wtsynth
}  // namespace echolab
