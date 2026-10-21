// The lockstep tick loop, its RNG streams, clocks, events, barriers and nets.

#include "port/lockstep.h"
#include "port/determinism.h"
#include "port/online.h"
#include "port/region.h"
#include "port/simhash.h"

#include "Game/Ball.h"
#include "Game/Goalie.h"
#include "Game/DB/StatsTracker.h"
#include "NL/nlDLRing.h"
#include "Game/FixedUpdateTask.h"
#include "Game/FlickDetection.h"
#include "Game/Game.h"
#include "Game/GameInfo.h"
#include "Game/NisPlayer.h"
#include "Game/Render/NetMesh.h"
#include "Game/Sys/clock.h"
#include "Game/Sys/eventman.h"
#include "Game/Team.h"
#include "NL/globalpad.h"
#include "NL/nlConfig.h"
#include "NL/nlMath.h"
#include "NL/platpad.h"
#include "dolphin/os.h"
#include "dolphin/pad.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>


extern void AIEventHandler(Event*, void*);
extern float g_fFixedUpdateTick;
extern float g_fSimulationTick;

extern "C" {
int g_simLockstep;
}

extern "C" void PortLockstepInit(void)
{
    const char* v = std::getenv("STRIKERS_LOCKSTEP");
    g_simLockstep = v != NULL && std::strcmp(v, "1") == 0;
}

namespace
{
unsigned int s_tick;
unsigned int s_match;
int s_inTick;
}   // namespace

extern "C" unsigned int PortSimTick(void) { return s_tick; }

extern "C" unsigned int PortSimMatch(void) { return s_match; }

extern "C" int PortSimInTick(void) { return s_inTick; }

extern "C" void PortSimTickBegin(void)
{
    s_inTick = 1;
}

extern "C" void PortSimTickEnd(void)
{
    s_inTick = 0;
    s_tick++;
}

namespace
{
// Nanoseconds of simulation time due and not yet run, so no float residue carries between matches.
long long s_dueNs;
// Set by BeginGame: the match's schedule starts at its first frame, not at the load before it.
int s_startSchedule;
// A time scale for the top of the next tick.
int s_scalePending;
float s_pendingScale;
PADStatus s_pads[4];
// Buttons down in any frame since local input last went into a tick.
u16 s_pressedSinceTick[4];
// The sticks' polar form as the last tick left it; a pad keeps its angle while its stick rests,
// and the frame's own pad update must not set it.
nlPolar s_polar[4][2];
// An online session feeds the input, holds the ticks and hears each tick's hash.
int s_online;
// Online, the session let this match's first tick run.
int s_begun;

long long tick_ns(void) { return std::llround((double)g_fFixedUpdateTick * 1e9); }

float pace_scale(void) { return s_scalePending ? s_pendingScale : FixedUpdateTask::mTimeScale; }

void nets_restore(void);
void nets_save(void);
bool online_may_run(unsigned int k);
void online_tick_top(unsigned int k);
void online_tick_end(unsigned int k);

void run_tick(unsigned int k)
{
    PADStatus frame[4];

    PortSimTickBegin();
    nets_restore();
    if (s_scalePending)
    {
        FixedUpdateTask::mTimeScale = s_pendingScale;
        s_scalePending = 0;
    }
    const int scope = PortRngSetScope(PORT_RNG_SIM);
    if (s_online)
        online_tick_top(k);
    PortSimRunTickActions(k);

    std::memcpy(frame, PadStatus::s_Current, sizeof frame);
    std::memcpy(PadStatus::s_Current, s_pads, sizeof frame);
    nlPolar framePolar[4][2];
    for (int i = 0; i < 4; i++)
    {
        cGlobalPad* pad = cPadManager::GetPad(i);
        framePolar[i][0] = pad->m_polarAnalogLeft;
        framePolar[i][1] = pad->m_polarAnalogRight;
        pad->m_polarAnalogLeft = s_polar[i][0];
        pad->m_polarAnalogRight = s_polar[i][1];
    }
    UseFixedUpdatePad();
    UpdatePlatPad(g_fFixedUpdateTick);
    cPadManager::Update(g_fFixedUpdateTick);
    FlickDetection::Update();

    PortSimStepClocks(g_fSimulationTick);
    FixedUpdateTask::CallFixedUpdateTasks();
    PortSimDispatchEvents();
    PortSimCameraStep();
    nets_save();

    std::memcpy(PadStatus::s_Current, frame, sizeof frame);
    for (int i = 0; i < 4; i++)
    {
        cGlobalPad* pad = cPadManager::GetPad(i);
        s_polar[i][0] = pad->m_polarAnalogLeft;
        s_polar[i][1] = pad->m_polarAnalogRight;
        pad->m_polarAnalogLeft = framePolar[i][0];
        pad->m_polarAnalogRight = framePolar[i][1];
    }
    PortRngSetScope(scope);
    if (s_online)
        online_tick_end(k);
    PortSimTickEnd();
}
}   // namespace

