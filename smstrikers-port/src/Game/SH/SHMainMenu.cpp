#include "Game/SH/SHMainMenu.h"
#include "Game/GameInfo.h"
#include "Game/GameSceneManager.h"
#include "Game/Audio/WorldAudio.h"
#include "Game/FE/FEAudio.h"
#include "Game/FE/feFinder.h"
#include "Game/FE/feMusic.h"
#include "Game/FE/fePopupMenu.h"
#include "Game/FE/feScene.h"
#include "Game/FE/feAnimation.h"
#include "Game/FE/tlImageInstance.h"
#include "Game/SH/SHBackground.h"
#include "Game/main.h"
#include "port/fe_clone.h"
#include "port/input.h"
#include "port/overlay.h"
#include "NL/gl/glStruct.h"
#include "NL/nlLocalization.h"
#include "NL/nlPrint.h"

#include "NL/nlBind.h"

typedef Detail::MemFunImpl<void, void (SHMainMenu::*)(TLComponentInstance*)> MainMenuMemFun_t;
typedef BindExp2<void, MainMenuMemFun_t, SHMainMenu*, Placeholder<0> > MainMenuBind_t;

extern nlColour MenuHighliteColour;


bool SHMainMenu::mSnapMenuIntoPosition = false;
int SHMainMenu::mLastMenuItem = 0;

static char sSlideIn[] = "in";
static char sSlideOut[] = "out";

// PORT: one more for the Quit row, which reuses the Options description.
static unsigned long sUnlockedTickerMessages[SHMainMenu::NUM_ITEMS + 1] = {
    0x10B8D08F,
    0xDB24E3FA,
    0x0755A109,
    0x35AABB0D,
    0x1B176F3B,
    0x268EF6F5,
    0x7B1F3B7E,
    0x7B1F3B7E,
};

// PORT: one more for the Quit row, which reuses the Options description.
static unsigned long sLockedTickerMessages[SHMainMenu::NUM_ITEMS + 1] = {
    0x10B8D08F,
    0xDB24E3FA,
    0x0F4F37A4,
    0x35AABB0D,
    0x1B176F3B,
    0x268EF6F5,
    0x7B1F3B7E,
    0x7B1F3B7E,
};

/**
 * Offset/Address/Size: 0x1B14 | 0x800AB570 | size: 0x78
 */
static void onSelectFriendly(TLComponentInstance*)
{
    GameInfoManager::Instance()->SetMode(GameInfoManager::GM_FRIENDLY);
    GameInfoManager::Instance()->SetTeam(0, (eTeamID)3);
    GameInfoManager::Instance()->SetTeam(1, (eTeamID)3);
    GameSceneManager::Instance()->PopEntireStack();
    GameInfoManager::Instance()->ResetPlayingSides();
    GameSceneManager::Instance()->Push(SCENE_CHOOSE_CAPTAINS, SCREEN_FORWARD, false);
}

/**
 * Offset/Address/Size: 0x1AD0 | 0x800AB52C | size: 0x44
 */
static void onSelectCup(TLComponentInstance*)
{
    GameSceneManager::Instance()->PopEntireStack();
    GameSceneManager::Instance()->Push(SCENE_CUP_CHOOSE_CUP, SCREEN_FORWARD, false);
}

/**
 * Offset/Address/Size: 0x1A50 | 0x800AB4AC | size: 0x80
 */
static void onSelectSuperCup(TLComponentInstance*)
{
    if (!GameInfoManager::Instance()->IsSuperCupModeUnlocked())
    {
        FEPopupMenu* menu = (FEPopupMenu*)GameSceneManager::Instance()->Push(SCENE_POPUP_MENU, SCREEN_FORWARD, false);
        menu->Create(POPUP_SUPER_CUPS_LOCKED);
    }
    else
    {
        GameSceneManager::Instance()->PopEntireStack();
        GameSceneManager::Instance()->Push(SCENE_SUPER_CUP_CHOOSE_CUP, SCREEN_FORWARD, false);
    }
}

/**
 * Offset/Address/Size: 0x19C4 | 0x800AB420 | size: 0x8C
 */
