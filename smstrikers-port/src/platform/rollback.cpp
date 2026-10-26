// The rollback ring: the simulation heap and the registered statics, saved at the top of a tick.

#include "port/rollback.h"

#include "NL/nlSlotPool.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
const char* PortSymbolName(const void* p);
size_t PortSimHeapSnapshotSize(void);
void PortSimHeapSave(void* out);
void PortSimHeapRestore(const void* in);
}

namespace
{
struct Region
{
    void* p;
    size_t size;
    std::vector<unsigned char> initial;   // its bytes when registered
};

// Constructed on first use: registrations come from static initialisers in any order.
std::vector<Region>& regions()
{
    static std::vector<Region>* r = new std::vector<Region>();
    return *r;
}

size_t s_staticBytes;

struct Hook
{
    PortSimStateSave save;
    PortSimStateLoad load;
};

std::vector<Hook>& hooks()
{
    static std::vector<Hook>* h = new std::vector<Hook>();
    return *h;
}

std::vector<unsigned char>* s_appendTo;

void append(const void* p, size_t size)
{
    const unsigned char* b = (const unsigned char*)p;
    s_appendTo->insert(s_appendTo->end(), b, b + size);
}

struct Slot
{
    unsigned int tick;
    bool valid;
    std::vector<unsigned char> heap;
    std::vector<unsigned char> statics;
    std::vector<unsigned char> hooked;
    size_t regionCount;
};

Slot s_ring[PORT_ROLLBACK_TICKS];
Slot s_scratch[4];   // diagnostics' saves, ticks PORT_ROLLBACK_SCRATCH and up, apart from the ring

Slot& slot(unsigned int tick)
{
    return tick >= PORT_ROLLBACK_SCRATCH ? s_scratch[(tick - PORT_ROLLBACK_SCRATCH) & 3]
                                         : s_ring[tick % PORT_ROLLBACK_TICKS];
}
int s_resimulating;
unsigned int s_confirmed = 0xFFFFFFFFu;
}   // namespace

extern "C" void PortSimState(void* p, size_t size)
{
    for (const Region& r : regions())
        if (r.p == p)
            return;
    const unsigned char* b = (const unsigned char*)p;
    regions().push_back(Region{ p, size, std::vector<unsigned char>(b, b + size) });
    s_staticBytes += size;
}

extern "C" void PortSimSlotPool(SlotPoolBase* pool)
{
    if (pool->m_pPresentation == NULL)
    {
        SlotPoolBase* twin = new SlotPoolBase();
        twin->m_Initial = pool->m_Delta != 0 ? pool->m_Delta : pool->m_Initial;
        twin->m_Delta = pool->m_Delta;
        twin->m_AllocFn = pool->m_AllocFn;
        twin->m_FreeFn = pool->m_FreeFn;
        pool->m_pPresentation = twin;
    }
    PortSimState(pool, sizeof *pool);
}

extern "C" void PortSimStateForget(void* p)
{
    std::vector<Region>& r = regions();
    for (size_t i = 0; i < r.size(); i++)
        if (r[i].p == p)
        {
            s_staticBytes -= r[i].size;
            r.erase(r.begin() + (long)i);
            PortRollbackReset();
            return;
        }
}

extern "C" void PortSimStateHook(PortSimStateSave save, PortSimStateLoad load)
{
    for (const Hook& h : hooks())
        if (h.save == save)
            return;
    hooks().push_back(Hook{ save, load });
}

// The tick the last load restored, until the next save: that save must match what was loaded.
static unsigned int s_loadedTick = 0xFFFFFFFFu;
static const void* s_inputRegion;   // the lockstep input array, rewritten before each save
extern "C" void PortRollbackInputRegion(const void* p) { s_inputRegion = p; }

