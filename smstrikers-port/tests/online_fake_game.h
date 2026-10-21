// A stand-in game: the lockstep loop's rules, a hash chain of the inputs, barriers and loading.

#ifndef ONLINE_FAKE_GAME_H
#define ONLINE_FAKE_GAME_H

#include "../src/platform/online/session.h"

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

namespace online_test
{
using namespace online;

inline uint64_t mix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// What the fake game calls: a session directly, or the C API and its network thread.
class Side
{
  public:
    virtual ~Side() {}
    virtual void Update() = 0;
    virtual void SampleLocalPads(const LocalPad ports[4]) = 0;
    virtual const OnlineState& State() = 0;
    virtual void LoadProgress(int percent) = 0;
    virtual void MatchLoaded() = 0;
    virtual int MayBeginMatch() = 0;
    virtual float ClockRate() = 0;
    virtual int TickInput(uint32_t tick, SlotInput out[ONLINE_MAX_SLOTS]) = 0;
    virtual void BarrierReached(int kind, uint32_t tick) = 0;
    virtual void BarrierDone(int kind) = 0;
    virtual int MayRunTick(uint32_t tick) = 0;
    virtual int TickActions(uint32_t tick, OnlineTickActions* out) = 0;
    virtual void TickHash(uint32_t tick, uint64_t hash) = 0;
    virtual void TickComponents(uint32_t tick, const uint64_t* hashes, const char* const* names,
                                int count) = 0;
    virtual void MatchEnded() = 0;
    virtual void Flush() = 0;
};

class SessionSide : public Side
{
  public:
    explicit SessionSide(Session* session) : s(session) {}
    void Update() override
    {
        s->Update(1.0f);
        uint32_t serial;
        const std::string report = s->TakeReport(serial);
        s->ReportSaved(s->SaveReport(report), serial);
    }
    void SampleLocalPads(const LocalPad ports[4]) override { s->SampleLocalPads(ports); }
    const OnlineState& State() override { return s->State(); }
    void LoadProgress(int percent) override { s->LoadProgress(percent); }
    void MatchLoaded() override { s->MatchLoaded(); }
    int MayBeginMatch() override { return s->MayBeginMatch(); }
    float ClockRate() override { return s->ClockRate(); }
    int TickInput(uint32_t tick, SlotInput out[ONLINE_MAX_SLOTS]) override
    {
        return s->TickInput(tick, out);
    }
    void BarrierReached(int kind, uint32_t tick) override { s->BarrierReached(kind, tick); }
    void BarrierDone(int kind) override { s->BarrierDone(kind); }
    int MayRunTick(uint32_t tick) override { return s->MayRunTick(tick); }
    int TickActions(uint32_t tick, OnlineTickActions* out) override
    {
        return s->TickActions(tick, out);
    }
    void TickHash(uint32_t tick, uint64_t hash) override { s->TickHash(tick, hash); }
    void TickComponents(uint32_t tick, const uint64_t* hashes, const char* const* names,
                        int count) override
    {
        s->TickComponents(tick, hashes, names, count);
    }
    void MatchEnded() override { s->MatchEnded(); }
    void Flush() override { s->Flush(); }

    Session* s;
};

struct MatchResult
{
    uint32_t serial;
    uint32_t ticks;
    uint64_t state;
    int goals;
};

class FakeGame
{
  public:
    enum
    {
        kTickUs = 20000
    };

    FakeGame(Side* side, uint32_t seed, int humans)
        : s(side), rng(seed), humans(humans), matchTicks(3000), goalOdds(1500), flipAtTick(-1),
          loadedSerial(0), loadStartUs(0), loadUs(0), playing(false), tick(0), dueUs(0), state(0),
          armedKind(0), freezeTick(0), held(false), presenting(false), presentEndUs(0), goals(0),
          stalling(false), stallStartUs(0), maxStallMs(0), stallCount(0), longStalls(0),
          barrierHoldUs(0), ticksRun(0)
    {
        std::memset(pads, 0, sizeof pads);
        for (int i = 0; i < 4; i++)
            pads[i].connected = i < humans;
    }