static void onSelect101(TLComponentInstance*)
{
    GameInfoManager::Instance()->SetMode(GameInfoManager::GM_FRIENDLY);
    GameInfoManager::Instance()->SetTeam(0, (eTeamID)3);
    GameInfoManager::Instance()->SetTeam(1, (eTeamID)3);
    GameSceneManager::Instance()->PopEntireStack();
    GameInfoManager::Instance()->ResetPlayingSides();
    GameSceneManager::Instance()->Push(SCENE_CHOOSE_CAPTAINS, SCREEN_FORWARD, false);
    GameInfoManager::Instance()->mIsInStrikers101Mode = true;
    FEMusic::StartStreamIfDifferent(5);
}

/**
 * Offset/Address/Size: 0x1964 | 0x800AB3C0 | size: 0x60
 */
static void newTourn()
{
    GameSceneManager::Instance()->PopEntireStack();
    GameSceneManager::Instance()->Push(SCENE_TOURN_SETPARAMS, SCREEN_FORWARD, false);
    if (GameInfoManager::Instance()->mCustomTournamentInfo.m_cupConstructed)
    {
        GameInfoManager::Instance()->mCustomTournamentInfo.m_cup->mCupStarted = false;
    }
}

/**
 * Offset/Address/Size: 0x1914 | 0x800AB370 | size: 0x50
 */
static void continueTourn()
{
    GameInfoManager::Instance()->SetMode(GameInfoManager::GM_TOURNAMENT);
    GameSceneManager::Instance()->PopEntireStack();
    GameSceneManager::Instance()->Push(SCENE_TOURNAMENT_STANDINGS, SCREEN_FORWARD, false);
}

/**
 * Offset/Address/Size: 0x182C | 0x800AB288 | size: 0xE8
 */
static void confirmNewTourn()
{
    FEPopupMenu* menu = (FEPopupMenu*)nlSingleton<GameSceneManager>::Instance()->Push(SCENE_POPUP_MENU, SCREEN_NOTHING, false);
    menu->Create(
        POPUP_REALLY_OVERWRITE,
        Function<FnVoidVoid>(newTourn),
        Function<FnVoidVoid>(FEPopupMenu::Nothing));
    menu->mUnknownAA4 = false;
}

/**
 * Offset/Address/Size: 0x1680 | 0x800AB0DC | size: 0x1AC
 */
static void onSelectTournament(TLComponentInstance*)
{
    if (GameInfoManager::Instance()->mCustomTournamentInfo.m_cupConstructed
        && GameInfoManager::Instance()->mCustomTournamentInfo.m_cup->mCupStarted)
    {
        FEPopupMenu* menu = (FEPopupMenu*)nlSingleton<GameSceneManager>::Instance()->Push(SCENE_POPUP_MENU, SCREEN_NOTHING, false);
        menu->Create(
            POPUP_START_NEW_TOURNAMENT,
            Function<FnVoidVoid>(continueTourn),
            Function<FnVoidVoid>(confirmNewTourn));
        menu->SetBackButtonCallback(Function<FnVoidVoid>(FEPopupMenu::Nothing));
        GameInfoManager::Instance()->SetMode(GameInfoManager::GM_TOURNAMENT);
    }
    else
    {
        GameSceneManager::Instance()->PopEntireStack();
        GameSceneManager::Instance()->Push(SCENE_TOURN_SETPARAMS, SCREEN_FORWARD, false);

        if (GameInfoManager::Instance()->mCustomTournamentInfo.m_cupConstructed)
        {
            GameInfoManager::Instance()->mCustomTournamentInfo.m_cup->mCupStarted = false;
        }
    }
}

/**
 * Offset/Address/Size: 0x1634 | 0x800AB090 | size: 0x4C
 */
static void onSelectTrophies(TLComponentInstance*)
{
    GameSceneManager::Instance()->PopEntireStack();
    GameSceneManager::Instance()->Push(SCENE_TROPHY_ROOM, SCREEN_FORWARD, false);
    FEMusic::StartStreamIfDifferent(6);
}

