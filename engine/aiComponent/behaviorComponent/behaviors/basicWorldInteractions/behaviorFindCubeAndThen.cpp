/**
 * File: BehaviorFindCubeAndThen.cpp
 *
 * Author: Sam Russell
 * Created: 2018-08-20
 *
 * Description: Run BehaviorFindCube. Upon locating a cube, go to the pre-dock pose if found/known, connect to the cube,
 *              (visiblly if this behavior was user driven) then delegate to the followUp behavior, if specified.
 *
 * Copyright: Anki, Inc. 2018
 *
 **/


#include "engine/aiComponent/behaviorComponent/behaviors/basicWorldInteractions/behaviorFindCubeAndThen.h"

#include "engine/actions/animActions.h"
#include "engine/actions/basicActions.h"
#include "engine/actions/driveToActions.h"
#include "engine/aiComponent/behaviorComponent/behaviorContainer.h"
#include "engine/aiComponent/behaviorComponent/behaviorExternalInterface/behaviorExternalInterface.h"
#include "engine/aiComponent/behaviorComponent/behaviorExternalInterface/beiRobotInfo.h"
#include "engine/aiComponent/behaviorComponent/behaviors/basicWorldInteractions/behaviorFindCube.h"
#include "engine/blockWorld/blockWorld.h"
#include "engine/blockWorld/blockWorldFilter.h"
#include "engine/components/cubes/cubeConnectionCoordinator.h"
#include "engine/components/sensors/proxSensorComponent.h"

#include "coretech/common/engine/utils/timer.h"
#include "util/logging/logging.h"

#define LOG_CHANNEL "Behaviors"

#define SET_STATE(s) do{ \
                          _dVars.state = FindCubeState::s; \
                          SetDebugStateName(#s); \
                        } while(0);

