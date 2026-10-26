// The lockstep simulation camera and the stick turn that stands in for its remap.

#include "port/lockstep.h"
#include <cstdio>
#include <cstdlib>
#include "port/rollback.h"
#include "port/simcamera.h"

#include "Game/Camera/CameraMan.h"
#include "Game/Camera/GameplayCam.h"
#include "Game/Character.h"
#include "Game/CharacterTemplate.h"
#include "Game/CharacterTweaks.h"
#include "Game/FixedUpdateTask.h"
#include "Game/Game.h"
#include "Game/Player.h"
#include "NL/nlMath.h"
#include "NL/platvmath.h"
#include "dolphin/pad.h"

#include <cstring>
#include <new>

extern float g_fFixedUpdateTick;

bool g_portSimCameraAutoZoom = true;
float g_portSimCameraZoomLevel = 0.5f;

namespace
{

// The console's projection: 33.4 degrees across, ratio 1.25, near 0.25, far 1024; no libm tan().
const float kProjection[4][4] = {
    { 0x1.aaa56cp+1f, 0.0f, 0.0f, 0.0f },
    { 0.0f, 0x1.0aa764p+2f, 0.0f, 0.0f },
    { 0.0f, 0.0f, -0x1.001002p-12f, -0x1.001002p-2f },
    { 0.0f, 0.0f, -1.0f, 0.0f },
};

// The slowest retail time scale, 0.005, makes one tick 4 s of camera time.
const float kMaxCameraStep = 4.0f;

alignas(GameplayCamera) unsigned char s_storage[sizeof(GameplayCamera)];
GameplayCamera* s_camera;   // NULL until a lockstep match begins
bool s_stepped;             // Reactivate reads the knot tables the first Update installs
bool s_held;                // another camera was on top of the stack when last looked at
nlMatrix4 s_projection;
nlMatrix4 s_view;   // the camera's view with the rumble offset, as the stack filters it
nlVector4 s_frustum[6];
nlVector3 s_render[10];

// The spring of cRumbleFilter without its sound events, which belong to the camera on screen.
struct Rumble
{
    nlVector2 pos0, pos1, vel0, vel1;
} s_rumble;

// The simulation camera rolls back with the match; s_render is the screen's and does not.
const bool s_rollbackState =
    (PortSimState(s_storage, sizeof s_storage), PortSimState(&s_stepped, sizeof s_stepped),
     PortSimState(&s_held, sizeof s_held), PortSimState(&s_projection, sizeof s_projection),
     PortSimState(&s_view, sizeof s_view), PortSimState(s_frustum, sizeof s_frustum),
     PortSimState(&s_rumble, sizeof s_rumble), true);

void rumble_reset(void) { std::memset(&s_rumble, 0, sizeof s_rumble); }

void rumble_step(float dt)
{
    const float Ks = 5000.0f;
    const float Kd = 10.0f;
    const float step = dt <= 0.02f ? dt : 0.02f;

    const float dy = s_rumble.pos0.y - s_rumble.pos1.y;
    const float dx = s_rumble.pos0.x - s_rumble.pos1.x;
    const float len = nlSqrt(dx * dx + dy * dy, true);
    const float fHTerm = len * Ks;
    const float dvy = s_rumble.vel0.y - s_rumble.vel1.y;
    const float dvx = s_rumble.vel0.x - s_rumble.vel1.x;
    const float proj = (dvx * dx) + (dvy * dy);
    const float fDTerm = len == 0.0f ? 0.0f : (proj * Kd) / len;

    float ux = 0.0f;
    float uy = 0.0f;
    if (len != 0.0f)
    {
        const float invLen = 1.0f / len;
        ux = invLen * dx;
        uy = invLen * dy;
    }

    const float total = -(fHTerm + fDTerm);
    ux = total * ux;
    uy = total * uy;
    const float force0x = ux + 0.0f;
    const float force0y = uy + 0.0f;
    const float force1x = 0.0f - ux;
    const float force1y = 0.0f - uy;

    s_rumble.vel0.x = s_rumble.vel0.x + (step * (0.0f * force0x));
    s_rumble.vel0.y = s_rumble.vel0.y + (step * (0.0f * force0y));
    s_rumble.pos0.x = s_rumble.pos0.x + (step * s_rumble.vel0.x);
    s_rumble.pos0.y = s_rumble.pos0.y + (step * s_rumble.vel0.y);
    s_rumble.vel1.x = s_rumble.vel1.x + (step * (1.0f * force1x));
    s_rumble.vel1.y = s_rumble.vel1.y + (step * (1.0f * force1y));
    s_rumble.pos1.x = s_rumble.pos1.x + (step * s_rumble.vel1.x);
    s_rumble.pos1.y = s_rumble.pos1.y + (step * s_rumble.vel1.y);
}

// What cCameraManager::Remove does to the camera that comes back on top.
void snap(void)
{
    rumble_reset();
    if (s_stepped)
        s_camera->Reactivate();
}

// The stack updates only its top camera, so a Super Strike camera the tick pushes holds this too.
bool held_by_stack(void)
{
    cBaseCamera* top = cCameraManager::m_cameraStack != NULL ? cCameraManager::PeekCamera() : NULL;
    const bool held = top == NULL || top->GetType() != eCameraType_Gameplay;
    if (s_held && !held)
        snap();
    s_held = held;
    return held;
}

// World::ExtractFrustumPlanes over the simulation camera's view and projection.
void extract_frustum(void)
{
    nlMatrix4 projection = s_projection;
    const float m34 = projection.m34;
    projection.m34 = projection.m43;
    projection.m43 = m34;
    projection.m33 = projection.m33 - 1.0f;

    nlMatrix4 vp;
    nlMultMatrices(vp, s_view, projection);

    float p[6][4];
    for (int r = 0; r < 4; r++)
    {
        p[0][r] = vp.e2[r][3] - vp.e2[r][0];
        p[1][r] = vp.e2[r][3] + vp.e2[r][0];
        p[2][r] = vp.e2[r][3] + vp.e2[r][1];
        p[3][r] = vp.e2[r][3] - vp.e2[r][1];
        p[4][r] = vp.e2[r][2];
        p[5][r] = vp.e2[r][3] - vp.e2[r][2];
    }
    for (int i = 0; i < 6; i++)
    {
        const float length =
            nlSqrt(p[i][0] * p[i][0] + p[i][1] * p[i][1] + p[i][2] * p[i][2], true);
        s_frustum[i].x = p[i][0] / length;
        s_frustum[i].y = p[i][1] / length;
        s_frustum[i].z = p[i][2] / length;
        s_frustum[i].w = p[i][3] / length;
    }
}

void set_view(void)
{
    s_view = s_camera->GetViewMatrix();
    s_view.m41 += s_rumble.pos1.x;
    s_view.m42 += s_rumble.pos1.y;
    extract_frustum();
}

// What glplatViewProjectPoint does, through the simulation camera.
void project(const nlVector3& world, nlVector3& ndc)
{
    nlVector3 eye;
    nlMultPosVectorMatrix(eye, world, s_view);
    nlMultPosVectorMatrix(ndc, eye, s_projection);
    const float wc = 1.0f / -eye.z;
    ndc.x = ndc.x * wc;
    ndc.y = -ndc.y * wc;
    ndc.z = ndc.z * wc;
}

s8 quantise(float v)
{
    const int i = (int)(v < 0.0f ? v - 0.5f : v + 0.5f);
    return (s8)(i < -128 ? -128 : i > 127 ? 127 : i);
}

void turn(s8& x, s8& y, float s, float c)
{
    const float fx = (float)x;
    const float fy = (float)y;
    x = quantise(fx * c - fy * s);
    y = quantise(fx * s + fy * c);
}

}   // namespace

