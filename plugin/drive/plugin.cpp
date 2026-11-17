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
namespace drive {

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
        if (def.map == Map::List) {
            for (int i = 0; i <= (int)(def.max - def.min); ++i)
                if (toText(def, toNorm(def, def.min + i)) == buf) { v = toNorm(def, def.min + i); return true; }
            return false;
        }
        if (!std::strncmp(buf, "Off", 3)) { v = def.map == Map::LogOffHigh ? 1.0 : 0.0; return true; }
        double x = std::atof(buf);
        if (std::strstr(buf, "k")) x *= 1000;
        v = toNorm(def, x);
        return true;
    }
    ParamValue toPlain(ParamValue v) const SMTG_OVERRIDE { return drive::toPlain(def, v); }
    ParamValue toNormalized(ParamValue v) const SMTG_OVERRIDE { return drive::toNorm(def, v); }

private:
    ParamDef def;
};

constexpr uint32 kStateVersion = 1;

}  // namespace

tresult PLUGIN_API DriveLabPlugin::initialize(FUnknown* context) {
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

tresult PLUGIN_API DriveLabPlugin::setBusArrangements(SpeakerArrangement* in, int32 ni, SpeakerArrangement* out, int32 no) {
    if (ni == 1 && no == 1 && in[0] == out[0] && (in[0] == SpeakerArr::kStereo || in[0] == SpeakerArr::kMono))
        return SingleComponentEffect::setBusArrangements(in, ni, out, no);
    return kResultFalse;
}

tresult PLUGIN_API DriveLabPlugin::canProcessSampleSize(int32 s) { return s == kSample32 ? kResultTrue : kResultFalse; }

tresult PLUGIN_API DriveLabPlugin::setupProcessing(ProcessSetup& setup) {
    sampleRate = setup.sampleRate;
    return SingleComponentEffect::setupProcessing(setup);
}

tresult PLUGIN_API DriveLabPlugin::setActive(TBool state) {
    if (state) {
        engine.prepare(sampleRate);
        currentStyle = -1;
        applyParams();
        engine.reset();
        ic1.fill(0); ic2.fill(0);
    }
    return SingleComponentEffect::setActive(state);
}

void DriveLabPlugin::applyParams() {
    auto plain = [&](int id) { return toPlain(kParams[id], norm[id]); };
    int st = std::clamp((int)plain(kStyle), 0, kNumDriveVoices - 1);
    if (st != currentStyle) { engine.setVoice(kDriveVoices[st]); currentStyle = st; }
    DriveParams p;
    p.drive = plain(kDrive);
    p.punish = plain(kPunish) > 0.5;
    p.toneDb = plain(kTone);
    p.lowCutHz = plain(kLowCut);
    p.highCutHz = plain(kHighCut);
    p.mix = plain(kMix) / 100.0;
    p.autoGain = plain(kAutoGain) > 0.5;
    p.outputDb = plain(kOutput);
    engine.update(p);
    pumpDepth = plain(kPump) / 100.0;
    pumpRate = (int)plain(kPumpRate);
    filterHz = plain(kFilter);
    filterRes = plain(kResonance) / 100.0;
}

tresult PLUGIN_API DriveLabPlugin::process(ProcessData& data) {
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

    {  // editor: input level + the raw input samples (they ride the transfer curve on screen)
        int p = scopePos.load(std::memory_order_relaxed); double sq = 0;
        for (int i = 0; i < n; ++i) { scope[p] = L[i]; p = (p + 1) % kScope; sq += (double)L[i] * L[i]; }
        scopePos.store(p, std::memory_order_relaxed);
        float r = (float)std::sqrt(sq / n); if (r > mIn.load(std::memory_order_relaxed)) mIn.store(r, std::memory_order_relaxed);
    }
    engine.process(L, R, n);

    // --- host tempo/beat for the pump (free-running at the last tempo when stopped)
    double tempo = 120, beat = freeBeat;
    bool playing = false;
    if (auto* ctx = data.processContext) {
        if (ctx->state & ProcessContext::kTempoValid) tempo = ctx->tempo;
        if ((ctx->state & ProcessContext::kProjectTimeMusicValid) && (ctx->state & ProcessContext::kPlaying)) {
            beat = ctx->projectTimeMusic;
            playing = true;
        }
    }
    const double beatsPerSample = tempo / 60.0 / sampleRate;
    const double perBeat = pumpRate == 0 ? 0.5 : pumpRate == 1 ? 1.0 : 2.0;  // 1/2, 1/4, 1/8
    // --- resonant low-pass (TPT SVF), off at max
    const bool filt = filterHz > 0 && filterHz < 19900;
    double g = 0, k = 2, a1 = 0, a2 = 0, a3 = 0;
    if (filt) {
        g = std::tan(3.14159265358979 * std::min(filterHz, sampleRate * 0.45) / sampleRate);
        k = 2.0 - 1.9 * filterRes;  // resonance up to a strong (but stable) peak
        a1 = 1.0 / (1.0 + g * (g + k)); a2 = g * a1; a3 = g * a2;
    }
    for (int i = 0; i < n; ++i) {
        double gain = 1.0;
        if (pumpDepth > 0) {
            double ph = std::fmod((beat + i * beatsPerSample) * perBeat, 1.0);
            if (ph < 0) ph += 1.0;
            // instant duck on the beat, smooth recovery (sidechain-style curve)
            double rec = 1.0 - std::pow(1.0 - std::min(ph / 0.85, 1.0), 2.2);
            gain = 1.0 - pumpDepth * (1.0 - rec);
        }
        for (int c = 0; c < 2; ++c) {
            float* buf = c == 0 ? L : R;
            if (!buf) continue;
            double x = buf[i];
            if (filt) {
                double v3 = x - ic2[c], v1 = a1 * ic1[c] + a2 * v3, v2 = ic2[c] + a2 * ic1[c] + a3 * v3;
                ic1[c] = 2 * v1 - ic1[c]; ic2[c] = 2 * v2 - ic2[c];
                x = v2;
            }
            buf[i] = (float)(x * gain);
        }
    }
    if (!playing) freeBeat = std::fmod(beat + n * beatsPerSample, 4096.0);
    {
        double sq = 0; for (int i = 0; i < n; ++i) sq += (double)L[i] * L[i];
        float r = (float)std::sqrt(sq / n); if (r > mOut.load(std::memory_order_relaxed)) mOut.store(r, std::memory_order_relaxed);
        if (pumpDepth > 0) {
            double ph = std::fmod((beat + n * beatsPerSample) * perBeat, 1.0); if (ph < 0) ph += 1.0;
            mPump.store((float)(1.0 - pumpDepth * (1.0 - (1.0 - std::pow(1.0 - std::min(ph / 0.85, 1.0), 2.2)))), std::memory_order_relaxed);
        } else mPump.store(1.0f, std::memory_order_relaxed);
    }
    return kResultOk;
}

IPlugView* PLUGIN_API DriveLabPlugin::createView(FIDString name) {
    if (!FIDStringsEqual(name, ViewType::kEditor)) return nullptr;
    WebEditor::Hooks h;
    h.unit = "drive";
    h.feed = [this] { return feedJson(); };
    h.setParam = [this](const std::string& n, double v) { setFromEditor(n, v); };
    return new WebEditor(std::move(h), 1060, 640);
}

void DriveLabPlugin::setFromEditor(const std::string& name, double plain) {
    for (int id = 0; id < kNumParams; ++id) {
        if (name != kParams[id].name) continue;
        double nv = toNorm(kParams[id], plain);
        beginEdit(id); performEdit(id, nv); setParamNormalized(id, nv); endEdit(id);
        return;
    }
}

std::string DriveLabPlugin::feedJson() {
    std::string s = "{\"p\":{";
    char buf[128];
    for (int id = 0; id < kNumParams; ++id) {
        std::snprintf(buf, sizeof buf, "%s\"%s\":%.5g", id ? "," : "", kParams[id].name, toPlain(kParams[id], getParamNormalized(id)));
        s += buf;
    }
    s += "},\"names\":[";
    for (int i = 0; i < kNumDriveVoices; ++i) { s += i ? ",\"" : "\""; s += kDriveVoices[i].name; s += "\""; }
    // the true transfer curve of the current voice at the current drive (normalised to its peak)
    double g = engine.inputGain(), ys[129], peak = 1e-9;
    for (int i = 0; i <= 128; ++i) { ys[i] = engine.clip((i / 64.0 - 1.0) * g); peak = std::max(peak, std::abs(ys[i])); }
    s += "],\"curve\":[";
    for (int i = 0; i <= 128; ++i) { std::snprintf(buf, sizeof buf, "%s%.4f", i ? "," : "", ys[i] / peak); s += buf; }
    s += "],\"pre\":[";
    int p = scopePos.load(std::memory_order_relaxed);
    for (int i = 0; i < kScope; ++i) { std::snprintf(buf, sizeof buf, "%s%.4f", i ? "," : "", scope[(p + i) % kScope]); s += buf; }
    std::snprintf(buf, sizeof buf, "],\"inL\":%.5g,\"out\":%.5g,\"pump\":%.4f}", mIn.exchange(0), mOut.exchange(0), mPump.load());
    s += buf;
    return s;
}

tresult PLUGIN_API DriveLabPlugin::setState(IBStream* state) {
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

tresult PLUGIN_API DriveLabPlugin::getState(IBStream* state) {
    IBStreamer s(state, kLittleEndian);
    s.writeInt32u(kStateVersion);
    s.writeInt32u(kNumParams);
    for (int i = 0; i < kNumParams; ++i) { s.writeInt32(i); s.writeDouble(getParamNormalized(i)); }
    return kResultOk;
}

}  // namespace drive
}  // namespace echolab
