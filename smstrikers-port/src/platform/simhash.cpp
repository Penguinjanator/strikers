// The simulation state hashes the online desync check compares.

#include "port/simhash.h"
#include "port/lockstep.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "port/rollback.h"

#include "Game/AI/Fielder.h"
#include "Game/AI/Powerups.h"
#include "Game/AI/ShotMeter.h"
#include "Game/Ball.h"
#include "Game/BasicStadium.h"
#include "Game/Character.h"
#include "Game/CharacterTemplate.h"
#include "Game/Game.h"
#include "Game/Goalie.h"
#include "Game/Physics/PhysicsAIBall.h"
#include "Game/Physics/PhysicsCharacter.h"
#include "Game/Player.h"
#include "Game/Render/Bowser.h"
#include "Game/Render/ChainChomp.h"
#include "Game/Render/NPCManager.h"
#include "Game/SAnim/pnSAnimController.h"
#include "Game/Sys/clock.h"
#include "Game/Team.h"
#include "NL/nlMath.h"
#include "ode/objects.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

extern PowerupBase* g_pPowerups[25];

namespace
{

uint32_t float_bits(float x)
{
    uint32_t bits;
    std::memcpy(&bits, &x, sizeof bits);
    return bits;
}

// -0.0 hashes as 0.0, and every NaN as one: payload and sign differ by instruction set.
uint32_t canonical_float_bits(float x)
{
    const uint32_t bits = float_bits(x);
    if ((bits & 0x7FFFFFFFu) == 0)
        return 0;
    if ((bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu) != 0)
        return 0x7FC00000u;
    return bits;
}

// FNV-1a over 32-bit words, splitmix64 finaliser.
struct Hasher
{
    uint64_t h = 0xcbf29ce484222325ull;
    void word(uint32_t v)
    {
        h ^= v;
        h *= 0x100000001b3ull;
    }
    void u32(uint32_t v) { word(v); }
    void i(int v) { word((uint32_t)v); }
    void b(bool v) { word(v ? 1u : 0u); }
    void fs(const float* p, int count)
    {
        for (int k = 0; k < count; k++)
            word(canonical_float_bits(p[k]));
    }
    void f(float x) { fs(&x, 1); }
    void v3(const nlVector3& v) { fs(v.e, 3); }
    void q(const nlQuaternion& v) { fs(v.e, 4); }
    void timer(const Timer& t) { u32(t.m_uPackedTime); }
    void h64(uint64_t v)
    {
        word((uint32_t)v);
        word((uint32_t)(v >> 32));
    }
    uint64_t done()
    {
        uint64_t z = h;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
};

#define HU(h, x) (h).u32((uint32_t)(x))
#define HI(h, x) (h).i((int)(x))
#define HB(h, x) (h).b((x) ? true : false)
#define HF(h, x) (h).f((x))
#define HV(h, x) (h).v3((x))
#define HQ(h, x) (h).q((x))
#define HT(h, x) (h).timer((x))
#define HX(h, x) (h).h64((x))

int char_index(const void* p)
{
    if (p == NULL)
        return -1;
    for (int i = 0; i < 10; i++)
    {
        if ((const void*)g_pCharacters[i] == p)
            return i;
    }
    return -2;
}

extern "C" int PortSimHeapOwns(const void* p);

void hash_body(Hasher& h, dBodyID body, const PhysicsObject* owner, const char* what)
{
    if (body == NULL)
    {
        h.u32(0xB0D1E5u);
        return;
    }
    // Diagnostic: a body that no longer belongs to the physics object pointing at it.
    static const bool check = std::getenv("STRIKERS_ROLLBACK_CHECK") != NULL;
    if (check && (!PortSimHeapOwns(body) || dBodyGetData(body) != (const void*)owner))
    {
        std::fprintf(
            stderr,
            "[rollback] %s physics %p (heap %d) has body %p (heap %d) owned by %p, tick %u%s\n",
            what, (const void*)owner, PortSimHeapOwns(owner), (const void*)body,
            PortSimHeapOwns(body), PortSimHeapOwns(body) ? dBodyGetData(body) : NULL, PortSimTick(),
            PortRollbackResimulating() ? " again" : "");
        h.u32(0xBADB0D1u);
        return;
    }
    h.fs(dBodyGetPosition(body), 3);
    h.fs(dBodyGetQuaternion(body), 4);
    h.fs(dBodyGetLinearVel(body), 3);
    h.fs(dBodyGetAngularVel(body), 3);
}

uint64_t hash_ball(const cBall* b)
{
    if (b == NULL)
        return 0;
    Hasher h;
    HU(h, b->m_bBallPathChangeCount);
    HU(h, b->m_bBallDeflectCount);
    HT(h, b->m_tShotTimer);
    HT(h, b->m_tNoPickupTimer);
    HT(h, b->m_tPassTargetTimer);
    HT(h, b->m_tBuzzerBeaterTimer);
    HF(h, b->m_fTotalPassTime);
    HI(h, char_index(b->m_pOwner));
    HI(h, char_index(b->m_pPrevOwner));
    HI(h, char_index(b->m_pLastTouch));
    HI(h, char_index(b->m_pPassTarget));
    HI(h, char_index(b->m_pShooter));
    HI(h, char_index(b->mpDamageTarget));
    HV(h, b->m_v3Position);
    HV(h, b->m_v3PrevPosition);
    HV(h, b->m_v3Velocity);
    HV(h, b->m_v3PassIntercept);
    HQ(h, b->m_qOrientation);
    HV(h, b->m_v3ShotTarget);
    HV(h, b->m_v3ShotOrigin);
    HU(h, b->m_uGoalType);
    HB(h, b->mbIsPerfectShot);
    HB(h, b->mbHyperSTS);
    HB(h, b->mbCanDamage);
    HB(h, b->m_unk_0xA3);
    HB(h, b->m_unk_0xA4);
    HB(h, b->m_unk_0xA5);
    HB(h, b->m_unk_0xA6);
    // Spin lives only in the ODE body.
    hash_body(h, b->m_pPhysicsBall != NULL ? b->m_pPhysicsBall->m_bodyID : NULL, b->m_pPhysicsBall,
              "ball");
    return h.done();
}

void hash_fielder(Hasher& h, const cFielder* f)
{
    HB(h, f->m_bHasBeenUpdated);
    HI(h, f->m_eActionState);
    HF(h, f->m_fShootToScoreActiveTime);
    HT(h, f->m_tFrozenTimer);
    HF(h, f->mActionElectrocutionVars.electrocutionTime);
    HF(h, f->mActionOneTimerVars.fOneTimerAnimTime);
    HI(h, f->mActionSlideAttackVars.eSlideAttackState);
    HB(h, f->mActionSlideAttackVars.bAttackSucceeded);
    HF(h, f->mActionShootToScoreVars.fFrameButtonDownTime2);
    HF(h, f->mActionShootToScoreVars.fFrameButtonDownTime1);
    HF(h, f->mActionShootToScoreVars.fGreenRegionWidth);
    HF(h, f->mActionShootToScoreVars.fShootToScoreActiveTime);
    HF(h, f->mActionShootToScoreVars.fMeterFractionTime);
    HB(h, f->mActionShootToScoreVars.bShootWasPressed);
    HB(h, f->mActionShootToScoreVars.isCaptainSts);
    HB(h, f->mActionShootToScoreVars.isCurrentlyInvincible);
    HB(h, f->mActionShootToScoreVars.isInUnbreakablePart);
    HF(h, f->mActionShootToScoreVars.fCaptainYellowWidth);
    HI(h, f->m_eFielderDesireState);
    HI(h, f->m_ePrevFielderDesireState);
    HI(h, f->m_eDesireSubState);
    HT(h, f->m_tDesireDuration);
    HF(h, f->m_fDesireConfidence);
    HT(h, f->m_DesireCommonVars.tMiscTimer);
    HT(h, f->m_DesireCommonVars.tAge);
    HF(h, f->m_DesireCommonVars.fMisc);
    HV(h, f->m_DesireCommonVars.v3DesiredPosition);
    HT(h, f->mtKickOffWaitTimer);
    HT(h, f->m_tPowerupEffectTime);
    HI(h, f->m_ePowerup);
    HI(h, f->mnNumPowerups);
    HI(h, char_index(f->m_pPowerupTarget));
    HI(h, f->m_nPowerupAnimID);
    HT(h, f->mtBombImpactTime);
    HV(h, f->mv3BombImpactLocation);
    HF(h, f->mfBombImpactRadius);
    HI(h, f->m_ePenaltyCardStatus);
    HI(h, char_index(f->m_pMark));
    HI(h, char_index(f->m_pMarker));
    HI(h, f->m_eRole);
    HV(h, f->m_v3DesiredPosition);
    HF(h, f->m_fDistanceToDesiredPosition);
    HV(h, f->m_v3AccumDesiredPos);
    HF(h, f->m_fAccumDesiredPosWeight);
    HI(h, f->meS2SResult);
    HB(h, f->mbCanKickoff);
    HB(h, f->mbWasHitByPowerupThisFrame);
    HB(h, f->mbCaptShootToScoreEffectOn);
    const ShotMeter* m = f->m_pShotMeter;
    if (m != NULL)
    {
        HI(h, m->m_eShotMeterState);
        HF(h, m->m_fTime);
        HF(h, m->m_fScoreValue);
        HF(h, m->m_fSpeedValue);
        HF(h, m->m_fSTSValue);
        HF(h, m->mfSShotAimValue);
    }
}

void hash_goalie(Hasher& h, const Goalie* g)
{
    HI(h, g->mGoalieActionState);
    HI(h, g->mPrevGoalieActionState);
    HI(h, g->mUrgency);
    HI(h, char_index(g->mpPassTarget));
    HI(h, char_index(g->mpShooter));
    HI(h, g->mnSubstate);
    HI(h, g->mMoveDirection);
    HI(h, g->mCrouchType);
    HF(h, g->mfSwitchTime);
    HU(h, g->muSaveType);
    HF(h, g->mfWaitTime);
    HF(h, g->mfTimeTilSave);
    HB(h, g->mbPlayMiss);
    HB(h, g->mbShouldMiss);
    HB(h, g->mbStunEffectActive);
    HB(h, g->mbDoIntercept);
    HB(h, g->mbDoNavigate);
    HB(h, g->mbDoHeadTrack);
    HB(h, g->mbBallImpacted);
    HB(h, g->mbNoUserControl);
    HB(h, g->mbIsPosed);
    HB(h, g->mbPickedUp);
    HV(h, g->mv3LocalContactPosition);
    HV(h, g->mv3LocalContactVelocity);
    HV(h, g->mv3TargetPosition);
    HV(h, g->mv3NavTarget);
    HV(h, g->mv3LocalNavTarget);
    HI(h, g->maLocalAngle);
    HF(h, g->mfTargetTime);
    HF(h, g->mfTargetDist);
    HF(h, g->mfSpeedScale);
    HU(h, g->muBallChangeCount);
    HU(h, g->muBallDeflectCount);
    HF(h, g->mFatigue.mfEnergyLevel);
    HF(h, g->mFatigue.mfRecoverRate);
    HF(h, g->mFatigue.mfTimeSinceLastSave);
    HF(h, g->mFatigue.mfHotStreakTimer);
    HI(h, g->mnOffplayPending);
}

uint64_t hash_character(int index)
{
    const cCharacter* c = g_pCharacters[index];
    if (c == NULL)
        return 0;
    Hasher h;
    HI(h, c->m_eClassType);
    HI(h, c->m_eCharacterClass);
    HV(h, c->m_v3Position);
    HV(h, c->m_v3PrevPosition);
    HV(h, c->m_v3Velocity);
    HI(h, c->m_eMovementState);
    HU(h, c->m_aDesiredFacingDirection);
    HU(h, c->m_aActualFacingDirection);
    HU(h, c->m_aPrevFacingDirection);
    HU(h, c->m_aDesiredMovementDirection);
    HU(h, c->m_aActualMovementDirection);
    HB(h, c->m_bFromAnimBlended);
    HF(h, c->m_fAnimAdjustBeginTime);
    HF(h, c->m_fAnimAdjustEndTime);
    HF(h, c->m_fDirectionSeekSpeed);
    HF(h, c->m_fDirectionSeekFalloff);
    HF(h, c->m_fAccel);
    HF(h, c->m_fDecel);
    HF(h, c->m_fDesiredSpeed);
    HF(h, c->m_fActualSpeed);
    HF(h, c->m_fLeanAmount);
    HI(h, c->m_nAnimTurnAdjust);
    HV(h, c->m_v3AnimMoveAdjust);
    HI(h, c->m_eAnimID);
    HF(h, c->m_pCurrentAnimController != NULL ? c->m_pCurrentAnimController->m_fTime : -1.0f);
    hash_body(h, c->m_pPhysicsCharacter != NULL ? c->m_pPhysicsCharacter->m_bodyID : NULL,
              c->m_pPhysicsCharacter, "character");

    if (c->m_eClassType == PLAYER || c->m_eClassType == FIELDER || c->m_eClassType == GOALIE)
    {
        const cPlayer* p = static_cast<const cPlayer*>(c);
        HI(h, p->m_ID);
        HB(h, p->m_bIsContactingWall);
        HI(h, p->m_ePositionSeekState);
        HV(h, p->m_v3AIPosition);
        HI(h, p->m_eBallRotationMode);
        HB(h, p->m_ResetBaseBallOrientation);
        HQ(h, p->m_BaseBallOrientation);
        HT(h, p->m_tBallPossessionTimer);
        HT(h, p->m_tBallUnPossessionTimer);
        HT(h, p->m_tNoPickupTimer);
        HT(h, p->m_tNoPickupPassInterceptTimer);
        HF(h, p->m_fShotStrengthTime);
        HT(h, p->m_tSlideAttackTimer);
        HT(h, p->m_tLooseBallPassTimer);
        HT(h, p->m_tInactivityTimer);
        HT(h, p->m_tSwapControllerTimer[0]);
        HT(h, p->m_tSwapControllerTimer[1]);
        HT(h, p->m_tSwapControllerTimer[2]);
        HT(h, p->m_tSwapControllerTimer[3]);
        HB(h, p->m_bCanTestController);
        HI(h, p->m_eLastPadAction);
        HU(h, p->m_aSwapFacingDirection);
        HT(h, p->m_tSwapFacingTimer);
        HF(h, p->m_UserControlledTime);
    }
    if (c->m_eClassType == FIELDER)
        hash_fielder(h, static_cast<const cFielder*>(c));
    else if (c->m_eClassType == GOALIE)
        hash_goalie(h, static_cast<const Goalie*>(c));
    return h.done();
}

void hash_clock(Hasher& h, const Clock* c)
{
    if (c == NULL)
    {
        h.u32(0xC10Cu);
        return;
    }
    HF(h, c->m_fTimeScale);
    HF(h, c->m_fTimer);
    HF(h, c->m_fEndTime);
    HU(h, c->m_uActiveStates);
    HI(h, c->m_clockState);
}

}   // namespace
extern "C" int PortSimHeapOwns(const void* p);
namespace
{
uint64_t hash_powerup(int slot)
{
    const PowerupBase* p = g_pPowerups[slot];
    Hasher h;
    HB(h, p->m_bShouldDestroy);
    HI(h, char_index(p->m_pTarget));
    HI(h, char_index(p->m_pThrower));
    HI(h, p->m_eType);
    HT(h, p->mtActiveTimer);
    HT(h, p->mtNoHitTimer);
    HU(h, p->m_aOrientation);
    HF(h, p->m_scale);
    HV(h, p->m_v3Position);
    HV(h, p->m_v3PrevPosition);
    HV(h, p->m_v3Velocity);
    HI(h, p->m_nIndex);
    HI(h, p->m_nThrowerPadID);
    HI(h, p->meSize);
    HB(h, p->mbExploder);
    hash_body(h, p->m_pPhysicsObject != NULL ? p->m_pPhysicsObject->m_bodyID : NULL,
              p->m_pPhysicsObject, "powerup");
    return h.done();
}

void hash_npc(Hasher& h, const SkinAnimatedMovableNPC* n)
{
    HV(h, n->mv3Position);
    HV(h, n->mv3Velocity);
    HU(h, n->maFacingDirection);
    HU(h, n->maDesiredFacingDirection);
    HF(h, n->mfDesiredSpeed);
}

}   // namespace

extern "C" int PortSimCharIndex(const void* character) { return char_index(character); }

extern "C" void PortSimHash(PortSimHashes* out)
{
    PortSimHashes& r = *out;
    const cGame* g = g_pGame;

    r.ball = hash_ball(g_pBall);

    for (int i = 0; i < 10; i++)
        r.perChar[i] = hash_character(i);
    {
        Hasher chr;
        for (int i = 0; i < 10; i++)
            HX(chr, r.perChar[i]);
        r.chr = chr.done();
    }

    {
        Hasher clk;
        if (g != NULL)
        {
            hash_clock(clk, g->m_pGameClock);
            hash_clock(clk, g->m_pPostResetClock);
            hash_clock(clk, g->m_pPostGameDoneClock);
        }
        r.clk = clk.done();
    }

    {
        Hasher score;
        if (g != NULL)
        {
            HB(score, g->m_bBallInNet);
            HI(score, g->m_nLastTeamToScore);
        }
        for (int t = 0; t < 2; t++)
        {
            if (g_pTeams[t] != NULL)
                HI(score, g_pTeams[t]->m_nScore);
        }
        r.score = score.done();
    }

    {
        Hasher game;
        if (g != NULL)
        {
            HF(game, g->m_fGameDuration);
            HI(game, g->m_eGameState);
            HI(game, char_index(g->m_pScorer));
            HI(game, char_index(g->m_pAssister));
            HI(game, char_index(g->m_pTeamTouch[0]));
            HI(game, char_index(g->m_pTeamTouch[1]));
            HB(game, g->mbCaptainShotToScoreOn);
            HB(game, g->mIsPure);
            HB(game, g->mInSuddenDeath);
            HT(game, g->mBowserTimer);
            HF(game, g->mfCheatTilt);
            HI(game, g->mThoughtsAllowedThisUpdate);
            if (g->m_pRandomPlayersArray != NULL)
            {
                for (int i = 0; i < 10; i++)
                    HI(game, char_index(g->m_pRandomPlayersArray[i]));
            }
        }
        for (int t = 0; t < 2; t++)
        {
            const cTeam* team = g_pTeams[t];
            if (team == NULL)
                continue;
            HI(game, team->mpCurrentSituation);
            HI(game, team->meCurrentTeamStyle);
            HT(game, team->mtTeamStyleTimer);
            HT(game, team->mtMarkTimer);
            HT(game, team->mtRoleTimer);
            HT(game, team->mtDefensiveZoneTimer);
            for (int i = 0; i < 4; i++)
            {
                HI(game, char_index(team->m_pAIOrderedFielders[i]));
                HF(game, team->mfBallInterceptTimes[i]);
            }
            HT(game, team->mtBallInterceptTimer);
        }
        r.game = game.done();
    }

    {
        uint64_t slots[25];
        for (int i = 0; i < 25; i++)
            slots[i] = g_pPowerups[i] != NULL ? hash_powerup(i) : 0;
        Hasher pwr;
        for (int t = 0; t < 2; t++)
        {
            const cTeam* team = g_pTeams[t];
            if (team == NULL)
                continue;
            HI(pwr, team->m_nCurrentPowerUp);
            // Without bIsNew: only the HUD reads it, and it clears it on render frames.
            HI(pwr, team->m_ePowerupList[0].eType);
            HI(pwr, team->m_ePowerupList[0].nnumOfPowerups);
            HI(pwr, team->m_ePowerupList[1].eType);
            HI(pwr, team->m_ePowerupList[1].nnumOfPowerups);
            HF(pwr, team->mfPowerupMeter);
            HF(pwr, team->mfPowerupTimer);
            HB(pwr, team->mbHasToggledPowerup);
        }
        for (int i = 0; i < 25; i++)
        {
            if (g_pPowerups[i] != NULL)
            {
                HI(pwr, i);
                HX(pwr, slots[i]);
            }
        }
        r.pwr = pwr.done();
    }

    {
        Hasher npc;
        BasicStadium* stadium = BasicStadium::GetCurrentStadium();
        const NPCManager* npcs = stadium != NULL ? stadium->mpNPCManager : NULL;
        if (npcs != NULL && npcs->mpBowser != NULL)
        {
            const Bowser* bw = npcs->mpBowser;
            hash_npc(npc, bw);
            HI(npc, char_index(bw->mpTarget));
            HV(npc, bw->mv3TargetPos);
            HI(npc, bw->meBowserState);
            HT(npc, bw->mtStateTimer);
            HT(npc, bw->mtActiveTimer);
            HI(npc, bw->mAnimID);
            HB(npc, bw->mbDoTilt);
            HI(npc, bw->mAttackType);
            HI(npc, bw->mStompStage);
            HB(npc, bw->mbFirstTime);
            HB(npc, bw->mbAlive);
            HB(npc, bw->mbResetPending);
        }
        if (npcs != NULL && npcs->mpChainChomp != NULL)
        {
            const ChainChomp* cc = npcs->mpChainChomp;
            hash_npc(npc, cc);
            HI(npc, char_index(cc->mpTarget));
            HI(npc, cc->meChainChompState);
            HT(npc, cc->mtStateTimer);
            HI(npc, char_index(cc->mpThrower));
            HI(npc, cc->mnThrowerPadID);
        }
        r.npc = npc.done();
    }

    {
        Hasher scr;
        for (int i = 0; i < 10; i++)
        {
            if (g_pCharacters[i] != NULL)
                HV(scr, g_pCharacters[i]->m_v3ScreenPosition);
        }
        r.scr = scr.done();
    }

    r.seed = nlDefaultSeed;
    {
        Hasher all;
        HX(all, r.ball);
        HX(all, r.chr);
        HX(all, r.clk);
        HX(all, r.score);
        HX(all, r.game);
        HX(all, r.pwr);
        HX(all, r.npc);
        HU(all, r.seed);
        r.all = all.done();
    }
}
