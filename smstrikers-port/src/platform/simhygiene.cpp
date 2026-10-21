// The floating-point environment and statics every match starts from.

#include "port/simhygiene.h"
#include "Game/FixedUpdateTask.h"
#include "Game/Physics/PhysicsFakeBall.h"

#include <cstdint>
#include <cstdio>

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace
{

#if defined(__aarch64__)
// FPCR: RMode (bits 23:22) 0 is round to nearest; FZ (24) and FIZ (0) flush denormals to zero.
const uint64_t kFpMask = (3ull << 22) | (1ull << 24) | 1ull;

uint64_t fp_read(void)
{
    uint64_t v;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(v));
    return v;
}

void fp_write(uint64_t v) { __asm__ __volatile__("msr fpcr, %0" : : "r"(v)); }
#elif defined(__x86_64__) || defined(_M_X64)
// MXCSR: RC (bits 14:13) 0 is round to nearest; FTZ (15) and DAZ (6) flush denormals to zero.
const uint64_t kFpMask = 0x6000u | 0x8000u | 0x40u;

uint64_t fp_read(void) { return _mm_getcsr(); }

void fp_write(uint64_t v) { _mm_setcsr((unsigned int)v); }
#endif

void fp_env_begin_match(void)
{
#if defined(__aarch64__) || defined(__x86_64__) || defined(_M_X64)
    const uint64_t was = fp_read();
    if ((was & kFpMask) == 0)
        return;
    fp_write(was & ~kFpMask);
    const uint64_t now = fp_read();
    std::fprintf(stderr, "[fpenv] control register was %#llx at match start, now %#llx%s\n",
                 (unsigned long long)was, (unsigned long long)now,
                 (now & kFpMask) != 0 ? "; round to nearest without flush to zero did not hold"
                                      : "");
#endif
}

}   // namespace

extern "C" void PortSimHygieneBeginMatch(void)
{
    fp_env_begin_match();
    // The ball predictions are keyed on the simulation time, so they go with it.
    FixedUpdateTask::mSimulationTime = 0.0f;
    FakeBallWorld::InvalidateBallCache();
    PortResetFielderActionsStatics();
    PortResetEffectsStatics();
}
