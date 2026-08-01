/**
 * File: behaviorSinging.h
 *
 * Description: Behavior for Cozmo-style singing on Vector.
 *              Sets Wwise song switch, then plays GetIn → BPM song → GetOut.
 *
 * Ported from Viccyware / Cozmo; cube-shake vibrato omitted for WireOS P1/P2.
 */

#ifndef __Cozmo_Basestation_Behaviors_BehaviorSinging_H__
#define __Cozmo_Basestation_Behaviors_BehaviorSinging_H__

#include "engine/aiComponent/behaviorComponent/behaviors/iCozmoBehavior.h"
#include "clad/audio/audioSwitchTypes.h"
#include "clad/types/animationTrigger.h"

namespace Anki {
namespace Vector {

class BehaviorSinging : public ICozmoBehavior
{
protected:
  friend class BehaviorFactory;
  BehaviorSinging(const Json::Value& config);

public:
  virtual ~BehaviorSinging();

  virtual bool WantsToBeActivatedBehavior() const override;

protected:
  virtual void GetBehaviorOperationModifiers(BehaviorOperationModifiers& modifiers) const override {}
  virtual void GetBehaviorJsonKeys(std::set<const char*>& expectedKeys) const override;

  virtual void OnBehaviorActivated() override;
  virtual void OnBehaviorDeactivated() override;

private:
  using AudioSwitchGroup = AudioMetaData::SwitchState::SwitchGroupType;
  AudioSwitchGroup _audioSwitchGroup;
  AudioMetaData::SwitchState::GenericSwitch _audioSwitch;
  AnimationTrigger _songAnimTrigger = AnimationTrigger::Count;
};

} // namespace Vector
} // namespace Anki

#endif // __Cozmo_Basestation_Behaviors_BehaviorSinging_H__