extern "C" void PortLockstepRunTicks(float dt)
{
    const long long tick = tick_ns();
    for (int i = 0; i < 4; i++)
        s_pressedSinceTick[i] |= PadStatus::s_Current[i].button;

    // Online, the first tick waits for every machine to load and for the host's start time.
    if (s_online && !s_begun)
    {
        if (!PortOnlineMayBeginMatch())
        {
            FixedUpdateTask::mAccumulatedDeltaT = 0.0f;
            return;
        }
        s_begun = 1;
    }
    // The first frame of a match runs tick 0 alone; its dt is the time spent loading.
    if (s_startSchedule)
        s_startSchedule = 0;
    else
    {
        // Online, time sync and frame stretching scale the frame time the schedule gets.
        const double rate = s_online ? (double)PortOnlineClockRate() : 1.0;
        s_dueNs += std::llround((double)dt * (double)pace_scale() * rate * 1e9);
    }
    while (s_dueNs >= tick)
    {
        const bool may = s_online ? online_may_run(s_tick) : PortSimMayRunTick(s_tick) != 0;
        if (!may || !(s_online ? PortOnlineTickInput(s_tick, s_pads) : PortSimLocalInput(s_tick, s_pads)))
        {
            // Held ticks build no backlog; the picture stays on the newest tick.
            s_dueNs = tick;
            break;
        }
        s_dueNs -= tick;
        const float before = pace_scale();
        run_tick(s_tick);
        // A pace the tick set starts at the tick's due time, so real time already past it counts at
        // the new pace and the next tick falls a fixed real time after this one.
        const float after = pace_scale();
        if (after != before && before > 0.0f)
            s_dueNs = std::llround((double)s_dueNs * (double)after / (double)before);
    }
    FixedUpdateTask::mAccumulatedDeltaT = (float)((double)(s_dueNs < tick ? s_dueNs : tick) / 1e9);
}

extern "C" void PortSimSetTimeScale(float scale)
{
    s_pendingScale = scale;
    s_scalePending = 1;
}

extern "C" int PortSimLocalInput(unsigned int tick, PADStatus* pads)
{
    (void)tick;
    std::memcpy(pads, PadStatus::s_Current, 4 * sizeof(PADStatus));
    // The camera's turn comes before quantising, as a sender's would.
    PortSimTurnSticks(pads);
    for (int i = 0; i < 4; i++)
    {
        const PADStatus in = pads[i];
        PADStatus& out = pads[i];
        std::memset(&out, 0, sizeof out);
        out.button = (u16)((in.button | s_pressedSinceTick[i]) & PORT_INPUT_BUTTONS);
        out.stickX = PortSimQuantizeStick(in.stickX);
        out.stickY = PortSimQuantizeStick(in.stickY);
        out.substickX = PortSimQuantizeStick(in.substickX);
        out.substickY = PortSimQuantizeStick(in.substickY);
        out.triggerLeft = in.triggerLeft;
        out.triggerRight = in.triggerRight;
        out.err = in.err;
        s_pressedSinceTick[i] = 0;
    }
    return 1;
}

