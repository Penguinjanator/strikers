// Debug hooks the decompilation declares and does not define.

#include <cstdio>
#include <cstdlib>

#include "NL/plat/plataudio.h"

void InstallFloatingPointExceptionHandler(void)
{
    std::fprintf(stderr, "[port] floatingPointExceptions: ignored; the host does not "
                         "raise the console's FPU exceptions.\n");
}

void InstallCallStackDumper(void)
{
    std::fprintf(stderr, "[port] callStackDumper: ignored; src/platform/crashlog.c "
                         "already reports a backtrace on a fatal signal.\n");
}

namespace PlatAudio
{
// Reverb is a MusyX feature and MusyX is not built.
bool SetSFXReverbVol(unsigned long uVoiceID, float fVol)
{
    (void)uVoiceID;
    (void)fVol;
    return false;
}
}   // namespace PlatAudio

// STRIKERS_UNLOCK_ALL answers the game's own `givealltrophies` test, unset on disc.
static int s_unlockAll = -1;

bool PortUnlockAll(void)
{
    if (s_unlockAll < 0)
        s_unlockAll = std::getenv("STRIKERS_UNLOCK_ALL") != nullptr ? 1 : 0;
    return s_unlockAll != 0;
}

extern "C" void PortSetUnlockAll(int on) { s_unlockAll = on ? 1 : 0; }
extern "C" int PortGetUnlockAll(void) { return PortUnlockAll() ? 1 : 0; }