/**
 * Offset/Address/Size: 0x15E8 | 0x800AB044 | size: 0x4C
 */
static void onSelectOptions(TLComponentInstance*)
{
    GameSceneManager::Instance()->PopEntireStack();
    GameSceneManager::Instance()->Push(SCENE_OPTIONS, SCREEN_FORWARD, false);
    FEMusic::StartStreamIfDifferent(7);
}

// PORT: the Quit row; a computer has no power button to end the game with.
static void onSelectQuit(TLComponentInstance*)
{
    PortRequestQuit();
}

// PORT: the disc's "QUIT", from the pause menu.
static const unsigned long kQuitLocStrId = 0xE37945FF;

// PORT: the disc's rows are 40 apart and fill the panel; eight at 35 fit above the ticker.
static const float kRowTop = 129.0f;
static const float kRowStep = 35.0f;

// PORT: everything on the slide expires at its end, so eight rows land closer than the disc's 0.15 s.
static const float kRowStagger = 0.15f * 6.0f / 7.0f;

static void ShiftRowAnimation(FEAnimation* anim, float y, float dt)
{
    v3AnimationKeyframe* head = (v3AnimationKeyframe*)anim->m_DLRingHead;
    v3AnimationKeyframe* k = head;
    while (k != NULL)
    {
        k->pKeyFrameDataX.m_fTime += dt;
        k->pKeyFrameDataY.m_fTime += dt;
        k->pKeyFrameDataZ.m_fTime += dt;
        k->pKeyFrameDataY.m_fPoint = y;
        if (k->pKeyFrameDataY.m_fControl1 != -1.0f)
            k->pKeyFrameDataY.m_fControl1 = y;
        if (k->pKeyFrameDataY.m_fControl2 != -1.0f)
            k->pKeyFrameDataY.m_fControl2 = y;
        k = k->m_next;
        if (k == head)
            break;
    }
}

static float KeyframeStart(FEAnimation* anim)
{
    v3AnimationKeyframe* first = nlDLRingGetStart<v3AnimationKeyframe>((v3AnimationKeyframe*)anim->m_DLRingHead);
    return first->pKeyFrameDataX.m_fTime;
}

// PORT: a clone shares its source's fly-in keyframes unless they are copied.
static void* CopyKeyframeRing(void* src)
{
    v3AnimationKeyframe* head = (v3AnimationKeyframe*)src;
    v3AnimationKeyframe* first = NULL;
    v3AnimationKeyframe* prev = NULL;
    v3AnimationKeyframe* k = head;
    while (k != NULL)
    {
        v3AnimationKeyframe* copy = (v3AnimationKeyframe*)PortFETrack(nlMalloc(sizeof(v3AnimationKeyframe), 8, false));
        if (copy == NULL)
            return NULL;
        *copy = *k;
        if (first == NULL)
            first = copy;
        else
        {
            prev->m_next = copy;
            copy->m_prev = prev;
        }
        prev = copy;
        k = k->m_next;
        if (k == head)
            break;
    }
    if (first != NULL)
    {
        prev->m_next = first;
        first->m_prev = prev;
    }
    return first;
}