namespace
{
thread_local int s_rngScope = PORT_RNG_COSMETIC;

unsigned int s_cosmeticSeed;
bool s_cosmeticSeeded;

PortSimMatchSettings s_settings;
bool s_sessionSettings;
// The online session installed this tick's seed; a kickoff in the tick keeps it.
bool s_reseeded;

unsigned int* cosmetic_stream(void)
{
    // Takes over where the process's stream stood, so the front end draws as it always did.
    if (!s_cosmeticSeeded)
    {
        s_cosmeticSeed = nlDefaultSeed;
        s_cosmeticSeeded = true;
    }
    return &s_cosmeticSeed;
}

// The murmur3 finaliser: every bit of the seed and the goals reaches every bit.
uint32_t mix32(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}
}   // namespace

extern "C" int PortRngScope(void) { return s_rngScope; }

extern "C" int PortRngSetScope(int scope)
{
    const int previous = s_rngScope;
    s_rngScope = scope;
    return previous;
}

extern "C" unsigned int* PortRngCosmeticSeed(void)
{
    s_cosmeticSeeded = true;
    return &s_cosmeticSeed;
}

extern "C" unsigned int* PortRngStream(void)
{
    if (s_rngScope == PORT_RNG_SIM)
        return &nlDefaultSeed;
    return cosmetic_stream();
}

extern "C" void PortSimSetMatchSettings(const PortSimMatchSettings* settings)
{
    s_sessionSettings = settings != NULL;
    if (settings != NULL)
        s_settings = *settings;
}

extern "C" const PortSimMatchSettings* PortSimSettings(void) { return &s_settings; }

extern "C" int PortRngBeginMatch(void)
{
    const int previous = PortRngSetScope(PORT_RNG_SIM);
    if (!g_simLockstep)
        return previous;
    if (!s_sessionSettings)
    {
        unsigned int seed;
        if (!PortFixedSeed(&seed))
            seed = nlRandom(0xFFFFFFFFu, cosmetic_stream());
        s_settings.seed = seed;
        s_settings.bowserRepeat = GetConfigBool(Config::Global(), "bowser_repeat", false) ? 1 : 0;
        s_settings.pureGame = 0;
        s_settings.pauseAllowed = 0;
    }
    nlSetRandomSeed(s_settings.seed, &nlDefaultSeed);
    return previous;
}

extern "C" int PortRngBeginKickoff(unsigned int goals)
{
    const int previous = PortRngSetScope(PORT_RNG_SIM);
    if (g_simLockstep && !s_reseeded)
    {
        const uint32_t h = mix32(mix32(mix32(s_settings.seed) ^ goals));
        nlSetRandomSeed(h, &nlDefaultSeed);
    }
    return previous;
}

// cGame's three clocks, its only ones; in lockstep mode ClockUpdateTask leaves them to the tick.
extern "C" void PortSimStepClocks(float dt) { ClockManager::Update(dt); }

extern "C" int PortSimEventHandler(const EventHandler* handler)
{
    const EventCallback callback = handler->m_pCBFunction;
    return callback == AIEventHandler || callback == Goalie::EventHandler ||
           callback == StatsTracker::eventHandler;
}

extern "C" void PortSimHoldEvent(EventManager* manager, Event* event)
{
    if (!g_simLockstep || manager->m_handlers == NULL)
        return;
    EventHandler* handler = nlDLRingGetStart(manager->m_handlers);
    for (;;)
    {
        if (PortSimEventHandler(handler) &&
            (handler->m_uDestinationMask & manager->m_dest[event->m_uEventID]) != 0)
        {
            event->m_nReferenceCount = 1;
            return;
        }
        if (handler == manager->m_handlers)
            return;
        handler = handler->m_next;
    }
}

