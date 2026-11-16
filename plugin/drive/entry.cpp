#include "plugin.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "version.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

BEGIN_FACTORY_DEF(stringCompanyName, stringCompanyWeb, stringCompanyEmail)
DEF_CLASS2(INLINE_UID(0x2D7A41F3, 0x8C1B4E62, 0x9F04A7D5, 0x51E3C2B8),
           PClassInfo::kManyInstances, kVstAudioEffectClass, "Prometheus Drive", 0,
           PlugType::kFxDistortion, FULL_VERSION_STR, kVstVersionString,
           echolab::drive::DriveLabPlugin::createInstance)
END_FACTORY
