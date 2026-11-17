#include "plugin.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "public.sdk/source/vst/vstparameters.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace echolab {
namespace reverb {

namespace {

class TableParameter : public Parameter {
public:
    explicit TableParameter(const ParamDef& d)
        : Parameter(USTRING(d.name), d.id, USTRING(d.units), toNorm(d, d.def), stepCount(d),
                    d.id == kBypass ? (ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass)
                                    : d.map == Map::List ? (ParameterInfo::kCanAutomate | ParameterInfo::kIsList)
                                                         : ParameterInfo::kCanAutomate),
          def(d) {}
    void toString(ParamValue v, String128 s) const SMTG_OVERRIDE { UString(s, 128).fromAscii(toText(def, v).c_str()); }
    bool fromString(const TChar* string, ParamValue& v) const SMTG_OVERRIDE {
        char buf[128] = {0};
        UString(const_cast<TChar*>(string), 128).toAscii(buf, 128);
        bool ok; double n = toNormFromText(def, buf, ok);
        if (ok) v = n;
        return ok;
    }
    ParamValue toPlain(ParamValue v) const SMTG_OVERRIDE { return reverb::toPlain(def, v); }
    ParamValue toNormalized(ParamValue v) const SMTG_OVERRIDE { return reverb::toNorm(def, v); }

private:
    ParamDef def;
};

constexpr uint32 kStateVersion = 1;

}  // namespace

tresult PLUGIN_API LethePlugin::initialize(FUnknown* context) {
    tresult r = SingleComponentEffect::initialize(context);
    if (r != kResultOk) return r;
    addAudioInput(STR16("Stereo In"), SpeakerArr::kStereo);
    addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);
    for (int id : kDisplayOrder) {
        parameters.addParameter(new TableParameter(kParams[id]));
        norm[id] = toNorm(kParams[id], kParams[id].def);
    }
    return kResultOk;
}

tresult PLUGIN_API LethePlugin::setBusArrangements(SpeakerArrangement* in, int32 ni, SpeakerArrangement* out, int32 no) {
    if (ni == 1 && no == 1 && in[0] == out[0] && (in[0] == SpeakerArr::kStereo || in[0] == SpeakerArr::kMono))
        return SingleComponentEffect::setBusArrangements(in, ni, out, no);
    return kResultFalse;
}

tresult PLUGIN_API LethePlugin::canProcessSampleSize(int32 s) { return s == kSample32 ? kResultTrue : kResultFalse; }

tresult PLUGIN_API LethePlugin::setupProcessing(ProcessSetup& setup) {
    sampleRate = setup.sampleRate;
    return SingleComponentEffect::setupProcessing(setup);
}

tresult PLUGIN_API LethePlugin::setActive(TBool state) {
    if (state) {
        engine.prepare(sampleRate);
        currentStyle = -1;
        applyParams();
        engine.reset();
    }
    return SingleComponentEffect::setActive(state);
}

void LethePlugin::applyParams() {
    auto plain = [&](int id) { return toPlain(kParams[id], norm[id]); };
    int st = std::clamp((int)plain(kStyle), 0, kNumVerbStyles - 1);
    verb::Params p;
    p.size = plain(kSize) / 100.0;
    p.decayS = plain(kDecay);
    p.predelayMs = plain(kPredelay);
    p.mix = plain(kMix) / 100.0;
    p.dampingHz = plain(kDamping);
    p.bass = plain(kBass) / 100.0;
    p.diffusion = plain(kDiffusion) / 100.0;
    p.modulation = plain(kModulation) / 100.0;
    p.early = plain(kEarly) / 100.0;
    p.lowCutHz = plain(kLowCut);
    p.highCutHz = plain(kHighCut);
    p.width = plain(kWidth) / 100.0;
    p.duck = plain(kDuck) / 100.0;
    p.duckReleaseMs = plain(kDuckRelease);
    p.freeze = plain(kFreeze) > 0.5;
    p.outputDb = plain(kOutput);
    engine.update(p, verb::kVerbStyles[st]);
    currentStyle = st;
}

