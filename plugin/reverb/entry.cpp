#include "plugin.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "version.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

BEGIN_FACTORY_DEF(stringCompanyName, stringCompanyWeb, stringCompanyEmail)
DEF_CLASS2(INLINE_UID(0xFF1D44C6, 0xE7914A8C, 0xA39BB60A, 0xFDC4E9CC),
           PClassInfo::kManyInstances, kVstAudioEffectClass, "Lethe Reverb", 0,
           PlugType::kFxReverb, FULL_VERSION_STR, kVstVersionString,
           echolab::reverb::LethePlugin::createInstance)
END_FACTORY
