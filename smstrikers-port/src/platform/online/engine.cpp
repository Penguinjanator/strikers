#include "engine.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace online
{

void LinkStats::Reset()
{
    std::memset(m_rtt, 0, sizeof m_rtt);
    std::memset(m_seq, 0, sizeof m_seq);
    for (int i = 0; i < 60; i++)
        m_rtt[i].second = ~0ull;
    for (int i = 0; i < 10; i++)
        m_seq[i].second = ~0ull;
    m_srtt = 0;
    m_rttvar = 0;
    m_samples = 0;
}

void LinkStats::OnSeq(uint64_t seq, uint64_t nowUs)
{
    const uint64_t second = nowUs / 1000000u;
    SeqBucket& b = m_seq[second % 10];
    if (b.second != second)
    {
        b.second = second;
        b.count = 0;
        b.minSeq = seq;
        b.maxSeq = seq;
    }
    b.count++;
    if (seq < b.minSeq)
        b.minSeq = seq;
    if (seq > b.maxSeq)
        b.maxSeq = seq;
}

void LinkStats::OnRtt(uint32_t rttUs, uint64_t nowUs)
{
    const uint64_t second = nowUs / 1000000u;
    RttBucket& b = m_rtt[second % 60];
    if (b.second != second)
    {
        b.second = second;
        b.count = 0;
        b.sum = 0;
        b.sumSq = 0;
    }
    b.count++;
    b.sum += rttUs;
    b.sumSq += (double)rttUs * rttUs;

    if (m_samples == 0)
    {
        m_srtt = rttUs;
        m_rttvar = rttUs / 2;
    }
    else
    {
        const uint32_t diff = m_srtt > rttUs ? m_srtt - rttUs : rttUs - m_srtt;
        m_rttvar = (3 * m_rttvar + diff) / 4;
        m_srtt = (7 * m_srtt + rttUs) / 8;
    }
    m_samples++;
}

void LinkStats::Pool(uint64_t nowUs, int seconds, uint32_t* count, double* mean, double* sd) const
{
    const uint64_t now = nowUs / 1000000u;
    uint32_t n = 0;
    double sum = 0;
    double sumSq = 0;
    for (int i = 0; i < 60; i++)
    {
        const RttBucket& b = m_rtt[i];
        if (b.second == ~0ull || b.second > now || now - b.second >= (uint64_t)seconds)
            continue;
        n += b.count;
        sum += b.sum;
        sumSq += b.sumSq;
    }
    *count = n;
    if (n == 0)
    {
        *mean = 0;
        *sd = 0;
        return;
    }
    *mean = sum / n;
    const double var = sumSq / n - *mean * *mean;
    *sd = var > 0 ? std::sqrt(var) : 0;
}

uint32_t LinkStats::JitterUs(uint64_t nowUs) const
{
    uint32_t n;
    double mean;
    double sd;
    Pool(nowUs, 10, &n, &mean, &sd);
    return (uint32_t)(sd / std::sqrt(2.0));
}

int LinkStats::LossPercent(uint64_t nowUs) const
{
    const uint64_t now = nowUs / 1000000u;
    uint64_t lo = ~0ull;
    uint64_t hi = 0;
    uint64_t got = 0;
    for (int i = 0; i < 10; i++)
    {
        const SeqBucket& b = m_seq[i];
        if (b.second == ~0ull || b.second > now || now - b.second >= 10)
            continue;
        got += b.count;
        if (b.minSeq < lo)
            lo = b.minSeq;
        if (b.maxSeq > hi)
            hi = b.maxSeq;
    }
    if (got < 10 || hi < lo)
        return 0;
    const uint64_t expected = hi - lo + 1;
    if (got >= expected)
        return 0;
    return (int)((expected - got) * 100 / expected);
}

bool LinkStats::Window(uint64_t nowUs, int seconds, double* rttUs, double* oneWaySdUs) const
{
    uint32_t n;
    double mean;
    double sd;
    Pool(nowUs, seconds, &n, &mean, &sd);
    if (n == 0)
    {
        if (m_samples == 0)
            return false;
        *rttUs = m_srtt;
        *oneWaySdUs = m_rttvar / std::sqrt(2.0);
        return true;
    }
    *rttUs = mean;
    *oneWaySdUs = sd / std::sqrt(2.0);
    return true;
}

void Lockstep::Clear()
{
    m_active = false;
    m_serial = 0;
    m_self = -1;
    m_machines = 0;
    for (int s = 0; s < ONLINE_MAX_SLOTS; s++)
        m_slotMachine[s] = -1;
    m_delay = ONLINE_MIN_DELAY_TICKS;
    m_mispredicted = kNoTick;
    m_mispredictions = 0;
    std::memset(m_latest, 0, sizeof m_latest);
    std::memset(m_taps, 0, sizeof m_taps);
    std::memset(m_connected, 0, sizeof m_connected);
    for (int m = 0; m < ONLINE_MAX_MACHINES; m++)
    {
        m_log[m].clear();
        m_have[m] = 0;
        for (int i = 0; i < kPredictRing; i++)
            m_predicted[m][i].tick = kNoTick;
        m_dropTick[m] = kNoTick;
        m_newestHash[m] = kNoTick;
        m_componentsHead[m] = 0;
        for (int i = 0; i < kHashRing; i++)
        {
            m_hash[m][i].tick = kNoTick;
            m_hash[m][i].hash = 0;
        }
        for (int i = 0; i < kComponentRing; i++)
        {
            m_components[m][i].tick = kNoTick;
            m_components[m][i].count = 0;
            m_components[m][i].names = NULL;
        }
    }
    m_conflicts = 0;
    m_lastAgreed = kNoTick;
}

void Lockstep::Begin(uint32_t serial, int self, uint8_t machineMask,
                     const int8_t slotMachine[ONLINE_MAX_SLOTS])
{
    Clear();
    m_active = true;
    m_serial = serial;
    m_self = self;
    m_machines = machineMask;
    for (int s = 0; s < ONLINE_MAX_SLOTS; s++)
        m_slotMachine[s] = slotMachine[s];
    // Five minutes of ticks; longer matches grow the log.
    for (int m = 0; m < ONLINE_MAX_MACHINES; m++)
        if (InMatch(m))
            m_log[m].reserve(15000 * kRecordSize);
}

int Lockstep::HumansOf(int machine) const
{
    int n = 0;
    for (int s = 0; s < ONLINE_MAX_SLOTS; s++)
        if (m_slotMachine[s] == machine)
            n++;
    return n;
}

int Lockstep::SlotOf(int machine, int human) const
{
    for (int s = 0; s < ONLINE_MAX_SLOTS; s++)
        if (m_slotMachine[s] == machine && human-- == 0)
            return s;
    return -1;
}

void Lockstep::Sample(int human, const PadBytes& pad, bool connected, bool taps)
{
    if (!m_active || human < 0 || human >= HumansOf(m_self))
        return;
    m_latest[human] = pad;
    m_taps[human] = taps ? (uint16_t)(m_taps[human] | (pad.buttons & PORT_INPUT_BUTTONS)) : 0;
    m_connected[human] = connected;
}

void Lockstep::StartGeneration()
{
    // The first ticks are neutral on every machine: the intro poses players between them.
    for (int m = 0; m < ONLINE_MAX_MACHINES; m++)
    {
        if (!InMatch(m))
            continue;
        const int humans = HumansOf(m);
        std::vector<uint8_t>& log = m_log[m];
        while (m_have[m] < kNeutralTicks)
        {
            uint8_t rec[kRecordSize] = {};
            for (int h = 0; h < humans; h++)
                rec[16] = (uint8_t)(rec[16] | (1 << h));
            log.insert(log.end(), rec, rec + kRecordSize);
            m_have[m]++;
        }
    }
    const int humans = HumansOf(m_self);
    std::vector<uint8_t>& log = m_log[m_self];
    // With input delay, the ticks before the first one a sample can reach are neutral too.
    const uint32_t neutral = (uint32_t)m_delay > kNeutralTicks ? (uint32_t)m_delay : kNeutralTicks;
    while (m_have[m_self] < neutral)
    {
        uint8_t rec[kRecordSize] = {};
        for (int h = 0; h < humans; h++)
            if (m_connected[h])
                rec[16] = (uint8_t)(rec[16] | (1 << h));
        log.insert(log.end(), rec, rec + kRecordSize);
        m_have[m_self]++;
    }
}

uint32_t Lockstep::Generate(uint32_t through)
{
    if (!m_active || through == kNoTick)
        return kNoTick;
    const uint32_t first = m_have[m_self];
    const int humans = HumansOf(m_self);
    std::vector<uint8_t>& log = m_log[m_self];
    while (m_have[m_self] <= through)
    {
        uint8_t rec[kRecordSize] = {};
        for (int h = 0; h < humans; h++)
        {
            if (!m_connected[h])
            {
                m_taps[h] = 0;
                continue;
            }
            PadBytes pad = m_latest[h];
            pad.buttons = (uint16_t)((pad.buttons | m_taps[h]) & PORT_INPUT_BUTTONS);
            RecordPut(rec, h, pad);
            rec[16] = (uint8_t)(rec[16] | (1 << h));
            m_taps[h] = 0;
        }
        log.insert(log.end(), rec, rec + kRecordSize);
        m_have[m_self]++;
    }
    return m_have[m_self] > first ? first : kNoTick;
}

int Lockstep::Add(int machine, uint32_t firstTick, int count, const uint8_t (*records)[kRecordSize])
{
    if (!InMatch(machine) || machine == m_self)
        return -1;
    const int humans = HumansOf(machine);
    for (int i = 0; i < count; i++)
        if (!RecordValid(records[i], humans))
            return -1;

    int added = 0;
    std::vector<uint8_t>& log = m_log[machine];
    for (int i = 0; i < count; i++)
    {
        const uint32_t t = firstTick + (uint32_t)i;
        if (t < firstTick || t >= m_dropTick[machine])
            break;
        if (t < m_have[machine])
        {
            // A duplicate record keeps the first copy.
            if (std::memcmp(&log[(size_t)t * kRecordSize], records[i], kRecordSize) != 0)
                m_conflicts++;
            continue;
        }
        if (t > m_have[machine])
            break;
        // A tick that ran on a guess at this record runs again if the guess was wrong.
        const Prediction& p = m_predicted[machine][t % kPredictRing];
        if (p.tick == t && std::memcmp(p.rec, records[i], kRecordSize) != 0)
        {
            m_mispredictions++;
            if (m_mispredicted == kNoTick || t < m_mispredicted)
                m_mispredicted = t;
            if (t < 12 && std::getenv("STRIKERS_ROLLBACK_LOG") != NULL)
                std::fprintf(stderr, "[rollback] mispredicted m%d t=%u\n", machine, t);
        }
        log.insert(log.end(), records[i], records[i] + kRecordSize);
        m_have[machine]++;
        added++;
    }
    return added;
}

const uint8_t* Lockstep::Record(int machine, uint32_t tick) const
{
    if (machine < 0 || machine >= ONLINE_MAX_MACHINES || tick >= m_have[machine])
        return NULL;
    return &m_log[machine][(size_t)tick * kRecordSize];
}

bool Lockstep::Ready(uint32_t tick) const { return m_active && Missing(tick) < 0; }

int Lockstep::Missing(uint32_t tick) const
{
    for (int m = 0; m < ONLINE_MAX_MACHINES; m++)
    {
        if (!InMatch(m) || tick >= m_dropTick[m])
            continue;
        if (m_have[m] <= tick)
            return m;
    }
    return -1;
}

uint32_t Lockstep::Confirmed() const
{
    uint32_t c = kNoTick;
    for (int m = 0; m < ONLINE_MAX_MACHINES; m++)
    {
        if (!InMatch(m))
            continue;
        if (m_dropTick[m] != kNoTick && m_have[m] >= m_dropTick[m])
            continue;
        if (m_have[m] < c)
            c = m_have[m];
    }
    return c;
}

void Lockstep::Read(uint32_t tick, SlotInput out[ONLINE_MAX_SLOTS]) const
{
    int nth[ONLINE_MAX_MACHINES] = {};
    for (int s = 0; s < ONLINE_MAX_SLOTS; s++)
    {
        SlotInput& in = out[s];
        std::memset(&in, 0, sizeof in);
        const int m = m_slotMachine[s];
        if (m < 0 || !InMatch(m))
        {
            in.kind = ONLINE_SLOT_EMPTY;
            continue;
        }
        const int h = nth[m]++;
        if (tick >= m_dropTick[m])
        {
            in.kind = ONLINE_SLOT_CPU;
            continue;
        }
        in.kind = m == m_self ? ONLINE_SLOT_LOCAL : ONLINE_SLOT_REMOTE;
        const uint8_t* rec = Record(m, tick);
        if (rec == NULL)
            continue;
        in.pad = RecordGet(rec, h);
        in.connected = (rec[16] >> h) & 1;
    }
}

void Lockstep::ReadPredicted(uint32_t tick, SlotInput out[ONLINE_MAX_SLOTS])
{
    Read(tick, out);
    int nth[ONLINE_MAX_MACHINES] = {};
    for (int s = 0; s < ONLINE_MAX_SLOTS; s++)
    {
        const int m = m_slotMachine[s];
        if (m < 0 || !InMatch(m))
            continue;
        const int h = nth[m]++;
        if (m == m_self || tick >= m_dropTick[m] || tick < m_have[m])
            continue;
        // The newest record this machine has sent stands in for the ones still on their way.
        static const uint8_t kNone[kRecordSize] = {};
        const uint8_t* last =
            m_have[m] != 0 ? &m_log[m][(size_t)(m_have[m] - 1) * kRecordSize] : kNone;
        Prediction& p = m_predicted[m][tick % kPredictRing];
        p.tick = tick;
        std::memcpy(p.rec, last, kRecordSize);
        out[s].pad = RecordGet(last, h);
        out[s].connected = (last[16] >> h) & 1;
    }
}

uint32_t Lockstep::TakeMispredicted()
{
    const uint32_t t = m_mispredicted;
    m_mispredicted = kNoTick;
    return t;
}

void Lockstep::Drop(int machine, uint32_t tick)
{
    if (!InMatch(machine) || machine == m_self || m_dropTick[machine] != kNoTick)
        return;
    m_dropTick[machine] = tick;
    // Ticks from here that ran on this machine's input, guessed or not, run again with the CPU.
    if (m_mispredicted == kNoTick || tick < m_mispredicted)
        m_mispredicted = tick;
}

bool Lockstep::SetHash(int machine, uint32_t tick, uint64_t hash, uint32_t* mismatchTick)
{
    if (!InMatch(machine) || tick == kNoTick)
        return false;
    HashEntry& e = m_hash[machine][tick % kHashRing];
    e.tick = tick;
    e.hash = hash;
    if (m_newestHash[machine] == kNoTick || tick > m_newestHash[machine])
        m_newestHash[machine] = tick;

    bool bad = false;
    for (int m = 0; m < ONLINE_MAX_MACHINES; m++)
    {
        if (m == machine || !InMatch(m) || (machine != m_self && m != m_self))
            continue;
        const HashEntry& o = m_hash[m][tick % kHashRing];
        if (o.tick != tick)
            continue;
        if (o.hash != hash)
            bad = true;
        else if (m_lastAgreed == kNoTick || tick > m_lastAgreed)
            m_lastAgreed = tick;
    }
    if (bad && mismatchTick != NULL)
        *mismatchTick = tick;
    return bad;
}

bool Lockstep::HashOf(int machine, uint32_t tick, uint64_t* out) const
{
    if (machine < 0 || machine >= ONLINE_MAX_MACHINES)
        return false;
    const HashEntry& e = m_hash[machine][tick % kHashRing];
    if (e.tick != tick)
        return false;
    *out = e.hash;
    return true;
}

void Lockstep::SetComponents(int machine, uint32_t tick, const uint64_t* hashes, int count,
                             const char* const* names)
{
    if (!InMatch(machine) || count < 1)
        return;
    if (count > kMaxComponents)
        count = kMaxComponents;
    int& head = m_componentsHead[machine];
    head = (head + 1) % kComponentRing;
    Components& c = m_components[machine][head];
    c.tick = tick;
    c.count = count;
    std::memcpy(c.hash, hashes, sizeof(uint64_t) * (size_t)count);
    c.names = names;
}

const Lockstep::Components* Lockstep::ComponentsAt(int machine, int index) const
{
    if (machine < 0 || machine >= ONLINE_MAX_MACHINES || index < 0 || index >= kComponentRing)
        return NULL;
    const Components& c =
        m_components[machine]
                    [(m_componentsHead[machine] + kComponentRing - index) % kComponentRing];
    return c.tick == kNoTick ? NULL : &c;
}
}   // namespace online