tresult PLUGIN_API LethePlugin::process(ProcessData& data) {
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
    if (data.numInputs == 0 || data.numOutputs == 0 || data.numSamples <= 0) return kResultOk;

    AudioBusBuffers& in = data.inputs[0];
    AudioBusBuffers& out = data.outputs[0];
    const int nch = std::min(in.numChannels, out.numChannels);
    float* L = out.channelBuffers32[0];
    float* R = out.numChannels > 1 ? out.channelBuffers32[1] : nullptr;
    const int n = data.numSamples;
    if (L != in.channelBuffers32[0]) std::memcpy(L, in.channelBuffers32[0], sizeof(float) * n);
    if (R && R != (nch > 1 ? in.channelBuffers32[1] : in.channelBuffers32[0]))
        std::memcpy(R, nch > 1 ? in.channelBuffers32[1] : in.channelBuffers32[0], sizeof(float) * n);
    out.silenceFlags = 0;
    if (toPlain(kParams[kBypass], norm[kBypass]) > 0.5) return kResultOk;

    engine.process(L, R, n);
    auto& m = engine.meter;
    if (m.n > 0) {
        auto up = [](std::atomic<float>& a, float v) { if (v > a.load(std::memory_order_relaxed)) a.store(v, std::memory_order_relaxed); };
        up(mIn, (float)std::sqrt(m.in / m.n)); up(mWet, (float)std::sqrt(m.wet / m.n)); up(mEr, (float)std::sqrt(m.er / m.n));
        float dg = (float)(m.duckGain / m.n);
        if (dg < mDuck.load(std::memory_order_relaxed)) mDuck.store(dg, std::memory_order_relaxed);
        m = {};
    }
    return kResultOk;
}

IPlugView* PLUGIN_API LethePlugin::createView(FIDString name) {
    if (!FIDStringsEqual(name, ViewType::kEditor)) return nullptr;
    WebEditor::Hooks h;
    h.unit = "reverb";
    h.feed = [this] { return feedJson(); };
    h.setParam = [this](const std::string& n, double v) { setFromEditor(n, v); };
    return new WebEditor(std::move(h), 1060, 640);
}

void LethePlugin::setFromEditor(const std::string& name, double plain) {
    for (int id = 0; id < kNumParams; ++id) {
        if (name != kParams[id].name) continue;
        double nv = toNorm(kParams[id], plain);
        beginEdit(id); performEdit(id, nv); setParamNormalized(id, nv); endEdit(id);
        return;
    }
}

std::string LethePlugin::feedJson() {
    std::string s = "{\"p\":{";
    char buf[256];
    for (int id = 0; id < kNumParams; ++id) {
        std::snprintf(buf, sizeof buf, "%s\"%s\":%.5g", id ? "," : "", kParams[id].name, toPlain(kParams[id], getParamNormalized(id)));
        s += buf;
    }
    s += "},\"names\":[";
    for (int i = 0; i < kNumVerbStyles; ++i) { s += i ? ",\"" : "\""; s += verb::kVerbStyles[i].name; s += "\""; }
    std::snprintf(buf, sizeof buf, "],\"inL\":%.5g,\"wet\":%.5g,\"er\":%.5g,\"duckGain\":%.5g}",
                  mIn.exchange(0), mWet.exchange(0), mEr.exchange(0), mDuck.exchange(1));
    s += buf;
    return s;
}

tresult PLUGIN_API LethePlugin::setState(IBStream* state) {
    IBStreamer s(state, kLittleEndian);
    uint32 version = 0, count = 0;
    if (!s.readInt32u(version) || !s.readInt32u(count)) return kResultFalse;
    for (uint32 i = 0; i < count; ++i) {
        int32 id; double v;
        if (!s.readInt32(id) || !s.readDouble(v)) return kResultFalse;
        if (id >= 0 && id < kNumParams) { norm[id] = v; setParamNormalized(id, v); }
    }
    applyParams();
    return kResultOk;
}

tresult PLUGIN_API LethePlugin::getState(IBStream* state) {
    IBStreamer s(state, kLittleEndian);
    s.writeInt32u(kStateVersion);
    s.writeInt32u(kNumParams);
    for (int i = 0; i < kNumParams; ++i) { s.writeInt32(i); s.writeDouble(getParamNormalized(i)); }
    return kResultOk;
}

}  // namespace reverb
}  // namespace echolab
