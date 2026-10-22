// smstrikers-port: GXAdjustForOverscan against the SDK's arithmetic.
#include <gtest/gtest.h>

#include <dolphin/gx.h>

namespace {

// NTSC interlaced, double-strike field buffer, so the vertical trim is applied whole to every height.
TEST(OverscanTest, DoesTheSdkArithmetic) {
  GXRenderModeObj out{};
  GXAdjustForOverscan(&GXNtsc480Int, &out, 8, 16);
  EXPECT_EQ(out.fbWidth, 640 - 16);
  EXPECT_EQ(out.efbHeight, 480 - 32);
  EXPECT_EQ(out.xfbHeight, 480 - 32);
  EXPECT_EQ(out.viWidth, 640 - 16);
  EXPECT_EQ(out.viHeight, 480 - 32);
  EXPECT_EQ(out.viXOrigin, GXNtsc480Int.viXOrigin + 8);
  EXPECT_EQ(out.viYOrigin, GXNtsc480Int.viYOrigin + 16);
  EXPECT_EQ(out.xFBmode, GXNtsc480Int.xFBmode);
}

// A single-strike field buffer at 240 lines trims the XFB by half the amount,
// which is the one place the SDK's formula is not uniform.
TEST(OverscanTest, HalvesTheXfbTrimForASingleStrikeFieldBuffer) {
  GXRenderModeObj in = GXNtsc480Int;
  in.xFBmode = VI_XFBMODE_SF;
  in.xfbHeight = 240;
  GXRenderModeObj out{};
  GXAdjustForOverscan(&in, &out, 0, 16);
  EXPECT_EQ(out.xfbHeight, 240 - 16);
  EXPECT_EQ(out.efbHeight, 480 - (32 * 480) / 240);
}

} // namespace
