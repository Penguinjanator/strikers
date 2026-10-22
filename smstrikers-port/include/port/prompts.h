#ifndef PORT_PROMPTS_H
#define PORT_PROMPTS_H

#include <stdint.h>

#ifdef __cplusplus
class nlFont;
union SDL_Event;

extern "C" {

void PortPromptsFontLoaded(nlFont* font);
void PortPromptsFontUnloading(nlFont* font);
void PortPromptsEvent(const SDL_Event* event);
#endif

// RGBA menu art from the same resolved binding as the retail prompts; stable until exit.
const unsigned char* PortPromptsMenuImage(unsigned button, unsigned* width, unsigned* height);

void PortPromptsFrame(void);

void PortPromptsLegendsLoaded(void);
void PortPromptsLegendReloaded(unsigned long hash);
void PortPromptsLegendsUnloading(void);

int PortPromptsSetFamily(const char* family);

// The choose-sides controller images in pad order, and the texture for one, or 0 for the disc's.
void PortPromptsPadInstances(const void* const* instances);
uintptr_t PortPromptsPadTexture(const void* instance);

#ifdef __cplusplus
}
#endif

#endif // PORT_PROMPTS_H
