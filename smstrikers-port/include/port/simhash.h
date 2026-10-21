// Simulation hashes for desync checks; floats hash as bits with -0.0 and NaN folded.

#ifndef PORT_SIMHASH_H
#define PORT_SIMHASH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PortSimHashes
{
    uint64_t all;   // ball, chr, clk, score, game, pwr, npc and the seed
    uint64_t ball, chr, clk, score, game, pwr, npc;
    uint64_t scr;   // the screen positions the AI reads, outside `all`
    uint64_t perChar[10];
    uint32_t seed;   // nlDefaultSeed
} PortSimHashes;

// The match as it stands.
void PortSimHash(PortSimHashes* out);

// The g_pCharacters index of `character`, -1 for NULL and -2 for anything else.
int PortSimCharIndex(const void* character);

#ifdef __cplusplus
}
#endif

#endif   // PORT_SIMHASH_H
