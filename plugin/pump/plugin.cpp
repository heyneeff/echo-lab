#include "plugin.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/vst/vstparameters.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace echolab {
namespace pumpfx {

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
    ParamValue toPlain(ParamValue v) const SMTG_OVERRIDE { return hostPlain(def, v); }
    ParamValue toNormalized(ParamValue v) const SMTG_OVERRIDE { return hostNorm(def, v); }

private:
    ParamDef def;
};

constexpr uint32 kStateVersion = 1;

}  // namespace

tresult PLUGIN_API CharonPlugin::initialize(FUnknown* context) {
    tresult r = SingleComponentEffect::initialize(context);
    if (r != kResultOk) return r;
    addAudioInput(STR16("Stereo In"), SpeakerArr::kStereo);
    addAudioInput(STR16("Sidechain"), SpeakerArr::kStereo, BusTypes::kAux, 0);  // route a kick here
    addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);
    for (int id : kDisplayOrder) {
        parameters.addParameter(new TableParameter(kParams[id]));
        norm[id] = toNorm(kParams[id], kParams[id].def);
    }
    return kResultOk;
}

tresult PLUGIN_API CharonPlugin::setBusArrangements(SpeakerArrangement* in, int32 ni, SpeakerArrangement* out, int32 no) {
    if (ni >= 1 && no == 1 && in[0] == out[0] && (in[0] == SpeakerArr::kStereo || in[0] == SpeakerArr::kMono) &&
        (ni == 1 || in[1] == SpeakerArr::kStereo || in[1] == SpeakerArr::kMono))
        return SingleComponentEffect::setBusArrangements(in, ni, out, no);
    return kResultFalse;
}

tresult PLUGIN_API CharonPlugin::canProcessSampleSize(int32 s) { return s == kSample32 ? kResultTrue : kResultFalse; }

tresult PLUGIN_API CharonPlugin::setupProcessing(ProcessSetup& setup) {
    sampleRate = setup.sampleRate;
    return SingleComponentEffect::setupProcessing(setup);
}

tresult PLUGIN_API CharonPlugin::setActive(TBool state) {
    if (state) {
        engine.prepare(sampleRate);
        currentStyle = -1;
        applyParams();
        engine.reset();
    }
    return SingleComponentEffect::setActive(state);
}

void CharonPlugin::applyParams() {
    auto plain = [&](int id) { return toPlain(kParams[id], norm[id]); };
    pump::Params p;
    p.mode = (int)plain(kMode);
    p.depth = plain(kDepth) / 100.0;
    p.rate = (int)plain(kRate);
    p.attack = plain(kAttack) / 100.0;
    p.hold = plain(kHold) / 100.0;
    p.curve = plain(kCurve) / 100.0;
    p.phase = plain(kPhase) / 100.0;
    p.scThreshDb = plain(kScThresh);
    p.scAttackMs = plain(kScAttack);
    p.scReleaseMs = plain(kScRelease);
    p.filterHz = plain(kFilter);
    p.filterAmt = plain(kFilterAmt) / 100.0;
    p.resonance = plain(kResonance) / 100.0;
    p.mix = plain(kMix) / 100.0;
    p.outputDb = plain(kOutput);
    engine.update(p);
}

tresult PLUGIN_API CharonPlugin::process(ProcessData& data) {
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

    // host tempo + musical position (free-running at the last tempo when the transport is stopped)
    double beat = freeBeat; bool playing = false;
    if (auto* ctx = data.processContext) {
        if (ctx->state & ProcessContext::kTempoValid) hostTempo = ctx->tempo;
        if ((ctx->state & ProcessContext::kProjectTimeMusicValid) && (ctx->state & ProcessContext::kPlaying)) { beat = ctx->projectTimeMusic; playing = true; }
    }
    const double bps = hostTempo / 60.0 / sampleRate;
    const float *scL = nullptr, *scR = nullptr;
    if (data.numInputs > 1 && data.inputs[1].numChannels > 0 && !(data.inputs[1].silenceFlags & 1)) {
        scL = data.inputs[1].channelBuffers32[0]; scR = data.inputs[1].numChannels > 1 ? data.inputs[1].channelBuffers32[1] : nullptr;
    }
    static thread_local float zeros[4096] = {0};
    if (!scL && n <= 4096) scL = zeros;  // no sidechain routed: the follower sees silence
    double sqIn = 0, sqOut = 0, scPk = 0;
    for (int i = 0; i < n; ++i) { sqIn += (double)L[i] * L[i]; if (scL) scPk = std::max(scPk, (double)std::abs(scL[i])); }
    engine.process(L, R, scL, scR, n, beat, bps);
    for (int i = 0; i < n; ++i) sqOut += (double)L[i] * L[i];
    if (!playing) freeBeat = std::fmod(beat + n * bps, 4096.0);
    beatNow.store(beat + n * bps, std::memory_order_relaxed);
    auto up = [](std::atomic<float>& a, float v) { if (v > a.load(std::memory_order_relaxed)) a.store(v, std::memory_order_relaxed); };
    up(mIn, (float)std::sqrt(sqIn / n)); up(mOut, (float)std::sqrt(sqOut / n)); up(mSc, (float)scPk);
    if (engine.lastGain < mGain.load(std::memory_order_relaxed)) mGain.store((float)engine.lastGain, std::memory_order_relaxed);
    return kResultOk;
}

IPlugView* PLUGIN_API CharonPlugin::createView(FIDString name) {
    if (!FIDStringsEqual(name, ViewType::kEditor)) return nullptr;
    WebEditor::Hooks h;
    h.unit = "pump";
    h.feed = [this] { return feedJson(); };
    h.setParam = [this](const std::string& n, double v) { setFromEditor(n, v); };
    return new WebEditor(std::move(h), 1060, 640);
}

void CharonPlugin::setFromEditor(const std::string& name, double plain) {
    for (int id = 0; id < kNumParams; ++id) {
        if (name != kParams[id].name) continue;
        double nv = toNorm(kParams[id], plain);
        beginEdit(id); performEdit(id, nv); setParamNormalized(id, nv); endEdit(id);
        return;
    }
}

std::string CharonPlugin::feedJson() {
    std::string s = "{\"p\":{";
    char buf[256];
    for (int id = 0; id < kNumParams; ++id) {
        std::snprintf(buf, sizeof buf, "%s\"%s\":%.5g", id ? "," : "", kParams[id].name, toPlain(kParams[id], getParamNormalized(id)));
        s += buf;
    }
    std::snprintf(buf, sizeof buf, "},\"tempo\":%.3f,\"beat\":%.5f,\"inL\":%.5g,\"out\":%.5g,\"sc\":%.5g,\"gain\":%.5g}",
                  hostTempo, beatNow.load(), mIn.exchange(0), mOut.exchange(0), mSc.exchange(0), mGain.exchange(1));
    s += buf;
    return s;
}

tresult PLUGIN_API CharonPlugin::setState(IBStream* state) {
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

tresult PLUGIN_API CharonPlugin::getState(IBStream* state) {
    IBStreamer s(state, kLittleEndian);
    s.writeInt32u(kStateVersion);
    s.writeInt32u(kNumParams);
    for (int i = 0; i < kNumParams; ++i) { s.writeInt32(i); s.writeDouble(getParamNormalized(i)); }
    return kResultOk;
}

}  // namespace pumpfx
}  // namespace echolab
