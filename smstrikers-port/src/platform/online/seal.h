// Link keys and sealed datagrams: X25519, BLAKE2b keys per direction, XChaCha20-Poly1305.

#ifndef PORT_ONLINE_SEAL_H
#define PORT_ONLINE_SEAL_H

#include <cstddef>
#include <cstdint>

namespace online
{
struct KeyPair
{
    uint8_t secret[32];
    uint8_t publicKey[32];
};

// `secret` must already hold 32 random bytes.
void KeyPairFromSecret(KeyPair* pair);

// Both sides derive the same two keys; false for a low-order peer key.
bool DeriveLinkKeys(const uint8_t mySecret[32], const uint8_t theirPublic[32],
                    const uint8_t guestPublic[32], const uint8_t hostPublic[32],
                    const uint8_t nonce[16], uint32_t linkId, uint8_t guestToHost[32],
                    uint8_t hostToGuest[32]);

// Writes header, ciphertext and tag to `out`; returns the datagram's size.
size_t SealDatagram(const uint8_t key[32], uint32_t linkId, uint64_t seq, const uint8_t* plain,
                    size_t size, uint8_t* out);

// Authenticates and decrypts a datagram whose header has been checked; false on a bad tag.
bool OpenDatagram(const uint8_t key[32], const uint8_t* datagram, size_t size, uint8_t* plain,
                  size_t* plainSize);

void Wipe(void* secret, size_t size);

// The highest sequence number seen and a bitmap of the 64 below it.
class ReplayWindow
{
  public:
    ReplayWindow() : m_top(0), m_bits(0) {}
    bool Fresh(uint64_t seq) const;
    void Accept(uint64_t seq);

  private:
    uint64_t m_top;
    uint64_t m_bits;
};
}   // namespace online

#endif   // PORT_ONLINE_SEAL_H
