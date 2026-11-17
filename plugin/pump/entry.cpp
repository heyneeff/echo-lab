#include "plugin.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "version.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

BEGIN_FACTORY_DEF(stringCompanyName, stringCompanyWeb, stringCompanyEmail)
DEF_CLASS2(INLINE_UID(0xF47F59EA, 0xEC2F4CF4, 0x890735B1, 0x4A6439DF),
           PClassInfo::kManyInstances, kVstAudioEffectClass, "Charon Pump", 0,
           PlugType::kFxDynamics, FULL_VERSION_STR, kVstVersionString,
           echolab::pumpfx::CharonPlugin::createInstance)
END_FACTORY
