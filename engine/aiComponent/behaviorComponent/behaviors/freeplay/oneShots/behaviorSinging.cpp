/**
 * File: behaviorSinging.cpp
 *
 * Description: Behavior for Cozmo-style singing on Vector.
 *              Sets Wwise song switch, then plays BPM song → GetOut
 *              (GetIn skipped — too long / often silent after Xiaozhi TTS).
 *
 * Ported from Viccyware / Cozmo (cube-shake vibrato omitted for WireOS P1/P2).
 */

#include "engine/aiComponent/behaviorComponent/behaviors/freeplay/oneShots/behaviorSinging.h"

#include "engine/actions/animActions.h"
#include "engine/audio/engineRobotAudioClient.h"
#include "coretech/common/engine/jsonTools.h"

#include "clad/audio/audioEventTypes.h"

namespace Anki {
namespace Vector {

using AMD_GE_GE = AudioMetaData::GameEvent::GenericEvent;
using AMD_GOT = AudioMetaData::GameObjectType;

namespace {
  static const char* kAudioSwitchGroup = "audioSwitchGroup";
  static const char* kAudioSwitch = "audioSwitch";

  static const AnimationTrigger kGetOutTrigger = AnimationTrigger::DEPRECATED_Singing_GetOut;

  static const AnimationTrigger k80BpmTrigger = AnimationTrigger::DEPRECATED_Singing_80bpm;
  static const AnimationTrigger k100BpmTrigger = AnimationTrigger::DEPRECATED_Singing_100bpm;
  static const AnimationTrigger k120BpmTrigger = AnimationTrigger::DEPRECATED_Singing_120bpm;
}

BehaviorSinging::BehaviorSinging(const Json::Value& config)
: ICozmoBehavior(config)
{
  std::string value;
  bool res = JsonTools::GetValueOptional(config, kAudioSwitchGroup, value);
  DEV_ASSERT_MSG(res,
                 "BehaviorSinging.NoAudioSwitchGroup",
                 "No audioSwitchGroup for singing behavior %s",
                 GetDebugLabel().c_str());
  _audioSwitchGroup = AudioMetaData::SwitchState::SwitchGroupTypeFromString(value);

  res = JsonTools::GetValueOptional(config, kAudioSwitch, value);
  DEV_ASSERT_MSG(res,
                 "BehaviorSinging.NoAudioSwitch",
                 "No audioSwitch for singing behavior %s",
                 GetDebugLabel().c_str());

  using GenericSwitch = AudioMetaData::SwitchState::GenericSwitch;
  switch(_audioSwitchGroup)
  {
    case AudioSwitchGroup::Cozmo_Sings_80Bpm:
    {
      _audioSwitch = static_cast<GenericSwitch>(AudioMetaData::SwitchState::Cozmo_Sings_80BpmFromString(value));
      _songAnimTrigger = k80BpmTrigger;
      break;
    }
    case AudioSwitchGroup::Cozmo_Sings_100Bpm:
    {
      _audioSwitch = static_cast<GenericSwitch>(AudioMetaData::SwitchState::Cozmo_Sings_100BpmFromString(value));
      _songAnimTrigger = k100BpmTrigger;
      break;
    }
    case AudioSwitchGroup::Cozmo_Sings_120Bpm:
    {
      _audioSwitch = static_cast<GenericSwitch>(AudioMetaData::SwitchState::Cozmo_Sings_120BpmFromString(value));
      _songAnimTrigger = k120BpmTrigger;
      break;
    }
    default:
    {
      DEV_ASSERT_MSG(false,
                     "BehaviorSinging.BadAudioSwitchGroup",
                     "Unsupported audioSwitchGroup for singing behavior %s",
                     GetDebugLabel().c_str());
      _audioSwitch = GenericSwitch::Invalid;
      _audioSwitchGroup = AudioSwitchGroup::Cozmo_Sings_80Bpm;
      _songAnimTrigger = k80BpmTrigger;
    }
  }
}

BehaviorSinging::~BehaviorSinging()
{
}

void BehaviorSinging::GetBehaviorJsonKeys(std::set<const char*>& expectedKeys) const
{
  const char* list[] = {
    kAudioSwitchGroup,
    kAudioSwitch,
  };
  expectedKeys.insert( std::begin(list), std::end(list) );
}

bool BehaviorSinging::WantsToBeActivatedBehavior() const
{
  return true;
}

void BehaviorSinging::OnBehaviorActivated()
{
  if(ANKI_VERIFY(GetBEI().HasRobotAudioClient(),
                 "BehaviorSinging.OnBehaviorActivated.MissingAudioClient","")){
    GetBEI().GetRobotAudioClient().PostSwitchState(_audioSwitchGroup,
                                                   _audioSwitch,
                                                   AudioMetaData::GameObjectType::Default);
  }

  // Skip GetIn for voice UX: Cozmo GetIn is ~5–8s of face/motion before the
  // melody, and GetIn_01's VO events often EventFailed after Xiaozhi ALSA TTS
  // (silent "singing face" then delayed song). Song anim posts Play_Cozmo_*Bpm
  // at t=0; GetOut still cleans up.
  CompoundActionSequential* action = new CompoundActionSequential();
  action->AddAction(new TriggerAnimationAction(_songAnimTrigger));
  action->AddAction(new TriggerAnimationAction(kGetOutTrigger));
  DelegateIfInControl(action);

  if(GetBEI().HasRobotAudioClient()){
    GetBEI().GetRobotAudioClient().PostEvent(AMD_GE_GE::Stop__Robot_Singing, AMD_GOT::Behavior);
  }
}

void BehaviorSinging::OnBehaviorDeactivated()
{
  if(GetBEI().HasRobotAudioClient()){
    auto& audioClient = GetBEI().GetRobotAudioClient();
    audioClient.PostEvent(AMD_GE_GE::Stop__Robot_Singing,
                          AudioMetaData::GameObjectType::Default);
    audioClient.PostEvent(AMD_GE_GE::Stop__Robot_Singing, AMD_GOT::Behavior);
  }
}

} // namespace Vector
} // namespace Anki
