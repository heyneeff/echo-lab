#include "plugin.h"

#include <cmath>
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

constexpr uint32 kStateVersion = 1;

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
    for (int i = 0; i < n; ++i) { L[i] = wt::Synth::safety(L[i]); if (R != L) R[i] = wt::Synth::safety(R[i]); }
    out.silenceFlags = synth.activeVoices() == 0 && !driveOn ? 3 : 0;
    return kResultOk;
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
    return kResultOk;
}

tresult PLUGIN_API SynthPlugin::getState(IBStream* state) {
    IBStreamer s(state, kLittleEndian);
    s.writeInt32u(kStateVersion);
    s.writeInt32u(kNumParams);
    for (int i = 0; i < kNumParams; ++i) { s.writeInt32(i); s.writeDouble(getParamNormalized(i)); }
    return kResultOk;
}

}  // namespace wtsynth
}  // namespace echolab