namespace Anki {
namespace Vector {

namespace{
const char* const kFollowUpBehaviorKey = "followUpBehaviorID";
const char* const kSkipConnectToCubeBehaviorKey = "skipConnectToCubeBehavior";
const char* const kSkipFindIfCubeVisibleKey = "skipFindIfCubeVisible";
const char* const kSkipFindMaxCubeAgeKey = "skipFindMaxCubeAge_s";
const float kDefaultSkipFindMaxCubeAge_s = 5.0f;
} // namespace

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
BehaviorFindCubeAndThen::InstanceConfig::InstanceConfig()
: skipConnectToCubeBehavior(true) //default to true and override for user driven instances
, skipFindIfCubeVisible(false)
, skipFindMaxCubeAge_s(kDefaultSkipFindMaxCubeAge_s)
{
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
BehaviorFindCubeAndThen::DynamicVariables::DynamicVariables()
: state(FindCubeState::FindCube)
, cubeID()
{
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
BehaviorFindCubeAndThen::BehaviorFindCubeAndThen(const Json::Value& config)
 : ICozmoBehavior(config)
{
  JsonTools::GetValueOptional(config, kFollowUpBehaviorKey, _iConfig.followUpBehaviorID);
  JsonTools::GetValueOptional(config, kSkipConnectToCubeBehaviorKey, _iConfig.skipConnectToCubeBehavior);
  JsonTools::GetValueOptional(config, kSkipFindIfCubeVisibleKey, _iConfig.skipFindIfCubeVisible);
  JsonTools::GetValueOptional(config, kSkipFindMaxCubeAgeKey, _iConfig.skipFindMaxCubeAge_s);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
BehaviorFindCubeAndThen::~BehaviorFindCubeAndThen()
{
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool BehaviorFindCubeAndThen::WantsToBeActivatedBehavior() const
{
  return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorFindCubeAndThen::GetBehaviorOperationModifiers(BehaviorOperationModifiers& modifiers) const
{
  modifiers.wantsToBeActivatedWhenOnCharger = true;
  modifiers.wantsToBeActivatedWhenCarryingObject = false; 
  modifiers.cubeConnectionRequirements = BehaviorOperationModifiers::CubeConnectionRequirements::OptionalActive;
  modifiers.connectToCubeInBackground = true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorFindCubeAndThen::GetAllDelegates(std::set<IBehavior*>& delegates) const
{
  delegates.insert(_iConfig.findCubeBehavior.get());
  delegates.insert(_iConfig.connectToCubeBehavior.get());
  if(nullptr != _iConfig.followUpBehavior){
    delegates.insert(_iConfig.followUpBehavior.get());
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorFindCubeAndThen::GetBehaviorJsonKeys(std::set<const char*>& expectedKeys) const
{
  const char* list[] = {
    kFollowUpBehaviorKey,
    kSkipConnectToCubeBehaviorKey,
    kSkipFindIfCubeVisibleKey,
    kSkipFindMaxCubeAgeKey
  };
  expectedKeys.insert( std::begin(list), std::end(list) );
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorFindCubeAndThen::InitBehavior()
{
  const auto& BC = GetBEI().GetBehaviorContainer();

  BC.FindBehaviorByIDAndDowncast<BehaviorFindCube>(BEHAVIOR_ID(FindCube),
                                                   BEHAVIOR_CLASS(FindCube),
                                                   _iConfig.findCubeBehavior);
  DEV_ASSERT(nullptr != _iConfig.findCubeBehavior,
             "BehaviorFindCubeAndThen.InitBehavior.NullFindCubeBehavior");
             
  _iConfig.connectToCubeBehavior = BC.FindBehaviorByID(BEHAVIOR_ID(ConnectToCube));
  DEV_ASSERT(nullptr != _iConfig.connectToCubeBehavior,
             "BehaviorFindCubeAndThen.InitBehavior.NullConnectToCubeBehavior");

  if(!_iConfig.followUpBehaviorID.empty()){
    _iConfig.followUpBehavior = FindBehavior(_iConfig.followUpBehaviorID);
    DEV_ASSERT(nullptr != _iConfig.followUpBehavior,
              "BehaviorFindCubeAndThen.InitBehavior.NullFollowUpBehavior");
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool BehaviorFindCubeAndThen::TryGetRecentlyVisibleCube()
{
  BlockWorldFilter filter;
  filter.AddFilterFcn(&BlockWorldFilter::IsLightCubeFilter);

  const ObservableObject* obj = GetBEI().GetBlockWorld().FindMostRecentlyObservedObject(filter);
  if(obj == nullptr){
    return false;
  }

  const RobotTimeStamp_t cubeTime_ms = obj->GetLastObservedTime();
  const RobotTimeStamp_t lastImage_ms = GetBEI().GetRobotInfo().GetLastImageTimeStamp();
  const RobotTimeStamp_t maxAge_ms =
    static_cast<RobotTimeStamp_t>(1000.f * _iConfig.skipFindMaxCubeAge_s);

  if(cubeTime_ms + maxAge_ms < lastImage_ms){
    return false;
  }

  _dVars.cubeID = obj->GetID();
  return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorFindCubeAndThen::OnBehaviorActivated() 
{
  // reset dynamic variables
  _dVars = DynamicVariables();

  PRINT_NAMED_INFO("KeepawayFlow",
                   "FindCubeAndThen step=activated followUp=%s skipConnect=%d skipFindIfVisible=%d",
                   _iConfig.followUpBehaviorID.c_str(),
                   _iConfig.skipConnectToCubeBehavior ? 1 : 0,
                   _iConfig.skipFindIfCubeVisible ? 1 : 0);

  // 3A: logo already seen recently + off charger → straight to Keepaway (no FindCube / DriveToPredock).
  const bool onCharger = GetBEI().GetRobotInfo().IsOnChargerContacts();
  const bool hasFreshCube = _iConfig.skipFindIfCubeVisible && TryGetRecentlyVisibleCube();
  if(_iConfig.skipFindIfCubeVisible && !onCharger && hasFreshCube){
    PRINT_NAMED_INFO("KeepawayFlow",
                     "FindCubeAndThen step=skip_FindCube cube=%d ageMax_s=%.1f followUp=%s",
                     _dVars.cubeID.GetValue(),
                     _iConfig.skipFindMaxCubeAge_s,
                     _iConfig.followUpBehaviorID.c_str());
    auto& ccc = GetBEI().GetCubeConnectionCoordinator();
    ccc.SubscribeToCubeConnection(this);
    TransitionToFollowUpBehavior();
    return;
  }

  if(_iConfig.skipFindIfCubeVisible){
    PRINT_NAMED_INFO("KeepawayFlow",
                     "FindCubeAndThen step=FindCube (no skip: onCharger=%d hasFreshCube=%d) followUp=%s",
                     onCharger ? 1 : 0,
                     hasFreshCube ? 1 : 0,
                     _iConfig.followUpBehaviorID.c_str());
    _dVars.cubeID = ObjectID();
  }

  TransitionToFindCube();
}


// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorFindCubeAndThen::BehaviorUpdate() 
{
  if( !IsActivated() ) {
    return;
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorFindCubeAndThen::TransitionToFindCube()
{
  SET_STATE(FindCube);
  PRINT_NAMED_INFO("KeepawayFlow", "FindCubeAndThen step=FindCube followUp=%s",
                   _iConfig.followUpBehaviorID.c_str());

  if(_iConfig.findCubeBehavior->WantsToBeActivated()){
    DelegateIfInControl(_iConfig.findCubeBehavior.get(),
      [this](){
        _dVars.cubeID = _iConfig.findCubeBehavior->GetFoundCubeID();
        auto* targetCube = GetTargetCube(); 
        if(nullptr != targetCube){
          PRINT_NAMED_INFO("KeepawayFlow", "FindCubeAndThen step=cube_found id=%d followUp=%s",
                           _dVars.cubeID.GetValue(), _iConfig.followUpBehaviorID.c_str());
          TransitionToDriveToPredockPose();
        } else {
          PRINT_NAMED_INFO("KeepawayFlow", "FindCubeAndThen step=cube_NOT_found followUp=%s — exit",
                           _iConfig.followUpBehaviorID.c_str());
        } //else just exit, the get-out will have been handled by the findCubeBehavior since it didn't find anything
      });
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorFindCubeAndThen::TransitionToDriveToPredockPose()
{
  SET_STATE(DriveToPredockPose);
  PRINT_NAMED_INFO("KeepawayFlow", "FindCubeAndThen step=DriveToPredock followUp=%s",
                   _iConfig.followUpBehaviorID.c_str());

  DelegateIfInControl( new DriveToObjectAction( _dVars.cubeID, PreActionPose::ActionType::DOCKING ),
                       &BehaviorFindCubeAndThen::TransitionToAttemptConnection );
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorFindCubeAndThen::TransitionToAttemptConnection()
{
  SET_STATE(AttemptConnection);
  PRINT_NAMED_INFO("KeepawayFlow", "FindCubeAndThen step=AttemptConnection skip=%d followUp=%s",
                   _iConfig.skipConnectToCubeBehavior ? 1 : 0,
                   _iConfig.followUpBehaviorID.c_str());

  if(_iConfig.skipConnectToCubeBehavior){
    auto& ccc = GetBEI().GetCubeConnectionCoordinator();
    ccc.SubscribeToCubeConnection(this);
    TransitionToFollowUpBehavior();
  }
  else{
    if(_iConfig.connectToCubeBehavior->WantsToBeActivated()){
      DelegateIfInControl(_iConfig.connectToCubeBehavior.get(),
        [this](){
          auto& ccc = GetBEI().GetCubeConnectionCoordinator();
          if(ccc.IsConnectedToCube()){
            // Convert the background subscription to foreground
            ccc.SubscribeToCubeConnection(this);
          }

          TransitionToFollowUpBehavior();
        });
    }
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorFindCubeAndThen::TransitionToFollowUpBehavior()
{
  SET_STATE(FollowUpBehavior);
  PRINT_NAMED_INFO("KeepawayFlow", "FindCubeAndThen step=FollowUp followUp=%s wants=%d",
                   _iConfig.followUpBehaviorID.c_str(),
                   (nullptr != _iConfig.followUpBehavior && _iConfig.followUpBehavior->WantsToBeActivated()) ? 1 : 0);
  if(nullptr != _iConfig.followUpBehavior && _iConfig.followUpBehavior->WantsToBeActivated()){
    // Let the behavior end when the followup does
    DelegateIfInControl(_iConfig.followUpBehavior.get());
  }
  // If there is no followUp behavior, end now
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorFindCubeAndThen::TransitionToGetOutFailure()
{
  SET_STATE(GetOutFailure);
  DelegateIfInControl(new TriggerLiftSafeAnimationAction(AnimationTrigger::FetchCubeFailure));
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
ObservableObject* BehaviorFindCubeAndThen::GetTargetCube()
{
  return GetBEI().GetBlockWorld().GetLocatedObjectByID(_dVars.cubeID);
}

} // namespace Vector
} // namespace Anki
