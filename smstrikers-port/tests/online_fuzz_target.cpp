// One input through every online decoder; anything accepted must encode back to the same bytes.

#include "../src/platform/online/wire.h"

#include <cstring>

using namespace online;

// The decoder whose round trip failed last, for the report.
const char* g_onlineFuzzWhy = "";

namespace
{
bool same(const uint8_t* a, const uint8_t* b, size_t n) { return std::memcmp(a, b, n) == 0; }

bool plain(const uint8_t* p, size_t n)
{
    uint8_t out[kHelloSize];
    Discover d;
    if (DecodeDiscover(p, n, &d) && (EncodeDiscover(d, out) != n || !same(out, p, n)))
        return false;
    Announce a;
    if (DecodeAnnounce(p, n, &a) && (EncodeAnnounce(a, out) != n || !same(out, p, n)))
        return false;
    Hello h;
    if (DecodeHello(p, n, &h) && (EncodeHello(h, out) != n || !same(out, p, n)))
        return false;
    Welcome w;
    if (DecodeWelcome(p, n, &w) && (EncodeWelcome(w, out) != n || !same(out, p, n)))
        return false;
    Refuse r;
    if (DecodeRefuse(p, n, &r) && (EncodeRefuse(r, out) != n || !same(out, p, n)))
        return false;
    SealedHeader sh;
    if (DecodeSealedHeader(p, n, &sh))
    {
        EncodeSealedHeader(sh, out);
        if (!same(out, p, kSealedHeader))
            return false;
    }
    return true;
}

bool message(uint8_t kind, const uint8_t* p, size_t n)
{
    Message m;
    if (!DecodeMessageBody(kind, p, n, &m))
        return true;
    uint8_t out[kMaxMessageBody];
    return m.kind == kind && EncodeMessageBody(m, out) == n && same(out, p, n);
}

// Walks the chunks as the parser did and encodes each parsed one again.
bool payload(const uint8_t* p, size_t n)
{
    static Payload parsed;
    if (!ParsePayload(p, n, &parsed))
        return true;
    if (parsed.inputCount > kMaxChunksPerMachine || parsed.messageCount > kMaxMessagesPerPayload)
        return false;
    int input = 0;
    int inputAck = 0;
    int hash = 0;
    int components = 0;
    int messages = 0;
    size_t at = 0;
    while (at < n)
    {
        const uint8_t kind = p[at];
        const size_t len = Get16(p + at + 1);
        uint8_t buf[kMaxPayload + 8];
        ChunkWriter w(buf, sizeof buf);
        switch (kind)
        {
        case CH_TIMING: w.Timing(parsed.timing); break;
        case CH_RELACK: w.RelAck(parsed.relAck.nextRelSeq, parsed.relAck.bitmap); break;
        case CH_INPUT:
        {
            const InputChunk& c = parsed.input[input++];
            w.Input(c.matchSerial, c.machine, c.firstTick, c.count, &c.records[0][0]);
            for (int i = 0; i < c.count; i++)
                if (!RecordValid(c.records[i], ONLINE_MAX_LOCAL_HUMANS))
                    return false;
            break;
        }
        case CH_INPUT_ACK:
        {
            const InputAckChunk& c = parsed.inputAck[inputAck++];
            w.InputAck(c.matchSerial, c.machine, c.nextTick);
            break;
        }
        case CH_HASH:
        {
            const HashChunk& c = parsed.hash[hash++];
            w.Hash(c.matchSerial, c.machine, c.firstTick, c.count, c.hash);
            break;
        }
        case CH_COMPONENTS:
        {
            const ComponentsChunk& c = parsed.components[components++];
            w.Components(c.matchSerial, c.machine, c.tick, c.count, c.hash);
            break;
        }
        case CH_BYE: w.Bye(parsed.byeReason); break;
        default:
        {
            const Message& m = parsed.messages[messages++];
            uint8_t body[kMaxMessageBody];
            const size_t size = EncodeMessageBody(m, body);
            if (m.kind != kind)
                return false;
            w.Reliable(kind, m.relSeq, body, size);
            break;
        }
        }
        if (w.Size() != 3 + len || !same(buf, p + at, w.Size()))
            return false;
        at += 3 + len;
    }
    return input == parsed.inputCount && inputAck == parsed.inputAckCount &&
           hash == parsed.hashCount && components == parsed.componentsCount &&
           messages == parsed.messageCount;
}
}   // namespace

bool OnlineFuzzOne(const uint8_t* data, size_t size)
{
    g_onlineFuzzWhy = "plaintext datagram";
    if (!plain(data, size))
        return false;
    g_onlineFuzzWhy = "payload";
    if (!payload(data, size))
        return false;
    g_onlineFuzzWhy = "message body";
    if (size >= 1 && !message(data[0], data + 1, size - 1))
        return false;
    g_onlineFuzzWhy = "descriptor or name";
    if (size >= ONLINE_DESC_BYTES)
    {
        OnlineMatchDesc d;
        if (PortOnlineDescDecode(data, &d) == NULL)
        {
            uint8_t out[ONLINE_DESC_BYTES];
            PortOnlineDescEncode(&d, out);
            if (!same(out, data, ONLINE_DESC_BYTES))
                return false;
        }
    }
    if (size >= ONLINE_NAME_MAX)
    {
        char name[ONLINE_NAME_MAX + 1];
        if (GetName(data, name))
        {
            uint8_t out[ONLINE_NAME_MAX];
            PutName(out, name);
            if (!same(out, data, ONLINE_NAME_MAX))
                return false;
        }
    }
    return true;
}