static TLComponentInstance* AddQuitRow(TLSlide* slide, const char* const* names, int count)
{
    FEAnimation* rowAnim[SHMainMenu::NUM_ITEMS] = { NULL };
    TLInstance* row[SHMainMenu::NUM_ITEMS] = { NULL };

    for (int i = 0; i < count; i++)
    {
        row[i] = FEFinder<TLInstance, 4>::Find<TLSlide>(slide, InlineHasher(nlStringLowerHash("Layer")),
                                                        InlineHasher(nlStringLowerHash(names[i])));
        FEAnimation* anim = nlDLRingGetStart<FEAnimation>(slide->m_animations);
        while (anim != NULL)
        {
            if (anim->m_pTLInstanceTarget == row[i] && anim->m_type == eAnimPosition)
                rowAnim[i] = anim;
            if (nlDLRingIsEnd<FEAnimation>(slide->m_animations, anim))
                break;
            anim = anim->m_next;
        }
        if (row[i] == NULL || rowAnim[i] == NULL)
            return NULL;
    }

    TLInstance* last = row[count - 1];
    TLInstance* quit = PortFEClone(last, "MENU ITEM QUIT");
    if (quit == NULL || PortFECloneOverflowed())
        return NULL;

    void* keys = CopyKeyframeRing(rowAnim[count - 1]->m_DLRingHead);
    void* mem = PortFETrack(nlMalloc(sizeof(FEAnimation), 8, false));
    if (keys == NULL || mem == NULL)
        return NULL;
    FEAnimation* quitAnim = new (mem) FEAnimation();
    quitAnim->m_cast_type = rowAnim[count - 1]->m_cast_type;
    quitAnim->m_type = eAnimPosition;
    quitAnim->m_DLRingHead = keys;
    quitAnim->m_pTLInstanceTarget = quit;

    PortFEInsertAfter(last, quit);
    // PORT: not AddEnd; with this as the ring head the highlight and ticker stop drawing.
    nlDLRingAddStart(&slide->m_animations, quitAnim);

    const float firstStart = KeyframeStart(rowAnim[0]);
    for (int i = 0; i < count; i++)
    {
        const float want = firstStart + kRowStagger * i;
        ShiftRowAnimation(rowAnim[i], kRowTop - kRowStep * i, want - KeyframeStart(rowAnim[i]));
        row[i]->m_overloadedAttributes.v3Position.f.y = kRowTop - kRowStep * i;
    }
    ShiftRowAnimation(quitAnim, kRowTop - kRowStep * count,
                      firstStart + kRowStagger * count - KeyframeStart(quitAnim));
    quit->m_overloadedAttributes.v3Position.f.y = kRowTop - kRowStep * count;

    // PORT: the "in" and "out" slides each carry their own label.
    TLSlide* head = quit->m_component->pChildren;
    for (TLSlide* cs = head; cs != NULL;)
    {
        TLTextInstance* label = FEFinder<TLTextInstance, 3>::Find<TLSlide>(cs, InlineHasher(nlStringLowerHash("R JUST")));
        if (label != NULL)
        {
            label->m_LocStrId = kQuitLocStrId;
            label->m_OverloadFlags |= 8;
        }
        cs = cs->m_next;
        if (cs == head)
            break;
    }
    return (TLComponentInstance*)quit;
}

/**
 * Offset/Address/Size: 0x1538 | 0x800AAF94 | size: 0xB0
 */
SHMainMenu::SHMainMenu()
    : BaseSceneHandler()
    , m_itemDescriptions(NULL)
    , mMenuItems()
    , mButtons()
{
    mHighlightColour = MenuHighliteColour;
}

/**
 * Offset/Address/Size: 0x1380 | 0x800AADDC | size: 0x158
 */
SHMainMenu::~SHMainMenu()
{
    if (m_itemDescriptions != NULL)
    {
        delete m_itemDescriptions;
    }
    // PORT: the Quit row lives outside the package, which is freed after this.
    PortFEReleaseClones();
}

static BackgroundScene* GetBackgroundScene()
{
    BackgroundScene* bgscene = (BackgroundScene*)GameSceneManager::Instance()->GetScene(SCENE_MARIO_BACKGROUND);
    return bgscene;
}

/**
 * Offset/Address/Size: 0xA3C | 0x800AA498 | size: 0x8C0
 */
