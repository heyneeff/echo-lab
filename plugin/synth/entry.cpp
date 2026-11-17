#include "plugin.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "version.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

BEGIN_FACTORY_DEF(stringCompanyName, stringCompanyWeb, stringCompanyEmail)
DEF_CLASS2(INLINE_UID(0x47789B12, 0x26984841, 0xA9562883, 0xB721A238),
           PClassInfo::kManyInstances, kVstAudioEffectClass, "Morpheus", 0,
           PlugType::kInstrumentSynth, FULL_VERSION_STR, kVstVersionString,
           echolab::wtsynth::SynthPlugin::createInstance)
END_FACTORY
