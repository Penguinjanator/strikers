// Online test hooks: instances on one machine over a simulated link, with wandering pads.
//   STRIKERS_ONLINE_AUTOTEST=host:<port> | join:<host>:<port>   at the main menu, set up and start
//   STRIKERS_ONLINE_NETSIM=<one-way ms>,<jitter ms>,<loss %>    every datagram this machine sends
//   STRIKERS_ONLINE_WANDER=<seed>                               this machine's pads walk at random
//   STRIKERS_ONLINE_WANDER_START=1                              ...and press Start now and then
//   STRIKERS_ONLINE_MACHINES=<n>                                the host waits for n machines
//   STRIKERS_ONLINE_LEAVE_AT=<tick>                             leave the match at that tick
//   STRIKERS_ONLINE_REMATCHES=<n>                               after a match, vote rematch n times

#include "pipe.h"

#include "port/lockstep.h"
#include "Game/FE/feManager.h"
#include "port/online.h"
#include "dolphin/pad.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

extern "C" unsigned long long port_monotonic_ns(void);

namespace
{
struct NetSim
{
    bool on = false;
    double oneWayMs = 0, jitterMs = 0, loss = 0;
    std::mt19937 rng{ 12345 };
};

NetSim& netsim()
{
    static NetSim* n = [] {
        NetSim* s = new NetSim();
        const char* v = std::getenv("STRIKERS_ONLINE_NETSIM");
        if (v != NULL && std::sscanf(v, "%lf,%lf,%lf", &s->oneWayMs, &s->jitterMs, &s->loss) >= 1)
            s->on = true;
        return s;
    }();
    return *n;
}

uint64_t now_us(void) { return port_monotonic_ns() / 1000u; }

class LaggyPipe : public online::NetPipe
{
  public:
    explicit LaggyPipe(online::NetPipe* inner) : m_inner(inner) {}
    ~LaggyPipe() override { delete m_inner; }

    int Send(const online::NetAddr& to, const uint8_t* data, size_t size) override
    {
        Flush();
        NetSim& n = netsim();
        std::uniform_real_distribution<double> u(0.0, 1.0);
        if (u(n.rng) * 100.0 < n.loss)
            return 0;
        std::normal_distribution<double> jitter(0.0, n.jitterMs > 0 ? n.jitterMs : 1e-9);
        double ms = n.oneWayMs + jitter(n.rng);
        if (ms < 0)
            ms = 0;
        Held h;
        h.due = now_us() + (uint64_t)(ms * 1000.0);
        h.to = to;
        h.bytes.assign(data, data + size);
        m_held.push_back(h);
        return 0;
    }

    int Receive(online::NetAddr* from, uint8_t* data, size_t cap) override
    {
        Flush();
        return m_inner->Receive(from, data, cap);
    }

    online::NetPipe* SocketPipe() override { return m_inner->SocketPipe(); }

  private:
    struct Held
    {
        uint64_t due;
        online::NetAddr to;
        std::vector<uint8_t> bytes;
    };

    void Flush()
    {
        const uint64_t now = now_us();
        for (size_t i = 0; i < m_held.size();)
        {
            if (m_held[i].due <= now)
            {
                m_inner->Send(m_held[i].to, m_held[i].bytes.data(), m_held[i].bytes.size());
                m_held.erase(m_held.begin() + (long)i);
            }
            else
                i++;
        }
    }

    online::NetPipe* m_inner;
    std::vector<Held> m_held;
};

struct Wander
{
    bool on = false;
    std::mt19937 rng;
    PADStatus pad = {};
};

Wander& wander()
{
    static Wander* w = [] {
        Wander* s = new Wander();
        const char* v = std::getenv("STRIKERS_ONLINE_WANDER");
        if (v != NULL && *v != '\0')
        {
            s->on = true;
            s->rng.seed((unsigned)std::strtoul(v, NULL, 10));
        }
        return s;
    }();
    return *w;
}

int8_t walk(std::mt19937& rng, int8_t v)
{
    int n = v + (int)(rng() % 31) - 15;
    if (rng() % 61 == 0)
        n = (int)(rng() % 200) - 100;
    return (int8_t)(n < -100 ? -100 : n > 100 ? 100 : n);
}

enum
{
    kIdle,
    kHosted,
    kJoined,
    kDone
};
int s_stage = kIdle;
}   // namespace

// Wraps a game socket in the simulated link when one is asked for.
online::NetPipe* PortOnlineAutotestPipe(online::NetPipe* pipe)
{
    if (pipe == NULL || !netsim().on)
        return pipe;
    return new LaggyPipe(pipe);
}

// Replaces port 0's sample with a random walk: sticks drift, buttons change every few frames.
extern "C" void PortOnlineAutotestPads(PADStatus* ports)
{
    Wander& w = wander();
    if (!w.on || ports == NULL)
        return;
    PADStatus& p = w.pad;
    p.err = 0;
    p.stickX = walk(w.rng, p.stickX);
    p.stickY = walk(w.rng, p.stickY);
    if (w.rng() % 9 == 0)
        p.substickX = walk(w.rng, p.substickX);
    if (w.rng() % 9 == 0)
        p.substickY = walk(w.rng, p.substickY);
    if (w.rng() % 6 == 0)
        p.button = (uint16_t)(w.rng() & (PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_X | PAD_BUTTON_Y |
                                         PAD_TRIGGER_R | PAD_TRIGGER_L));
    static const bool start = std::getenv("STRIKERS_ONLINE_WANDER_START") != NULL;
    if (start && w.rng() % 400 == 0)
        p.button |= PAD_BUTTON_START;
    p.triggerLeft = (p.button & PAD_TRIGGER_L) ? 200 : 0;
    p.triggerRight = (p.button & PAD_TRIGGER_R) ? 200 : 0;
    ports[0] = p;
}