static void dispatch_sim_event(EventManager* manager, Event* event)
{
    EventHandler* handler = nlDLRingGetStart(manager->m_handlers);
    if (handler == NULL)
        return;
    for (;;)
    {
        if (PortSimEventHandler(handler) &&
            (handler->m_uDestinationMask & manager->m_dest[event->m_uEventID]) != 0)
            handler->m_pCBFunction(event, handler->m_pCBParam);
        if (handler == manager->m_handlers)
            return;
        handler = handler->m_next;
    }
}

extern "C" void PortSimDispatchEvents(void)
{
    const int scope = PortRngSetScope(PORT_RNG_SIM);
    EventManager* manager = g_pEventManager;
    manager->m_dispatching = true;
    // Held events precede queued events; callbacks defer new events until the next pass.
    while (manager->m_keep != NULL)
    {
        Event* event = nlDLRingRemoveStart(&manager->m_keep);
        dispatch_sim_event(manager, event);
        event->m_nReferenceCount = 0;
        nlDLRingAddEnd(&manager->m_free, event);
    }
    if (manager->m_queue != NULL)
    {
        Event* event = nlDLRingGetStart(manager->m_queue);
        for (;;)
        {
            if (event->m_nReferenceCount != 0)
            {
                dispatch_sim_event(manager, event);
                event->m_nReferenceCount = 0;
            }
            if (event == manager->m_queue)
                break;
            event = event->m_next;
        }
    }
    manager->m_dispatching = false;
    nlDLRingAppendRing(&manager->m_queue, manager->m_deferred);
    manager->m_deferred = NULL;
    PortRngSetScope(scope);
}

namespace
{
// The armed barrier; clearing it alone disarms everything below.
int s_barrier = PORT_BARRIER_NONE;
unsigned int s_freeze;   // the first tick held, and the tick play resumes at
unsigned int s_goalTick;
int s_event7;     // event 7 is still to come, in tick s_goalTick + 1
int s_done;       // this machine's presentation is over
int s_released;   // play resumes at the freeze tick: s_done offline, the host's RESUME online
int s_reached;    // online: the session has heard that the freeze holds
int s_pauseSlot = -1;
int s_pausesLeft[2];
u16 s_lastTickButtons[4];   // each slot's tick input in the tick before, for Start's edge
const int kPausesPerSide = 3;

// Ticks 0-2 run before the intro, as a fresh process at 50 Hz ran them.
const unsigned int kIntroTicks = 3;
// The goal script's second of simulation time: ticks g+1 to g+50 run after goal tick g.
const unsigned int kPostGoalTicks = 50;

const char* barrier_name(int kind)
{
    return kind == PORT_BARRIER_INTRO  ? "intro"
           : kind == PORT_BARRIER_GOAL ? "goal"
           : kind == PORT_BARRIER_END  ? "end"
                                       : "pause";
}

unsigned int next_tick(void) { return PortSimTick() + (PortSimInTick() ? 1u : 0u); }
}   // namespace

extern "C" void PortSimBarrierArm(int kind)
{
    s_barrier = kind;
    s_done = 0;
    s_released = 0;
    s_reached = 0;
    s_event7 = 0;
    if (kind == PORT_BARRIER_INTRO)
        s_freeze = kIntroTicks;
    else if (kind == PORT_BARRIER_GOAL)
    {
        s_goalTick = PortSimTick();
        s_event7 = 1;
        s_freeze = s_goalTick + kPostGoalTicks + 1;
    }
    else
    {
        s_freeze = next_tick();
        OSReport("[lockstep] end freezes t=%u\n", s_freeze);
    }
}

extern "C" int PortSimMayRunTick(unsigned int tick)
{
    return s_barrier == PORT_BARRIER_NONE || s_released || tick < s_freeze;
}

