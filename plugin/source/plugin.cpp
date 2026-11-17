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

namespace {

// A host-visible parameter whose display/parsing uses our table (log ranges, "Off", units).
class TableParameter : public Parameter {
public:
    explicit TableParameter(const ParamDef& d)
        : Parameter(USTRING(d.name), d.id, USTRING(d.units), toNorm(d, d.def), stepCount(d),
                    d.id == kBypass ? (ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass)
                                    : d.map == Map::List ? (ParameterInfo::kCanAutomate | ParameterInfo::kIsList)
                                                         : ParameterInfo::kCanAutomate),
          def(d) {}
    void toString(ParamValue v, String128 string) const SMTG_OVERRIDE {
        UString(string, 128).fromAscii(toText(def, v).c_str());
    }
    bool fromString(const TChar* string, ParamValue& v) const SMTG_OVERRIDE {
        char buf[128] = {0};
        UString(const_cast<TChar*>(string), 128).toAscii(buf, 128);
        if (def.map == Map::List) {
            for (int i = 0; i <= (int)(def.max - def.min); ++i)
                if (toText(def, toNorm(def, def.min + i)) == buf) { v = toNorm(def, def.min + i); return true; }
            return false;
        }
        if (!std::strncmp(buf, "Off", 3) || !std::strncmp(buf, "off", 3)) {
            v = def.map == Map::LogOffHigh ? 1.0 : 0.0;
            return true;
        }
        double x = std::atof(buf);
        if (std::strstr(buf, "k")) x *= 1000;
        if (def.units[0] == 'm' && std::strstr(buf, " s")) x *= 1000;
        v = toNorm(def, x);
        return true;
    }
    ParamValue toPlain(ParamValue v) const SMTG_OVERRIDE { return echolab::toPlain(def, v); }
    ParamValue toNormalized(ParamValue v) const SMTG_OVERRIDE { return echolab::toNorm(def, v); }

private:
    ParamDef def;
};

constexpr uint32 kStateVersion = 1;

}  // namespace

tresult PLUGIN_API EchoLabPlugin::initialize(FUnknown* context) {
    tresult r = SingleComponentEffect::initialize(context);
    if (r != kResultOk) return r;
    addAudioInput(STR16("Stereo In"), SpeakerArr::kStereo);
    addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);
    for (int id : kDisplayOrder) {
        const auto& d = kParams[id];
        parameters.addParameter(new TableParameter(d));
        norm[d.id] = toNorm(d, d.def);
    }
    return kResultOk;
}

tresult PLUGIN_API EchoLabPlugin::setBusArrangements(SpeakerArrangement* inputs, int32 numIns,
                                                     SpeakerArrangement* outputs, int32 numOuts) {
    if (numIns == 1 && numOuts == 1 && inputs[0] == outputs[0] &&
        (inputs[0] == SpeakerArr::kStereo || inputs[0] == SpeakerArr::kMono))
        return SingleComponentEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
    return kResultFalse;
}