// Diagnostic (STRIKERS_ROLLBACK_CHECK): a slot saved again for its own tick must match the old one.
static void resave_compare(const Slot& s, const std::vector<unsigned char>& heap,
                           const std::vector<unsigned char>& statics,
                           const std::vector<unsigned char>& hooked)
{
    static int reports;
    if (reports >= 8)
        return;
    const char* part = NULL;
    size_t at = 0;
    if (heap != s.heap)
    {
        part = "heap";
        while (at < heap.size() && at < s.heap.size() && heap[at] == s.heap[at])
            at++;
    }
    else if (statics != s.statics)
    {
        // The tick's own input is written before its save, so a replay saves the corrected input.
        size_t base = 0;
        for (size_t r = 0; r < regions().size() && base < statics.size();
             base += regions()[r].size, r++)
        {
            if (regions()[r].p == s_inputRegion)
                continue;
            const size_t end = base + regions()[r].size;
            for (size_t i = base; i < end && i < statics.size() && i < s.statics.size(); i++)
                if (statics[i] != s.statics[i])
                {
                    part = "static";
                    at = i;
                    break;
                }
            if (part != NULL)
                break;
        }
    }
    // A difference only in the input region leaves part unset, so the hooks are still compared.
    if (part == NULL && hooked != s.hooked)
    {
        part = "hook";
        while (at < hooked.size() && at < s.hooked.size() && hooked[at] == s.hooked[at])
            at++;
        // Which hook: re-run each save into a scratch buffer to find the spans.
        std::vector<unsigned char> probe;
        std::vector<unsigned char>* was = s_appendTo;
        size_t start = 0;
        for (size_t h = 0; h < hooks().size(); h++)
        {
            probe.clear();
            s_appendTo = &probe;
            hooks()[h].save(append);
            if (at < start + probe.size())
            {
                // A strict prefix ends one buffer at `at`, so its byte there reads as --.
                char now[3] = "--", old[3] = "--";
                if (at < hooked.size())
                    std::snprintf(now, sizeof now, "%02x", hooked[at]);
                if (at < s.hooked.size())
                    std::snprintf(old, sizeof old, "%02x", s.hooked[at]);
                std::fprintf(stderr,
                             "[rollback]   hook #%zu (%s), %zu bytes, offset %zu: %s -> %s\n", h,
                             PortSymbolName((const void*)hooks()[h].save), probe.size(), at - start,
                             now, old);
                break;
            }
            start += probe.size();
        }
        s_appendTo = was;
    }
    if (part == NULL)
        return;
    reports++;
    size_t region = 0, off = at;
    if (part[0] == 's')
        for (; region < regions().size() && off >= regions()[region].size; region++)
            off -= regions()[region].size;
    std::fprintf(stderr, "[rollback] resave t=%u differs in %s at byte %zu (sizes %zu/%zu)%s%s\n",
                 s.tick, part, at,
                 part[0] == 'h' && part[1] == 'e' ? heap.size()
                 : part[0] == 's'                 ? statics.size()
                                                  : hooked.size(),
                 part[0] == 'h' && part[1] == 'e' ? s.heap.size()
                 : part[0] == 's'                 ? s.statics.size()
                                                  : s.hooked.size(),
                 part[0] == 's' ? " region " : "",
                 part[0] == 's' ? PortSymbolName(regions()[region].p) : "");
    if (part[0] == 's')
        std::fprintf(
            stderr, "[rollback]   region #%zu at %p, %zu bytes, offset %zu: %02x -> %02x\n", region,
            regions()[region].p, regions()[region].size, off, statics[at], s.statics[at]);
}