void SHMainMenu::SceneCreated()
{
    typedef MenuItem<TLComponentInstance>::Callback MenuCallback;
    static const char* MenuNameTable[] = { "MENU ITEM1", "MENU ITEM2", "MENU ITEM3", "MENU ITEM4", "MENU ITEM7", "MENU ITEM5", "MENU ITEM6" };
    static void (*const ApplyFuncTable[])(TLComponentInstance*) = { onSelectFriendly, onSelectCup, onSelectSuperCup, onSelectTournament, onSelect101, onSelectTrophies, onSelectOptions };
    FEMusic::StartStreamIfDifferent(0);
    FEPresentation* presentation = m_pFEScene->m_pFEPackage->GetPresentation();
    TLTextInstance* scrollText;
    const gl_ScreenInfo* screenInfo;
    char menuname[64];
    TLComponentInstance* buttons;
    MenuItem<TLComponentInstance>* item;
    BackgroundScene* scene;
    scrollText = FEFinder<TLTextInstance, 3>::Find<TLSlide>(
        presentation->m_currentSlide,
        InlineHasher(nlStringLowerHash("Layer")),
        InlineHasher(nlStringLowerHash("TickerText")));

    screenInfo = glGetScreenInfo();
    FEScrollText* scrollTextDisplay = new (nlMalloc(sizeof(FEScrollText), 8, false)) FEScrollText(scrollText, 0, screenInfo->ScreenWidth + 50);
    m_itemDescriptions = scrollTextDisplay;
    scene = GetBackgroundScene();
    scene->SetVisible(false);
    for (int i = 0; i < NUM_ITEMS; i++)
    {
        nlSNPrintf(menuname, 64, "MENU ITEM%d", i + 1);
        TLInstance* instance = FEFinder<TLInstance, 4>::Find<TLSlide>(
            presentation->m_currentSlide,
            InlineHasher(nlStringLowerHash("Layer")),
            InlineHasher(nlStringLowerHash(MenuNameTable[i])));
        TLComponentInstance* compinstance = (TLComponentInstance*)instance;
        item = mMenuItems.AddItem(compinstance);

        {
            MenuCallback openFunc(Bind<void>(MemFun<SHMainMenu, void, TLComponentInstance*>(&SHMainMenu::OpenItem), this, placeholder0));
            item->SetCallback(ON_HIGHLIGHT, openFunc);
        }

        {
            MenuCallback closeFunc(Bind<void>(MemFun<SHMainMenu, void, TLComponentInstance*>(&SHMainMenu::CloseItem), this, placeholder0));
            item->SetCallback(ON_UNHIGHLIGHT, closeFunc);
        }

        {
            MenuItem<TLComponentInstance>::Callback applyFunc(ApplyFuncTable[i]);
            item->SetCallback(ON_APPLY, applyFunc);
        }

        item->SetLockedFlag(false);
        if (i == mLastMenuItem)
        {
            OpenItem(compinstance);
            item->SetDisabledFlag(false);
        }
        else if (i == 2 && !g_e3_Build && !nlSingleton<GameInfoManager>::Instance()->IsSuperCupModeUnlocked())
        {
            CloseItem(compinstance);
            item->SetLockedFlag(true);
        }
        else
        {
            CloseItem(compinstance);
            if (g_e3_Build)
            {
                item->SetDisabledFlag(true);
            }
        }
        if (i == 2)
        {
            TLComponentInstance* lockedType = item->GetType();
            TLComponentInstance* lockedComp = FEFinder<TLComponentInstance, 4>::Find<TLSlide>(
                lockedType->GetActiveSlide(),
                InlineHasher(nlStringLowerHash("locked")),
                InlineHasher(0));
            u8 locked = item->IsLocked();
            lockedComp->m_bVisible = (bool)locked;
        }
    }
    // PORT: an eighth row to quit the game.
    if (TLComponentInstance* quit = AddQuitRow(presentation->m_currentSlide, MenuNameTable, NUM_ITEMS))
    {
        item = mMenuItems.AddItem(quit);
        MenuCallback openFunc(Bind<void>(MemFun<SHMainMenu, void, TLComponentInstance*>(&SHMainMenu::OpenItem), this, placeholder0));
        MenuCallback closeFunc(Bind<void>(MemFun<SHMainMenu, void, TLComponentInstance*>(&SHMainMenu::CloseItem), this, placeholder0));
        item->SetCallback(ON_HIGHLIGHT, openFunc);
        item->SetCallback(ON_UNHIGHLIGHT, closeFunc);
        item->SetCallback(ON_APPLY, MenuCallback(onSelectQuit));
        item->SetLockedFlag(false);
        if (mLastMenuItem == NUM_ITEMS)
            OpenItem(quit);
        else
            CloseItem(quit);
    }
    if (mLastMenuItem >= mMenuItems.GetNumItemsAdded())
        mLastMenuItem = 0;
    mMenuItems.SetItem(mLastMenuItem);
    mMenuItems.SetFlag(1);
    scene->SetVisible(true);
    scene->mDesiredPlayMode = PM_STOP_AT_END;
    buttons = FEFinder<TLComponentInstance, 4>::Find<TLSlide>(
        presentation->m_currentSlide,
        InlineHasher(nlStringLowerHash("Layer")),
        InlineHasher(nlStringLowerHash("buttons")));
    mButtons.mButtonInstance = buttons;
    mButtons.SetState(ButtonComponent::BS_A_AND_B);
    if (mSnapMenuIntoPosition)
    {
        FEAudio::EnableSounds(false);
        TLSlide* slide = presentation->m_currentSlide;
        BaseSceneHandler::Update(slide->m_start + slide->m_duration);
        FEAudio::EnableSounds(true);
    }
    mMenuItems.RunCallbackOnCurrent(ON_HIGHLIGHT);
    mSnapMenuIntoPosition = true;
}

