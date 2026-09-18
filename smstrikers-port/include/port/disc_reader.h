#ifndef PORT_DISC_READER_H
#define PORT_DISC_READER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Runs jobs in order on one reader thread. Blocks if the queue is full.
void PortDiscQueue(void (*work)(void*), void* ctx);

// Starts a separate worker. PortDiscJoin waits for completion and deletes the thread object.
void* PortDiscSpawn(void (*work)(void*), void* ctx);
void PortDiscJoin(void* thread);

#ifdef __cplusplus
}
#endif

// Release/acquire publication for reader state and completed data.
static inline void port_store_release_i32(int32_t* p, int32_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static inline int32_t port_load_acquire_i32(const int32_t* p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static inline void port_store_release_u64(unsigned long long* p, unsigned long long v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static inline unsigned long long port_load_acquire_u64(const unsigned long long* p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

#endif