extern "C" void PortRollbackSave(unsigned int tick)
{
    Slot& s = slot(tick);
    static const bool check = std::getenv("STRIKERS_ROLLBACK_CHECK") != NULL;
    std::vector<unsigned char> oldHeap, oldStatics, oldHooked;
    const bool compare = check && s.valid && s.tick == tick && tick == s_loadedTick;
    s_loadedTick = 0xFFFFFFFFu;
    if (compare)
    {
        oldHeap = s.heap;
        oldStatics = s.statics;
        oldHooked = s.hooked;
    }
    s.tick = tick;
    s.valid = true;
    s.heap.resize(PortSimHeapSnapshotSize());
    PortSimHeapSave(s.heap.data());
    s.statics.resize(s_staticBytes);
    s.regionCount = regions().size();
    unsigned char* out = s.statics.data();
    for (const Region& r : regions())
    {
        std::memcpy(out, r.p, r.size);
        out += r.size;
    }
    s.hooked.clear();
    s_appendTo = &s.hooked;
    for (const Hook& h : hooks())
        h.save(append);
    s_appendTo = nullptr;
    if (compare)
        resave_compare(s, oldHeap, oldStatics, oldHooked);
    if (tick < 4 && std::getenv("STRIKERS_ROLLBACK_LOG") != NULL)
    {
        // Per region, so two machines' logs can be lined up to find the one that differs.
        uint64_t heap = 1469598103934665603ull;
        for (unsigned char b : s.heap)
            heap = (heap ^ b) * 1099511628211ull;
        std::fprintf(stderr, "[rollback] save t=%u heap %zu %016llx\n", tick, s.heap.size(),
                     (unsigned long long)heap);
        const unsigned char* in = s.statics.data();
        for (size_t i = 0; i < regions().size(); i++)
        {
            uint64_t h = 1469598103934665603ull;
            for (size_t j = 0; j < regions()[i].size; j++)
                h = (h ^ in[j]) * 1099511628211ull;
            in += regions()[i].size;
            std::fprintf(stderr, "[rollback] save t=%u static %zu %zu %016llx\n", tick, i,
                         regions()[i].size, (unsigned long long)h);
        }
    }
}

extern "C" int PortRollbackHas(unsigned int tick)
{
    const Slot& s = slot(tick);
    return s.valid && s.tick == tick;
}

extern "C" void PortRollbackLoad(unsigned int tick)
{
    s_loadedTick = tick;
    const Slot& s = slot(tick);
    PortSimHeapRestore(s.heap.data());
    const unsigned char* in = s.statics.data();
    for (size_t i = 0; i < s.regionCount; i++)
    {
        const Region& r = regions()[i];
        std::memcpy(r.p, in, r.size);
        in += r.size;
    }
    // A function's static first used since the save goes back to how it was made.
    for (size_t i = s.regionCount; i < regions().size(); i++)
        std::memcpy(regions()[i].p, regions()[i].initial.data(), regions()[i].size);
    const unsigned char* hin = s.hooked.data();
    for (const Hook& h : hooks())
        hin += h.load(hin);
}

extern "C" void PortRollbackReset(void)
{
    for (Slot& s : s_ring)
        s.valid = false;
}

extern "C" unsigned int PortRollbackConfirmed(void) { return s_confirmed; }
extern "C" void PortRollbackSetConfirmed(unsigned int tick) { s_confirmed = tick; }

extern "C" int PortRollbackResimulating(void) { return s_resimulating; }
extern "C" void PortRollbackSetResimulating(int on) { s_resimulating = on; }

// Diagnostic: report where two saved slots differ, as heap offsets and registered regions.
extern "C" void PortRollbackDiff(unsigned int a, unsigned int b,
                                 void (*report)(const char* what, size_t offset))
{
    const Slot& x = slot(a);
    const Slot& y = slot(b);
    const size_t hn = x.heap.size() < y.heap.size() ? x.heap.size() : y.heap.size();
    for (size_t o = 0; o < hn; o += 8)
        if (std::memcmp(&x.heap[o], &y.heap[o], o + 8 <= hn ? 8 : hn - o) != 0)
            report("heap", o);
    size_t at = 0;
    for (size_t i = 0; i < regions().size() && at < x.statics.size(); i++)
    {
        const Region& r = regions()[i];
        if (std::memcmp(&x.statics[at], &y.statics[at], r.size) != 0)
            report("static", i);
        at += r.size;
    }
}

extern "C" const void* PortRollbackRegion(size_t index, size_t* size)
{
    if (index >= regions().size())
        return nullptr;
    *size = regions()[index].size;
    return regions()[index].p;
}

#if defined(__APPLE__) || defined(__linux__)
#include <dlfcn.h>