/**
 * Offset/Address/Size: 0x60C | 0x800AA068 | size: 0x430
 */
void SHMainMenu::OpenItem(TLComponentInstance* compinstance)
{

    compinstance->SetActiveSlide(sSlideIn);
    compinstance->Update(0.0f);

    TLComponentInstance* highlight = FEFinder<TLComponentInstance, 4>::Find<TLSlide>(
        compinstance->GetActiveSlide(),
        InlineHasher(nlStringLowerHash("high")));

    highlight->SetActiveSlide(sSlideIn);
    highlight->Update(0.0f);

    TLComponentInstance* flash = FEFinder<TLComponentInstance, 4>::Find<TLSlide>(
        compinstance->GetActiveSlide(),
        InlineHasher(nlStringLowerHash("flasher")));

    flash->SetActiveSlide("Slide1");
    flash->Update(0.0f);

    FEFinder<TLImageInstance, 2>::Find<TLSlide>(
        highlight->GetActiveSlide(),
        InlineHasher(nlStringLowerHash("may_highlite")))
        ->SetAssetColour(mHighlightColour);

    if (mMenuItems.GetMenuItem()->IsDisabled())
    {

        TLTextInstance* text = FEFinder<TLTextInstance, 3>::Find<TLSlide>(
            compinstance->GetActiveSlide(),
            InlineHasher(nlStringLowerHash("R JUST")));

        text->m_LocStrId = 0x38202C30;
        text->m_OverloadFlags |= 8;
    }

    if (mMenuItems.GetMenuItem()->IsLocked())
    {

        TLTextInstance* text = FEFinder<TLTextInstance, 3>::Find<TLSlide>(
            compinstance->GetActiveSlide(),
            InlineHasher(nlStringLowerHash("R JUST")));

        if (g_pLocalization->m_CurrentLanguage == nlLocalization::LangJapanese)
        {
            text->m_LocStrId = 0x67452206;
            text->m_OverloadFlags |= 8;
        }
        else
        {
            text->m_LocStrId = 0x2A68AC55;
            text->m_OverloadFlags |= 8;
        }

        TLComponentInstance* lockedComp = FEFinder<TLComponentInstance, 4>::Find<TLSlide>(
            compinstance->GetActiveSlide(),
            InlineHasher(nlStringLowerHash("locked")));

        if (lockedComp != NULL)
        {
            lockedComp->m_bVisible = true;
        }
    }
    else
    {

        TLComponentInstance* lockedComp = FEFinder<TLComponentInstance, 4>::Find<TLSlide>(
            compinstance->GetActiveSlide(),
            InlineHasher(nlStringLowerHash("locked")));

        if (lockedComp != NULL)
        {
            lockedComp->m_bVisible = false;
        }
    }

    if (mMenuItems.GetMenuItem()->IsLocked())
    {
        m_itemDescriptions->SetDisplayMessage(sLockedTickerMessages[mMenuItems.GetActiveItemIndex()]);
    }
    else
    {
        m_itemDescriptions->SetDisplayMessage(sUnlockedTickerMessages[mMenuItems.GetActiveItemIndex()]);
    }

    BaseSceneHandler* scene = nlSingleton<GameSceneManager>::Instance()->GetScene(SCENE_MARIO_BACKGROUND);
    if (scene->m_bVisible)
    {
        FEAudio::PlayAnimAudioEvent("sfx_main_menu_highlight_open", false);
    }
}

