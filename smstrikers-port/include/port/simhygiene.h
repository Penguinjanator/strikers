// Match-start hygiene that holds in every mode, lockstep or not.

#ifndef PORT_SIMHYGIENE_H
#define PORT_SIMHYGIENE_H

#ifdef __cplusplus
extern "C" {
#endif

// Top of cGame::BeginGame: FP mode, simulation time, ball predictions and per-match statics reset.
void PortSimHygieneBeginMatch(void);

// Each restores its translation unit's statics that would carry one match into the next.
void PortResetFielderActionsStatics(void);
void PortResetEffectsStatics(void);

#ifdef __cplusplus
}
#endif

#endif   // PORT_SIMHYGIENE_H
