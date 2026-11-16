#include "plugin.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "version.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

BEGIN_FACTORY_DEF(stringCompanyName, stringCompanyWeb, stringCompanyEmail)

DEF_CLASS2(INLINE_UID(0x6E0B2A4C, 0x91D34F7A, 0xB5E21C88, 0x3F7D9A10),
           PClassInfo::kManyInstances,
           kVstAudioEffectClass,
           "Oread Echo",
           0,  // single component effect
           PlugType::kFxDelay,
           FULL_VERSION_STR,
           kVstVersionString,
           echolab::EchoLabPlugin::createInstance)

END_FACTORY
