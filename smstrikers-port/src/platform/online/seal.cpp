#include "seal.h"

#include "wire.h"

#include "monocypher.h"

#include <cstring>

namespace online
{
namespace
{
const char kLabel[] = "strikers online v1";

void make_nonce(uint8_t nonce[24], uint64_t seq, uint32_t linkId)
{
    std::memset(nonce, 0, 24);
    Put64(nonce, seq);
    Put32(nonce + 8, linkId);
}
}   // namespace

void KeyPairFromSecret(KeyPair* pair) { crypto_x25519_public_key(pair->publicKey, pair->secret); }

bool DeriveLinkKeys(const uint8_t mySecret[32], const uint8_t theirPublic[32],
                    const uint8_t guestPublic[32], const uint8_t hostPublic[32],
                    const uint8_t nonce[16], uint32_t linkId, uint8_t guestToHost[32],
                    uint8_t hostToGuest[32])
{
    uint8_t shared[32];
    crypto_x25519(shared, mySecret, theirPublic);
    uint8_t any = 0;
    for (int i = 0; i < 32; i++)
        any |= shared[i];
    if (any == 0)
    {
        Wipe(shared, sizeof shared);
        return false;
    }

    uint8_t id[4];
    Put32(id, linkId);
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, 64);
    crypto_blake2b_update(&ctx, shared, 32);
    crypto_blake2b_update(&ctx, (const uint8_t*)kLabel, sizeof kLabel - 1);
    crypto_blake2b_update(&ctx, guestPublic, 32);
    crypto_blake2b_update(&ctx, hostPublic, 32);
    crypto_blake2b_update(&ctx, nonce, 16);
    crypto_blake2b_update(&ctx, id, 4);
    uint8_t k[64];
    crypto_blake2b_final(&ctx, k);
    std::memcpy(guestToHost, k, 32);
    std::memcpy(hostToGuest, k + 32, 32);
    Wipe(k, sizeof k);
    Wipe(shared, sizeof shared);
    return true;
}

size_t SealDatagram(const uint8_t key[32], uint32_t linkId, uint64_t seq, const uint8_t* plain,
                    size_t size, uint8_t* out)
{
    SealedHeader h;
    h.linkId = linkId;
    h.seq = seq;
    EncodeSealedHeader(h, out);
    uint8_t nonce[24];
    make_nonce(nonce, seq, linkId);
    crypto_aead_lock(out + kSealedHeader, out + kSealedHeader + size, key, nonce, out,
                     kSealedHeader, plain, size);
    return kSealedHeader + size + kTagSize;
}

bool OpenDatagram(const uint8_t key[32], const uint8_t* datagram, size_t size, uint8_t* plain,
                  size_t* plainSize)
{
    if (size < kSealedHeader + kTagSize)
        return false;
    const size_t n = size - kSealedHeader - kTagSize;
    uint8_t nonce[24];
    make_nonce(nonce, Get64(datagram + 8), Get32(datagram + 4));
    if (crypto_aead_unlock(plain, datagram + kSealedHeader + n, key, nonce, datagram, kSealedHeader,
                           datagram + kSealedHeader, n) != 0)
        return false;
    *plainSize = n;
    return true;
}

void Wipe(void* secret, size_t size) { crypto_wipe(secret, size); }

bool ReplayWindow::Fresh(uint64_t seq) const
{
    if (seq == 0)
        return false;
    if (seq > m_top)
        return true;
    const uint64_t back = m_top - seq;
    if (back == 0 || back > 64)
        return false;
    return (m_bits & (1ull << (back - 1))) == 0;
}

void ReplayWindow::Accept(uint64_t seq)
{
    if (seq > m_top)
    {
        const uint64_t shift = seq - m_top;
        if (shift > 64)
            m_bits = 0;
        else
        {
            // The old top becomes bit (shift - 1).
            m_bits = shift == 64 ? 0 : m_bits << shift;
            if (m_top != 0)
                m_bits |= 1ull << (shift - 1);
        }
        m_top = seq;
        return;
    }
    const uint64_t back = m_top - seq;
    if (back >= 1 && back <= 64)
        m_bits |= 1ull << (back - 1);
}
}   // namespace online