// At the main menu: host or join, and on the host start once the guest is ready.
extern "C" void PortOnlineAutotestFrame(int atMainMenu)
{
    const char* v = std::getenv("STRIKERS_ONLINE_AUTOTEST");
    if (v == NULL)
        return;
    const OnlineState* st = PortOnlineGetState();
    static int s_rematches = -1;
    static uint32_t s_votedFor;
    if (s_rematches < 0)
    {
        const char* r = std::getenv("STRIKERS_ONLINE_REMATCHES");
        s_rematches = r != NULL ? std::atoi(r) : 0;
    }
    if (st->phase == ONLINE_PHASE_POST_MATCH && s_rematches > 0 && s_votedFor != st->matchSerial)
    {
        s_votedFor = st->matchSerial;
        s_rematches--;
        PortOnlineVote(-1, ONLINE_VOTE_REMATCH);
        std::fprintf(stderr, "[autotest] voted rematch after match %u\n", st->matchSerial);
    }
    static const long leaveAt = std::getenv("STRIKERS_ONLINE_LEAVE_AT") != NULL
                                    ? std::atol(std::getenv("STRIKERS_ONLINE_LEAVE_AT"))
                                    : -1;
    static bool s_left;
    if (leaveAt >= 0 && !s_left && st->phase == ONLINE_PHASE_MATCH && (long)st->tick >= leaveAt)
    {
        s_left = true;
        std::fprintf(stderr, "[autotest] leaving the match at tick %u\n", st->tick);
        PortOnlineLeave();
        return;
    }
    // A host's new settings clear every guest's ready flag; a joined guest readies again.
    if (std::strncmp(v, "join:", 5) == 0 && st->phase == ONLINE_PHASE_ROOM && st->self > 0 &&
        st->self < ONLINE_MAX_MACHINES && !st->machines[st->self].ready)
        PortOnlineSetReady(1);
    // The pause menu reads the real pads, not the wander; after a few seconds, Continue.
    static int s_pausedFrames;
    const int pauser = PortSimPauseSlot();
    if (PortSimBarrier() == PORT_BARRIER_PAUSE && pauser >= 0 &&
        PortOnlineSlotLocalPort(pauser) >= 0)
    {
        if (++s_pausedFrames % 360 == 0)
        {
            std::fprintf(stderr,
                         "[autotest] continuing from the pause slot %d opened (menu open %d)\n",
                         pauser, (int)FrontEnd::m_bInPauseMenuState);
            if (FrontEnd::m_bInPauseMenuState)
                FrontEnd::ExitMenuState();
            else
                PortSimBarrierDone(PORT_BARRIER_PAUSE);
        }
    }
    else
        s_pausedFrames = 0;
    if (s_stage == kDone)
        return;
    switch (s_stage)
    {
    case kIdle:
        if (!atMainMenu)
            return;
        if (std::strncmp(v, "host:", 5) == 0)
        {
            if (PortOnlineHost((uint16_t)std::atoi(v + 5), "Host") != 0)
            {
                std::fprintf(stderr, "[autotest] hosting on %s\n", v + 5);
                s_stage = kHosted;
            }
        }
        else if (std::strncmp(v, "join:", 5) == 0 && PortOnlineJoin(v + 5, "Guest") != 0)
        {
            std::fprintf(stderr, "[autotest] joining %s\n", v + 5);
            s_stage = kJoined;
        }
        return;
    case kJoined:
        if (st->phase == ONLINE_PHASE_ROOM)
        {
            PortOnlineSetReady(1);
            std::fprintf(stderr, "[autotest] guest ready\n");
            s_stage = kDone;
        }
        return;
    case kHosted:
        // Each machine's first human takes its default slot; the CPU plays the rest.
        static const int machines = std::getenv("STRIKERS_ONLINE_MACHINES") != NULL
                                        ? std::atoi(std::getenv("STRIKERS_ONLINE_MACHINES"))
                                        : 2;
        static int s_lastCount = -1, s_lastCan = -1;
        if (st->machineCount != s_lastCount || st->canStart != s_lastCan)
        {
            s_lastCount = st->machineCount;
            s_lastCan = st->canStart;
            std::fprintf(stderr, "[autotest] room: %d machines of %d, can start %d\n",
                         st->machineCount, machines, (int)st->canStart);
        }
        if (st->phase == ONLINE_PHASE_ROOM && st->machineCount >= machines)
        {
            const uint8_t pause = std::getenv("STRIKERS_ONLINE_WANDER_START") != NULL ? 1 : 0;
            if (st->desc.gameTime != 120 || st->desc.pauseAllowed != pause)
            {
                OnlineMatchDesc d = st->desc;
                d.gameTime = 120;
                d.pauseAllowed = pause;
                PortOnlineSetDesc(&d);
            }
            if (st->canStart && PortOnlineStart() != 0)
            {
                std::fprintf(stderr, "[autotest] host started\n");
                s_stage = kDone;
            }
        }
        return;
    default: return;
    }
}
