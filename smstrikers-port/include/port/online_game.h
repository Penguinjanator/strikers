// The online session in the game: local pads and match transitions.

#ifndef PORT_ONLINE_GAME_H
#define PORT_ONLINE_GAME_H

#ifdef __cplusplus
extern "C" {
#endif

// Once at boot, after the config and the disc, which set this machine's online identity.
void PortOnlineGameInit(void);

// Each frame after the pad sample and before the tasks.
void PortOnlineGameFrame(void);

// At exit: peers hear that this machine left.
void PortOnlineGameShutdown(void);

// Non-zero while the VS screen waits for every machine to load.
int PortOnlineGameHoldLoading(void);
int PortOnlineGameSelecting(void);
int PortOnlineGameSelectionSide(int port);
int PortOnlineGameSelectStadium(int stadium);
int PortOnlineGameSelectionBack(void);
void PortOnlineGameSaveOptions(void);
int PortOnlineGameStadiumUnlocked(void);
// Every stadium while an online room selects; otherwise `own`, the save's unlock.
int PortOnlineGameStadiumOpen(int own);
int PortOnlineGameOptionsOpen(void);

#ifdef __cplusplus
}

class GameInfoManager;
class ChooseCaptainsSceneV2;
class ButtonComponent;
void PortOnlineGameStadiumButtons(ButtonComponent* buttons, bool unlocked);
void PortOnlineGameCaptainsCreated(ChooseCaptainsSceneV2* scene);
int PortOnlineGameCaptainsUpdate(ChooseCaptainsSceneV2* scene, float dt);

// OnPreGameState: an online match's settings onto the current-game copies, never the card's.
void PortOnlineGameApplySettings(GameInfoManager* info);
#endif

#endif   // PORT_ONLINE_GAME_H