// Diagnostic: the symbol an address falls in, for the diagnostics' reports.
extern "C" const char* PortSymbolName(const void* p)
{
    Dl_info info;
    return dladdr(p, &info) && info.dli_sname != NULL ? info.dli_sname : "?";
}
#else
extern "C" const char* PortSymbolName(const void*) { return "?"; }
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <cstdio>

namespace
{
// Diagnostic: the main binary's writable data, to find state a tick changes that no snapshot holds.
struct DataCopy
{
    std::vector<std::pair<unsigned char*, std::vector<unsigned char>>> parts;
    void Take(void)
    {
        static std::vector<std::pair<unsigned char*, unsigned long>> sects;
        if (sects.empty())
        {
            const mach_header_64* mh = (const mach_header_64*)_dyld_get_image_header(0);
            for (const char* seg : { "__DATA", "__DATA_DIRTY" })
                for (const char* sect : { "__data", "__bss", "__common" })
                {
                    unsigned long n = 0;
                    unsigned char* p = getsectiondata(mh, seg, sect, &n);
                    if (p != NULL && n > 0)
                        sects.push_back({ p, n });
                }
        }
        parts.resize(sects.size());
        for (size_t i = 0; i < sects.size(); i++)
        {
            parts[i].first = sects[i].first;
            parts[i].second.assign(sects[i].first, sects[i].first + sects[i].second);
        }
    }
    void Report(const DataCopy& other) const
    {
        int shown = 0;
        const void* lastSym = NULL;
        for (size_t i = 0; i < parts.size() && i < other.parts.size(); i++)
        {
            const std::vector<unsigned char>& a = parts[i].second;
            const std::vector<unsigned char>& b = other.parts[i].second;
            for (size_t o = 0; o < a.size() && o < b.size() && shown < 40; o += 4)
            {
                if (std::memcmp(&a[o], &b[o], o + 4 <= a.size() ? 4 : a.size() - o) == 0)
                    continue;
                const unsigned char* at = parts[i].first + o;
                Dl_info info;
                const bool named = dladdr(at, &info) && info.dli_sname != NULL;
                if (named && info.dli_saddr == lastSym)
                    continue;
                lastSym = named ? info.dli_saddr : NULL;
                shown++;
                std::fprintf(stderr, "[rollback]   data %p %s+0x%lx\n", (const void*)at,
                             named ? info.dli_sname : "?",
                             named ? (unsigned long)(at - (const unsigned char*)info.dli_saddr)
                                   : 0ul);
            }
        }
    }
};

DataCopy s_data[2];
}   // namespace

extern "C" void PortRollbackDataTake(int which) { s_data[which & 1].Take(); }
extern "C" void PortRollbackDataReport(void) { s_data[0].Report(s_data[1]); }

// Diagnostic: every binary-data symbol that ever differed between the two starts, once each.
extern "C" void PortRollbackDataCensus(void)
{
    static std::vector<const void*> seen;
    const DataCopy& a = s_data[0];
    const DataCopy& b = s_data[1];
    for (size_t i = 0; i < a.parts.size() && i < b.parts.size(); i++)
    {
        const std::vector<unsigned char>& x = a.parts[i].second;
        const std::vector<unsigned char>& y = b.parts[i].second;
        for (size_t o = 0; o + 4 <= x.size() && o + 4 <= y.size(); o += 4)
        {
            if (std::memcmp(&x[o], &y[o], 4) == 0)
                continue;
            const unsigned char* at = a.parts[i].first + o;
            Dl_info info;
            const bool named = dladdr(at, &info) && info.dli_sname != NULL;
            const void* key = named ? info.dli_saddr : (const void*)at;
            bool dup = false;
            for (const void* k : seen)
                dup = dup || k == key;
            if (dup)
                continue;
            seen.push_back(key);
            std::fprintf(stderr, "[census] %p %s+0x%lx\n", (const void*)at,
                         named ? info.dli_sname : "?",
                         named ? (unsigned long)(at - (const unsigned char*)info.dli_saddr) : 0ul);
        }
    }
}
#else
extern "C" void PortRollbackDataTake(int) {}
extern "C" void PortRollbackDataReport(void) {}
extern "C" void PortRollbackDataCensus(void) {}
#endif
