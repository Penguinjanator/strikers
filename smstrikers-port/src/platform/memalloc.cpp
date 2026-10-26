// MemoryAllocator over an address range the port owns, so a pointer is identified by comparison.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "NL/MemAlloc.h"

#if defined(_WIN32)
#include <windows.h>
#elif !defined(__SWITCH__)
#include <sys/mman.h>
#endif

namespace
{

#if defined(__SWITCH__)
// Allocated from the heap up front, since newlib has no mmap to reserve address space.
const std::size_t kRegionSize = 512u * 1024u * 1024u;
#else
// Address space, committed by the OS on first touch; nothing here is returned to the OS.
const std::size_t kRegionSize = 768u * 1024u * 1024u;
#endif

// At least the largest alignment any caller asks for, so the payload never runs over the header.
const std::size_t kHeaderSize = 32;

struct BlockHeader
{
    std::size_t size;     // payload bytes, as the caller asked for them
    std::size_t offset;   // payload - block start, so Free can recover it
    void* owner;          // which allocator's accounting this belongs to
    std::size_t pad;
};

char* s_region;
char* s_bump;
char* s_end;

bool region_init()
{
    if (s_region != nullptr)
        return true;
#if defined(_WIN32)
    void* p = VirtualAlloc(nullptr, kRegionSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#elif defined(__SWITCH__)
    // Blocks must start on a kHeaderSize boundary; newlib's malloc only guarantees 16 bytes.
    void* p = ::aligned_alloc(4096, kRegionSize);
#else
    void* p =
        mmap(nullptr, kRegionSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        p = nullptr;
#endif
    if (p == nullptr)
        return false;
#if defined(__SWITCH__)
    // Match the initial contents of anonymous mappings and VirtualAlloc.
    std::memset(p, 0, kRegionSize);
#endif
    s_region = (char*)p;
    s_bump = s_region;
    s_end = s_region + kRegionSize;
    return true;
}

inline bool region_owns(const void* p)
{
    // Never dereferences p, so it is safe for a foreign pointer.
    return s_region != nullptr && (const char*)p >= s_region && (const char*)p < s_end;
}

// Per-instance accounting; the class layout is fixed, so the unused m_free_block_list points here.
struct AllocState
{
    std::size_t live;
    std::size_t pool;
    // Size class i holds blocks of 2^(i+5) bytes.
    void* freeList[32];
};

AllocState* state_for(FreeBlockList** slot)
{
    if (*slot == nullptr)
        *slot = (FreeBlockList*)std::calloc(1, sizeof(AllocState));
    return (AllocState*)*slot;
}

inline int size_class(std::size_t n)
{
    int c = 0;
    std::size_t s = 32;
    while (s < n && c < 31)
    {
        s <<= 1;
        c++;
    }
    return c;
}

inline std::size_t class_size(int c) { return (std::size_t)32 << c; }

// The simulation heap: inside a sim scope every allocation comes from here, one span to snapshot.
#if defined(__SWITCH__)
const std::size_t kSimSize = 48u * 1024u * 1024u;
#else
const std::size_t kSimSize = 64u * 1024u * 1024u;
#endif

struct SimHeap
{
    char* lo;
    char* bump;
    char* end;
    AllocState state;
};
SimHeap s_sim;
int s_simScope;
int s_simPause;   // presentation called from inside a tick allocates outside the simulation heap

bool sim_init()
{
    if (s_sim.lo != nullptr)
        return true;
    if (!region_init())
        return false;
    std::uintptr_t a = ((std::uintptr_t)s_bump + 4095) & ~(std::uintptr_t)4095;
    if ((char*)(a + kSimSize) > s_end)
        return false;
    s_bump = (char*)(a + kSimSize);
    s_sim.lo = (char*)a;
    s_sim.bump = s_sim.lo;
    s_sim.end = s_sim.lo + kSimSize;
    return true;
}

}   // namespace

void MemoryAllocator::Initialize(void* memory, unsigned int size)
{
    // The size is kept so TotalFreeMemory still reports what the game budgeted for.
    (void)memory;
    m_free_block_list = nullptr;
    AllocState* st = state_for(&m_free_block_list);
    st->pool = size;
    st->live = 0;
}

void* MemoryAllocator::Allocate(unsigned long size, unsigned int alignment, bool fromEnd)
{
    // fromEnd placed long-lived blocks at the top of the console's arena.
    (void)fromEnd;
    if (!region_init())
        return nullptr;
    if (alignment < alignof(std::max_align_t))
        alignment = alignof(std::max_align_t);
    if (alignment > kHeaderSize)
        alignment = kHeaderSize;   // the header already reserves the largest

    const bool sim = s_simScope > 0 && s_simPause == 0 && this == &StandardAllocator && sim_init();
    AllocState* st = sim ? &s_sim.state : state_for(&m_free_block_list);
    char** bump = sim ? &s_sim.bump : &s_bump;
    char* end = sim ? s_sim.end : s_end;
    const std::size_t want = kHeaderSize + (size ? (std::size_t)size : 1);
    const int cls = size_class(want);

    char* block;
    bool recycled = false;
    if (st->freeList[cls] != nullptr)
    {
        block = (char*)st->freeList[cls];
        st->freeList[cls] = *(void**)block;
        recycled = true;
    }
    else
    {
        const std::size_t take = class_size(cls);
        if (*bump + take > end)
        {
            if (sim)
                std::fprintf(stderr, "memalloc: simulation heap full at %zu MB\n", kSimSize >> 20);
            return nullptr;
        }
        block = *bump;
        *bump += take;
    }

    std::uintptr_t raw = (std::uintptr_t)block + kHeaderSize;
    std::uintptr_t aligned = (raw + alignment - 1) & ~(std::uintptr_t)(alignment - 1);
    BlockHeader* h = (BlockHeader*)(aligned - sizeof(BlockHeader));
    h->size = (std::size_t)size;
    h->offset = (std::size_t)(aligned - (std::uintptr_t)block);
    h->owner = st;
    st->live += (std::size_t)size;
    // Every mode starts allocations zeroed; fresh blocks are zeroed by the system.
    if (recycled)
        std::memset((void*)aligned, 0, (std::size_t)size);
    return (void*)aligned;
}

void MemoryAllocator::Free(void* p)
{
    if (p == nullptr)
        return;

    if (!region_owns(p))
    {
        // A foreign allocation: no header probe, since that read would be out of bounds.
        std::free(p);
        return;
    }

    BlockHeader* h = (BlockHeader*)((char*)p - sizeof(BlockHeader));
    char* block = (char*)p - h->offset;

    // Charge the block to whoever allocated it: nlFree picks its instance by testing bit 31.
    AllocState* st = h->owner ? (AllocState*)h->owner : state_for(&m_free_block_list);
    st->live -= h->size < st->live ? h->size : st->live;

    const int cls = size_class(h->offset + h->size);
    *(void**)block = st->freeList[cls];
    st->freeList[cls] = block;
}

unsigned int MemoryAllocator::TotalFreeMemory()
{
    AllocState* st = state_for(&m_free_block_list);
    return (unsigned int)(st->pool > st->live ? st->pool - st->live : 0);
}

unsigned int MemoryAllocator::LargestFreeBlock()
{
    // No fragmentation to report; the region satisfies anything up to the budget.
    return TotalFreeMemory();
}

// A carve-out for the VM window in vm.c; from the region, or pointers inside it test as foreign.
extern "C" void* port_region_reserve(std::size_t size, std::size_t align)
{
    if (!region_init())
        return nullptr;
    if (align < alignof(std::max_align_t))
        align = alignof(std::max_align_t);
    std::uintptr_t aligned = ((std::uintptr_t)s_bump + align - 1) & ~(std::uintptr_t)(align - 1);
    if ((char*)(aligned + size) > s_end)
        return nullptr;
    s_bump = (char*)(aligned + size);
    return (void*)aligned;
}

// Replaces the 0x8xxxxxxx tests that told a console pointer from a small integer id.
extern "C" int port_region_owns(const void* p) { return region_owns(p) ? 1 : 0; }

extern "C" void port_region_stats(std::size_t* used, std::size_t* total)
{
    if (used)
        *used = (s_region != nullptr) ? (std::size_t)(s_bump - s_region) : 0;
    if (total)
        *total = (s_region != nullptr) ? (std::size_t)(s_end - s_region) : 0;
}

extern "C" int g_simLockstep;

// Off the lockstep path the simulation never rolls back, so its objects stay in the region.
extern "C" void PortSimHeapEnter(void)
{
    if (g_simLockstep)
        s_simScope++;
}

extern "C" void PortSimHeapLeave(void)
{
    if (g_simLockstep)
        s_simScope--;
}

extern "C" int PortSimHeapActive(void) { return s_simScope > 0 && s_simPause == 0; }

extern "C" void PortSimHeapPause(void) { s_simPause++; }
extern "C" int PortSimHeapPaused(void) { return s_simScope > 0 && s_simPause > 0; }
extern "C" void PortSimHeapResume(void) { s_simPause--; }

extern "C" int PortSimHeapOwns(const void* p)
{
    return s_sim.lo != nullptr && (const char*)p >= s_sim.lo && (const char*)p < s_sim.end;
}

extern "C" std::size_t PortSimHeapUsed(void)
{
    return s_sim.lo ? (std::size_t)(s_sim.bump - s_sim.lo) : 0;
}

// The used span plus sizeof(SimHeap) bookkeeping; past the saved bump everything is free again.
extern "C" std::size_t PortSimHeapSnapshotSize(void) { return PortSimHeapUsed() + sizeof(SimHeap); }

extern "C" void PortSimHeapSave(void* out)
{
    const std::size_t used = PortSimHeapUsed();
    std::memcpy(out, &s_sim, sizeof s_sim);
    if (used != 0)
        std::memcpy((char*)out + sizeof s_sim, s_sim.lo, used);
}

extern "C" void PortSimHeapRestore(const void* in)
{
    SimHeap saved;
    std::memcpy(&saved, in, sizeof saved);
    const std::size_t used = (std::size_t)(saved.bump - saved.lo);
    if (used != 0)
        std::memcpy(s_sim.lo, (const char*)in + sizeof saved, used);
    // Fresh blocks past the restored bump must read as zero again, as the system's pages did.
    if (s_sim.bump > saved.bump)
        std::memset(saved.bump, 0, (std::size_t)(s_sim.bump - saved.bump));
    s_sim = saved;
}

extern "C" const void* PortSimHeapBase(void) { return s_sim.lo; }

// Diagnostic: the block of the simulation heap that holds `p`, as its payload start and size.
extern "C" int PortSimHeapBlockOf(const void* p, const void** start, std::size_t* size)
{
    if (s_sim.lo == nullptr || (const char*)p < s_sim.lo || (const char*)p >= s_sim.bump)
        return 0;
    // Blocks are laid out back to back from lo, each a size class long.
    char* block = s_sim.lo;
    while (block < s_sim.bump)
    {
        const BlockHeader* h = nullptr;
        // The payload begins kHeaderSize past the block, aligned; its header sits just before it.
        for (std::size_t off = kHeaderSize; off <= kHeaderSize * 2; off += 8)
        {
            const BlockHeader* c = (const BlockHeader*)(block + off - sizeof(BlockHeader));
            if (c->offset == off && c->owner == &s_sim.state)
            {
                h = c;
                break;
            }
        }
        if (h == nullptr)
            return 0;
        const std::size_t span = class_size(size_class(h->offset + h->size));
        if ((const char*)p < block + span)
        {
            *start = block + h->offset;
            *size = h->size;
            return 1;
        }
        block += span;
    }
    return 0;
}