    // One rendered frame at local time `now`, `dt` after the previous one.
    void Frame(uint64_t now, uint64_t dt)
    {
        s->Update();
        if (wander)
            WanderPads();
        s->SampleLocalPads(pads);
        const OnlineState& st = s->State();

        if (st.phase == ONLINE_PHASE_LOADING && st.matchSerial != loadedSerial)
        {
            if (loadStartUs == 0)
            {
                loadStartUs = now;
                loadUs = 200000 + (uint64_t)(rng() % 1300000);
            }
            const uint64_t spent = now - loadStartUs;
            if (spent >= loadUs)
            {
                s->MatchLoaded();
                loadedSerial = st.matchSerial;
                loadStartUs = 0;
            }
            else
                s->LoadProgress((int)(spent * 100 / loadUs));
        }
        if (playing && st.phase != ONLINE_PHASE_MATCH && st.phase != ONLINE_PHASE_POST_MATCH &&
            st.phase != ONLINE_PHASE_LOADING)
            playing = false;
        if (!playing && st.matchSerial == loadedSerial && loadedSerial != 0 &&
            startedSerial != loadedSerial && s->MayBeginMatch())
            BeginGame(now);

        if (playing)
        {
            const float rate = s->ClockRate();
            dueUs += (int64_t)((double)dt * rate);
            while (dueUs >= kTickUs && playing)
            {
                if (!MayRun(now, tick))
                {
                    dueUs = kTickUs;
                    break;
                }
                SlotInput in[ONLINE_MAX_SLOTS];
                if (!s->TickInput(tick, in))
                {
                    if (!stalling)
                    {
                        stalling = true;
                        stallStartUs = now;
                    }
                    dueUs = kTickUs;
                    break;
                }
                if (stalling)
                {
                    stalling = false;
                    const uint32_t ms = (uint32_t)((now - stallStartUs) / 1000);
                    stallCount++;
                    if (ms > maxStallMs)
                        maxStallMs = ms;
                    if (ms > 250)
                        longStalls++;
                }
                dueUs -= kTickUs;
                RunTick(in);
                tick++;
            }
            if (presenting && now >= presentEndUs)
            {
                presenting = false;
                s->BarrierDone(armedKind);
            }
        }
        s->Flush();
    }

    Side* s;
    std::mt19937 rng;
    int humans;
    uint32_t matchTicks;
    uint32_t goalOdds;
    int64_t flipAtTick;
    bool wander = true;   // false leaves `pads` to the test
    LocalPad pads[4];

    uint32_t loadedSerial;
    uint32_t startedSerial = 0;
    uint64_t loadStartUs;
    uint64_t loadUs;

    bool playing;
    uint32_t tick;
    int64_t dueUs;
    uint64_t state;
    int armedKind;
    uint32_t freezeTick;
    bool held;
    bool presenting;
    uint64_t presentEndUs;
    int goals;
    uint8_t cpuSlots = 0;

    bool stalling;
    uint64_t stallStartUs;
    uint32_t maxStallMs;
    int stallCount;
    int longStalls;
    uint64_t barrierHoldUs;
    uint64_t ticksRun;
    std::vector<MatchResult> results;
    std::vector<int> kickoffDelays;

  private:
    void WanderPads()
    {
        for (int i = 0; i < humans && i < 4; i++)
        {
            PadBytes& p = pads[i].pad;
            auto walk = [&](int8_t v) {
                int n = v + (int)(rng() % 21) - 10;
                if (rng() % 97 == 0)
                    n = (int)(rng() % 256) - 128;
                return (int8_t)(n < -128 ? -128 : n > 127 ? 127 : n);
            };
            p.stickX = walk(p.stickX);
            p.stickY = walk(p.stickY);
            p.substickX = walk(p.substickX);
            p.substickY = walk(p.substickY);
            p.triggerL = (uint8_t)(rng() % 7 == 0 ? rng() % 256 : p.triggerL);
            p.triggerR = (uint8_t)(rng() % 7 == 0 ? rng() % 256 : p.triggerR);
            // Mostly held for a few frames; Start comes now and then and travels like the rest.
            if (rng() % 5 == 0)
                p.buttons = (uint16_t)(rng() & 0x1F7F);
        }
    }