tresult PLUGIN_API EchoLabPlugin::canProcessSampleSize(int32 size) {
    return size == kSample32 ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API EchoLabPlugin::setupProcessing(ProcessSetup& setup) {
    sampleRate = setup.sampleRate;
    return SingleComponentEffect::setupProcessing(setup);
}

tresult PLUGIN_API EchoLabPlugin::setActive(TBool state) {
    if (state) {
        engine.prepare(sampleRate, 4.0);
        currentStyle = -1;
        applyParams();
        engine.reset();
    }
    return SingleComponentEffect::setActive(state);
}

void EchoLabPlugin::applyParams() {
    auto plain = [&](int id) { return toPlain(kParams[id], norm[id]); };
    int st = std::clamp((int)plain(kStyle), 0, kNumStyles - 1);
    if (st != currentStyle) {
        engine.setStyle(kStyles[st]);
        currentStyle = st;
    }
    Params p;
    p.timeMs[0] = plain(kTime);
    p.timeMs[1] = plain(kTimeR);
    p.feedback = plain(kFeedback) / 100.0;
    p.mix = plain(kMix) / 100.0;
    p.saturationDb = plain(kSaturation);
    p.lowCutHz = plain(kLowCut);
    p.highCutHz = plain(kHighCut);
    p.wobble = plain(kWobble) / 100.0;
    p.duck = plain(kDuck) / 100.0;
    p.duckReleaseMs = plain(kDuckRelease);
    p.width = plain(kWidth) / 100.0;
    p.mode = (int)plain(kMode);
    p.outputDb = plain(kOutput);

    // --- performance macros: offsets on top of the base settings (all at rest = no change)
    const double throwAmt = plain(kThrow) / 100.0, age = plain(kAge) / 100.0;
    const double space = plain(kSpace) / 100.0, warp = plain(kWarp);
    if (throwAmt > 0) {  // dub throw: feedback toward self-oscillation, echo forward, less ducking
        p.feedback += throwAmt * (1.05 - p.feedback);
        p.mix += throwAmt * (std::max(p.mix, 0.6) - p.mix);
        p.duck *= 1.0 - throwAmt;
    }
    if (age > 0) {  // older tape: more wobble, more drive, darker top
        p.wobble *= 1.0 + 3.0 * age;
        p.saturationDb += 12.0 * age;
        double cut = 20000.0 * std::pow(3500.0 / 20000.0, age);
        p.highCutHz = p.highCutHz > 0 ? std::min(p.highCutHz, cut) : (age > 0.02 ? cut : 0.0);
    }
    if (space > 0) {  // spread: right repeat lands later, full width
        if (p.mode == 0) { p.mode = 1; p.timeMs[1] = p.timeMs[0]; }
        p.timeMs[1] *= 1.0 + 0.5 * space;
        p.width += space * (1.0 - p.width);
    }
    p.timeMs[0] = std::clamp(p.timeMs[0] * warp, 5.0, 3900.0);  // tape speed (repitches while moving)
    p.timeMs[1] = std::clamp(p.timeMs[1] * warp, 5.0, 3900.0);
    engine.updateParams(p);
}

tresult PLUGIN_API EchoLabPlugin::process(ProcessData& data) {
    // parameter changes: take the last value of each queue for this block
    bool changed = false;
    if (data.inputParameterChanges) {
        int32 n = data.inputParameterChanges->getParameterCount();
        for (int32 i = 0; i < n; ++i) {
            IParamValueQueue* q = data.inputParameterChanges->getParameterData(i);
            if (!q) continue;
            int32 points = q->getPointCount();
            ParamValue v;
            int32 offset;
            ParamID id = q->getParameterId();
            if (points > 0 && id < (ParamID)kNumParams && q->getPoint(points - 1, offset, v) == kResultTrue) {
                norm[id] = v;
                changed = true;
            }
        }
    }
    if (changed) applyParams();
    if (data.numInputs == 0 || data.numOutputs == 0 || data.numSamples <= 0) return kResultOk;

    AudioBusBuffers& in = data.inputs[0];
    AudioBusBuffers& out = data.outputs[0];
    const int nch = std::min(in.numChannels, out.numChannels);
    float* inL = in.channelBuffers32[0];
    float* inR = nch > 1 ? in.channelBuffers32[1] : inL;
    float* outL = out.channelBuffers32[0];
    float* outR = out.numChannels > 1 ? out.channelBuffers32[1] : nullptr;
    const int n = data.numSamples;
    if (outL != inL) std::memcpy(outL, inL, sizeof(float) * n);
    if (outR && outR != inR) std::memcpy(outR, inR, sizeof(float) * n);
    out.silenceFlags = 0;
    if (toPlain(kParams[kBypass], norm[kBypass]) > 0.5) return kResultOk;  // dry passthrough
    engine.process(outL, outR, n);
    // meters for the editor (peak-held until it reads them)
    auto& m = engine.meter;
    if (m.n > 0) {
        auto up = [](std::atomic<float>& a, float v) { if (v > a.load(std::memory_order_relaxed)) a.store(v, std::memory_order_relaxed); };
        up(mIn, (float)std::sqrt(m.in / m.n)); up(mWet, (float)std::sqrt(m.wet / m.n)); up(mWr, (float)std::sqrt(m.wr / m.n));
        if (m.wr > 1e-12) {  // brightness of what is written: frequency estimate from the first-difference ratio
            double r = std::sqrt(m.wrd / m.wr);
            mWf.store((float)(std::asin(std::min(1.0, r / 2)) * sampleRate / 3.14159265358979), std::memory_order_relaxed);
        }
        float dg = (float)(m.duckGain / m.n);
        if (dg < mDuck.load(std::memory_order_relaxed)) mDuck.store(dg, std::memory_order_relaxed);
        double lastW = m.lastW; m = {}; m.lastW = lastW;
    }
    return kResultOk;
}

IPlugView* PLUGIN_API EchoLabPlugin::createView(FIDString name) {
    if (!FIDStringsEqual(name, ViewType::kEditor)) return nullptr;
    WebEditor::Hooks h;
    h.unit = "echo";
    h.feed = [this] { return feedJson(); };
    h.setParam = [this](const std::string& n, double v) { setFromEditor(n, v); };
    return new WebEditor(std::move(h), 1060, 640);
}

void EchoLabPlugin::setFromEditor(const std::string& name, double plain) {
    for (int id = 0; id < kNumParams; ++id) {
        if (name != kParams[id].name) continue;
        double nv = toNorm(kParams[id], plain);
        beginEdit(id); performEdit(id, nv); setParamNormalized(id, nv); endEdit(id);
        return;
    }
}

std::string EchoLabPlugin::feedJson() {
    std::string s = "{\"p\":{";
    char buf[96];
    for (int id = 0; id < kNumParams; ++id) {
        std::snprintf(buf, sizeof buf, "%s\"%s\":%.5g", id ? "," : "", kParams[id].name, toPlain(kParams[id], getParamNormalized(id)));
        s += buf;
    }
    std::snprintf(buf, sizeof buf, "},\"inL\":%.5g,\"wet\":%.5g,\"wr\":%.5g,\"wf\":%.5g,\"duckGain\":%.5g}",
                  mIn.exchange(0), mWet.exchange(0), mWr.exchange(0), mWf.load(), mDuck.exchange(1));
    s += buf;
    return s;
}

tresult PLUGIN_API EchoLabPlugin::setState(IBStream* state) {
    IBStreamer s(state, kLittleEndian);
    uint32 version = 0, count = 0;
    if (!s.readInt32u(version) || !s.readInt32u(count)) return kResultFalse;
    for (uint32 i = 0; i < count; ++i) {
        int32 id;
        double v;
        if (!s.readInt32(id) || !s.readDouble(v)) return kResultFalse;
        if (id >= 0 && id < kNumParams) {
            norm[id] = v;
            setParamNormalized(id, v);
        }
    }
    applyParams();
    return kResultOk;
}

tresult PLUGIN_API EchoLabPlugin::getState(IBStream* state) {
    IBStreamer s(state, kLittleEndian);
    s.writeInt32u(kStateVersion);
    s.writeInt32u(kNumParams);
    for (int i = 0; i < kNumParams; ++i) {
        s.writeInt32(i);
        s.writeDouble(getParamNormalized(i));
    }
    return kResultOk;
}

}  // namespace echolab
