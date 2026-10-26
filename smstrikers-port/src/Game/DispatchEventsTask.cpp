#include "Game/DispatchEventsTask.h"

#include "Game/Sys/eventman.h"
#include "port/lockstep.h"

extern EventManager* g_pEventManager;

/**
 * Offset/Address/Size: 0x0 | 0x801715A0 | size: 0x24
 */
void DispatchEventsTask::Run(float dt)
{
    // PORT: lockstep mode's simulation handlers run at tick ends, and here only while the task state runs no ticks.
    // PORT: an online pause sets it on the pausing machine alone; events wait for the resume tick.
    if (g_simLockstep && nlTaskManager::m_pInstance->m_CurrState != 2 && PortSimPauseSlot() < 0)
        PortSimDispatchEvents();
    g_pEventManager->DispatchEvents();
}