extern "C" void PortSimCameraBeginMatch(void)
{
    if (!g_simLockstep)
    {
        s_camera = NULL;
        return;
    }

    std::memset(s_storage, 0, sizeof s_storage);
    s_camera = new (s_storage) GameplayCamera();
    s_stepped = false;
    s_held = false;
    rumble_reset();
    std::memcpy(s_projection.e2, kProjection, sizeof kProjection);
    set_view();

    for (int i = 0; i < 10; i++)
    {
        if (g_pCharacters[i] != NULL)
            nlVec3Set(g_pCharacters[i]->m_v3ScreenPosition, 0.0f, 0.0f, 0.0f);
    }
}

extern "C" void PortSimCameraStep(void)
{
    if (s_camera == NULL)
        return;

    if (!held_by_stack())
    {
        // Retail's camera runs on the frame clock, which slow motion does not slow.
        float left = g_fFixedUpdateTick / FixedUpdateTask::mTimeScale;
        if (!(left <= kMaxCameraStep))
            left = kMaxCameraStep;

        s_camera->mUpVector = cCameraManager::m_UpVectorStack[cCameraManager::m_UpVectorStackSize];
        while (left > 0.0f)
        {
            const float step = left < g_fFixedUpdateTick ? left : g_fFixedUpdateTick;
            s_camera->Update(step);
            rumble_step(step);
            left -= step;
        }
        s_stepped = true;
    }
    set_view();

    // The render leaves the AI's screen positions alone while a captain's Super Strike plays.
    if (g_pGame->mbCaptainShotToScoreOn)
        return;

    for (int i = 0; i < 10; i++)
    {
        cPlayer* player = (cPlayer*)g_pCharacters[i];
        nlVector3 world = player->m_v3Position;
        world.z += player->m_pTweaks->fPhysCapsuleHeight * 0.5f;
        project(world, player->m_v3ScreenPosition);
    }
}

GameplayCamera* PortSimCamera(void) { return s_camera; }

const nlVector3& PortSimCameraTarget(void)
{
    if (s_camera != NULL)
        return s_camera->GetTargetPosition();
    return cCameraManager::PeekCamera()->GetTargetPosition();
}

void PortSimCameraForceNeutral(bool force)
{
    if (s_camera == NULL)
        return;

    s_camera->m_ForceNeutralAndNearZoom = force;
    if (force)
        snap();
}

void PortSimCameraRumble(float x, float y)
{
    if (s_camera == NULL || held_by_stack())
        return;

    rumble_reset();
    nlVec2Set(s_rumble.pos1, x, y);
}

const nlVector4* PortSimCameraFrustum(void) { return s_camera != NULL ? s_frustum : NULL; }

nlVector3& PortRenderScreenPosition(int index) { return s_render[index]; }

const nlVector3& PortCameraScreenPosition(const GameplayCamera* camera, const cCharacter* character)
{
    if (s_camera != NULL && camera != s_camera)
    {
        for (int i = 0; i < 10; i++)
        {
            if (g_pCharacters[i] == character)
                return s_render[i];
        }
    }
    return character->m_v3ScreenPosition;
}

extern "C" void PortSimTurnSticks(PADStatus* pads)
{
    // The simulation reads sticks through the gameplay camera's remap, 0x4000.
    const u16 angle = (u16)(cCameraManager::m_aJoystickRemap - 0x4000);
    if (angle == 0)
        return;

    float s;
    float c;
    nlSinCos(&s, &c, angle);
    for (int i = 0; i < 4; i++)
    {
        turn(pads[i].stickX, pads[i].stickY, s, c);
        turn(pads[i].substickX, pads[i].substickY, s, c);
    }
}
