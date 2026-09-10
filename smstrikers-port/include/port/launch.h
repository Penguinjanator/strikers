// The AuroraConfig fields that have to be chosen before the window exists.

#ifndef PORT_LAUNCH_H
#define PORT_LAUNCH_H

#if defined(PORT_USE_AURORA)

#include <aurora/aurora.h>

#ifdef __cplusplus
extern "C" {
#endif

// Apply STRIKERS_FULLSCREEN, STRIKERS_WINDOW_SIZE, STRIKERS_PAUSE_ON_FOCUS_LOST and STRIKERS_CACHE_DIR
// to `cfg`, give it a window icon, and place the window with its title bar on screen.
void PortAuroraConfigure(AuroraConfig* cfg);

void PortFollowRenderScale(unsigned int windowHeight);

void PortSetRenderScale(float scale);

float PortRenderScale(void);

#ifdef __cplusplus
}
#endif

#endif // PORT_USE_AURORA

#endif // PORT_LAUNCH_H