/**
 * Offset/Address/Size: 0x404 | 0x800A9E60 | size: 0x208
 */
void SHMainMenu::CloseItem(TLComponentInstance* compinstance)
{

    compinstance->SetActiveSlide(sSlideOut);
    compinstance->Update(0.0f);

    TLComponentInstance* highlight = FEFinder<TLComponentInstance, 4>::Find<TLSlide>(
        compinstance->GetActiveSlide(),
        InlineHasher(nlStringLowerHash("high")));

    highlight->SetActiveSlide(sSlideOut);
    highlight->Update(0.0f);

    FEFinder<TLImageInstance, 2>::Find<TLSlide>(
        highlight->GetActiveSlide(),
        InlineHasher(nlStringLowerHash("may_highlite")))
        ->SetAssetColour(mHighlightColour);

    BaseSceneHandler* scene = nlSingleton<GameSceneManager>::Instance()->GetScene(SCENE_MARIO_BACKGROUND);
    if (scene->m_bVisible)
    {
        Audio::gWorldSFX.Stop((Audio::eWorldSFX)0xC, cGameSFX::SFX_STOP_FIRST);
        FEAudio::PlayAnimAudioEvent("sfx_main_menu_highlight_close", false);
    }

    TLComponentInstance* lockedComp = FEFinder<TLComponentInstance, 4>::Find<TLSlide>(
        compinstance->GetActiveSlide(),
        InlineHasher(nlStringLowerHash("locked")));

    if (lockedComp != NULL)
    {
        if (mMenuItems.GetMenuItem()->IsLocked())
        {
            lockedComp->m_bVisible = true;
        }
        else
        {
            lockedComp->m_bVisible = false;
        }
    }
}

/**
 * Offset/Address/Size: 0x0 | 0x800A9A5C | size: 0x404
 */
void SHMainMenu::Update(float fDeltaT)
{
    BaseSceneHandler::Update(fDeltaT);
    mButtons.CentreButtons();

    FEPresentation* presentation = m_pFEScene->m_pFEPackage->GetPresentation();
    TLSlide* slide = presentation->m_currentSlide;

    if (presentation->m_fadeDuration >= slide->m_start + slide->m_duration)
    {
        m_itemDescriptions->Update(fDeltaT);
    }
    else
    {
        return;
    }

    if (g_pFEInput->IsAutoPressed(FE_ALL_PADS, 0xE, true, NULL))
    {
        mMenuItems.NextItem();
        return;
    }

    if (g_pFEInput->IsAutoPressed(FE_ALL_PADS, 0xD, true, NULL))
    {
        mMenuItems.PreviousItem();
        return;
    }

    eFEINPUT_PAD padused;
    if (g_pFEInput->JustPressed(FE_ALL_PADS, 0x100, false, &padused))
    {
        if (mMenuItems.RunCallbackOnCurrent(ON_APPLY) == RES_OK)
        {
            GameInfoManager::Instance()->mMainUserPadNumber = padused;
            FEAudio::PlayAnimAudioEvent("sfx_accept", false);
            mLastMenuItem = mMenuItems.GetActiveItemIndex();
        }
        else
        {
            FEAudio::PlayAnimAudioEvent("sfx_deny", false);
        }

        return;
    }

    if (g_pFEInput->JustPressed(FE_ALL_PADS, 0x200, false, NULL))
    {
        GameSceneManager::Instance()->PopEntireStack();
        GameSceneManager::Instance()->Push(SCENE_TITLE, SCREEN_BACK, false);
    }

    // PORT: Escape does what the Quit row does.
    if (g_pFEInput->m_bInputAllowed && PortInputQuitPressed())
        PortRequestQuit();
}
