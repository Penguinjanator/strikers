#include "Game/Triggers/AnimTrigger.h"
#include "port/rollback.h"

SlotPool<AnimTriggerCallbackInfo> AnimTriggerCallbackInfo::m_AnimTriggerCallbackInfoSlotPool(0x10, 0x10);

static const bool s_rollbackState = (PortSimSlotPool(&AnimTriggerCallbackInfo::m_AnimTriggerCallbackInfoSlotPool), true);