extern "C" void PortSimRunTickActions(unsigned int tick)
{
    if (s_barrier == PORT_BARRIER_NONE)
        return;
    if (s_event7 && tick == s_goalTick + 1)
    {
        s_event7 = 0;
        // The goal script's call only presents; the 20 draws and the targets belong to the match.
        NisPlayer::Instance()->PlayCharacterDirection();
        OSReport("[lockstep] goal t=%u, event 7 t=%u, freeze t=%u\n", s_goalTick, tick, s_freeze);
    }
    if (s_released && tick >= s_freeze)
    {
        const int kind = s_barrier;
        s_barrier = PORT_BARRIER_NONE;
        s_pauseSlot = -1;
        OSReport("[lockstep] %s resumes t=%u\n", barrier_name(kind), tick);
        if (kind != PORT_BARRIER_END && kind != PORT_BARRIER_PAUSE &&
            g_pGame->m_eGameState != GS_END_GAME)
            g_pGame->ChangeGameState(GS_KICKOFF);
    }
}

extern "C" void PortSimBarrierReached(int kind)
{
    // Only the run event 7 started counts, so event 7 always comes before the freeze.
    const unsigned int next = next_tick();
    if (kind == s_barrier && !s_event7 && next < s_freeze)
    {
        s_freeze = next;
        OSReport("[lockstep] %s event 8 t=%u, freeze t=%u\n", barrier_name(kind), next - 1, next);
    }
}

// Offline this machine is every machine, so play resumes at the freeze tick, or runs through it.
extern "C" void PortSimBarrierDone(int kind)
{
    if (kind == PORT_BARRIER_NONE || kind != s_barrier || s_done)
        return;
    s_done = 1;
    if (!s_online)
        s_released = 1;
    else if (s_reached)
        PortOnlineBarrierDone(kind);
}

extern "C" unsigned int PortSimPresentationId(void)
{
    if (s_barrier == PORT_BARRIER_INTRO)
        return 1;
    if (s_barrier == PORT_BARRIER_GOAL)
        return s_goalTick + 2;
    if (s_barrier == PORT_BARRIER_END)
        return s_freeze + 2;
    return 0;
}

extern "C" int PortSimPauseSlot(void)
{
    return s_barrier == PORT_BARRIER_PAUSE && !s_released ? s_pauseSlot : -1;
}

extern "C" int PortSimBarrier(void)
{
    return (s_barrier != PORT_BARRIER_NONE && !s_done && PortSimTick() >= s_freeze)
               ? s_barrier
               : PORT_BARRIER_NONE;
}

namespace
{
// Replays step, relax and jolt the live nets to draw them; the match keeps its own copy per tick.
struct NetCopy
{
    NetMesh* mesh;
    unsigned char object[sizeof(NetMesh)];
    std::vector<nlVector3> position;
    std::vector<nlVector3> prevPosition;
    std::vector<nlVector3> accel;
};

NetCopy s_nets[2];

void net_copy(NetCopy& c, bool save)
{
    NetMesh* m = c.mesh;
    const size_t n = (size_t)m->m_NumParticles;
    if (save)
    {
        std::memcpy(c.object, (const void*)m, sizeof c.object);
        std::memcpy(c.position.data(), m->m_v3Position, n * sizeof(nlVector3));
        std::memcpy(c.prevPosition.data(), m->m_v3PrevPosition, n * sizeof(nlVector3));
        std::memcpy(c.accel.data(), m->m_v3Accel, n * sizeof(nlVector3));
    }
    else
    {
        std::memcpy((void*)m, c.object, sizeof c.object);
        std::memcpy(m->m_v3Position, c.position.data(), n * sizeof(nlVector3));
        std::memcpy(m->m_v3PrevPosition, c.prevPosition.data(), n * sizeof(nlVector3));
        std::memcpy(m->m_v3Accel, c.accel.data(), n * sizeof(nlVector3));
    }
}

void nets_capture(void)
{
    NetMesh* meshes[2] = { NetMesh::spPositiveXNetMesh, NetMesh::spNegativeXNetMesh };
    for (int i = 0; i < 2; i++)
    {
        NetCopy& c = s_nets[i];
        c.mesh = meshes[i] != NULL && meshes[i]->m_v3Position != NULL ? meshes[i] : NULL;
        if (c.mesh == NULL)
            continue;
        const size_t n = (size_t)c.mesh->m_NumParticles;
        c.position.resize(n);
        c.prevPosition.resize(n);
        c.accel.resize(n);
        net_copy(c, true);
    }
}

// Only the nets captured at BeginGame, still the stadium's.
bool net_current(const NetCopy& c, const NetMesh* live)
{
    return c.mesh != NULL && c.mesh == live && (size_t)live->m_NumParticles == c.position.size();
}

void nets_restore(void)
{
    if (net_current(s_nets[0], NetMesh::spPositiveXNetMesh))
        net_copy(s_nets[0], false);
    if (net_current(s_nets[1], NetMesh::spNegativeXNetMesh))
        net_copy(s_nets[1], false);
}

void nets_save(void)
{
    if (net_current(s_nets[0], NetMesh::spPositiveXNetMesh))
        net_copy(s_nets[0], true);
    if (net_current(s_nets[1], NetMesh::spNegativeXNetMesh))
        net_copy(s_nets[1], true);
}
}   // namespace

