// THP movies: pacing, and audio in the port mixer's channel order; Aurora decodes the video.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dolphin/types.h"
#include "port/benchmark.h"
#include "port/determinism.h"
#include "port/host.h"

s32 THPVideoDecode(const void* file, void* tileY, void* tileU, void* tileV, void* work);

u32 OSGetTick(void);

// OSGetTick units: the console's 40.5 MHz timer, which src/platform/os.c and PortVirtualTicker both
// count in.
#define PORT_THP_TICKS_PER_SECOND 40500000.0

static u32 port_thp_now(void)
{
    // Under STRIKERS_FIXED_DT the virtual clock advances one frame period per retrace, which is
    // what keeps a capture at frame N the same frame N.
    return PortFixedTimestep() ? PortVirtualTicker() : OSGetTick();
}

static u32 s_pace_base;
static unsigned long long s_pace_wall_base;
static long s_pace_shown;
static int s_pace_started;

static void port_thp_pace_reset(void)
{
    s_pace_started = 0;
    s_pace_shown = 0;
    s_pace_base = 0;
}

static void port_thp_pace_begin(u32 now)
{
    if (!s_pace_started)
    {
        s_pace_started = 1;
        s_pace_base = now;
        s_pace_wall_base = port_monotonic_ns();
        s_pace_shown = 0;
    }
}

int port_thp_frame_due(float frameRate)
{
    u32 now;
    double elapsed;

    if (!(frameRate > 0.0f))
    {
        return 1; // no rate in the header: decode as fast as asked
    }

    now = port_thp_now();
    port_thp_pace_begin(now);

    // Movie frames elapsed since the movie started, on the game's clock.
    elapsed = (double)(u32)(now - s_pace_base) / PORT_THP_TICKS_PER_SECOND
            * (double)frameRate;
    return elapsed >= (double)s_pace_shown;
}

// One frame decoded. Counted here rather than in port_thp_frame_due because a movie has two
// possible clocks, this file's and the audio buffer's, and the progress line has to be right under
// both.
void port_thp_frame_shown(void)
{
    u32 now = port_thp_now();

    port_thp_pace_begin(now);
    s_pace_shown++;

    // Progress against the clock, so "the movie runs fast" is a number rather than an impression:
    // 500 frames at 29.97 fps is 16.7 s.
    if (getenv("STRIKERS_LOG_SCENES") != NULL && (s_pace_shown % 500) == 0)
    {
        // Both clocks, because the game clock is the wall clock in play and the virtual one under
        // STRIKERS_FIXED_DT: when they differ that is why, and when they agree the pacing is right.
        fprintf(stderr, "[port] movie: %ld frames shown in %.1f s on the game clock, "
                        "%.1f s on the wall\n",
                s_pace_shown,
                (double)(u32)(now - s_pace_base) / PORT_THP_TICKS_PER_SECOND,
                (double)(port_monotonic_ns() - s_pace_wall_base) / 1e9);
    }
}

static int port_thp_movies_disabled(void)
{
    const char* v;

    // STRIKERS_BENCHMARK's demo run is measuring a match and has to get to one.
    PortBenchInit();
    if (PortBenchWantsDemo())
    {
        return 1;
    }

    v = getenv("STRIKERS_NO_MOVIES");
    return v != NULL && v[0] != '\0' && v[0] != '0';
}

int port_thp_open(void)
{
    port_thp_pace_reset();
    return !port_thp_movies_disabled();
}

s32 port_thp_video_decode(const void* frame, void* tileY, void* tileU, void* tileV)
{
    static int reported;
    s32 rc;

    rc = THPVideoDecode(frame, tileY, tileU, tileV, NULL);
    if (rc != 0 && !reported)
    {
        reported = 1;
        fprintf(stderr, "[port] THP video: decoder error %d; the movie will be skipped.\n", (int)rc);
    }
    return rc == 0 ? 0 : -1;
}

// Audio: Nintendo DSP-ADPCM, two channels, one record per video frame.

static u32 rd_be32(const u8* p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static s16 rd_be16(const u8* p)
{
    return (s16)(((u32)p[0] << 8) | (u32)p[1]);
}

static s16 clip16(long long v)
{
    if (v < -32768)
        return (s16)-32768;
    if (v > 32767)
        return (s16)32767;
    return (s16)v;
}

// Writes `count` samples into `dst`, stepping `step` shorts per sample so the two channels can be
// interleaved into one buffer without a second pass.
static void decode_adpcm_channel(s16* dst, int step, const u8* nibbles,
                                 const s16* coef, s16 yn1, s16 yn2, u32 count)
{
    s32 s1 = yn1;
    s32 s2 = yn2;
    u32 n = 0;

    while (n < count)
    {
        u32 header = *nibbles++;
        s32 scale = 1 << (header & 0x0F);
        const s16* c = coef + ((header >> 4) & 7) * 2;
        u32 k;
        u8 byte = 0;

        for (k = 0; k < 14 && n < count; k++, n++)
        {
            s32 nib;
            // 64-bit accumulator: the console's DSP had a wider one than the 32 bits this
            // arithmetic would otherwise get, and a coefficient pair near the extremes can carry
            // past 2^31 before the shift.
            long long v;

            if (k & 1)
            {
                nib = (s32)(byte & 0x0F);
            }
            else
            {
                byte = *nibbles++;
                nib = (s32)(byte >> 4);
            }
            if (nib >= 8)
            {
                nib -= 16;
            }

            v = ((long long)(nib * scale) << 11) + 1024
                + (long long)s1 * (long long)c[0] + (long long)s2 * (long long)c[1];
            v >>= 11;
            s2 = s1;
            s1 = clip16(v);
            *dst = (s16)s1;
            dst += step;
        }
    }
}

u32 port_thp_audio_decode(s16* audioBuffer, u8* audioFrame, s32 flag)
{
    u32 offsetNextChannel;
    u32 sampleSize;
    s16 coefL[16];
    s16 coefR[16];
    s16 lyn1, lyn2, ryn1, ryn2;
    const u8* nibbles;
    int i;

    (void)flag; // the SDK's "decode into the second half of the buffer" flag;
                // THPSimple only ever passes 0.

    if (audioBuffer == NULL || audioFrame == NULL)
    {
        return 0;
    }

    offsetNextChannel = rd_be32(audioFrame);
    sampleSize = rd_be32(audioFrame + 4);
    if (sampleSize == 0)
    {
        return 0;
    }

    for (i = 0; i < 16; i++)
    {
        coefL[i] = rd_be16(audioFrame + 8 + i * 2);
        coefR[i] = rd_be16(audioFrame + 40 + i * 2);
    }
    lyn1 = rd_be16(audioFrame + 72);
    lyn2 = rd_be16(audioFrame + 74);
    ryn1 = rd_be16(audioFrame + 76);
    ryn2 = rd_be16(audioFrame + 78);

    nibbles = audioFrame + 80;

    decode_adpcm_channel(audioBuffer, 2, nibbles, coefL, lyn1, lyn2, sampleSize);

    if (offsetNextChannel != 0)
    {
        decode_adpcm_channel(audioBuffer + 1, 2, nibbles + offsetNextChannel, coefR,
                             ryn1, ryn2, sampleSize);
    }
    else
    {
        // Mono record: the TEV, the mixer and the buffer are all stereo, so duplicate rather than
        // leave one side silent.
        u32 n;
        for (n = 0; n < sampleSize; n++)
        {
            audioBuffer[n * 2 + 1] = audioBuffer[n * 2];
        }
    }

    return sampleSize;
}
