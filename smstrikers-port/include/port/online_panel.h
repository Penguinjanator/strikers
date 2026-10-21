// The online panel: the ImGui screens over the session's polled state and commands; main thread.

#ifndef PORT_ONLINE_PANEL_H
#define PORT_ONLINE_PANEL_H

#ifdef __cplusplus
extern "C" {
#endif

// Each frame inside Aurora's ImGui frame: the panel, notices and the in-match status.
void PortOnlinePanelDraw(void);

// Upload changed prompt art before the game opens its render pass.
void PortOnlinePanelPrepare(void);

// Every key event; F5 opens and closes the panel. Non-zero when the key was the panel's.
int PortOnlinePanelHandleKey(int scancode, int down);

// Show or hide the panel.
void PortOnlinePanelSetOpen(int open);
int PortOnlinePanelOpen(void);

// No active session, panel, pending toggle or toast.
int PortOnlinePanelIdle(void);

// Non-zero while the panel has the pads and the game reads them as neutral.
int PortOnlinePanelHasPads(void);

#ifdef __cplusplus
}
#endif

#endif   // PORT_ONLINE_PANEL_H