namespace
{
int s_ended;   // the session has heard this match's last tick

// The components a desync report compares; it names the first that differs.
const char* const kComponents[] = { "ball",  "c0",   "c1",  "c2",  "c3",   "c4",
                                    "c5",    "c6",   "c7",  "c8",  "c9",   "clk",
                                    "score", "game", "pwr", "npc", "seed", "scr" };
const int kComponentCount = (int)(sizeof kComponents / sizeof kComponents[0]);
const unsigned int kComponentEvery = 50;


bool online_may_run(unsigned int k)
{
    if (!PortOnlineMayRunTick(k))
        return false;
    OnlineTickActions a;
    if (s_barrier != PORT_BARRIER_NONE && !s_released && k >= s_freeze &&
        PortOnlineTickActions(k, &a) && (a.actions & ONLINE_ACTION_RESUME) != 0)
        s_released = 1;
    if (PortSimMayRunTick(k))
        return true;
    if (s_barrier != PORT_BARRIER_NONE && !s_reached)
    {
        s_reached = 1;
        PortOnlineBarrierReached(s_barrier, k);
        if (s_done)
            PortOnlineBarrierDone(s_barrier);
    }
    return false;
}

// What Choose Sides does from the pause menu, at a tick every machine runs.
void slots_to_cpu(unsigned int slots)
{
    GameInfoManager* info = nlSingleton<GameInfoManager>::Instance();
    for (int s = 0; s < 4; s++)
    {
        if (slots & (1u << s))
            info->SetPlayingSide((unsigned short)s, -1);
    }
    g_pTeams[0]->UpdateControllers();
    g_pTeams[1]->UpdateControllers();
    info->ApplyDifficultySettings();
    g_pGame->SetDifficulty(info->GetDifficulty(0), info->GetDifficulty(1), (eDifficultyID)3);
}

void online_tick_top(unsigned int k)
{
    OnlineTickActions a;
    if (!PortOnlineTickActions(k, &a))
        return;
    if (a.actions & ONLINE_ACTION_RESEED)
    {
        nlSetRandomSeed(a.seed, &nlDefaultSeed);
        s_reseeded = true;
    }
    if (a.actions & ONLINE_ACTION_SLOTS_TO_CPU)
    {
        slots_to_cpu(a.cpuSlots);
        OSReport("[online] slots %x go to the CPU at t=%u\n", (unsigned)a.cpuSlots, k);
    }
    if (a.actions & ONLINE_ACTION_END_MATCH)
        OSReport("[online] the match ends unfinished at t=%u\n", k);
}

// A slot's new Start in tick k, in play and with no barrier armed, freezes tick k+1.
void online_pause_check(unsigned int k)
{
    const int state = g_pGame->m_eGameState;
    const bool inPlay = state == GS_KICKOFF || state == GS_GAMEPLAY || state == GS_OVERTIME;
    for (int s = 0; s < 4; s++)
    {
        const bool edge = (s_pads[s].button & PAD_BUTTON_START) != 0 &&
                          (s_lastTickButtons[s] & PAD_BUTTON_START) == 0;
        s_lastTickButtons[s] = s_pads[s].button;
        if (!edge || !s_settings.pauseAllowed || !inPlay || s_barrier != PORT_BARRIER_NONE)
            continue;
        const int side = nlSingleton<GameInfoManager>::Instance()->GetPlayingSide((unsigned short)s);
        if (side < 0 || side > 1 || s_pausesLeft[side] == 0)
            continue;
        s_pausesLeft[side]--;
        s_barrier = PORT_BARRIER_PAUSE;
        s_freeze = k + 1;
        s_done = 0;
        s_released = 0;
        s_reached = 0;
        s_event7 = 0;
        s_pauseSlot = s;
        OSReport("[lockstep] slot %d pauses, freeze t=%u, %d left for side %d\n", s, s_freeze,
                 s_pausesLeft[side], side);
    }
}

void online_tick_end(unsigned int k)
{
    PortSimHashes h;
    PortSimHash(&h);
    PortOnlineTickHash(k, h.all);
    if (k % kComponentEvery == 0)
    {
        uint64_t c[kComponentCount] = { h.ball };
        for (int i = 0; i < 10; i++)
            c[1 + i] = h.perChar[i];
        const uint64_t rest[] = { h.clk, h.score, h.game, h.pwr, h.npc, h.seed, h.scr };
        std::memcpy(&c[11], rest, sizeof rest);
        PortOnlineTickComponents(k, c, kComponents, kComponentCount);
    }
    s_reseeded = false;
    online_pause_check(k);
    // The last tick: the end holds the next one, and so does a sudden-death goal's freeze.
    if (!s_ended && s_barrier != PORT_BARRIER_NONE && !s_released && k + 1 >= s_freeze &&
        (s_barrier == PORT_BARRIER_END || g_pGame->m_eGameState == GS_END_GAME))
    {
        s_ended = 1;
        PortOnlineMatchEnded();
        OSReport("[online] last tick t=%u, hash %016llx\n", k, (unsigned long long)h.all);
    }
    // A cutscene stops the task that asks for ticks, so the session hears the freeze here.
    if (!s_ended && s_barrier != PORT_BARRIER_NONE && !s_released && !s_reached &&
        k + 1 >= s_freeze)
    {
        s_reached = 1;
        PortOnlineBarrierReached(s_barrier, k + 1);
        if (s_done)
            PortOnlineBarrierDone(s_barrier);
    }
}
}   // namespace

extern "C" void PortSimSetOnline(int online)
{
    s_online = online != 0;
}

extern "C" int PortSimOnline(void) { return s_online; }

extern "C" void PortSimBeginMatch(void)
{
    s_tick = 0;
    s_match++;
    // One tick due, as a fresh process's accumulator holds, so every match starts like the first.
    s_dueNs = tick_ns();
    s_startSchedule = 1;
    s_scalePending = 0;
    std::memset(s_pressedSinceTick, 0, sizeof s_pressedSinceTick);
    std::memset(s_polar, 0, sizeof s_polar);
    s_barrier = PORT_BARRIER_NONE;
    s_pauseSlot = -1;
    s_pausesLeft[0] = s_pausesLeft[1] = kPausesPerSide;
    std::memset(s_lastTickButtons, 0, sizeof s_lastTickButtons);
    s_begun = 0;
    s_ended = 0;
    s_reseeded = false;
    PortSimCameraBeginMatch();
    if (g_simLockstep)
        nets_capture();
}
