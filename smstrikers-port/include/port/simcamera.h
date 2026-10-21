// The lockstep simulation camera: stepped in the tick, with session options and a 4:3 frustum.
// While g_simLockstep is clear every function here answers as retail does.

#ifndef PORT_SIMCAMERA_H
#define PORT_SIMCAMERA_H

class cCharacter;
class GameplayCamera;
class nlVector3;
class nlVector4;
class nlMatrix4;

// The session's camera options, the same on every machine whatever its player chose.
extern bool g_portSimCameraAutoZoom;
extern float g_portSimCameraZoomLevel;

// The simulation camera, or NULL while no lockstep match has begun.
GameplayCamera* PortSimCamera(void);

// The point the simulation aims at: the simulation camera's target, or the top camera's.
const nlVector3& PortSimCameraTarget(void);

// The kickoff framing; setting it snaps to the target, as the stack's camera snaps at each kickoff.
void PortSimCameraForceNeutral(bool force);

// A camera rumble fired from the tick; it moves this camera's view as it moves the stack's.
void PortSimCameraRumble(float x, float y);

// The six planes of the simulation camera's frustum, or NULL when the frame's frustum decides.
const nlVector4* PortSimCameraFrustum(void);

// Character `index` as this frame's render projects it; in lockstep only the stack camera reads it.
nlVector3& PortRenderScreenPosition(int index);

// The screen position of `character` that `camera`'s automatic zoom reads.
const nlVector3& PortCameraScreenPosition(const GameplayCamera* camera,
                                          const cCharacter* character);

#endif   // PORT_SIMCAMERA_H
