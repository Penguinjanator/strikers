#include "Game/AI/AILoader.h"

#include "NL/nlMemory.h"
#include "port/rollback.h"

#include "Game/Ball.h"
#include "Game/Physics/PhysicsFakeBall.h"
#include "Game/CharacterLifecycle.h"
#include "Game/Render/SidelineExplodable.h"

AILoader TheAILoader;

/**
 * Offset/Address/Size: 0x0 | 0x800056C0 | size: 0x50
 */
bool AILoader::StartLoad(LoadingManager* manager)
{
    PortSimHeapScope simHeap;
    g_pBall = new (nlMalloc(sizeof(cBall), 8, false)) cBall();

    FakeBallWorld::Init(g_pBall);

    CreateCharacters();

    // PORT: set up here, not on the first tick, so every snapshot a rollback loads already has it.
    if (!SidelineExplodableManager::sbIsInitialized)
        SidelineExplodableManager::Initialize();

    return true;
}