    void BeginGame(uint64_t now)
    {
        (void)now;
        const OnlineState& st = s->State();
        startedSerial = loadedSerial;
        playing = true;
        tick = 0;
        dueUs = kTickUs;
        state = mix64(st.desc.seed);
        armedKind = 1;
        freezeTick = 3;
        held = false;
        presenting = false;
        goals = 0;
        cpuSlots = 0;
        stalling = false;
        kickoffDelays.push_back(s->State().delayTicks);
    }

    bool MayRun(uint64_t now, uint32_t k)
    {
        if (armedKind != 0 && k >= freezeTick)
        {
            if (!held)
            {
                held = true;
                s->BarrierReached(armedKind, k);
                presenting = true;
                presentEndUs = now + 300000 + (uint64_t)(rng() % 1700000);
                holdStartUs = now;
            }
            if (!s->MayRunTick(k))
                return false;
            barrierHoldUs += now - holdStartUs;
            armedKind = 0;
            held = false;
            kickoffDelays.push_back(s->State().delayTicks);
            return true;
        }
        return s->MayRunTick(k) != 0;
    }

    void RunTick(const SlotInput in[ONLINE_MAX_SLOTS])
    {
        OnlineTickActions a;
        bool end = false;
        if (s->TickActions(tick, &a))
        {
            if (a.actions & ONLINE_ACTION_RESEED)
                state = mix64(state ^ a.seed);
            if (a.actions & ONLINE_ACTION_SLOTS_TO_CPU)
                cpuSlots = (uint8_t)(cpuSlots | a.cpuSlots);
            if (a.actions & ONLINE_ACTION_END_MATCH)
                end = true;
        }
        for (int i = 0; i < ONLINE_MAX_SLOTS; i++)
        {
            uint64_t v = (uint64_t)in[i].pad.buttons | (uint64_t)(uint8_t)in[i].pad.stickX << 16 |
                         (uint64_t)(uint8_t)in[i].pad.stickY << 24 |
                         (uint64_t)(uint8_t)in[i].pad.substickX << 32 |
                         (uint64_t)(uint8_t)in[i].pad.substickY << 40 |
                         (uint64_t)in[i].pad.triggerL << 48 | (uint64_t)in[i].pad.triggerR << 56;
            if (in[i].kind == ONLINE_SLOT_REMOTE || in[i].kind == ONLINE_SLOT_LOCAL)
                v ^= in[i].connected ? 0x5555u : 0xAAAAu;
            if (cpuSlots & (1 << i))
                v = 0xC0C0C0C0u;
            state = mix64(state ^ v ^ ((uint64_t)i << 60));
        }
        state = mix64(state ^ tick);
        if ((int64_t)tick == flipAtTick)
            state ^= 1ull << 17;
        s->TickHash(tick, state);
        if (tick % 50 == 0)
        {
            static const char* const kNames[4] = { "ball", "characters", "clocks", "seed" };
            const uint64_t parts[4] = { state & 0xFFFF, state >> 16 & 0xFFFF, (uint64_t)tick,
                                        state >> 48 };
            s->TickComponents(tick, parts, kNames, 4);
        }
        ticksRun++;
        if (armedKind == 0 && tick > 10 && tick + 60 < matchTicks && goalOdds != 0 &&
            state % goalOdds == 0)
        {
            armedKind = 2;
            freezeTick = tick + 51;
            held = false;
            goals++;
        }
        if (end || tick + 1 >= matchTicks)
        {
            s->MatchEnded();
            MatchResult r;
            r.serial = s->State().matchSerial;
            r.ticks = tick + 1;
            r.state = state;
            r.goals = goals;
            results.push_back(r);
            playing = false;
        }
    }

    uint64_t holdStartUs = 0;
};
}   // namespace online_test

#endif   // ONLINE_FAKE_GAME_H
