#include <Debug.h>
#include <kenshi/GameWorld.h>
#include <kenshi/SaveManager.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/InputHandler.h>
#include <kenshi/Character.h>
#include <kenshi/Dialogue.h>
#include <kenshi/CharacterHuman.h>
#include <kenshi/CharBody.h>
#include <kenshi/Tasker.h>
#include <kenshi/CharMovement.h>
#include <kenshi/util/UtilityT.h>       // traceNoActors — detached OTS camera collision
#include <kenshi/CharStats.h>
#include <kenshi/Damages.h>
#include <kenshi/Inventory.h>
#include <kenshi/Item.h>
#include <kenshi/Gear.h>
#include <kenshi/combat/CombatClass.h>
#include <kenshi/combat/CombatState.h>
#include <kenshi/combat/CombatTechniqueData.h>  // native attack / defence technique metadata
#include <kenshi/Animation/AnimationClass.h>
#include <kenshi/CameraClass.h>
#include <kenshi/Globals.h>
#include <kenshi/OptionsHolder.h>
#include <kenshi/Enums.h>
#include <kenshi/gui/ForgottenGUI.h>
#include <kenshi/gui/MainBarGUI.h>
#include <kenshi/gui/OrdersPanel.h>
#include <kenshi/gui/OptionsWindow.h>
#include <kenshi/gui/DatapanelGUI.h>
#include <kenshi/gui/DataPanelLine.h>
#include <ogre/OgreSceneNode.h>
#include <ogre/OgreSceneManager.h>
#include <ogre/OgreEntity.h>
#include <ogre/OgreSubEntity.h>         // FP head hidden-parts mask
#include <ogre/OgreMaterial.h>          // FP head hidden-parts mask
#include <ogre/OgreTechnique.h>         // FP head hidden-parts mask
#include <ogre/OgrePass.h>              // FP head hidden-parts mask
#include <ogre/OgreGpuProgramParams.h>  // FP head hidden-parts mask
#include <ogre/OgreOldSkeletonInstance.h>   // first-person head-bone tracking
#include <ogre/OgreOldBone.h>               // first-person head-bone tracking
#include <ogre/OgreAnimationState.h>         // actual clip timeline for manual attacks
#include <kenshi/Appearance.h>              // AppearanceBase (head/hair hide, skeleton)
#include <kenshi/Town.h>                    // TownBase::withinBordersRange (slave camp bounds)
#include <kenshi/Building/Building.h>      // Building::hasInterior (FP floor cutaway gate)
#include <core/Functions.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#define DIRECTINPUT_VERSION 0x0800
#define IRONWIND_VERSION "131"
#define DIRECT_CONTROL_BASE_VERSION "1.5.0"
#include <dinput.h>                          // first-person 1kHz raw mouse-look (KenshiFP method)
#include <mmsystem.h>                        // timeBeginPeriod (1ms Sleep granularity for the poll thread)
#pragma comment(lib, "winmm.lib")
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <limits.h>
#include <map>
#include <mygui/MyGUI.h>
#include <mygui/MyGUI_TextBox.h>       // "Manual Attack" HUD label
#include <mygui/MyGUI_ImageBox.h>      // directional HUD atlas
#include <mygui/MyGUI_EditBox.h>        // vanilla speech text = an EditBox in a bubble layout (the bars step over it)

// Minimal forward declaration for ManagementScreen (world map / faction / tech /
// squad window).  The full KenshiLib header <kenshi/gui/ManagementScreen.h> pulls
// in a decompiled ReorderableList template that fails to compile, so we declare
// only the two methods we call — they resolve by mangled name from KenshiLib.lib
// (same mechanism as SaveManager::getSingleton()).  Signatures MUST match the
// header exactly (public, non-const getVisible) or the linker won't find them.
class ManagementScreen
{
public:
    static ManagementScreen* getSingleton();   // RVA 0x2967F0
    bool getVisible();                          // RVA 0x48B3F0
};

// -----------------------------------------------------------------------
// Locomotion tuning config — adjust values here without changing logic.
// -----------------------------------------------------------------------
struct LocoConfig
{
    float     wasdAccelerationMultiplier;  // pre-charges currentMotion for faster ramp-up (1.0 = vanilla)
    float     wasdDecelerationMultiplier;  // force-zeros currentMotion on key release (1.0 = halt only)
    ULONGLONG wasdInputGraceMs;            // hold previous motion for N ms on key release before stopping
    float     wasdTurnResponsiveness;      // extra limit boost applied on significant direction change
    bool      normalizeDiagonalMovement;   // normalize W+A diagonal to cardinal speed (true = same speed)
    ULONGLONG wasdNudgeTapWindowMs;        // max hold duration (ms) to treat as a nudge tap (100–150 ms)
};

static const LocoConfig g_loco = {
    /* wasdAccelerationMultiplier */ 1.25f,
    /* wasdDecelerationMultiplier */ 1.35f,
    /* wasdInputGraceMs           */ 25,
    /* wasdTurnResponsiveness     */ 1.25f,
    /* normalizeDiagonalMovement  */ true,
    /* wasdNudgeTapWindowMs       */ 125,
};

// -----------------------------------------------------------------------
// WASD release-stop config — governs behavior when all WASD keys are released.
// -----------------------------------------------------------------------
struct ReleaseStopConfig
{
    bool      wasdStopOnRelease;                 // master gate — false disables the whole sequence
    float     wasdReleaseDecelerationMultiplier; // >1 forces currentMotion to zero after halt()
    ULONGLONG wasdReleaseGraceMs;                // ms to hold motion after release (0 = instant stop)
    bool      wasdAnchorSnapOnRelease;           // snap free-move anchor to current pos on release
    bool      wasdZeroVelocityOnRelease;         // zero injected velocity fields on release
};

static const ReleaseStopConfig g_release = {
    /* wasdStopOnRelease                 */ true,
    /* wasdReleaseDecelerationMultiplier */ 999.0f,
    /* wasdReleaseGraceMs               */ 0,
    /* wasdAnchorSnapOnRelease          */ true,
    /* wasdZeroVelocityOnRelease        */ true,
};

// -----------------------------------------------------------------------
// DC camera config — vertical focus offset for close-zoom chest framing.
// -----------------------------------------------------------------------
struct CameraConfig
{
    bool  dcCameraCloseZoomChestOffset; // gate — false disables the offset entirely
    float dcCameraFocusOffsetY;         // world-unit Y raise at max zoom-in (taper to 0 at medium/far)
};

static const CameraConfig g_dcCam = {
    /* dcCameraCloseZoomChestOffset */ true,
    /* dcCameraFocusOffsetY         */ 4.5f,   // navel -> upper chest ~45 cm (user req 2026-06-12)
};

// -----------------------------------------------------------------------
// Logging config — set debugLogging = true for in-game diagnostics.
// In a release build all three should remain false.
// -----------------------------------------------------------------------
struct LogConfig
{
    bool debugLogging;              // master gate — false silences all debug/verbose logs
    bool verboseMovementLogs;       // movement_injection_allowed, free_camera_input_suppressed_dc
    bool verboseCommittedActionLogs;// committed_action_true / committed_action_false detail
    bool debugVerbose;              // per-frame spam: committed_action_true, enemy_targeting_player,
                                    //   movement_injection_allowed
};

static const LogConfig g_log = {
    /* debugLogging              */ false,
    /* verboseMovementLogs       */ false,
    /* verboseCommittedActionLogs*/ false,
    /* debugVerbose              */ false,
};
// v1.5: the per-event combat / movement diagnostics that drove the 1.5 tuning
// (every hit, swing, native defence result, block, stagger, aim change, release stop, load
// flag ...) stay in the build behind ONE runtime switch so a player report can
// be diagnosed by flipping a setting - [Settings] VerboseLog (tab:
// TROUBLESHOOTING > "Detailed log for bug reports").  Off by default.
static bool s_verboseLog = false;
#define VerbLog(x) do { if (s_verboseLog) DebugLog(x); } while (0)

// -----------------------------------------------------------------------
// Performance profiling — per-second accumulation, emits dc_perf once/sec.
// All accumulators store raw QPC ticks; converted to ms at emit time.
// -----------------------------------------------------------------------
static bool     s_profInited          = false;
static LONGLONG s_profFreq            = 1;    // QPC ticks per second (cached)
static LONGLONG s_prof_mainLoop       = 0;
static LONGLONG s_prof_charMove       = 0;
static LONGLONG s_prof_playerControl  = 0;
static LONGLONG s_prof_committedAct   = 0;
static LONGLONG s_prof_threatScan     = 0;
static LONGLONG s_prof_cameraLock     = 0;
static LONGLONG s_prof_wasdInject     = 0;
static LONGLONG s_prof_combatTarget   = 0;
static ULONGLONG s_prof_windowStart   = 0;
static int      s_nearbyEnemyCount    = 0;    // all enemies in scan, updated in step 8
static int      s_chaseFlapsCount     = 0;    // combat_entered/exited transitions per perf window
static int      s_pathfindingEnemyCount = 0;  // enemies in TARGET_PATHFINDING* (path-recalc proxy)

static inline LONGLONG qpcNow()
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

// Lightweight RAII scope timer — accumulates QPC ticks into a named counter.
struct ScopeTimer
{
    LONGLONG  _start;
    LONGLONG& _accum;
    ScopeTimer(LONGLONG& a) : _start(qpcNow()), _accum(a) {}
    ~ScopeTimer() { _accum += qpcNow() - _start; }
};

// -----------------------------------------------------------------------
// Control modes
// -----------------------------------------------------------------------
enum ControlMode { MODE_VANILLA = 0, MODE_FREE_MOVE = 1 };

static volatile ControlMode s_mode   = MODE_VANILLA;
static volatile bool        s_wHeld  = false;
static volatile bool        s_aHeld  = false;
static volatile bool        s_sHeld  = false;
static volatile bool        s_dHeld  = false;
// SPEEDSYNC-TWEAK: set when vanilla cycle_run_speed fires while DC is active;
// mainLoop re-syncs the anchor's desired speed to the (already-cycled) panel.
static volatile bool        s_syncSpeedToAnchor = false;
// RMB press edge — set by the poll thread, consumed on the main thread.
// The earliest reliable player-click signal: pure input level, cannot be
// blocked by any game-side dispatch path.  Used only to release the
// post-WASD hold; the click itself is never touched.
static volatile bool        s_rmbPressedEdge = false;
static bool                 s_rmbPrev        = false;  // poll thread only
// LMB double-click detection (poll thread).  In DC mode a SINGLE left click must
// only SELECT a squad member (vanilla), never switch which character WASD drives;
// only a DOUBLE click reassigns control (user req 2026-06-20 — restores pre-1.1.0
// behaviour, needed for Sentient Sands compatibility).  s_lmbDoubleClickMs is the
// timestamp of the most recent detected double-click; the main-thread retarget
// consumes it within a short window.
static bool                 s_lmbPrev          = false;     // poll thread only
static ULONGLONG            s_lastLmbDownMs    = 0;         // poll thread only
static volatile ULONGLONG   s_lmbDoubleClickMs = 0;        // set by poll, consumed by main
// XHAIR-TWEAK (user req 2026-09-04): draw our own fixed white "+" crosshair
// and HIDE the game pointer while FP/OTS is engaged, instead of parking the
// visible OS cursor at center.  The look math (cursor warp / rotate) is
// unchanged — it just runs behind a hidden pointer, so V/F shows
// the crosshair and hides the cursor with no visible sweep across the screen.
// Two WhiteSkin bars on the "Top" MyGUI layer; created lazily on the UI
// thread; hidden (pointer restored) whenever a UI suspends the mode.
static MyGUI::Widget* s_xhH = nullptr;      // horizontal bar
static MyGUI::Widget* s_xhV = nullptr;      // vertical bar
static bool           s_xhTried = false;    // one creation attempt (skip if it throws)
static bool           s_xhPointerHidden = false;
static bool           s_xhWasWant = false;   // want last frame (return-to-center edge)
// XHAIR-INDICATOR (user req 2026-09-11): because the pick position is pinned to
// the crosshair, the game's own hover logic already selects the contextual
// cursor (attk/talk/door/lockpick/...) there — we just hide it.  While a
// look-mode is active and the pointer is contextual, UNHIDE the real cursor
// icon (it renders at the pinned centre) and hide the plain "+".
// HOW THE NAME IS TRACKED: PointerManager::eventChangeMousePointer fires ONLY
// for widget-driven pointer changes (notifyChangeMouseFocus) — Kenshi's world
// hover calls setPointer() directly, which never fires it (verified in-game
// 2026-09-11: event subscribed OK, zero callbacks while vanilla icons visibly
// changed).  So we hook the MyGUIEngine_x64.dll exports themselves —
// record-and-forward only, never alter the call.
static std::string    s_xhPtrName("arrow");
static volatile bool  s_allowTalkCursor = false; // game thread: reticle has player dialogue
static volatile int   s_xhairTint = 0;           // 0 white, 1 yellow neutral, 2 green ally
static void dcNotePointerName(const std::string& _name) { s_xhPtrName = _name; }
static bool dcPointerIsContextual()
{
    // Keep the V-mode reticle.  Only the talk bubble may replace it, and only
    // when the character under the crosshair has a player-talk package.
    return s_allowTalkCursor
        && (s_xhPtrName == "talk" || s_xhPtrName == "s_talk");
}
// Hook trio: public setPointer(name) = world hover; private setPointer(name,
// owner) = widget path (also reached from inside the public one when the DLL
// doesn't inline it — duplicate notes are harmless); resetToDefaultPointer =
// the "back to arrow" transition (Kenshi's default pointer IS "arrow").
typedef void(__fastcall* XhPtrSetFn)(void* self, const std::string& name);
typedef void(__fastcall* XhPtrSetWFn)(void* self, const std::string& name, void* widget);
typedef void(__fastcall* XhPtrResetFn)(void* self);
static XhPtrSetFn   s_xhSetPtrOrig  = nullptr;
static XhPtrSetWFn  s_xhSetPtrWOrig = nullptr;
static XhPtrResetFn s_xhResetOrig   = nullptr;
static void __fastcall xhSetPointer_hook(void* self, const std::string& name)
{
    s_xhSetPtrOrig(self, name);
    dcNotePointerName(name);
}
static void __fastcall xhSetPointerW_hook(void* self, const std::string& name, void* widget)
{
    s_xhSetPtrWOrig(self, name, widget);
    dcNotePointerName(name);
}
static void __fastcall xhResetPointer_hook(void* self)
{
    s_xhResetOrig(self);
    dcNotePointerName("arrow");
}
static ULONGLONG s_invFaceRetrackUntilMs = 0; // post-face-cam follow WATCHDOG
static ULONGLONG s_invFaceRetrackLastMs  = 0; // window: the re-track at inventory
                                         // close gets stomped by the vanilla close
                                         // handling and even one deferred call
                                         // didn't stick (field 2026-09-02: |HARA|
                                         // follow point frozen mid-air while the
                                         // order-driven anchor walked away).  For
                                         // a few seconds after exit, MEASURE the
                                         // orbit-pivot-to-anchor distance and
                                         // re-track only while it's visibly broken.
static float s_otsCamDistApplied = -1.0f; // OtsDistance base tracker: re-seed the
                                          // session zoom when the slider changes.

// =====================================================================
//  DEDICATED DETACHED OTS CAMERA (2026-09-06 rebuild).  OTS now owns its
//  own camera exactly like FP owns s_fpNode — it does NOT ride the engine's
//  key->rotate.  That means WE own the cursor end-to-end (hide + center, no
//  engine cursor-sharing → the intermittent "stick" is impossible).  Cost:
//  a detached camera has no built-in collision, so otsCamDrive raycasts
//  (UtilityT::traceNoActors) to keep it from clipping walls/floors — M2.
//  Look reuses the FP raw-mouse infrastructure (only one mode is ever
//  active, so sharing the DirectInput accumulator is safe).
// =====================================================================
static bool             s_otsCamActive     = false;   // detached OTS engaged
static Ogre::SceneNode* s_otsNode          = nullptr; // our camera's own node
static float            s_otsYaw           = 0.0f;    // our look yaw (own, not engine)
static float            s_otsPitch         = 0.15f;   // our look pitch (+down)
static Ogre::Vector3    s_otsSavedCamPos;
static Ogre::Quaternion s_otsSavedCamOri;
static float            s_otsSavedFovR     = 0.0f;    // saved FOV (radians)
static float            s_otsSavedNearClip = 0.0f;
static bool             s_otsCamLocalsSaved = false;
static Ogre::Vector3    s_otsRootSm(0.0f, 0.0f, 0.0f);   // smoothed orbit root (ragdoll follow)
static bool             s_otsRootSmValid    = false;
static int              s_otsRootBlendFrames = 0;        // frames of easing after a ragdoll ends
static bool             s_otsHadAutoTrack  = false;
static bool             s_otsCursorCaptured = false;  // own mouse-look baseline
static float            s_otsZoomCur       = 20.0f;   // live zoom (base + wheel)
static float            s_otsCollDistSm    = 20.0f;   // collision-clamped distance (eased out, snaps in)
static const float      OTS_ZOOM_MAX       = 120.0f;
static const float      OTS_ZOOM_FP_ENTER  = 3.0f;
static const float      OTS_ZOOM_FP_EXIT   = 6.0f;
static bool             s_otsResumeZoomPending = false; // preserve wheel distance on FP -> OTS
// OTS rig coherence (v1.4.1): while the detached OTS camera owns the view the
// vanilla rig is parked (stopFollowing at enter) — but the AUDIO LISTENER and
// zone/foliage streaming key off that rig, so area sounds kept playing from
// wherever it was left (user reports 2026-09; F/SelectControl "fixed" it by
// re-tracking).  Mirror FP's throttled teleport (fpDriveFrame), anchored to
// the character ROOT.
static Ogre::Vector3    s_otsLastStreamPos = Ogre::Vector3::ZERO;
static bool             s_otsLastStreamValid = false;
static const float      OTS_PITCH_LIMIT    = 1.30f;   // ~74 deg up/down clamp
static float            s_otsFovDeg        = 60.0f;   // OTS field of view (3rd-person)
static float            s_otsShoulderSign  = 1.0f;    // +1 = right shoulder, -1 = left (SwapShoulder key)
static void exitOtsCam(bool restoreCamera);           // fwd decl (enterFirstPerson uses it)
// [Settings] OTS tunables (INI + settings-tab sliders; live).
static float s_otsCamDist   = 23.0f;  // OtsDistance — orbit zoom while engaged
static float s_otsCamHeight = 18.0f;  // OtsHeight — pivot height above the feet;
                                      // ~15 ≈ shoulder line, height-scaled per
                                      // character (user-tuned default 2026-09-06;
                                      // slider range -5..20)
static float s_otsNearClipMin = 0.15f; // OtsNearClip — near plane once the camera
                                      // is pulled all the way in (wall hug /
                                      // full wheel zoom); blends back to the
                                      // game's own near clip with distance so
                                      // the close cam stops slicing the
                                      // character open (user 2026-09-11).
static float s_otsCamSide   = 4.0f;   // OtsSide — shoulder offset (+ = right, - =
                                      // left; flipped live by the SwapShoulder key)
// Set by cameraUpdate_hook (player camera) when any UI wants the cursor. This is
// a generic camera/input gate, not the removed legacy Ctrl camera toggle.
static volatile bool        s_cameraUiOpen  = false;
// Poll thread: set on first WASD key press; consumed and cleared on full WASD release.
static volatile ULONGLONG   s_wasdTapStartMs = 0;
// SelectControl ("G" by default since v1.5, INI-configurable) press edge.  While DC is active,
// this hands WASD control to the currently selected (portrait-highlighted) character
// — an alternative to the double-click-portrait switch, so a plain portrait
// single-click is not the only way to change who WASD drives (user req 2026-06-28).
// Set by handleSelectPress on the down-edge (poll/native path); consumed in the main
// loop.  Vanilla F still re-centers the camera; the retarget centers on the new anchor.
static volatile bool        s_fSelectEdge    = false;
// Authoritative DC intent — set only by V-key; cleared only by V-key or hard shutdown.
static volatile bool        s_userWantsDC         = false;
// Hard-shutdown guard — set true the moment LOADGAME teardown is detected.
// All hooks check this and return immediately (calling orig only) while it is set.
// Cleared only when all six stabilization conditions are confirmed.
static volatile bool        s_dcShutdownInProgress = false;
// Per-hook-type blocked-log flags — each fires once per load event, reset in clearAllState.
static bool s_hookBlockLoggedMain    = false;
static bool s_hookBlockLoggedCharMov = false;
static bool s_hookBlockLoggedPCtrl   = false;
static bool s_hookBlockLoggedRemJob  = false;
static bool s_hookBlockLoggedAddJob  = false;
// Throttle for dc_loadgame_waiting_for_safe_reacquire — once per second.
static ULONGLONG s_shutdownWaitLogTick = 0;

// NPC loot UI suspension — blocks all V-Mode input and WASD injection while looting.
// s_mode is NOT changed; the suspend is a transparent pause that restores automatically.
static volatile bool   s_lootUiSuspendActive    = false;
static bool            s_lootUiWasPrevOpen      = false;
static ULONGLONG       s_lootSuspendStartTick   = 0;
// TRADE/LOOT classification for the inventory face-cam.  Set when
// showTradeWindow fires (a foreign party — shop/loot/corpse), cleared when ALL
// inventory windows close.  Edge-latched on purpose: the gui trade HAND fields
// (inventoryWindowTrader/NPC/tradeA/tradeB) stay STALE after a trade closes, so
// reading them directly mis-classifies the next OWN inventory as a trade.  Own
// inventory = (live window count >= 1) AND !s_tradeWindowActive — this also
// covers a character with a BACKPACK, whose own inventory opens as TWO windows
// (field 2026-06-17: cnt=2 char=1 with all trade fields 0) and used to be
// wrongly rejected by the old getNumOpenInventoryWindows()==1 test.
static bool            s_tradeWindowActive      = false;
static const ULONGLONG LOOT_SUSPEND_DEBOUNCE_MS = 250;

// Inventory "move-through" mode (user opt-in 2026-06-28 via InventoryFaceCam=false):
// instead of suspending DC while an inventory window is open, keep DC movement +
// camera lock LIVE and keep the game running so the player can WASD around while
// looting/trading.  Walking out of range lets the game close the window naturally.
// Dialogue still pauses (handled separately, never overridden).
// s_invMoveThroughActive is latched on the open edge so the close edge knows which
// path (suspend vs move-through) was taken; s_invMoveThroughForcedRun records that
// we forced the game to keep running so we only touch pause when we caused it.
static bool            s_invMoveThroughActive      = false;
static bool            s_invMoveThroughForcedRun   = false;
// Manual pause during move-through (user req 2026-08-05): Kenshi auto-pauses at
// inventory OPEN and again when the shown inventory SWITCHES to another squad
// member; only those auto-pauses are defeated (grace window after each edge).
// A pause appearing outside the grace is the PLAYER pausing — latched and
// respected (incl. across inventory switches) until they unpause themselves.
// s_invMoveThroughShownChar is IDENTITY ONLY for switch-edge detection — never
// dereferenced (the character behind a closing window can be torn down).
static bool            s_invMoveThroughPlayerPaused = false;
static bool            s_invPausedBeforeOpen        = false;  // pause state sampled while NO
                                                              // inventory is open — the open-edge
                                                              // pre-existing-pause guard (2026-08-31)
static Character*      s_invMoveThroughShownChar    = nullptr;
static ULONGLONG       s_invMoveThroughEdgeTick     = 0;
// Covers one slow UI-transition frame (mainLoop spikes to ~500 ms there) so the
// auto-pause is still caught; a player pause faster than this after opening/
// switching is eaten once — pressing pause again sticks.
static const ULONGLONG INV_MT_AUTOPAUSE_GRACE_MS   = 600;
// Merchant trade does NOT auto-close on distance in vanilla (you normally can't
// walk while trading).  Corpse/own-inventory windows DO close natively, so this
// explicit close is scoped to the trader window only.  Latched so closeTradeWindow
// fires once per session.
static bool            s_invTradeCloseRequested    = false;
// "Walk away" is measured as distance MOVED from where the controlled character
// stood when the trade opened — NOT absolute distance to the merchant, which can
// already be large at open (e.g. trading across a bar counter) and would slam the
// window shut instantly.  s_invTradeAnchorStart is captured on the first trade
// frame (zero-initialized; guarded by s_invTradeStartValid).
static bool            s_invTradeStartValid        = false;
static Ogre::Vector3   s_invTradeAnchorStart;       // zero-init (static storage)
static const float     INV_TRADE_AUTOCLOSE_DIST    = 6.0f;   // units walked from open spot
static const float     INV_TRADE_AUTOCLOSE_DIST_SQ = INV_TRADE_AUTOCLOSE_DIST * INV_TRADE_AUTOCLOSE_DIST;

// Per-frame snapshot of volatile input state — set once at the top of mainLoop_hook,
// consumed by all per-character hooks that fire during s_mainLoopOrig.
// Eliminates per-character volatile reads (memory fence overhead) for hooks that
// fire 100+ times per frame in large enemy encounters.
static ControlMode s_frameMode        = MODE_VANILLA;
static bool        s_frameWasdHeld    = false;
static bool        s_frameLootSuspend = false;

// Melee/combat awareness range.  No separate middle zone.
static const float     ATTACK_RANGE    = 500.0f;
static const ULONGLONG SCAN_INTERVAL_MS = 5000; // squad-threat scan interval

static const char* modeName(ControlMode m)
{
    return m == MODE_FREE_MOVE ? "FREE_MOVE" : "VANILLA";
}

static void setMode(ControlMode next)
{
    ControlMode prev = s_mode;
    if (next == prev) return;
    s_mode = next;
    char buf[128];
    sprintf_s(buf, sizeof(buf), "[WASDCombat] MODE: %s -> %s", modeName(prev), modeName(next));
    DebugLog(buf);
}

// =======================================================================
// Keybind configuration (v1.6) — user-configurable, international-keyboard
// friendly.  Bindings are ROLE-based: the poll thread reads the virtual-key
// code assigned to each role from s_bindVk and acts on the role, never on a
// hardcoded key.  Loaded from WASDCombatPlugin.ini (next to the plugin DLL)
// at startup; a commented default file is created if missing; any invalid
// entry falls back to its default.  Nothing downstream of the s_*Held flags
// is touched — movement, hold, point-click, camera, combat, and XP logic
// are unchanged.
// =======================================================================
enum KeyRole { KR_FORWARD = 0, KR_LEFT, KR_BACK, KR_RIGHT, KR_TOGGLE, KR_SELECT, KR_FP, KR_SNEAK, KR_OTS_SHOULDER, KR_BLOCK, KR_ATTACK, KR_COUNT };
static int s_bindVk[KR_COUNT] = { 'W', 'A', 'S', 'D', 'V', 'G', 'F', 'C', 'H', 'Q', 'E' };  // v118: X/Z are exclusively vanilla speed_3/speed_1
static const char* const KR_INI_KEY[KR_COUNT] =
    { "MoveForward", "MoveLeft", "MoveBackward", "MoveRight", "ToggleDC", "SelectControl", "FirstPerson", "SneakToggle", "SwapShoulder", "Block", "Attack" };
static const char* const KR_DEFAULT[KR_COUNT] =
    { "W", "A", "S", "D", "V", "G", "F", "C", "H", "Q", "E" };
static char    s_bindCfgStr[KR_COUNT][32];  // resolved strings for the summary log
static int     s_uiBindVk[KR_COUNT] = { 'W', 'A', 'S', 'D', 'V', 'G', 'F', 'C', 'H', 'Q', 'E' };
                                            // options-tab dropdown mirrors of s_bindVk:
                                            // the UI edits these; dcOptionsWatch commits
                                            // changes to s_bindVk + INI.  Seeded from the
                                            // resolved binds at the end of loadKeybinds.
static HMODULE s_thisModule = nullptr;      // captured in DllMain for the INI path

// [Settings] feature toggles (separate INI section from [Keybinds]).
// InventoryFaceCam: when true (default) the camera swings to face the character
// while their own inventory is open.  Users who loot/disarm mid-fight disliked
// the cam grabbing focus, so it is both INI-toggleable AND auto-suppressed while
// the character is in combat (user req 2026-06-20).
static bool    s_settingInventoryFaceCam = true;
// WASD speed cap (user req 2026-07-25).  The injected setDirectMovement ran at an
// uncapped ~99 move-limit, so shackled / injured / encumbered characters moved at
// full speed (Rebirth leg-shackle escape) and enemies could not catch a WASD-moving
// player.  When on, cap the WASD move-limit at the character's real max run speed
// (CharStats::getMaxRunSpeed — injury/encumbrance-aware) with an extra hard clamp
// while chained/shackled.  WasdSpeedMult scales it (1.0 = exactly legit).  Cap off
// (WasdSpeedCap=false) restores the legacy uncapped behaviour.
static bool    s_settingWasdSpeedCap     = true;

// [Settings] EnemyPursuit (v1.3.2) — feed the anchor's REAL WASD velocity to the
// enemy combat AI so enemies sprint-chase and can attack a moving player (fixes
// the "enemies stall in combat stance, kite anything" exploit — Workshop report
// 2026-08-22).  false = pre-1.3.2 behaviour (AI sees the anchor as stationary).
// Full mechanism doc at the AI MOTION FEED block above charMovUpdate_hook.
static bool    s_settingEnemyPursuit     = true;

// =======================================================================
// MANUAL COMBAT: the player owns the guard request; the original game still
// owns the successful block branch (pose, sound, spark and XP).
//
//  DIRECTIONAL: while RMB remains held, a valid accepted request from the
//      defendable front arc blocks only the matching direction. Eligibility
//      begins on the accepted press, not when the visible pose reaches its
//      final frame: vanilla can also resolve HIT_SWORD while its block pose is
//      still entering. Character::hitByMeleeAttack receives HIT_SWORD and runs
//      the complete native block branch itself.
//  AUTOMATIC: RMB is permission only. Kenshi chooses the direction/technique
//      and may fail according to its original defence logic.
//  KEYBOARD FALLBACK: the old non-directional E/Q mode keeps its hold budget
//      and cooldown as INI-only compatibility tuning.
//  Never written: the combat state machine (2026-07-29 changeState crash).
//  Vanilla's toggle_block command is suppressed while DC is on so the same
//  key doesn't also flip the panel's block MODE underneath the hold.
// =======================================================================
static bool              s_settingManualBlock  = true;   // [Settings] ManualBlock
// In look mode, LMB hold chooses a cut and release commits the swing; RMB hold
// chooses a guard. Outside manual look mode the original mouse UI stays active.
static bool              s_directionalCombat   = true;   // [Settings] DirectionalCombat
static volatile bool     s_mouseBlockHeld      = false;
static volatile LONG     s_recentMouseX        = 0;
static volatile LONG     s_recentMouseY        = 0;
static volatile ULONGLONG s_recentMouseStartMs = 0;
static volatile ULONGLONG s_recentMouseLastMs  = 0;
static volatile LONG     s_combatGestureDirection = CUT_DEFAULT;
static volatile int      s_attackDesired       = CUT_DEFAULT;
static int               s_attackDesiredForSwing = CUT_DEFAULT;
static volatile int      s_blockDesired        = CUT_DEFAULT;
// Bannerlord-style directional input without a chamber animation: LMB enters
// an input-only aiming state, mouse movement may change the cut, and release
// commits exactly one swing. RMB cancels that pending swing and becomes guard.
static volatile bool     s_attackAimHeld       = false;
static volatile bool     s_attackAimCancelled  = false;
static volatile bool     s_attackAimGestureSeen = false; // this hold has a real mouse cut
static volatile bool     s_attackAimHoldLocked = false; // v38: a during-hold gesture fixed the direction
static volatile int      s_attackAimDirection  = CUT_DEFAULT;
static volatile ULONGLONG s_attackAimStartedMs = 0;
static volatile int      s_lastAttackDirection = CUT_DOWNWARD;
static volatile bool     s_blockGestureSeen    = false;  // guard direction came from the mouse
static volatile ULONGLONG s_rmbCombatEdgeMs    = 0;      // poll: RMB press, classified on the game thread
static ULONGLONG         s_rmbCombatEdgeSeenMs = 0;
// A real native hit reaction temporarily owns the body, but it must not turn a
// continuously held RMB into a release/re-press requirement. The poll thread
// captures one guard direction while the reaction is live; the game thread
// re-arms that same request only after STUMBLE/REACTION_BLOCK actually ends.
// Releasing RMB clears this latch immediately, so no stale press can survive.
static volatile bool     s_reactionGuardHeld = false;
static volatile LONG     s_reactionGuardDesired = CUT_DEFAULT;
static volatile bool     s_reactionGuardGestureSeen = false;
// Directional combat gives LMB/RMB exclusively to attack/guard. E targets one
// nearby original RootObject (character, door, furniture, machine, etc.). The
// native context menu owns the cursor and an Ironwind-owned pause only while
// the interaction UI chain is alive; MODE_FREE_MOVE never changes.
static volatile bool     s_interactionRequested = false;
static bool              s_interactionMenuByE = false;
static bool              s_interactionOpenPending = false;
static hand              s_interactionTargetHandle;
static hand              s_interactionPendingHandle;
static bool              s_interactionPauseOwned = false;
static ULONGLONG         s_interactionResumeAfterMs = 0;
static MyGUI::Widget*    s_interactionHintBg = nullptr;
static MyGUI::TextBox*   s_interactionHint = nullptr;
static bool              s_interactionHintTried = false;
static bool              s_interactionHintWasVisible = false;

// Defence mode inside manual combat. false = directional player guard;
// true = original Kenshi AI owns both block timing and direction. The two
// modes are exclusive: an RMB direction never biases automatic defence.
static bool              s_settingAiBlocks     = false;  // [Settings] AiBlocksInManual
static volatile bool     s_blockHeld           = false;  // poll thread -> main thread
static volatile ULONGLONG s_blockPressMs       = 0;      // poll thread: last press EDGE tick
static bool              s_blockHeldPrev       = false;  // main thread edge tracking
static bool              s_blockOrderApplied   = false;  // we turned DEFENSIVE_COMBAT on
static Character*        s_blockOrderChar      = nullptr;// identity only (who we applied it to)
static ULONGLONG         s_blockInjectedPressMs = 0;     // G press already turned into a block pose
// Automatic guard gets exactly one native chooseBlock roll per notified enemy
// swing.  Keeping the swing token after a failed roll prevents a held RMB from
// retrying every frame until it becomes a guaranteed block.
static Character*        s_autoBlockAttemptWho = nullptr;
static ULONGLONG         s_autoBlockAttemptStartMs = 0;
// Exact ownership token for the one native BLOCK selected by automatic guard.
// It remains valid only while the original BLOCK state is still playing; this
// lets the native state finish even after the incoming-attack edge has ended.
static CombatTechniqueData* s_autoBlockActiveTech = nullptr;
// Timed block (user 2026-09-18: no unlimited hold).  A press opens a block for
// at most BlockHoldMs (the vanilla order + pose), then it drops even if the key
// stays down, and BlockCooldownMs must pass before the next press counts.
static float             s_blockHoldMs         = 1200.0f;// [Settings] BlockHoldMs
static float             s_blockCooldownMs     = 800.0f; // [Settings] BlockCooldownMs
static ULONGLONG         s_blockAcceptedPressMs = 0;     // press that passed the cooldown gate
static ULONGLONG         s_blockSeenPressMs    = 0;      // main thread: last press edge processed
static ULONGLONG         s_blockCooldownUntilMs = 0;
// Input forgiveness (user 2026-09-21): an accidental repeat must never restart
// a move.  Extra presses of a combat key inside InputRepeatGuardMs of the last
// accepted press are ignored; a Block re-press that soon after letting go
// RESUMES the same block (no cooldown, hold budget + perfect window still run
// from the first press, pose kept); a swing that just ended is followed by a
// short recovery in which new presses are ignored (follow-through plays).
static float             s_inputRepeatGuardMs  = 250.0f; // [Settings] InputRepeatGuardMs
static ULONGLONG         s_blockHoldStartMs    = 0;      // origin of the hold budget + perfect window (survives a resume)
static ULONGLONG         s_blockReleaseMs      = 0;      // tick of the last release (resume window)

// Stagger policy (user 2026-09-21, final): a stagger plays through.  NO input
// counts while staggered - attack and block presses are DROPPED (never buffered) and
// anything pending dies when it starts; inputs count again the frame it ends.
static ULONGLONG         s_staggerStartMs      = 0;      // rising edge of the current stagger (0 = not staggered)
static ULONGLONG         s_staggerRawSinceMs   = 0;      // raw staggered signal edge (debounced into s_staggerStartMs)
static const ULONGLONG   STAGGER_DEBOUNCE_MS   = 100;    // filters 31-47 ms false reaction-state flickers seen in logs
// v44: hit ownership follows the native _getHit(..., stumble) decision and
// native combat states.  There is no gameplay-duration timer here: a true
// `stumble` may briefly leave STUMBLE/REACTION_BLOCK in nextMove before go()
// enters it, then the actual state owns the body until vanilla leaves it.
// The poll thread reads only the interlocked input-lock flag; all CombatClass
// state is inspected on the game thread.
static bool               s_hitReactionPending    = false;
static volatile LONG      s_hitReactionInputLock  = 0;
static unsigned int       s_hitReactionSerial     = 0;
// A third-party combat controller can let _getHit queue STARTUP -> STUMBLE
// without scheduling this character's CombatClass::go again. Track whether
// the queued native reaction already received its normal go call this world
// frame, so mainLoop may supply exactly the missing call and nothing else.
static unsigned int       s_hitReactionAdvancedFrameSerial = 0;
static GameWorld*        s_gwFrame             = nullptr;// refreshed every mainLoop frame (foe scan)

// --- Build 17: MANUAL ATTACK (press) -------------------------------------------
// Disassembly (1.0.65): the AI starts a swing by moving to STARTUP with
// nextMove = CHOP_WEAPON; CombatClassAI::startupState then re-asserts the
// attack target, and calls getStateClass(nextMove)->initialise() =
// AttackState::_NV_initialise -> initialiseAttack(target): nearest enemy in
// the attack zone, victim notified (attackingYou), CharStats::chooseAttack
// (skill/weapon-gated, native), the technique clip started, targets in the
// attack zone computed - then combatState =
// CHOP_WEAPON and the attack state runs the impact checks and damage.  If the
// target is out of reach the AI path would normally step in and retry.  Ironwind
// instead calls initialiseAttack(target) synchronously for one player press:
// success enters CHOP immediately, failure becomes an air swing, and no AI
// approach/retry task exists. Technique choice, animation, hit detection, XP
// and sounds remain vanilla.
static volatile bool     s_attackHeld          = false;
static volatile ULONGLONG s_attackPressMs      = 0;
static float             s_attackWindowMs      = 400.0f; // [Settings] AttackWindowMs (press must find the AI within this)
static ULONGLONG         s_attackCommitUntilMs = 0;      // press live: WASD buffers, AI runs
static ULONGLONG         s_attackSeenPressMs   = 0;
static ULONGLONG         s_attackInjectedPressMs = 0;
static ULONGLONG         s_attackAcceptedPressMs = 0;    // press that passed the gates (ignored presses never inject)
static ULONGLONG         s_attackEngagePressMs = 0;
static bool              s_attackActive        = false;  // our swing in flight (STARTUP -> CHOP_WEAPON)
static ULONGLONG         s_attackActiveSinceMs = 0;      // bounded: a swing that never starts is released
static bool              s_attackSawChop       = false;  // the swing actually started (CHOP_WEAPON seen)
static bool              s_attackObservedUnfinished = false; // this swing, not the previous technique, owns the finish edge
static bool              s_attackStateRecovered = false; // one live unfinished clip may reclaim CHOP after an external early exit
static bool              s_attackNativeImpactFinished = false; // native damage phase ended; let its real clip play the recovery frames
static bool              s_attackGuardHandoffPending = false; // intentional post-impact CHOP exit; never recover it as an accidental state steal
static bool              s_attackDirectInitialising = false; // exact scope: one player-requested native initializer
// A directly initialised CHOP has no combat task to keep CombatClassAI::go alive.
// Advance only the native AttackState execution object, at most once per game
// main-loop frame; never run the surrounding AI decision layer for our swing.
static unsigned int      s_manualMainFrameSerial = 1;
static unsigned int      s_attackAdvancedFrameSerial = 0;
static ULONGLONG         s_attackAdvanceLogMs = 0;
// Resolved base-class implementation, not a virtual call. In CHOP this runs
// CombatClass::attackState (animation sections / impact / finish) without
// entering CombatClassAI::update or its decision/movement layer.
static void              (*s_combatBaseUpdateNative)(CombatClass*, float) = nullptr;
static float             s_attackRecoveryMs    = 400.0f; // [Settings] AttackRecoveryMs (after a swing ends)
static ULONGLONG         s_attackRecoveryUntilMs = 0;
static volatile ULONGLONG s_attackQueuedAtMs   = 0;
static volatile int      s_attackQueuedDirection = CUT_DEFAULT;
static const ULONGLONG   ATTACK_QUEUE_MS       = 1000;
// v38 live per-direction attack availability for the aim HUD (game thread
// only). Replaces the old post-release "no matching move" 1200 ms lockout.
// v49 lets every direction produce a selected-direction arrow: confirmed
// available directions are green, unavailable or unchecked directions are
// white. Releasing on an unavailable direction still simply does not start a
// swing. Refreshed by
// manualRefreshDirMoveAvailability() plus ground truth from real commits.
static int               s_dirMoveAvailable[4] = { -1, -1, -1, -1 }; // -1 unknown, 0 none, 1 ok
static ULONGLONG         s_dirMoveCheckedMs    = 0;
static Character*        s_dirMoveAnchor       = nullptr;
static int               s_dirMoveWeaponType   = -2;
static int               s_dirMoveHands        = -1;
static int               s_attackNativeContacts = 0;
static int               s_attackNativeHits     = 0;
static int               s_attackNativeBlocks   = 0;
static int               s_attackNativeDodges   = 0;
static CombatTechniqueData* s_freeAttackTech = nullptr;
static float             s_freeAttackSpeed = 1.0f;
static ULONGLONG         s_freeAttackStartMs = 0;
static bool              s_freeAttackSeenAnim = false;
static float             s_freeAttackLastProgress = -1.0f;
static ULONGLONG         s_freeAttackLastAdvanceMs = 0;
static CombatTechniqueData* s_lastFreeAttackTech = nullptr;
static Character*        s_attackIntentTarget = nullptr; // deliberate aimed target for this press, validated before use
static Character*        s_attackAimCapturedTarget = nullptr; // target under reticle at LMB-down, before the direction stroke turns the camera
static Character*        s_attackQueuedTarget = nullptr; // captured target carried with the single buffered strike
static Character*        s_attackWorldNotified[8] = {}; // confirmed neutral victims notified once per authorised native swing
static Character*        s_attackNeutralContact[8] = {}; // neutral just before native contact can turn relations hostile
static Character*        s_attackNeutralIntentTarget = nullptr; // target was neutral at LMB-down; survives failed native initialise
// Victim-side retaliation is deliberately deferred until the current original
// game loop has returned.  notifyTheCampaignOfAnAttack/attackingYou can update
// faction and awareness state during AttackState initialisation; attackTarget
// in that same nested call stack can observe the old neutral relation and be
// rejected.  These slots contain identity only and are update-list validated
// before every dereference.
static Character*        s_victimResponseVictim[8] = {};
static Character*        s_victimResponseAttacker[8] = {};
static const char*       s_victimResponseSource[8] = {};
static ULONGLONG         s_freeAttackPressMs = 0;
static bool              s_freeAttackImpactPassed = false; // gates RMB recovery cancel; air swings never deal damage
static ULONGLONG         s_attackIntentPressMs = 0;
static ULONGLONG         s_attackAimCapturedStartMs = 0;
// --- MANUAL ATTACK mode (Z toggle) -----------------------------------------------------
// While ON, in DC: the combat AI's OWN swings are denied at the game's attack
// initialiser (and any stray transition into the attack state becomes the
// armed ready stance), AND its own defence choice is denied unless automatic
// guard has been explicitly requested - so the character takes no combat
// action by itself (stagger reactions stay vanilla); attack and block require
// player input, so the character can be hit if the player does nothing. Targeting stays vanilla
// (the AI's current target).  A small fixed "Manual Attack" label shows while ON.
static volatile bool     s_manualAttackOn      = false;
static ULONGLONG         s_manualSuppressLogMs = 0;
static ULONGLONG         s_manualPinLogMs      = 0;
static int               s_anchorPrevState     = -1;     // DIAG: the anchor's combat state last frame (which state started a refused swing?)
static MyGUI::TextBox*   s_manualHud           = nullptr;
static bool              s_manualHudTried      = false;
static MyGUI::ImageBox*  s_directionHud[4]     = { nullptr, nullptr, nullptr, nullptr };
static bool              s_directionHudTried   = false;
static bool              s_directionArrowTexTried = false;
static bool              s_directionArrowTexOk = false;
static MyGUI::TextBox*   s_hitMarkerHud[4]     = { nullptr, nullptr, nullptr, nullptr };
static bool              s_hitMarkerHudTried   = false;
static volatile ULONGLONG s_hitMarkerUntilMs   = 0;
static const ULONGLONG   HIT_MARKER_VISIBLE_MS = 1000;

// --- AIM FOCUS (user 2026-09-21; replaces the frozen lock-on) ----------------------
// While manual combat (Z) is on, the enemy you POINT AT is the combat AI's
// target, re-evaluated live: the enemy under the mouse cursor when the cursor
// is free (vanilla camera, or a UI open), else the enemy nearest the view
// centre (FP / OTS crosshair) inside the aim cone.  The camera is NEVER moved
// for it (the old lock eased the OTS yaw onto the target and fought the
// mouse).  A brief hold keeps the focus through a cursor slip.  The same pick
// is the foe for E / Q / G.  s_lockTarget = the current aim focus (identity
// only; validated against the update list before every use).
static Character*        s_lockTarget          = nullptr;
static float             s_aimConeDeg          = 35.0f;  // [Settings] AimConeDeg (crosshair pick)
static float             s_aimRange            = 40.0f;  // units
static float             s_aimHoverPx          = 70.0f;  // [Settings] AimHoverPx (cursor pick radius)
static ULONGLONG         s_aimEvalMs           = 0;      // 20 Hz evaluation throttle
static Character*        s_aimLogged           = nullptr;// edge log
// RIGHT-CLICK focus (user 2026-09-22): with manual combat on and the character
// in combat, a right-click on / toward an enemy at ANY distance makes them the
// focus instantly (the hover/crosshair pick only reaches AimRange).  A clicked
// focus persists until the target is gone, the next click, or the cursor is
// pointed straight at a different enemy; the crosshair cone does not override it.
static volatile ULONGLONG s_rmbAimPressMs      = 0;      // poll thread: RMB press edge
static ULONGLONG         s_rmbAimSeenMs        = 0;
static bool              s_aimClicked          = false;  // the current focus came from a click
static bool              s_settingAimName      = true;   // [Settings] AimNameTag
// Observation marker: the NPC's name above their head, in the game's own small
// text skin with a shadow, like the vanilla name tag.
static MyGUI::TextBox*   s_nameTag             = nullptr;
static bool              s_nameTagTried        = false;
static Character*        s_nameTagFor          = nullptr;// identity: whose name the caption holds
// Compact anatomy panel under the name (user 2026-10-22).  Two real rows:
// blood/head/chest/stomach, then left/right arms and left/right legs.  Every
// strip reads its own live MedicalSystem value; there is no synthetic total HP.
enum FocusHealthSlot
{
    HB_BLOOD = 0, HB_HEAD, HB_CHEST, HB_STOMACH,
    HB_LEFT_ARM, HB_RIGHT_ARM, HB_LEFT_LEG, HB_RIGHT_LEG,
    HB_COUNT
};
static MyGUI::Widget*    s_hbBg[HB_COUNT]      = {};
enum FocusHealthLayer
{
    HBL_HEALTHY = 0, HBL_ROBOT, HBL_STUN, HBL_CUT, HBL_BANDAGED,
    HBL_WEAR, HBL_SPLINT, HBL_COUNT
};
static MyGUI::Widget*    s_hbLayer[HB_COUNT][HBL_COUNT] = {};
static bool              s_hbTried             = false;
static bool              s_hbNamesLogged       = false;  // DIAG: the body-part names seen, once
static const int         HB_COLS = 4, HB_ROWS = 2;
// Readable but compact anatomy strips. Chinese two-character labels use a
// 14 px font and touch the larger bar directly. Cells still touch each other;
// compactness comes from zero spacing, never undersized text or bars.
static const int         HB_LABEL_W = 29, HB_BAR_W = 56, HB_CELL_W = 85;
static const int         HB_H = 10, HB_GAP = 0, HB_ROW_H = 15;
static const int         HB_PANEL_W = HB_COLS * HB_CELL_W + (HB_COLS - 1) * HB_GAP;
static MyGUI::TextBox*   s_hbLabel[HB_COUNT]   = {};
static const int         HB_LH = 15;
static const int         HB_RAISE = 42;                   // larger rows stay close to, but clear of, the head
// Vanilla SPEECH TEXT (user 2026-09-30): while the tagged enemy has a speech
// bubble up, the name+bars group steps ABOVE it by the bubble's real on-screen
// height and drops back when it is gone.  Build 66: bubbles are found by
// scanning MyGUI's root widgets for a visible EditBox with text near the head
// (MyGUI owns their lifetime).  Build 64 tracked DialogueSpeechBubble objects
// through ctor/dtor hooks, and a bubble deleted through a path the hook did not
// see left a dangling pointer read every frame - an access violation inside
// the game's main loop that another plugin's handler swallowed, so the rest of
// the game update was skipped each frame = "toggling Z pauses the game".
// NATIVE INCOMING-ATTACK TRACKER. Character::attackingYou is the game's own
// "I am swinging at you" notification. Automatic guard consumes this event
// stream; v121 removed the old hand/weapon glow renderer entirely.
static const int         INCOMING_ATTACK_MAX   = 4;      // attackers tracked at once
struct IncomingAttackSlot { Character* who; ULONGLONG startMs; ULONGLONG chopMs; ULONGLONG impactMs; };
// who is identity only and must be validated before every use.
static IncomingAttackSlot s_incomingAttack[INCOMING_ATTACK_MAX] = {};
static CombatTechniqueData* s_dcBlock[8];                  // mod-owned copies of the BLOCK techniques
static int               s_dcBlockCount        = 0;
static CombatTechniqueData* s_dcAttack[256];
static int               s_dcAttackCount       = 0;
static bool              s_dcAttackBuilt       = false;
static ULONGLONG         s_freeBlockPressMs    = 0;
static bool              s_freeGuardUpperIdle  = false;
static bool              s_freeGuardCombatOwned = false;
static CombatTechniqueData* s_freeBlockTech    = nullptr;
static float             s_freeBlockSpeed     = 1.0f;
static bool              s_freeAnimCombatMode  = false;
static ULONGLONG         s_weaponDrawAttemptMs = 0;
static bool              s_weaponMissingLogged = false;
static bool              s_weaponDrawInProgress = false;
static bool              s_dcBlockBuilt        = false;
// The native defence-animation START call used when automatic guard's original
// chooser returns a dodge technique. It is not exported by KenshiLib, so the
// 1.0.65 address is byte-verified before use. This is only a pass-through for
// Kenshi's own accepted defence; Ironwind has no player-authored dodge action.
// Signature (from the disassembly): rcx = AnimationClass*, rdx = technique,
// xmm2 = speed (blockSpeed * animSpeedMultiplier), r9 = std::string* suffix.
typedef void (*DcTechPlayFn)(void* animClass, CombatTechniqueData* tech, float speed, std::string* suffix);
static DcTechPlayFn      s_defencePlayFn       = nullptr;
static bool              s_defencePlayChecked  = false;
static const uintptr_t   DC_DEFENCE_PLAY_RVA   = 0x5B6D70;   // Kenshi 1.0.65 (Steam/GOG RE_Kenshi exe)

static float   s_settingWasdSpeedMult    = 1.0f;
// Combat-mode FLICKERS (isInCombatMode blips false between swings / when the
// target is momentarily not engaged).  A single false frame at inventory-open
// used to latch the face-cam on (then the 16-frame close-debounce kept it up
// through the fight).  So "in combat" is treated as a HARD exit (no debounce) and
// held for a short grace after the last in-combat frame to smooth the flicker and
// cover looting/disarming right as a fight ends.
static ULONGLONG s_lastInCombatMs = 0;
static const ULONGLONG FACECAM_COMBAT_GRACE_MS = 1500;

// Runtime-state ownership helpers.
//
// These deliberately contain no engine calls.  They are safe to use from load,
// V-mode and reaction teardown paths even when cached Character/Ogre pointers
// are no longer valid.  Keep direct writes to the grouped fields inside these
// helpers or inside the state machine that actively owns the field; this avoids
// the historical class of bugs where one exit path cleared two members of a
// three-member latch and left the next V session permanently queued/locked.
static void manualAttackClearCapturedTarget()
{
    s_attackAimCapturedTarget = nullptr;
    s_attackAimCapturedStartMs = 0;
}

static void manualAttackClearNeutralIntent()
{
    s_attackNeutralIntentTarget = nullptr;
}

static void manualAttackClearAimInput(bool cancelled, bool clearCapturedTarget)
{
    s_attackAimHeld = false;
    s_attackAimCancelled = cancelled;
    s_attackAimGestureSeen = false;
    s_attackAimHoldLocked = false;
    s_attackAimDirection = CUT_DEFAULT;
    s_attackAimStartedMs = 0;
    if (clearCapturedTarget)
    {
        manualAttackClearCapturedTarget();
    }
}

static void manualAttackClearQueuedInput(bool clearCapturedTarget)
{
    s_attackQueuedAtMs = 0;
    s_attackQueuedDirection = CUT_DEFAULT;
    s_attackQueuedTarget = nullptr;
    if (clearCapturedTarget)
    {
        manualAttackClearCapturedTarget();
    }
}

static void manualAttackClearActiveRuntime()
{
    s_attackActive = false;
    s_attackActiveSinceMs = 0;
    s_attackSawChop = false;
    s_attackObservedUnfinished = false;
    s_attackStateRecovered = false;
    s_attackNativeImpactFinished = false;
    s_attackGuardHandoffPending = false;
    s_attackDirectInitialising = false;
    s_attackAdvancedFrameSerial = 0;
    s_attackAdvanceLogMs = 0;
    s_attackNativeContacts = 0;
    s_attackNativeHits = 0;
    s_attackNativeBlocks = 0;
    s_attackNativeDodges = 0;
}

static void manualHitReactionClearRuntime()
{
    s_hitReactionPending = false;
    s_hitReactionAdvancedFrameSerial = 0;
    s_reactionGuardHeld = false;
    s_reactionGuardDesired = CUT_DEFAULT;
    s_reactionGuardGestureSeen = false;
    InterlockedExchange(&s_hitReactionInputLock, 0);
}

static void manualInteractionClearRuntime(bool forgetAutoTarget)
{
    s_interactionRequested = false;
    s_interactionMenuByE = false;
    s_interactionOpenPending = false;
    s_interactionPendingHandle.setNull();
    s_interactionPauseOwned = false;
    s_interactionResumeAfterMs = 0;
    if (forgetAutoTarget)
    {
        s_interactionTargetHandle.setNull();
    }
}

// Read-only end-of-frame contract check.  It never repairs state: silently
// guessing which owner should win would recreate the animation/AI regressions
// this pass is meant to prevent.  A non-zero mask is therefore durable evidence
// for the next log analysis, while edge/5-second throttling avoids log floods.
static void manualRuntimeInvariantTick()
{
    unsigned int mask = 0;
    if (s_attackActive && s_freeAttackTech) mask |= 0x01;
    if (s_attackActive && s_freeBlockTech) mask |= 0x02;
    if (s_attackQueuedTarget && s_attackQueuedAtMs == 0) mask |= 0x04;
    if (s_attackQueuedAtMs != 0 && s_attackQueuedDirection == CUT_DEFAULT) mask |= 0x08;
    if (s_attackDirectInitialising && !s_attackActive) mask |= 0x10;
    if (s_attackNativeImpactFinished && !s_attackActive) mask |= 0x20;

    static unsigned int lastMask = 0;
    static ULONGLONG lastLogMs = 0;
    ULONGLONG now = GetTickCount64();
    if (mask && (mask != lastMask || now - lastLogMs >= 5000))
    {
        char b[192];
        sprintf_s(b, sizeof(b),
            "[Ironwind] invariant_violation mask=0x%02X attack=%d free_attack=%d free_block=%d queued=%d queued_target=%d direct_init=%d impact_done=%d",
            mask, s_attackActive ? 1 : 0, s_freeAttackTech ? 1 : 0,
            s_freeBlockTech ? 1 : 0, s_attackQueuedAtMs ? 1 : 0,
            s_attackQueuedTarget ? 1 : 0, s_attackDirectInitialising ? 1 : 0,
            s_attackNativeImpactFinished ? 1 : 0);
        ErrorLog(b);
        lastLogMs = now;
    }
    lastMask = mask;
}

// =======================================================================
// Inventory face-cam — a temporary DETACHED Ogre camera (s_fpNode) that swings
// around to face the selected character's front while their OWN inventory is
// open, so worn gear is visible.  The gameplay over-the-shoulder ("OTS") toggle
// this plumbing was originally built for was SCRAPPED (Kenshi's interior floor
// render is welded to the top-down RTS camera, mutually exclusive with a
// detached view); only the inventory face-cam survives.  Runs in
// cameraUpdate_hook (works under the inventory pause).  Kenshi scale ≈ 10cm/unit.
// =======================================================================
static bool             s_fpActive          = false;
// Inventory face-cam exit debounce.  The camera update hook is called TWICE per
// frame and the two calls DISAGREE on the own-inventory window count (one sees
// 1, the other does not) — so a naive enter/exit toggled detach/re-attach EVERY
// frame (field 2026-06-16/17: 4349 dc_cam_entered/exited pairs while one
// inventory was open).  That left s_fpActive true only half the frames, which
// silently broke the altitude-hold (scroll still zoomed name-tags) and the
// s_fpActive-gated point-click suppression (clicks landing on an "exit" frame
// walked the character).  Fix: require the own-inventory signal to be ABSENT for
// several consecutive calls before exiting, so a single dissenting per-frame
// call can no longer drop the face-cam.
static int              s_invFaceCloseStreak = 0;
static const int        INV_FACE_CLOSE_DEBOUNCE = 16;
// World name-tag (the floating squad/character labels above heads) hide while
// the inventory face-cam is up.  Scrolling/WASD still nudged those labels and
// camera-input gating couldn't stop it cleanly, so instead we just hide them
// for the duration (user request 2026-06-17) and restore the player's setting
// (`options->showNames`) on close.  s_savedShowNames is read BEFORE hiding so a
// showNames() that also writes the option can't poison the restore.
static bool             s_namesHidden       = false;
static bool             s_savedShowNames    = true;
static bool             s_fpCursorCaptured  = false;
static float            s_fpSensitivity     = 1.0f;
static float            s_fpYaw             = 0.0f;   // radians; fwd=(-sin,0,-cos)
static float            s_fpPitch           = 0.0f;
static float            s_otsDistCur        = 14.0f;  // face-cam camera distance
static bool             s_otsInvFaceActive  = false;  // own-inventory face-cam engaged
static Character*       s_otsInvFaceChar    = nullptr; // who it is aimed at (identity-compared only)
// Shoulder view saved on the FIRST inventory open, restored on close so the
// player returns to exactly the over-the-shoulder framing they had before.
static float            s_otsSavedYaw       = 0.0f;
static float            s_otsSavedPitch     = 0.0f;
static float            s_otsSavedDist      = 14.0f;
static float            s_fpFovDeg          = 65.0f;
static float            s_fpNearClip        = 0.2f;
// Crosshair/cursor horizontal offset as a fraction of client width (+ = right
// of center) so the crosshair clears the character body for selection clicks
// (user req 2026-06-13).  The cursor is pinned to this point and mouse-look
// deltas are measured from it.
static float            s_otsCrosshairOffsetX = 0.10f;  // 0.12 -> 0.10 (closer to the character, user req 2026-06-16)
static float            s_fpSavedNearClip   = 0.0f;
static Ogre::Radian     s_fpSavedFov;
static bool             s_fpCamLocalsSaved  = false;
static Ogre::Vector3    s_fpSavedCamPos     = Ogre::Vector3::ZERO;
static Ogre::Quaternion s_fpSavedCamOri;
static Ogre::SceneNode* s_fpNode            = nullptr;
static bool             s_fpHadAutoTrack    = false;
// Set when a load/teardown interrupts OTS: the detached Ogre camera persists
// across a save-load, so it must be re-attached to the rig AFTER the world is
// valid again (doing it mid-load crashed; not doing it left the camera stuck
// orphaned at the OTS position — field 2026-06-13).
static bool             s_otsRestorePending = false;
static float            s_otsSavedAltitude  = 0.0f;  // face-cam: held to block scroll-zoom

// =======================================================================
// First-person camera (v1.8-fp, ported into the live line 2026-07-22).  A
// SECOND drive mode for the SAME detached-camera machinery as the inventory
// face-cam above — both detach the Ogre camera onto s_fpNode and save/restore
// the camera locals (s_fpSavedCamPos/Ori/Fov/NearClip, s_fpHadAutoTrack,
// s_fpCamLocalsSaved) the same way, and both drive in cameraUpdate_hook.  They
// differ ONLY in what triggers them and where the camera is placed each frame:
//   * s_fpActive          = the INVENTORY FACE-CAM owns the detached view.
//   * s_firstPersonActive = FIRST-PERSON owns it (eye at the head bone, look out).
// The two are mutually exclusive (never both true — the camera has one node).
// Opening the inventory while first-person is active SUSPENDS first-person for
// the face-cam and AUTO-RETURNS to first-person when the inventory closes
// (s_fpSuspendedForInv).  Toggled by the FirstPerson key (default P) in DC.
// =======================================================================
static bool  s_firstPersonActive   = false;  // detached cam is in first-person drive mode

// The HUD arrow describes blade travel toward the crosshair, not the side on
// which the mouse sits: moving right shows a left-pointing arrow and therefore
// selects CUT_LEFT (weapon starts on screen-right and travels left). Moving
// left selects CUT_RIGHT. Moving up selects overhead, down selects thrust.
// The raw device reports counts rather than pixels.  In field input those raw
// counts are not perceptually square: v88 produced 47 horizontal selections
// versus 25 vertical selections, and the old 1.35 axis hysteresis then kept the
// already-common horizontal direction sticky.  Classify in a lightly
// normalised space (Y * 1.25) and use only a narrow 1.15 diagonal hysteresis.
// The four outputs remain four angular sectors; the compensation makes a
// deliberate up/down stroke as easy to select as the same felt left/right
// stroke while still absorbing camera tremor at the diagonal.
static CutDirection combatDirectionFromDelta(LONG x, LONG y, CutDirection previous = CUT_DEFAULT)
{
    LONG ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    const LONGLONG sx = (LONGLONG)ax * 4;
    const LONGLONG sy = (LONGLONG)ay * 5;
    if (sx * sx + sy * sy < (12LL * 4) * (12LL * 4)) return previous;
    bool previousHorizontal = previous == CUT_LEFT || previous == CUT_RIGHT;
    bool previousVertical = previous == CUT_DOWNWARD || previous == CUT_THRUST;
    if (previousHorizontal && sy * 100 <= sx * 115)
        return x >= 0 ? CUT_LEFT : CUT_RIGHT;
    if (previousVertical && sx * 100 <= sy * 115)
        return y < 0 ? CUT_DOWNWARD : CUT_THRUST;
    if (sx > sy) return x > 0 ? CUT_LEFT : CUT_RIGHT;
    return y < 0 ? CUT_DOWNWARD : CUT_THRUST;
}

// Attack input stores the direction in which the blade travels: a mouse move
// to the right produces the right-side inward arrow but selects CUT_LEFT.
// Kenshi block techniques, on the other hand, name the side being guarded.
// Convert only the horizontal pair when the same gesture starts a block;
// overhead and thrust guards already use the same enum as their gestures.
// This function is its own inverse, so the HUD can recover the original
// gesture arrow while s_blockDesired remains the native guard/verdict value.
static CutDirection combatGuardDirectionFromGesture(CutDirection gesture)
{
    if (gesture == CUT_LEFT) return CUT_RIGHT;
    if (gesture == CUT_RIGHT) return CUT_LEFT;
    return gesture;
}

static CutDirection combatRecentDirection()
{
    // v106: selection and its arrow are one state.  Keep the last valid
    // direction for 1200 ms, while every new mouse stroke can replace it
    // immediately.  The timeout is not a lock; it only clears an idle choice.
    ULONGLONG last = s_recentMouseLastMs;
    if (!last || GetTickCount64() - last > 1200) return CUT_DEFAULT;
    return (CutDirection)InterlockedCompareExchange(&s_combatGestureDirection, CUT_DEFAULT, CUT_DEFAULT);
}

static bool mouseCombatContext()
{
    return s_directionalCombat && s_settingManualBlock && s_manualAttackOn && s_mode == MODE_FREE_MOVE
        && (s_firstPersonActive || s_otsCamActive) && !s_cameraUiOpen
        && !s_lootUiSuspendActive && !s_dcShutdownInProgress
        && !s_interactionMenuByE;
}

static bool combatDirectionMatches(CutDirection wanted, CutDirection incoming)
{
    if (wanted == CUT_DEFAULT) return false;
    if (incoming == CUT_REAR_LEFT) incoming = CUT_LEFT;
    else if (incoming == CUT_REAR_RIGHT) incoming = CUT_RIGHT;
    else if (incoming == CUT_REAR_DOWNWARD) incoming = CUT_DOWNWARD;
    return wanted == incoming;
}

static const char* combatDirectionLabel(CutDirection direction)
{
    switch (direction)
    {
        case CUT_LEFT: return "Left";
        case CUT_RIGHT: return "Right";
        case CUT_DOWNWARD: return "Overhead";
        case CUT_THRUST: return "Thrust";
        default: return "Choose direction";
    }
}

// Slot index of the four on-screen arrows. Horizontal CutDirection values are
// blade travel directions, so CUT_RIGHT belongs to the left-side inward arrow
// and CUT_LEFT belongs to the right-side inward arrow. Rear cuts and unknowns
// have no slot (they never match a free-attack commit either).
static int combatDirectionSlot(CutDirection d)
{
    switch (d)
    {
        case CUT_RIGHT:     return 0;
        case CUT_LEFT:      return 1;
        case CUT_DOWNWARD:  return 2;
        case CUT_THRUST:    return 3;
        default:            return -1;
    }
}
// s_userWantsFP — PERSISTENT first-person INTENT (mirrors s_userWantsDC).  Survives
// survivable loads (chunk streaming / micro-loads / save-load into gameplay) so FP is
// re-entered after the scene rebuilds; cleared only on P-off, DC-off (V), new game, or
// hard teardown.  Fixes "FP disables when loading between chunks" (user 2026-07-29).
static bool  s_userWantsFP         = false;
static volatile bool s_fpToggleRequested = false;  // F-key edge -> consumed on game thread
static bool  s_fpSuspendedForInv   = false;  // FP paused while the inventory face-cam runs
static float s_fpSensitivityFP     = 1.0f;   // [FirstPerson] Sensitivity (mouse-look scale)
static float s_fpEyeHeight         = 16.5f;  // [FirstPerson] EyeHeight (fallback neck model)
static float s_fpFwdOffset         = 0.0f;   // [FirstPerson] ForwardOffset (Ironwind: eye 1 unit farther back)
static float s_fpFovDegFP          = 90.0f;  // [FirstPerson] FOV (Ironwind wide-angle default)
static float s_fpNearClipFP        = 1.0f;   // [FirstPerson] NearClip (1.0 since build 71: 0.1 gave 50x coarser depth than vanilla's 5.0/30000 -> distant grass z-fought and shimmered on every pan; 1.0 = 5x, user-confirmed sweet spot vs body clipping)
static float s_fpNeckLimitRad      = 1.745f; // [FirstPerson] NeckLimit (deg -> rad; 100 deg — user-pictured extents 2026-08-23)
static float s_fpSideLookFwd       = 1.0f;   // [FirstPerson] SideLookForward — extra eye reach at side-look so the shoulder clears the lens
static bool  s_fpHideHair          = true;   // [FirstPerson] HideHair
static bool  s_fpHideHead          = false;  // [FirstPerson] HideHead — LEGACY bone shrink.  Retired
                                             // as the default hide in v1.4.0 (HeadPixelHide), then
                                             // RE-EXPOSED in v1.4.1 (INI + settings row): the pixel
                                             // clip assumes the vanilla UV atlas, so custom races
                                             // with their own layouts need the bone shrink instead
                                             // (user reports 2026-09).  Also the auto-fallback when
                                             // another mod's character.hlsl wins the filename race.
static bool  s_fpHideInjuryOverlay = true;   // [FirstPerson] HideInjuryOverlay — suppress hand2head/handtowounds clips in FP
// (HideGear — a whole-attachment hide in FP — was tried in v1.3.2 and REVERTED
// 2026-08-26: it silently overrode the HideHair setting, which stopped doing
// anything.  HideHair/shaveHead is the sole hair control again.
// REINSTATED + REVERTED AGAIN 2026-08-31 during the zero-offset camera test:
// user verdict "didn't work" — it did not fix the in-camera hat/head-artifact
// view.  Do not retry without new evidence of what the artifact actually is.)
static bool  s_fpHairHidden        = false;  // restore tracking
static bool  s_fpHeadBoneHidden    = false;  // restore tracking
// FP head hidden-parts mask (2026-08-31, user-proposed): Kenshi's OWN
// per-pixel body-part hide — skin.hlsl line 134: "oWet.z = (partData.y &
// hiddenMask) ..." — the mechanism full-face helmets use to erase heads.
// partData is GENERATED at body build (it is NOT in the mesh files: vdecl
// scan shows no such channel), so this path is independent of every mesh/
// bone mechanism that failed (spatially re-rigged mesh STILL spun,
// 2026-08-31).  We OR our bits into the body material's "hiddenMask"
// vertex-program constant; restore = AppearanceHuman::updateHiddenParts()
// re-derives the truth from equipment.  Bits INI-tunable live (HeadMaskBits).
// FP head PIXEL hide (2026-08-31, the shader-override endgame): the 65535
// mask test proved (a) the parameter write path reaches pixels (torso+feet
// erased) and (b) HEAD pixels carry NO part bits — unmaskable by the part
// system.  So the mod ships its own character.hlsl (resource override —
// filename race proven won for meshes) adding `uniform float dcHideHead` +
// `clip(texCoord.y + 1.0 - dcHideHead)`: head-region UVs live below y=0, so
// 1.0 discards exactly the head-region pixels — including runtime-generated
// face geometry that ignores bones/rigging/part bits.  Default 0 = vanilla
// (GpuProgramParameters buffers zero-init), so only the FP anchor is touched.
static bool s_fpHeadPixelHide    = true;  // [FirstPerson] HeadPixelHide
static bool s_fpHeadPixelApplied = false; // restore tracking
// Pixel-hide auto-fallback (v1.4.1): if the dcHideHead uniform is missing from
// every body subentity, another mod's character.hlsl won the resource filename
// race (field 2026-09: Character Highlight ships its own copy — save-load crash
// thread) and the clip can never fire this session.  Engage the legacy bone
// shrink instead so the head still hides.  Cleared on FP exit and teardown.
static bool s_fpPixelFallbackOn  = false;
// FP headgear hide (2026-08-31, post-pixel-clip): hats + beards are separate
// entities with their own materials — the character.hlsl clip can't reach
// them.  Hats + scalp piece = the appearance-attachment registry
// (setAttachmentsVisible, the v6 mechanism); beards = the entity walk (they
// are OUTSIDE the registry — 2026-08 saga).  BOTH re-asserted per frame,
// because the engine re-derives facial-attachment visibility every frame
// (proven by the beard 1 Hz logs) — the same fight the injury-overlay
// suppression wins with per-frame post-orig placement.  Independent of
// HideHead (the user's config runs the pixel clip with HideHead=0).
static bool s_fpHideHeadgear = true;  // [FirstPerson] HideHeadgear
// Per-bone saved scale for the head shrink (doc at fpSetHeadBoneHidden).
// Kenshi sizes characters partly through bone scales — originals must be
// restored EXACTLY, never assumed 1.0.
static Ogre::Vector3 s_fpHideBoneSavedScale[2];
static bool          s_fpHideBoneSaved[2] = { false, false };
// HideHead beard companion (2026-08-31): mesh NAMES (never pointers — Rule 5)
// of beard entities WE hid, so restore only re-shows what we touched — a beard
// vanilla hid (helmet hide-beard flag) must survive an FP round-trip hidden.
static const int FP_BEARD_MAX = 8;   // beard + hat + modded hair etc.
static char s_fpHiddenBeards[FP_BEARD_MAX][96];
static int  s_fpHiddenBeardCount = 0;
// Anchor's head-bone height above the root, measured live in fpGetHeadWorld —
// scales EyeDrop so custom-height characters keep the same RELATIVE eye level
// (user req 2026-08-24).  16.0 = the standard-height reference EyeDrop=1.5 was
// tuned on.
static float s_fpHeadAboveRoot = 16.0f;
static float s_fpLeanFwd           = 0.0f;   // smoothed forward lean (fallback model only)
static Ogre::Vector3 s_fpHeadSmooth      = Ogre::Vector3::ZERO;  // smoothed head-bone eye
static bool          s_fpHeadSmoothValid = false;
static bool          s_fpBoneLogged      = false;  // one-shot head-bone tracking log
static float         s_fpBoneEyeUp       = 1.0f;   // eye height above the resolved mount bone
static float         s_fpEyeUpAdjust     = 0.0f;   // [FirstPerson] EyeUpAdjust — INI nudge added
                                                   // to the bone-mount eye height; raise to keep
                                                   // the chest/shoulders below frame when moving
static float         s_fpMoveLeanUp      = 0.0f;   // [FirstPerson] MoveLeanUp — extra eye height
                                                   // scaled by move speed (0..1); counters the
                                                   // forward jog/sprint lean that swings the body up
static float         s_fpMoveLeanFwd     = 0.0f;   // [FirstPerson] MoveLeanForward — extra eye
                                                   // forward scaled by move speed; pushes past the
                                                   // leaning torso/arms at jog/sprint speed
static float         s_fpMoveNearClip    = 0.0f;   // [FirstPerson] MoveNearClip — near-clip distance
                                                   // blended in by move speed; slices the arm/torso
                                                   // that sweeps into the lens at jog/sprint. 0 = off
static float         s_fpMoveLeanSmooth  = 0.0f;   // runtime: smoothed 0..1 speed factor
static float         s_fpJogForward      = 2.0f;   // [FirstPerson] JogForward — eye pushed forward along
                                                   // body facing while actively JOGging (closes the gap
                                                   // the forward jog lean opens; 0 = off)
static float         s_fpRunForward      = 4.0f;   // [FirstPerson] RunForward — same, while RUN/sprinting
static float         s_fpGaitFwdSmooth   = 0.0f;   // runtime: smoothed gait forward offset (units)
static float         s_fpActionClrSmooth = 0.0f;   // runtime: smoothed committed-action forward clearance (units)
static ULONGLONG     s_lastRigTeleportMs = 0;      // DIAG: last CameraClass::teleport we issued (rig coherence)
static float         s_fpStreamDist      = 2.0f;   // [FirstPerson] StreamUpdateDist — min WORLD UNITS (~10 cm each,
                                                   // so 2.0 = ~20 cm) the character ROOT must move before we
                                                   // re-teleport the game rig for zone/foliage
                                                   // streaming. Per-frame teleport (=0) re-pages grass every
                                                   // frame => flicker while moving; throttling to ~2m streams
                                                   // smoothly. Big value (e.g. 999) ~= never re-stream (test).
static Ogre::Vector3 s_fpLastStreamPos   = Ogre::Vector3::ZERO;  // runtime: last rig-teleport position
static bool          s_fpLastStreamValid = false;                // runtime: has s_fpLastStreamPos been set
static float         s_fpGrassRangeMult  = 1.0f;   // [FirstPerson] GrassRangeMult — while FP is active, multiply
                                                   // options->grassRange/foliageRange so grass loads in a wider
                                                   // ring. Kenshi grass range is tuned for the high top-down cam;
                                                   // at ground level the short ring pops at its edge as you turn/
                                                   // move. 1.0 = off (vanilla). Restored exactly on FP exit.
                                                   // NOTE: raising range does NOT fix the distant re-scatter —
                                                   // it just renders more grass that still re-scatters (user
                                                   // 2026-07-29). Reverted to 1..8 clamp / live 1.0.
static float         s_fpSavedGrassRange   = 0.0f;  // runtime: options->grassRange   saved on FP enter
static float         s_fpSavedFoliageRange = 0.0f;  // runtime: options->foliageRange saved on FP enter
static bool          s_fpOptRangeSaved     = false; // runtime: are the two saved values valid
static float         s_fpFollowTurn        = 0.08f; // [FirstPerson] FollowTurn — while the character moves on ITS
                                                    // OWN (point-click / combat / heal approach, NOT WASD), lerp the
                                                    // view yaw toward the movement heading so the camera follows the
                                                    // character like WASD does. 0 = off (view stays on mouse-look).
                                                    // YIELDS to the mouse: only eases in after FollowDelayMs of no
                                                    // mouse-look, so you can freely look around while moving.
static float         s_fpFollowDelayMs     = 400.0f;// [FirstPerson] FollowDelayMs — how long the mouse must be still
                                                    // before the FollowTurn auto-recentre kicks in. Higher = look
                                                    // around longer before the camera drifts back onto the path.
static ULONGLONG     s_fpLastMouseMoveMs   = 0;     // runtime: last tick the player actively moved the look
static int           s_fpFoliageCenterMode = 1;     // [FirstPerson] FoliageCenterFix — reconcile the camera CENTER
                                                    // node (Kenshi's grass/foliage streaming anchor) with the eye
                                                    // WHILE MOVING, via _setDerivedPosition + forced recompute
                                                    // (KenshiFP technique).  1 = on, 0 = off (legacy under-foot pop).
                                                    // At idle the center is left vanilla so panning can't re-scatter.
static float         s_fpLookSmooth        = 0.4f;  // [FirstPerson] LookSmooth — 0..0.9: smooths the RENDERED view
                                                    // orientation so each frame's rotation delta is smaller, which
                                                    // shrinks the 1-frame render-thread grass re-facing mismatch =
                                                    // less side-to-side foliage flicker. Costs a little mouse-look
                                                    // snappiness (camera "glide"). 0 = off/raw (default).
static float         s_fpYawSm             = 0.0f;  // runtime: render-smoothed yaw (== s_fpYaw when LookSmooth=0)
static float         s_fpPitchSm           = 0.0f;  // runtime: render-smoothed pitch
static bool          s_fpSmValid           = false; // runtime: smoothed view initialised this FP session

// --- KenshiFP-style camera port (2026-07-30) ------------------------------
// The reference mod (github.com/linguine2552/KenshiFP) welds the eye to the head
// bone's TRUE world position (via the game's own Character::getBoneWorldPosition)
// and reads look input from a 1kHz DirectInput device, instead of our synthetic
// "root + rotated model-offset" eye + cursor-warp look.  Ours reconstructed the
// head position by rotating the whole ~2m bone offset by the VIEW yaw, which
// swung the eye on an arc during pure rotation (the grass-repage/camera-swing we
// fought for weeks) and never tracked the real animated head.  These toggles let
// us A/B the new path against the old one.
static bool  s_fpTrueBoneEye  = true;   // [FirstPerson] TrueBoneEye — 1 = weld the eye to the head bone's
                                        // real world position (getBoneWorldPosition); 0 = old synthetic path.
static float s_fpEyeDrop      = -0.2f;  // [FirstPerson] EyeDrop — drop below the head-bone origin to eye level (-0.2 = v1.4.0 tuned default; the old 0.06 near-clip parking is obsolete — the head is pixel-hidden)
                                        // (units of the mount bone height; the head bone sits above the eyes).
static bool  s_fpRawMouse     = true;   // [FirstPerson] RawMouse — 1 = 1kHz DirectInput look deltas (framerate-
                                        // independent, smooth); 0 = cursor-warp deltas (old, fps-dependent).
static float s_fpMoveForward  = 0.0f;   // [FirstPerson] MoveForward — speed-scaled forward LEAD so the eye leads
                                        // faster movement instead of lagging the leaning head (KenshiFP).  0 = off.
static float s_fpMoveSpeedRef = 30.0f;  // [FirstPerson] MoveSpeedRef — ground speed (feet-delta/sec) that maps to
                                        // full run = lead 1.0.  Tune so a full sprint gives ~the MoveForward push.
// runtime: feet-delta ground-speed tracker that drives the forward lead (KenshiFP
// keys the lead off ON-SCREEN speed, not the noisy currentMotion magnitude — the
// same signal our own notes found unreliable, 2026-07-24).
static bool      s_fpHaveLastFeet   = false;
static float     s_fpLastFeetX      = 0.0f;
static float     s_fpLastFeetZ      = 0.0f;
static float     s_fpLastFeetY      = 0.0f;   // vertical feet sample (stair-climb detector)
static float     s_fpMoveSpeed      = 0.0f;   // low-passed ground speed (units/sec)
static float     s_fpMoveFwdSmooth  = 0.0f;   // smoothed forward-lead offset (units)
static float     s_fpClimbSpeedSmooth = 0.0f; // low-passed vertical speed (units/sec, + = ascending)
static ULONGLONG s_fpFeetTickMs     = 0;      // last feet-speed sample tick

// Ascent-aware forward pullback (user 2026-07-31): on stairs the horizontal
// ForwardOffset drives the eye INTO the rising steps.  While the character climbs
// (positive vertical speed) scale the forward push down toward StairForwardMinScale
// and lift the eye by StairEyeLift, so the camera stops jamming into the steps —
// full offset is restored on flat ground so the body still never clips.
// REBALANCED v1.4.1 (field 2026-09: "seeing inside my armor going uphill"):
// the old defaults (Reduce 0.25 / MinScale 0.30 / Lift 0.30) saturated at only
// 4 u/s of climb — even WALKING a mild hill pinned the pullback at max, which
// collapsed the eye back inside the arched-forward torso for the whole climb
// while the 0.3u lift did nothing visible.  New defaults: full effect needs a
// real stair-grade climb (~10 u/s), over half the forward reach survives at
// max, and the lift is big enough to clear the uphill body lean.
static float s_fpStairForwardReduce   = 0.10f; // [FirstPerson] StairForwardReduce — climb-speed sensitivity: higher
                                               // reaches full pullback at a gentler climb.  0 = feature off.
static float s_fpStairForwardMinScale = 0.55f; // [FirstPerson] StairForwardMinScale — forward-offset fraction kept at
                                               // full climb (0.55 = eye pulled back to 55% of ForwardOffset; 1 = no pullback).
static float s_fpStairEyeLift         = 1.20f; // [FirstPerson] StairEyeLift — extra eye height (units) at full climb,

// Enemy body-clip clearance (v1.3.0 combat polish).  When a live hostile stands
// inside EnemyClearRadius of the anchor, the forward eye offsets (ForwardOffset +
// gait/action pushes) collapse toward EnemyClearMinScale — the same mechanism as
// the stair pullback — so the eye retreats to the (hidden) skull instead of
// poking into the aggressor's model, and the near plane pulls in (EnemyNearClip)
// so whatever still overlaps slices as thin a cross-section as possible.
// Nearest-hostile distance is sampled per frame in mainLoop (game thread, where
// getCharacterUpdateList lives) and consumed here; -1 = no hostile in range.
static float s_fpEnemyClearRadius   = 12.0f;  // [FirstPerson] EnemyClearRadius (units; 0 = off)
static float s_fpEnemyClearMinScale = 0.15f;  // [FirstPerson] EnemyClearMinScale — fwd fraction kept at contact
static float s_fpEnemyNearClip      = 0.10f;  // [FirstPerson] EnemyNearClip — near plane at full overlap (0 = off)
// Build 72 (user 2026-10-01: "knocked down I want to see my body, not through
// it").  While DOWN / ragdolled / getting up the horizon does not matter, so the
// near plane drops to DownNearClip and the chest renders as a solid wall; it
// eases back to NearClip on standing.  BodySolidInside renders the player's own
// mesh double-sided in first person so the inside of the body is opaque.
static float s_fpDownNearClip       = 0.05f;  // [FirstPerson] DownNearClip
static float s_fpDownNearSmooth     = 0.0f;   // runtime 0..1 blend toward the down near plane
static bool  s_fpBodySolidInside    = true;   // [FirstPerson] BodySolidInside
static const int FP_BODY_SUB_MAX    = 48;
static Ogre::CullingMode       s_fpBodyCullSaved[FP_BODY_SUB_MAX];
static Ogre::ManualCullingMode s_fpBodyManualSaved[FP_BODY_SUB_MAX];
static int   s_fpBodyCullCount      = 0;      // subentities whose cull modes were changed (0 = none applied)
static float s_fpEnemyClearSmooth   = 1.0f;   // runtime: smoothed 0..1 offset scale (1 = no hostile near)
static float s_fpEnemyNearestDist   = -1.0f;  // runtime: nearest live hostile distance (set in mainLoop)

// Interior floor pre-reveal — TRIED AND REVERTED (2026-08-01): re-showing the
// storeys above the anchor (Building::setFloorVisibility after the game's
// updateFloorVisibility pass) worked mechanically, but Kenshi's upper-floor
// meshes are authored for the top-down cutaway — one-sided faces with no
// underside geometry — so from eye level below they render as floating planks,
// hollow shells and sky holes (user screenshots, shinobi tower).  The vanilla
// reveal-on-approach stays; do not re-attempt without a mesh-level solution.

// Below-floor reveal (user 2026-08-01: black voids through stairwell gaps when
// looking DOWN in FP, flickering on fast pans).  In FP restrictPosition — the
// vanilla cutaway refresh — is skipped, and the substitute per-frame
// updateFloorVisibility(characters) pass reveals storeys by where SQUAD MEMBERS
// stand, not what the camera should see: solo character on floor 2 → floors 0-1
// shell geometry hidden → black voids (furniture/NPCs are separate objects, so
// they still rendered, floating in the dark).  Fix: after that pass, force the
// cutaway to "everything up to my floor" via setFloorsVisibility — one
// consistent answer per frame, which also stops the two-writer flicker.
static bool s_fpFloorRevealBelow = true;   // [FirstPerson] FloorRevealBelow (live-retunable via P)

// Sneak toggle (Shift+C while first-person, user req 2026-08-01).  Poll-thread
// edge consumed on the game thread — the OrdersPanel sneak-button path (or
// setStealthMode fallback) must run where the anchor is valid.
static volatile bool s_sneakToggleRequested = false;
                                               // to look over the steps.  0 = off.

static const ULONGLONG FP_STRAFE_GRACE_MS  = 350;   // WASD-recency window that keeps the camera authoritative
                                                    // (body refaces to view, camera never snapped) through the
                                                    // release/coast of a strafe/backpedal — kills the jolt where
                                                    // the neck-limit used to snap the view to the body direction.
static float         s_fpActionClearFwd    = 2.0f;  // [FirstPerson] ActionClearForward — extra forward eye offset
                                                    // during committed actions (attack / block / heal / revive /
                                                    // get-up) to push the camera clear of the swinging/kneeling
                                                    // body that otherwise clips through the lens. 0 = off.
static float         s_fpBodyYaw         = 0.0f;   // runtime: smoothed VISIBLE body yaw (model only —
                                                   // the eye mounts on the view yaw, so turning the
                                                   // body never moves the camera). Lerps toward view.
static const float   FP_BODY_TURN        = 0.20f;  // per-frame body-yaw lerp toward the view
static const float FP_HEAD_LEN     = 1.8f;   // neck-pivot to eye (fallback model)
static const float FP_BONE_SMOOTH  = 0.45f;  // per-frame height lerp (damps stride bob)

// -----------------------------------------------------------------------
// Native Controls-menu keybinds (v1.7.3) — V toggle only.
//
// HARD CONSTRAINT learned in the field (2026-06-10): Kenshi's InputHandler
// allows exactly ONE command per physical key.  Vanilla camera panning
// owns W/S/A/D as alternate binds, so registering DC movement commands on
// those keys STOLE them from the camera (camera dead in vanilla mode) and
// the theft was then persisted into controls.cfg.  The hybrid design needs
// the same key to pan the camera in vanilla mode and move the character in
// DC mode — Kenshi's native system cannot express that.  Therefore:
//   - Movement keys (W/A/S/D roles) are ALWAYS handled by our own VK
//     polling (INI-configurable, v1.6 system); playerControl_hook already
//     context-switches the camera keys during DC.  NEVER register
//     dc_move_* commands in the InputHandler.
//   - Toggle (V) is registered natively (KEP pattern) and rebindable in the
//     Controls menu. X/Z remain vanilla speed_3/speed_1. The vanilla
//     cycle_run_speed event is only observed after dispatch so the directly
//     controlled character inherits the selected movement pace.
// Evidence the game persists plugin commands when bound: toggle_devtools
// =F12 (KEP) lives in controls.cfg; our dc_ lines were missing because
// vanilla camera lines re-stole W/A/S/D at loadConfig, leaving dc_move_*
// unbound at save time.
// -----------------------------------------------------------------------
static volatile bool s_nativeCommandsRegistered = false; // set by registerNativeCommands
static void watchNativeBindChanges();                 // defined with the keybind hooks below
static void registerNativeCommands(InputHandler* self);  // ditto

struct VkName { const char* name; int vk; };
static const VkName s_vkNames[] =
{
    { "VK_SPACE",   VK_SPACE   }, { "VK_TAB",      VK_TAB      },
    { "VK_RETURN",  VK_RETURN  }, { "VK_BACK",     VK_BACK     },
    { "VK_SHIFT",   VK_SHIFT   }, { "VK_LSHIFT",   VK_LSHIFT   }, { "VK_RSHIFT",   VK_RSHIFT   },
    { "VK_CONTROL", VK_CONTROL }, { "VK_LCONTROL", VK_LCONTROL }, { "VK_RCONTROL", VK_RCONTROL },
    { "VK_MENU",    VK_MENU    }, { "VK_LMENU",    VK_LMENU    }, { "VK_RMENU",    VK_RMENU    },
    { "VK_CAPITAL", VK_CAPITAL },
    { "VK_UP",      VK_UP      }, { "VK_DOWN",     VK_DOWN     },
    { "VK_LEFT",    VK_LEFT    }, { "VK_RIGHT",    VK_RIGHT    },
    { "VK_HOME",    VK_HOME    }, { "VK_END",      VK_END      },
    { "VK_PRIOR",   VK_PRIOR   }, { "VK_NEXT",     VK_NEXT     },
    { "VK_INSERT",  VK_INSERT  }, { "VK_DELETE",   VK_DELETE   },
    { "VK_NUMPAD0", VK_NUMPAD0 }, { "VK_NUMPAD1",  VK_NUMPAD1  }, { "VK_NUMPAD2", VK_NUMPAD2 },
    { "VK_NUMPAD3", VK_NUMPAD3 }, { "VK_NUMPAD4",  VK_NUMPAD4  }, { "VK_NUMPAD5", VK_NUMPAD5 },
    { "VK_NUMPAD6", VK_NUMPAD6 }, { "VK_NUMPAD7",  VK_NUMPAD7  }, { "VK_NUMPAD8", VK_NUMPAD8 },
    { "VK_NUMPAD9", VK_NUMPAD9 },
    { "VK_MULTIPLY", VK_MULTIPLY }, { "VK_ADD",     VK_ADD     },
    { "VK_SUBTRACT", VK_SUBTRACT }, { "VK_DECIMAL", VK_DECIMAL }, { "VK_DIVIDE", VK_DIVIDE },
    { "VK_OEM_1", VK_OEM_1 }, { "VK_OEM_2", VK_OEM_2 }, { "VK_OEM_3", VK_OEM_3 },
    { "VK_OEM_4", VK_OEM_4 }, { "VK_OEM_5", VK_OEM_5 }, { "VK_OEM_6", VK_OEM_6 },
    { "VK_OEM_7", VK_OEM_7 }, { "VK_OEM_8", VK_OEM_8 }, { "VK_OEM_102", VK_OEM_102 },
    { "VK_OEM_PLUS",  VK_OEM_PLUS  }, { "VK_OEM_COMMA",  VK_OEM_COMMA  },
    { "VK_OEM_MINUS", VK_OEM_MINUS }, { "VK_OEM_PERIOD", VK_OEM_PERIOD },
};
static const int NUM_VK_NAMES = sizeof(s_vkNames) / sizeof(s_vkNames[0]);

// parseKeyName — accepts named keys (table above), VK_A..VK_Z / VK_0..VK_9,
// bare single characters, VK_F1..VK_F24 / F1..F24, hex (0x56), or decimal
// virtual-key codes.  Returns -1 when unrecognised.
static int parseKeyName(const char* raw)
{
    char s[32];
    int  n = 0;
    for (const char* p = raw; *p && n < 31; ++p)
    {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') continue;
        s[n++] = (char)toupper((unsigned char)*p);
    }
    s[n] = '\0';
    if (n == 0) return -1;

    // Named-key table — both plain ("SPACE", "NUMPAD5", "OEM_3") and
    // VK_-prefixed ("VK_SPACE") forms are accepted.
    char prefixed[36];
    sprintf_s(prefixed, sizeof(prefixed), "VK_%s", s);
    for (int i = 0; i < NUM_VK_NAMES; ++i)
        if (strcmp(s, s_vkNames[i].name) == 0
            || strcmp(prefixed, s_vkNames[i].name) == 0)
            return s_vkNames[i].vk;

    const char* f = s;
    if (strncmp(f, "VK_", 3) == 0) f += 3;

    if (f[0] == 'F' && f[1] >= '0' && f[1] <= '9')
    {
        int fn = atoi(f + 1);
        if (fn >= 1 && fn <= 24) return VK_F1 + fn - 1;
    }
    if (strlen(f) == 1 &&
        ((f[0] >= 'A' && f[0] <= 'Z') || (f[0] >= '0' && f[0] <= '9')))
        return f[0];
    if (s[0] == '0' && s[1] == 'X')
    {
        long v = strtol(s, nullptr, 16);
        if (v > 0 && v < 256) return (int)v;
    }
    {
        char* end = nullptr;
        long  v   = strtol(s, &end, 10);
        if (end != s && *end == '\0' && v > 0 && v < 256) return (int)v;
    }
    return -1;
}

// parseBool — accepts true/false, 1/0, yes/no, on/off (case-insensitive).
// Returns the supplied default for anything unrecognised so a typo can't
// silently flip a feature off.
static bool parseBool(const char* raw, bool dflt)
{
    char s[16];
    int  n = 0;
    for (const char* p = raw; *p && n < 15; ++p)
    {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') continue;
        s[n++] = (char)tolower((unsigned char)*p);
    }
    s[n] = '\0';
    if (!strcmp(s, "true") || !strcmp(s, "1") || !strcmp(s, "yes") || !strcmp(s, "on"))
        return true;
    if (!strcmp(s, "false") || !strcmp(s, "0") || !strcmp(s, "no") || !strcmp(s, "off"))
        return false;
    return dflt;
}

static void getConfigPath(char* out, size_t cap)
{
    out[0] = '\0';
    if (s_thisModule &&
        GetModuleFileNameA(s_thisModule, out, (DWORD)cap) > 0)
    {
        char* slash = strrchr(out, '\\');
        if (slash) { *(slash + 1) = '\0'; }
        else       { out[0] = '\0'; }
    }
    strcat_s(out, cap, "WASDCombatPlugin.ini");
}

// MyGUI expects UTF-8 text, while this VC2010 project otherwise compiles
// narrow literals through the system ANSI code page. Read the Chinese UI
// strings as raw UTF-8 bytes from a sidecar next to the DLL. INI identifiers
// and saved key bindings remain unchanged for existing installations.
static const char* dcZh(const char* english)
{
    if (!english) return nullptr;
    static bool loaded = false;
    static std::map<std::string, std::string> strings;
    if (!loaded)
    {
        loaded = true;
        char path[MAX_PATH];
        getConfigPath(path, sizeof(path));
        char* slash = strrchr(path, '\\');
        if (slash) strcpy_s(slash + 1, sizeof(path) - (slash + 1 - path), "Settings.zh-CN.tsv");
        FILE* file = nullptr;
        if (fopen_s(&file, path, "rb") == 0 && file)
        {
            char line[2048];
            bool first = true;
            while (fgets(line, sizeof(line), file))
            {
                char* start = line;
                if (first && (unsigned char)start[0] == 0xEF
                    && (unsigned char)start[1] == 0xBB
                    && (unsigned char)start[2] == 0xBF) start += 3;
                first = false;
                if (*start == '#' || *start == '\r' || *start == '\n') continue;
                char* tab = strchr(start, '\t');
                if (!tab) continue;
                *tab++ = '\0';
                char* end = tab + strlen(tab);
                while (end > tab && (end[-1] == '\r' || end[-1] == '\n')) *--end = '\0';
                if (*start && *tab) strings[start] = tab;
            }
            fclose(file);
        }
        else DebugLog("[Ironwind] settings_zh_missing");
    }
    std::map<std::string, std::string>::const_iterator it = strings.find(english);
    return it == strings.end() ? english : it->second.c_str();
}

static void writeDefaultConfig(const char* path)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    // Player-facing settings only (user req 2026-08-01).  Every advanced camera
    // tunable still loads with a safe default when absent, and can be added to
    // [FirstPerson] by name for fine-tuning — the loader reads far more keys
    // than this template ships.
    static const char tmpl[] =
        "[Keybinds]\r\n"
        "; Valid names: letters, digits, F1..F24, SPACE, TAB, SHIFT, CONTROL,\r\n"
        "; arrow keys, NUMPAD0..9, OEM_1..8.  Invalid entries use the default.\r\n"
        "ToggleDC        = V\r\n"
        "MoveForward     = W\r\n"
        "MoveBackward    = S\r\n"
        "MoveLeft        = A\r\n"
        "MoveRight       = D\r\n"
        "SelectControl   = G\r\n"
        "FirstPerson     = F\r\n"
        "; Sneak is the chord SHIFT + this key (first-person and third-person views).\r\n"
        "SneakToggle     = C\r\n"
        "; Swap the V-mode third-person camera to the other shoulder.\r\n"
        "SwapShoulder    = H\r\n"
        "; Keyboard attack/block are used when DirectionalCombat is false.\r\n"
        "Block           = Q\r\n"
        "; V starts manual combat automatically. Attack key applies when directional mode is off.\r\n"
        "Attack          = E\r\n"
        "; While manual combat is on, your character fights whoever you point at:\r\n"
        "; the enemy under the mouse cursor, or nearest the crosshair in first person\r\n"
        "; and the V-mode third-person view.  The camera is never moved for it.\r\n"
        "; Key layout version (leave as is): 3 = v117 layout, view F / Block Q / control G.\r\n"
        "KeyLayout       = 3\r\n"
        "\r\n"
        "[Settings]\r\n"
        "; true = camera faces your character when their inventory opens.\r\n"
        "; false = keep moving with WASD while looting/trading.\r\n"
        "InventoryFaceCam = true\r\n"
        "; Cap WASD speed at the character's real max speed (injuries,\r\n"
        "; encumbrance, shackles).  Mult scales it (1.0 = exactly vanilla).\r\n"
        "WasdSpeedCap     = true\r\n"
        "WasdSpeedMult    = 1.0\r\n"
        "; Enemies see your real movement, chase at full speed and can attack\r\n"
        "; you while you move (false = old behaviour: kiting never gets you hit).\r\n"
        "EnemyPursuit     = true\r\n"
        "; Manual combat and Block key on/off.\r\n"
        "ManualBlock      = true\r\n"
        "; V opens shoulder view and manual combat. Hold left mouse while moving\r\n"
        "; to choose a direction; release to attack. Right mouse cancels into guard.\r\n"
        "DirectionalCombat = true\r\n"
        "; With V combat on, true keeps RMB as the guard request but lets\r\n"
        "; Kenshi choose the defence direction. Releasing RMB stops it.\r\n"
        "AiBlocksInManual = false\r\n"
        "; Keyboard fallback only: a press blocks for up to BlockHoldMs, then a cooldown.\r\n"
        "BlockHoldMs            = 1200\r\n"
        "BlockCooldownMs        = 800\r\n"
        "; Attack: the press must reach the combat AI within this many ms.\r\n"
        "AttackWindowMs         = 400\r\n"
        "; Input forgiveness: extra presses of the same combat key within this\r\n"
        "; many ms are ignored (a double-tap is one move). Letting go of Block and\r\n"
        "; pressing it again this soon resumes the same block (no cooldown).\r\n"
        "InputRepeatGuardMs     = 250\r\n"
        "; After a swing ends, new presses are ignored for this long so\r\n"
        "; the follow-through plays out instead of restarting the move.\r\n"
        "AttackRecoveryMs       = 400\r\n"
        "; Directional aim: how far off the view centre (degrees) an enemy still\r\n"
        "; counts as aimed at.\r\n"
        "AimConeDeg             = 35\r\n"
        "; Aim focus with a free cursor: an enemy this many pixels from the cursor counts as pointed at.\r\n"
        "AimHoverPx             = 70\r\n"
        "; Observation marker: the name and health of the NPC directly under the crosshair.\r\n"
        "AimNameTag             = true\r\n"
        "; Detailed log for bug reports (writes every combat/movement event).\r\n"
        "VerboseLog             = false\r\n"
        "; V-mode third-person camera: distance / height / shoulder offset.\r\n"
        "OtsDistance      = 23\r\n"
        "OtsHeight        = 18.0\r\n"
        "OtsSide          = 4.0\r\n"
        "; Near plane when a wall pushes the camera in close (lower keeps\r\n"
        "; your character's mesh intact at point-blank camera range).\r\n"
        "OtsNearClip      = 0.15\r\n"
        "\r\n"
        "[FirstPerson]\r\n"
        "; First-person view (press F while Direct Control is on).\r\n"
        "Sensitivity      = 1.0\r\n"
        "; Field of view in degrees (50-110).\r\n"
        "FOV              = 90\r\n"
        "; Hide your own head in first person (per-pixel - clean).\r\n"
        "HeadPixelHide    = 1\r\n"
        "; Legacy head hide (shrinks the head bone instead).  Turn this on -\r\n"
        "; and HeadPixelHide off - for custom races that render wrong with\r\n"
        "; the per-pixel hide (missing part maps, non-standard heads).\r\n"
        "HideHead         = 0\r\n"
        "; Hide your own worn hats/helmets/masks and beard.\r\n"
        "HideHeadgear     = 1\r\n"
        "; Hide your own hair (covers modded hairstyles too).\r\n"
        "HideHair         = 1\r\n"
        "; Suppress the hand-to-head / hand-to-wound injury poses while in\r\n"
        "; first person (they plant a hand across the camera).\r\n"
        "HideInjuryOverlay = 1\r\n"
        "; Render the storeys below you inside multi-floor buildings\r\n"
        "; (0 = vanilla reveal-on-approach).\r\n"
        "FloorRevealBelow = 1\r\n"
        "; Near clipping plane.  Lower shows more of your own body but makes\r\n"
        "; distant grass shimmer when you turn; 1.0 is the balance.\r\n"
        "NearClip         = 1.0\r\n"
        "; Near plane while knocked down / ragdolled (your body stays solid).\r\n"
        "DownNearClip     = 0.05\r\n"
        "; Draw your own body double-sided so it is solid from the inside.\r\n"
        "BodySolidInside  = 1\r\n";
    DWORD written = 0;
    WriteFile(h, tmpl, (DWORD)(sizeof(tmpl) - 1), &written, nullptr);
    CloseHandle(h);
}

// loadFirstPersonConfig — read + clamp the [FirstPerson] tunables from the INI.
// Split out of loadKeybinds so the P-toggle can re-read it live: edit the INI,
// toggle FP off then on, and the new camera values apply with NO game relaunch
// (called again at the top of enterFirstPerson).  Missing keys/section fall
// back to shipped defaults, so existing INIs keep working untouched.
static void loadFirstPersonConfig(const char* path)
{
    char fb[32];
    GetPrivateProfileStringA("FirstPerson", "Sensitivity", "1.0", fb, sizeof(fb), path);
    float sens = (float)atof(fb);
    if (sens < 0.1f) sens = 0.1f;  if (sens > 5.0f) sens = 5.0f;
    s_fpSensitivityFP = sens;
    GetPrivateProfileStringA("FirstPerson", "FOV", "90", fb, sizeof(fb), path);
    float fov = (float)atof(fb);
    if (fov < 50.0f) fov = 50.0f;  if (fov > 110.0f) fov = 110.0f;
    s_fpFovDegFP = fov;
    GetPrivateProfileStringA("FirstPerson", "EyeHeight", "16.5", fb, sizeof(fb), path);
    float eye = (float)atof(fb);
    if (eye < 1.0f) eye = 1.0f;    if (eye > 40.0f) eye = 40.0f;
    s_fpEyeHeight = eye;
    GetPrivateProfileStringA("FirstPerson", "ForwardOffset", "0.0", fb, sizeof(fb), path);
    float fwd = (float)atof(fb);
    if (fwd < 0.0f) fwd = 0.0f;    if (fwd > 10.0f) fwd = 10.0f;
    s_fpFwdOffset = fwd;
    GetPrivateProfileStringA("FirstPerson", "EyeUpAdjust", "0.0", fb, sizeof(fb), path);
    float eyeUp = (float)atof(fb);
    if (eyeUp < -6.0f) eyeUp = -6.0f;  if (eyeUp > 6.0f) eyeUp = 6.0f;
    s_fpEyeUpAdjust = eyeUp;
    GetPrivateProfileStringA("FirstPerson", "MoveLeanUp", "0.0", fb, sizeof(fb), path);
    float mlu = (float)atof(fb);
    if (mlu < 0.0f) mlu = 0.0f;    if (mlu > 12.0f) mlu = 12.0f;
    s_fpMoveLeanUp = mlu;
    GetPrivateProfileStringA("FirstPerson", "MoveLeanForward", "0.0", fb, sizeof(fb), path);
    float mlf = (float)atof(fb);
    if (mlf < 0.0f) mlf = 0.0f;    if (mlf > 12.0f) mlf = 12.0f;
    s_fpMoveLeanFwd = mlf;
    GetPrivateProfileStringA("FirstPerson", "MoveNearClip", "0.0", fb, sizeof(fb), path);
    float mnc = (float)atof(fb);
    if (mnc < 0.0f) mnc = 0.0f;    if (mnc > 10.0f) mnc = 10.0f;
    s_fpMoveNearClip = mnc;
    GetPrivateProfileStringA("FirstPerson", "JogForward", "2.0", fb, sizeof(fb), path);
    float jf = (float)atof(fb);
    if (jf < 0.0f) jf = 0.0f;    if (jf > 20.0f) jf = 20.0f;
    s_fpJogForward = jf;
    GetPrivateProfileStringA("FirstPerson", "RunForward", "4.0", fb, sizeof(fb), path);
    float rf = (float)atof(fb);
    if (rf < 0.0f) rf = 0.0f;    if (rf > 20.0f) rf = 20.0f;
    s_fpRunForward = rf;
    GetPrivateProfileStringA("FirstPerson", "StreamUpdateDist", "2.0", fb, sizeof(fb), path);
    float sud = (float)atof(fb);
    if (sud < 0.0f) sud = 0.0f;    if (sud > 1000.0f) sud = 1000.0f;
    s_fpStreamDist = sud;
    GetPrivateProfileStringA("FirstPerson", "GrassRangeMult", "1.0", fb, sizeof(fb), path);
    float grm = (float)atof(fb);
    if (grm < 1.0f) grm = 1.0f;    if (grm > 8.0f) grm = 8.0f;
    s_fpGrassRangeMult = grm;
    GetPrivateProfileStringA("FirstPerson", "FollowTurn", "0.08", fb, sizeof(fb), path);
    float ft = (float)atof(fb);
    if (ft < 0.0f) ft = 0.0f;    if (ft > 1.0f) ft = 1.0f;
    s_fpFollowTurn = ft;
    GetPrivateProfileStringA("FirstPerson", "FollowDelayMs", "400", fb, sizeof(fb), path);
    float fd = (float)atof(fb);
    if (fd < 0.0f) fd = 0.0f;    if (fd > 5000.0f) fd = 5000.0f;
    s_fpFollowDelayMs = fd;
    GetPrivateProfileStringA("FirstPerson", "LookSmooth", "0.4", fb, sizeof(fb), path);
    float ls = (float)atof(fb);
    if (ls < 0.0f) ls = 0.0f;    if (ls > 0.9f) ls = 0.9f;
    s_fpLookSmooth = ls;
    s_fpFoliageCenterMode = GetPrivateProfileIntA("FirstPerson", "FoliageCenterFix", 1, path);
    if (s_fpFoliageCenterMode < 0) s_fpFoliageCenterMode = 0;
    if (s_fpFoliageCenterMode > 2) s_fpFoliageCenterMode = 2;
    GetPrivateProfileStringA("FirstPerson", "ActionClearForward", "2.0", fb, sizeof(fb), path);
    float acf = (float)atof(fb);
    if (acf < 0.0f) acf = 0.0f;    if (acf > 8.0f) acf = 8.0f;
    s_fpActionClearFwd = acf;
    GetPrivateProfileStringA("FirstPerson", "SideLookForward", "1.0", fb, sizeof(fb), path);
    float slf = (float)atof(fb);
    if (slf < 0.0f) slf = 0.0f;    if (slf > 6.0f) slf = 6.0f;
    s_fpSideLookFwd = slf;
    GetPrivateProfileStringA("FirstPerson", "NeckLimit", "100", fb, sizeof(fb), path);
    float nl = (float)atof(fb);
    if (nl < 30.0f) nl = 30.0f;    if (nl > 170.0f) nl = 170.0f;
    s_fpNeckLimitRad = nl * 3.14159265f / 180.0f;
    GetPrivateProfileStringA("FirstPerson", "NearClip", "1.0", fb, sizeof(fb), path);
    float nc = (float)atof(fb);
    if (nc < 0.05f) nc = 0.05f;    if (nc > 5.0f) nc = 5.0f;
    s_fpNearClipFP = nc;
    s_fpHideHair = GetPrivateProfileIntA("FirstPerson", "HideHair", 1, path) != 0;
    s_fpHideHead = GetPrivateProfileIntA("FirstPerson", "HideHead", 0, path) != 0;
    s_fpHeadPixelHide =
        GetPrivateProfileIntA("FirstPerson", "HeadPixelHide", 1, path) != 0;
    s_fpHideHeadgear =
        GetPrivateProfileIntA("FirstPerson", "HideHeadgear", 1, path) != 0;
    s_fpHideInjuryOverlay =
        GetPrivateProfileIntA("FirstPerson", "HideInjuryOverlay", 1, path) != 0;
    // KenshiFP-style camera port (2026-07-30).
    s_fpTrueBoneEye = GetPrivateProfileIntA("FirstPerson", "TrueBoneEye", 1, path) != 0;
    s_fpRawMouse    = GetPrivateProfileIntA("FirstPerson", "RawMouse",    1, path) != 0;
    GetPrivateProfileStringA("FirstPerson", "EyeDrop", "-0.2", fb, sizeof(fb), path);
    float ed = (float)atof(fb);
    if (ed < -4.0f) ed = -4.0f;    if (ed > 8.0f) ed = 8.0f;
    s_fpEyeDrop = ed;
    GetPrivateProfileStringA("FirstPerson", "MoveForward", "0.0", fb, sizeof(fb), path);
    float mf = (float)atof(fb);
    if (mf < 0.0f) mf = 0.0f;      if (mf > 20.0f) mf = 20.0f;
    s_fpMoveForward = mf;
    GetPrivateProfileStringA("FirstPerson", "MoveSpeedRef", "30.0", fb, sizeof(fb), path);
    float msr = (float)atof(fb);
    if (msr < 1.0f) msr = 1.0f;    if (msr > 2000.0f) msr = 2000.0f;
    s_fpMoveSpeedRef = msr;
    // Ascent-aware stair pullback tunables.
    GetPrivateProfileStringA("FirstPerson", "StairForwardReduce", "0.10", fb, sizeof(fb), path);
    float sfr = (float)atof(fb);
    if (sfr < 0.0f) sfr = 0.0f;    if (sfr > 5.0f) sfr = 5.0f;
    s_fpStairForwardReduce = sfr;
    GetPrivateProfileStringA("FirstPerson", "StairForwardMinScale", "0.55", fb, sizeof(fb), path);
    float sms = (float)atof(fb);
    if (sms < 0.0f) sms = 0.0f;    if (sms > 1.0f) sms = 1.0f;
    s_fpStairForwardMinScale = sms;
    GetPrivateProfileStringA("FirstPerson", "StairEyeLift", "1.20", fb, sizeof(fb), path);
    float sel = (float)atof(fb);
    if (sel < 0.0f) sel = 0.0f;    if (sel > 4.0f) sel = 4.0f;
    s_fpStairEyeLift = sel;
    // Enemy body-clip clearance tunables.
    GetPrivateProfileStringA("FirstPerson", "EnemyClearRadius", "12.0", fb, sizeof(fb), path);
    float ecr = (float)atof(fb);
    if (ecr < 0.0f) ecr = 0.0f;    if (ecr > 60.0f) ecr = 60.0f;
    s_fpEnemyClearRadius = ecr;
    GetPrivateProfileStringA("FirstPerson", "EnemyClearMinScale", "0.15", fb, sizeof(fb), path);
    float ecm = (float)atof(fb);
    if (ecm < 0.0f) ecm = 0.0f;    if (ecm > 0.95f) ecm = 0.95f;
    s_fpEnemyClearMinScale = ecm;
    GetPrivateProfileStringA("FirstPerson", "EnemyNearClip", "0.10", fb, sizeof(fb), path);
    float enc = (float)atof(fb);
    if (enc < 0.0f) enc = 0.0f;    if (enc > 5.0f) enc = 5.0f;
    s_fpEnemyNearClip = enc;
    GetPrivateProfileStringA("FirstPerson", "DownNearClip", "0.05", fb, sizeof(fb), path);
    float dnc = (float)atof(fb);
    if (dnc < 0.02f) dnc = 0.02f;  if (dnc > 2.0f) dnc = 2.0f;
    s_fpDownNearClip = dnc;
    s_fpBodySolidInside = GetPrivateProfileIntA("FirstPerson", "BodySolidInside", 1, path) != 0;
    // Below-floor reveal (0 = legacy character-based reveal only).
    s_fpFloorRevealBelow = GetPrivateProfileIntA("FirstPerson", "FloorRevealBelow", 1, path) != 0;
}

static void loadKeybinds()
{
    char path[MAX_PATH];
    getConfigPath(path, sizeof(path));

    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
    {
        writeDefaultConfig(path);
        char cbuf[MAX_PATH + 64];
        sprintf_s(cbuf, sizeof(cbuf),
            "[WASDCombat] dc_keybinds_default_config_created path=%s", path);
        DebugLog(cbuf);
    }

    for (int r = 0; r < KR_COUNT; ++r)
    {
        char buf[32] = "";
        GetPrivateProfileStringA("Keybinds", KR_INI_KEY[r], KR_DEFAULT[r],
                                 buf, sizeof(buf), path);
        int vk = parseKeyName(buf);
        if (vk <= 0)
        {
            char ibuf[128];
            sprintf_s(ibuf, sizeof(ibuf),
                "[WASDCombat] dc_keybind_invalid name=%s fallback=%s",
                buf, KR_DEFAULT[r]);
            DebugLog(ibuf);
            strcpy_s(buf, sizeof(buf), KR_DEFAULT[r]);
            vk = parseKeyName(buf);
        }
        s_bindVk[r] = vk;
        strcpy_s(s_bindCfgStr[r], sizeof(s_bindCfgStr[r]), buf);
        s_uiBindVk[r] = vk;   // keep the options-tab dropdown mirror in step
    }

    // v1.5 key layout (user 2026-10-08): Block moved to F (easier in a fight),
    // control-switch moved to G.  Configs written before the change still say
    // SelectControl=F (and Block=G, or no Block at all = would now default to F
    // and collide) - swap them ONCE, then stamp KeyLayout=2 so a player who
    // later binds F/G the old way on purpose is never swapped back.
    int keyLayout = (int)GetPrivateProfileIntA("Keybinds", "KeyLayout", 1, path);
    if (keyLayout < 2)
    {
        bool oldPair = s_bindVk[KR_SELECT] == 'F'
                    && (s_bindVk[KR_BLOCK] == 'G' || s_bindVk[KR_BLOCK] == 'F');
        bool gFree = true;
        for (int r = 0; r < KR_COUNT; ++r)
            if (r != KR_SELECT && r != KR_BLOCK && s_bindVk[r] == 'G') gFree = false;
        if (oldPair && gFree)
        {
            s_bindVk[KR_SELECT] = 'G'; s_uiBindVk[KR_SELECT] = 'G'; strcpy_s(s_bindCfgStr[KR_SELECT], sizeof(s_bindCfgStr[KR_SELECT]), "G");
            s_bindVk[KR_BLOCK]  = 'F'; s_uiBindVk[KR_BLOCK]  = 'F'; strcpy_s(s_bindCfgStr[KR_BLOCK],  sizeof(s_bindCfgStr[KR_BLOCK]),  "F");
            WritePrivateProfileStringA("Keybinds", "SelectControl", "G", path);
            WritePrivateProfileStringA("Keybinds", "Block",         "F", path);
            DebugLog("[WASDCombat] dc_keybinds_migrated_v15 select=G block=F");
        }
        WritePrivateProfileStringA("Keybinds", "KeyLayout", "2", path);
        keyLayout = 2;
    }

    // v117 layout: F is the explicit first-person/OTS toggle alongside the
    // mouse-wheel endpoint switch. P leaves the shipped controls. Migrate only
    // the previous shipped P/F values so unrelated custom bindings remain custom.
    if (keyLayout < 3)
    {
        bool migratedView = s_bindVk[KR_FP] == 'P';
        bool migratedBlock = s_bindVk[KR_BLOCK] == 'F';
        if (migratedView)
        {
            s_bindVk[KR_FP] = 'F'; s_uiBindVk[KR_FP] = 'F';
            strcpy_s(s_bindCfgStr[KR_FP], sizeof(s_bindCfgStr[KR_FP]), "F");
            WritePrivateProfileStringA("Keybinds", "FirstPerson", "F", path);
        }
        if (migratedBlock)
        {
            s_bindVk[KR_BLOCK] = 'Q'; s_uiBindVk[KR_BLOCK] = 'Q';
            strcpy_s(s_bindCfgStr[KR_BLOCK], sizeof(s_bindCfgStr[KR_BLOCK]), "Q");
            WritePrivateProfileStringA("Keybinds", "Block", "Q", path);
        }
        if (migratedView || migratedBlock)
        {
            char mbuf[160];
            sprintf_s(mbuf, sizeof(mbuf),
                "[WASDCombat] dc_keybinds_migrated_v117 view=%s block=%s",
                s_bindCfgStr[KR_FP], s_bindCfgStr[KR_BLOCK]);
            DebugLog(mbuf);
        }
        WritePrivateProfileStringA("Keybinds", "KeyLayout", "3", path);
    }

    for (int i = 0; i < KR_COUNT; ++i)
        for (int j = i + 1; j < KR_COUNT; ++j)
            if (s_bindVk[i] == s_bindVk[j])
            {
                char dbuf[128];
                sprintf_s(dbuf, sizeof(dbuf),
                    "[WASDCombat] dc_keybind_duplicate %s and %s share key 0x%02X",
                    KR_INI_KEY[i], KR_INI_KEY[j], s_bindVk[i]);
                DebugLog(dbuf);
            }

    // [Settings] feature toggles.  Missing key/section => API returns the
    // supplied default ("true"), so existing users who never had this section
    // keep the shipped default ON without touching their file.
    {
        char sbuf[16] = "";
        GetPrivateProfileStringA("Settings", "InventoryFaceCam", "true",
                                 sbuf, sizeof(sbuf), path);
        s_settingInventoryFaceCam = parseBool(sbuf, true);

        GetPrivateProfileStringA("Settings", "WasdSpeedCap", "true",
                                 sbuf, sizeof(sbuf), path);
        s_settingWasdSpeedCap = parseBool(sbuf, true);

        GetPrivateProfileStringA("Settings", "EnemyPursuit", "true",
                                 sbuf, sizeof(sbuf), path);
        s_settingEnemyPursuit = parseBool(sbuf, true);

        GetPrivateProfileStringA("Settings", "ManualBlock", "true",
                                 sbuf, sizeof(sbuf), path);
        s_settingManualBlock = parseBool(sbuf, true);
        GetPrivateProfileStringA("Settings", "DirectionalCombat", "true",
                                 sbuf, sizeof(sbuf), path);
        s_directionalCombat = parseBool(sbuf, true);
        GetPrivateProfileStringA("Settings", "AiBlocksInManual", "false", sbuf, sizeof(sbuf), path);
        s_settingAiBlocks = parseBool(sbuf, false);
        GetPrivateProfileStringA("Settings", "VerboseLog", "false", sbuf, sizeof(sbuf), path);
        s_verboseLog = parseBool(sbuf, false);
        {
            char pbuf[16] = "";
            GetPrivateProfileStringA("Settings", "BlockHoldMs", "1200", pbuf, sizeof(pbuf), path);
            float bh = (float)atof(pbuf);
            if (bh < 200.0f) bh = 200.0f;  if (bh > 5000.0f) bh = 5000.0f;
            s_blockHoldMs = bh;
            GetPrivateProfileStringA("Settings", "BlockCooldownMs", "800", pbuf, sizeof(pbuf), path);
            float bc = (float)atof(pbuf);
            if (bc < 0.0f) bc = 0.0f;      if (bc > 10000.0f) bc = 10000.0f;
            s_blockCooldownMs = bc;
            GetPrivateProfileStringA("Settings", "AimNameTag", "true", pbuf, sizeof(pbuf), path);
            s_settingAimName = parseBool(pbuf, true);
            GetPrivateProfileStringA("Settings", "AimConeDeg", "35", pbuf, sizeof(pbuf), path);
            float ac = (float)atof(pbuf);
            if (ac < 5.0f) ac = 5.0f;      if (ac > 90.0f) ac = 90.0f;
            s_aimConeDeg = ac;
            GetPrivateProfileStringA("Settings", "AimHoverPx", "70", pbuf, sizeof(pbuf), path);
            float ahp = (float)atof(pbuf);
            if (ahp < 10.0f) ahp = 10.0f;  if (ahp > 400.0f) ahp = 400.0f;
            s_aimHoverPx = ahp;
            GetPrivateProfileStringA("Settings", "AttackWindowMs", "400", pbuf, sizeof(pbuf), path);
            float aw = (float)atof(pbuf);
            if (aw < 100.0f) aw = 100.0f;  if (aw > 2000.0f) aw = 2000.0f;
            s_attackWindowMs = aw;
            GetPrivateProfileStringA("Settings", "InputRepeatGuardMs", "250", pbuf, sizeof(pbuf), path);
            float rg = (float)atof(pbuf);
            if (rg < 0.0f) rg = 0.0f;      if (rg > 1000.0f) rg = 1000.0f;
            s_inputRepeatGuardMs = rg;
            GetPrivateProfileStringA("Settings", "AttackRecoveryMs", "400", pbuf, sizeof(pbuf), path);
            float ar = (float)atof(pbuf);
            if (ar < 0.0f) ar = 0.0f;      if (ar > 2000.0f) ar = 2000.0f;
            s_attackRecoveryMs = ar;
        }

        char mbuf[16] = "";
        GetPrivateProfileStringA("Settings", "WasdSpeedMult", "1.0",
                                 mbuf, sizeof(mbuf), path);
        float wsm = (float)atof(mbuf);
        if (wsm < 0.1f) wsm = 0.1f;   if (wsm > 5.0f) wsm = 5.0f;
        s_settingWasdSpeedMult = wsm;

        // V-mode OTS camera pose.
        GetPrivateProfileStringA("Settings", "OtsDistance", "23", mbuf, sizeof(mbuf), path);
        float od = (float)atof(mbuf);
        if (od < 5.0f) od = 5.0f;      if (od > OTS_ZOOM_MAX) od = OTS_ZOOM_MAX;
        s_otsCamDist = od;
        GetPrivateProfileStringA("Settings", "OtsHeight", "18.0", mbuf, sizeof(mbuf), path);
        float oh = (float)atof(mbuf);
        if (oh < -5.0f) oh = -5.0f;    if (oh > 28.0f) oh = 28.0f;
        s_otsCamHeight = oh;
        GetPrivateProfileStringA("Settings", "OtsSide", "4.0", mbuf, sizeof(mbuf), path);
        float os = (float)atof(mbuf);
        if (os < -8.0f) os = -8.0f;    if (os > 8.0f) os = 8.0f;
        s_otsCamSide = os;
        GetPrivateProfileStringA("Settings", "OtsNearClip", "0.15", mbuf, sizeof(mbuf), path);
        float onc = (float)atof(mbuf);
        if (onc < 0.05f) onc = 0.05f;  if (onc > 2.0f) onc = 2.0f;
        s_otsNearClipMin = onc;
    }

    // [FirstPerson] tunables — read via the shared helper (also called on every
    // F-enter for live re-tuning).  Missing keys/section fall back to shipped
    // defaults, so existing users keep the defaults without touching their INI.
    loadFirstPersonConfig(path);

    char lbuf[420];
    sprintf_s(lbuf, sizeof(lbuf),
        "[WASDCombat] dc_keybinds_loaded toggle=%s(0x%02X) forward=%s(0x%02X)"
        " back=%s(0x%02X) left=%s(0x%02X) right=%s(0x%02X)"
        " select=%s(0x%02X) inventoryFaceCam=%d",
        s_bindCfgStr[KR_TOGGLE],  s_bindVk[KR_TOGGLE],
        s_bindCfgStr[KR_FORWARD], s_bindVk[KR_FORWARD],
        s_bindCfgStr[KR_BACK],    s_bindVk[KR_BACK],
        s_bindCfgStr[KR_LEFT],    s_bindVk[KR_LEFT],
        s_bindCfgStr[KR_RIGHT],   s_bindVk[KR_RIGHT],
        s_bindCfgStr[KR_SELECT],  s_bindVk[KR_SELECT],
        s_settingInventoryFaceCam ? 1 : 0);
    DebugLog(lbuf);

    char fpbuf[240];
    sprintf_s(fpbuf, sizeof(fpbuf),
        "[WASDCombat] dc_firstperson_cfg key=%s(0x%02X) fov=%.0f sens=%.2f"
        " neckLimitDeg=%.0f hideHead=%d hideHair=%d sneak=SHIFT+%s(0x%02X)"
        " enemyClearR=%.1f",
        s_bindCfgStr[KR_FP], s_bindVk[KR_FP], s_fpFovDegFP, s_fpSensitivityFP,
        s_fpNeckLimitRad * 180.0f / 3.14159265f,
        s_fpHideHead ? 1 : 0, s_fpHideHair ? 1 : 0,
        s_bindCfgStr[KR_SNEAK], s_bindVk[KR_SNEAK],
        s_fpEnemyClearRadius);
    DebugLog(fpbuf);
}

// =======================================================================
//  NATIVE SETTINGS TAB — a "Direct Control" page inside the game's own
//  options window (v1.5).  Blueprint: KEP's ConfigManager.  The tab is a
//  MyGUI TabItem inserted into OptionsWindow::tabs; its content is a
//  DatapanelGUI built by the game's own factory (ForgottenGUI::
//  createDatapanel) with checkbox/slider rows bound DIRECTLY to the mod's
//  setting statics — moving a slider edits the live value.
//
//  Persistence: dcOptionsWatch (called every mainLoop frame; touches only
//  our statics, no GUI calls) detects changes and immediately writes the
//  INI key, so the F-enter INI reload (loadFirstPersonConfig) always
//  reads back exactly what the UI set — the two config paths can never
//  disagree.
//
//  Rule 5: NO widget pointers are stored.  The game owns every widget; if
//  the options window is ever torn down and rebuilt, OptionsWindow::create
//  runs again and optionsCreate_hook re-injects from scratch (duplicate
//  guard: tab looked up by name each time).
// =======================================================================

struct DcOptRow
{
    const char* section;  // INI section this row persists to
    const char* key;      // INI key; nullptr = section-header text row
    bool*       bp;       // checkbox binding (exclusive with fp)
    float*      fp;       // slider binding
    float       mn, mx;   // slider range (widget-enforced; loader clamps match)
    int         prec;     // slider decimals shown
    float       def;      // shipped default (the "Reset to defaults" value;
                          // bool rows use 0/1) — user's dialed-in preference
    const char* label;    // row text — must be UNIQUE (DatapanelGUI keys its
                          // content map by this string)
    const char* tip;      // tooltip, nullptr = none
};

static DcOptRow s_dcOptRows[] =
{
    { NULL, NULL, NULL, NULL, 0, 0, 0, 0, "MOVEMENT", NULL },
    { "Settings", "WasdSpeedMult", NULL, &s_settingWasdSpeedMult, 0.1f, 3.0f, 2, 1.0f,
      "Movement speed multiplier",
      "Scales how fast your character moves with the movement keys. 1.0 = the game's normal speed." },
    { NULL, NULL, NULL, NULL, 0, 0, 0, 0, "MANUAL COMBAT - BLOCK", NULL },
    { "Settings", "AiBlocksInManual", &s_settingAiBlocks, NULL, 0, 0, 0, 0,
      "Automatic block (AI chooses direction)",
      "Off: hold right mouse and choose the matching direction. On: hold right mouse to request a guard while Kenshi's combat AI chooses the direction; releasing it stops the request." },

    { NULL, NULL, NULL, NULL, 0, 0, 0, 0, "MANUAL COMBAT - TARGETING & FEEL", NULL },
    { "Settings", "AimConeDeg", NULL, &s_aimConeDeg, 5.0f, 90.0f, 0, 35.0f,
      "Crosshair target cone (degrees)",
      "In first person and the third-person camera, the enemy nearest the crosshair within this cone is the one your character fights." },

    { NULL, NULL, NULL, NULL, 0, 0, 0, 0, "MANUAL COMBAT - ON-SCREEN CUES", NULL },
    { "Settings", "AimNameTag", &s_settingAimName, NULL, 0, 0, 0, 1,
      "Show NPC name and health",
      "The NPC directly under the crosshair gets their name and full two-row, eight-part health panel." },
    { NULL, NULL, NULL, NULL, 0, 0, 0, 0, "THIRD PERSON CAMERA (V)", NULL },
    { "Settings", "OtsHeight", NULL, &s_otsCamHeight, -5.0f, 28.0f, 1, 18.0f,
      "Camera focus height",
      "Height of the point the camera looks at. Higher looks over the shoulder; lower looks at the back." },

    { NULL, NULL, NULL, NULL, 0, 0, 0, 0, "FIRST PERSON", NULL },
    { "FirstPerson", "Sensitivity", NULL, &s_fpSensitivityFP, 0.1f, 5.0f, 2, 1.0f,
      "Mouse look sensitivity",
      "How fast the view turns with the mouse." },
    { "FirstPerson", "FOV", NULL, &s_fpFovDegFP, 50.0f, 110.0f, 0, 90.0f,
      "Field of view (degrees)",
      "Wider shows more of your surroundings. Applies instantly." },
    { "FirstPerson", "HeadPixelHide", &s_fpHeadPixelHide, NULL, 0, 0, 0, 1,
      "Hide your own head",
      "Erases your head so it never blocks the view. Leave on unless a modded race looks wrong." },

    { NULL, NULL, NULL, NULL, 0, 0, 0, 0, "FIRST PERSON - EYE POSITION", NULL },
    { "FirstPerson", "EyeDrop", NULL, &s_fpEyeDrop, -4.0f, 8.0f, 2, -0.2f,
      "Eye drop below head bone",
      "Fine-tunes eye level. Negative raises the eye slightly." },
    { "FirstPerson", "ForwardOffset", NULL, &s_fpFwdOffset, 0.0f, 10.0f, 2, 0.0f,
      "Eye forward of the head",
      "Keeps the view out of the inside of the skull. Raise if you see your own face." },

    { NULL, NULL, NULL, NULL, 0, 0, 0, 0, "TROUBLESHOOTING", NULL },
    { "Settings", "VerboseLog", &s_verboseLog, NULL, 0, 0, 0, 0,
      "Detailed log for bug reports",
      "Writes every combat and movement event to RE_Kenshi_log.txt. Leave off normally; turn on when asked for a log, reproduce the problem, then send the file." },
};
static const int DC_OPT_ROW_COUNT = sizeof(s_dcOptRows) / sizeof(s_dcOptRows[0]);

// INI-only hotkeys exposed as key-picker DROPDOWNS (int* binding, same watch
// model as the sliders).  These CANNOT use the native press-a-key rows:
// Kenshi's command system is one-command-per-key and vanilla camera panning
// owns W/S/A/D (registering them stole the camera — June hard lesson).
// Toggle and the vanilla movement-speed cycle are not here: both get genuine
// KeyConfig rows in the tab. Only the toggle is plugin-owned.
struct DcBindRow { int role; const char* label; const char* tip; };   // role < 0 = section header
static const DcBindRow s_dcBindRows[] =
{
    { -1,         "MOVEMENT KEYS",       NULL },
    { KR_FORWARD, "Move forward",        "Walk, jog or run in the direction the camera faces." },
    { KR_LEFT,    "Move left",           NULL },
    { KR_BACK,    "Move backward",       NULL },
    { KR_RIGHT,   "Move right",          NULL },
    { KR_SELECT,  "Control hovered character", "Press with the mouse over one of your characters to control them instead." },
    { -1,         "CAMERA KEYS",         NULL },
    { KR_FP,      "First person (toggle)", "Puts the camera in your character's eyes. Press again to return to the shoulder camera started by V." },
    { KR_OTS_SHOULDER, "Swap camera shoulder", "Moves the third-person camera to the other shoulder." },
    { KR_SNEAK,   "Sneak (SHIFT + key)", "Hold SHIFT and press this key to toggle sneaking in first person or the third-person camera." },
    { -1,         "MANUAL COMBAT KEYS",  NULL },
    { KR_ATTACK,  "Interact / keyboard attack", "Directional mode: interacts with the nearby automatically selected target. Keyboard mode uses this key to attack." },
    { KR_BLOCK,   "Block / guard",  "Manual combat only. Hold to guard; in directional mode the incoming direction must match." },
};
static const int DC_BIND_ROW_COUNT = sizeof(s_dcBindRows) / sizeof(s_dcBindRows[0]);

// Key choices offered by the dropdowns.  Values are Windows VKs (the poll
// thread reads GetAsyncKeyState).  INI spelling comes from dcVkToIniName —
// always parseKeyName-compatible by construction.
struct DcKeyChoice { const char* label; int vk; };
static const DcKeyChoice s_dcKeyChoices[] =
{
    { "Space", VK_SPACE }, { "Tab", VK_TAB }, { "Enter", VK_RETURN },
    { "Backspace", VK_BACK }, { "Caps Lock", VK_CAPITAL },
    { "Up", VK_UP }, { "Down", VK_DOWN }, { "Left", VK_LEFT }, { "Right", VK_RIGHT },
    { "Home", VK_HOME }, { "End", VK_END }, { "Page Up", VK_PRIOR }, { "Page Down", VK_NEXT },
    { "Insert", VK_INSERT }, { "Delete", VK_DELETE },
    { "Numpad 0", VK_NUMPAD0 }, { "Numpad 1", VK_NUMPAD1 }, { "Numpad 2", VK_NUMPAD2 },
    { "Numpad 3", VK_NUMPAD3 }, { "Numpad 4", VK_NUMPAD4 }, { "Numpad 5", VK_NUMPAD5 },
    { "Numpad 6", VK_NUMPAD6 }, { "Numpad 7", VK_NUMPAD7 }, { "Numpad 8", VK_NUMPAD8 },
    { "Numpad 9", VK_NUMPAD9 },
    { "; :", VK_OEM_1 }, { "/ ?", VK_OEM_2 }, { "` ~", VK_OEM_3 },
    { "[ {", VK_OEM_4 }, { "\\ |", VK_OEM_5 }, { "] }", VK_OEM_6 }, { "' \"", VK_OEM_7 },
    { ", <", VK_OEM_COMMA }, { ". >", VK_OEM_PERIOD },
    { "- _", VK_OEM_MINUS }, { "= +", VK_OEM_PLUS },
};
static const int DC_KEY_CHOICE_COUNT = sizeof(s_dcKeyChoices) / sizeof(s_dcKeyChoices[0]);

// VK -> the spelling our INI parser (parseKeyName) reads back: bare char for
// A-Z/0-9, F1..F24, VK_ name from s_vkNames, decimal as the safety net.
static void dcVkToIniName(int vk, char* out, size_t cap)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
    {
        out[0] = (char)vk; out[1] = '\0';
        return;
    }
    if (vk >= VK_F1 && vk <= VK_F24)
    {
        sprintf_s(out, cap, "F%d", vk - VK_F1 + 1);
        return;
    }
    for (int i = 0; i < NUM_VK_NAMES; i++)
        if (s_vkNames[i].vk == vk)
        {
            strcpy_s(out, cap, s_vkNames[i].name);
            return;
        }
    sprintf_s(out, cap, "%d", vk);
}

static bool  s_dcOptSeeded = false;
static float s_dcOptLast[DC_OPT_ROW_COUNT];

// "Reset to defaults" button plumbing (user req 2026-09-04): the injected
// panel + its category, remembered so the button callback can refresh the
// visible widgets after resetting the bound statics.  Re-stored on every
// injection (window rebuild) — the callback only fires while the window is
// open, so the pointer is always current (Rule 5).
static DatapanelGUI* s_dcOptPanel = nullptr;
static int           s_dcOptCat   = 0;

// Reset every setting visible on this page to its shipped default
// (DcOptRow::def), then refresh the widgets. INI-only advanced compatibility
// values are deliberately left alone. dcOptionsWatch picks up
// the changed statics next frame → persists each to the INI + live-applies
// FOV/NearClip, exactly as if the player had moved every control by hand.
static void dcOnDefaultsPressed(MyGUI::Widget* /*sender*/)
{
    for (int i = 0; i < DC_OPT_ROW_COUNT; i++)
    {
        const DcOptRow& r = s_dcOptRows[i];
        if (!r.key) continue;
        if (r.bp)      *r.bp = (r.def != 0.0f);
        else if (r.fp) *r.fp = r.def;
    }
    if (s_dcOptPanel)
        for (int i = 0; i < DC_OPT_ROW_COUNT; i++)
        {
            const DcOptRow& r = s_dcOptRows[i];
            if (!r.key) continue;
            DataPanelLine* ln = s_dcOptPanel->getLine(dcZh(r.label), s_dcOptCat);
            if (ln) ln->refresh();   // pull the reset value into the widget
        }
    DebugLog("[WASDCombat] dc_opt_reset_to_defaults");
}

// Change-watch: runs every mainLoop frame.  Reads only our statics — never
// touches a widget (the rows write the statics through their bound
// pointers on the UI thread; we only observe the values).
static void dcOptionsWatch()
{
    if (!s_dcOptSeeded)
    {
        // First call is after plugin config load: snapshot, write nothing.
        for (int i = 0; i < DC_OPT_ROW_COUNT; i++)
        {
            const DcOptRow& r = s_dcOptRows[i];
            s_dcOptLast[i] = r.bp ? (*r.bp ? 1.0f : 0.0f)
                           : r.fp ? *r.fp : 0.0f;
        }
        s_dcOptSeeded = true;
        return;
    }

    for (int i = 0; i < DC_OPT_ROW_COUNT; i++)
    {
        const DcOptRow& r = s_dcOptRows[i];
        if (!r.key) continue;
        float cur = r.bp ? (*r.bp ? 1.0f : 0.0f) : *r.fp;
        if (cur == s_dcOptLast[i]) continue;
        s_dcOptLast[i] = cur;

        // FOV is normally pushed at first-person entry; re-push so its visible
        // slider applies instantly.
        if (r.fp == &s_fpFovDegFP && s_firstPersonActive && ou && ou->player
            && ou->player->camera && ou->player->camera->camera)
        {
            Ogre::Camera* ocOpt = ou->player->camera->camera;
            ocOpt->setFOVy(Ogre::Radian(Ogre::Degree(s_fpFovDegFP)));
        }

        // Persist immediately.  [Settings] is parsed with parseBool
        // (true/false); [FirstPerson] with GetPrivateProfileIntA/atof
        // (numeric).  Floats always write 3 decimals so the value
        // round-trips through the F-enter INI reload bit-stable.
        char path[MAX_PATH];
        getConfigPath(path, sizeof(path));
        char val[32];
        if (r.bp)
        {
            bool settingsSection = (strcmp(r.section, "Settings") == 0);
            strcpy_s(val, sizeof(val),
                     *r.bp ? (settingsSection ? "true" : "1")
                           : (settingsSection ? "false" : "0"));
        }
        else
        {
            sprintf_s(val, sizeof(val), "%.3f", cur);
        }
        WritePrivateProfileStringA(r.section, r.key, val, path);

        char obuf[160];
        sprintf_s(obuf, sizeof(obuf), "[WASDCombat] dc_opt_changed %s.%s=%s",
                  r.section, r.key, val);
        VerbLog(obuf);
    }

    // Keybind dropdowns: commit mirror -> live bind + INI.  s_bindVk is read
    // by the 1 kHz poll thread; an aligned int store is safe to publish.
    for (int i = 0; i < DC_BIND_ROW_COUNT; i++)
    {
        int role = s_dcBindRows[i].role;
        if (role < 0) continue;                 // build 75: section header rows (build 68) have no key - indexing [-1] clobbered s_otsCamSide
        int vk   = s_uiBindVk[role];
        if (vk == s_bindVk[role] || vk <= 0)
            continue;
        s_bindVk[role] = vk;

        char name[32];
        dcVkToIniName(vk, name, sizeof(name));
        strcpy_s(s_bindCfgStr[role], sizeof(s_bindCfgStr[role]), name);

        char path[MAX_PATH];
        getConfigPath(path, sizeof(path));
        WritePrivateProfileStringA("Keybinds", KR_INI_KEY[role], name, path);

        char bbuf[160];
        sprintf_s(bbuf, sizeof(bbuf),
                  "[WASDCombat] dc_opt_bind_changed %s=%s (0x%02X)",
                  KR_INI_KEY[role], name, vk);
        DebugLog(bbuf);

        // Same-key overlap is tolerated (matches the INI loader's behaviour)
        // but worth a log line so a "my key stopped working" report is
        // self-diagnosing.
        for (int j = 0; j < KR_COUNT; j++)
            if (j != role && s_bindVk[j] == vk)
            {
                char wbuf[128];
                sprintf_s(wbuf, sizeof(wbuf),
                          "[WASDCombat] dc_opt_bind_duplicate %s and %s share 0x%02X",
                          KR_INI_KEY[role], KR_INI_KEY[j], vk);
                DebugLog(wbuf);
            }
    }
}

// Build the tab.  Called from optionsCreate_hook (UI thread, right after
// the game builds the options window).  Known vanilla tab categories:
// General 0x1, Gameplay 0x22, Graphics 0x17, Audio 0x18, Controls 0x19,
// Mods 0x0 — we take 0x31 and collision-check against whatever exists.
static void dcInjectSettingsTab(OptionsWindow* self)
{
    if (!gui || !self->tabs)
        return;
    if (self->tabs->findItemWith(MyGUI::UString(dcZh("Direct Control"))) != NULL)
        return;   // already present (re-entry guard)

    size_t tabCount = self->tabs->getItemCount();
    int maxCat = 0;
    for (size_t i = 0; i < tabCount; i++)
    {
        DatapanelGUI** p = self->tabs->getItemDataAt<DatapanelGUI*>(i, false);
        // Sanity clamp: with RE_Kenshi/Mod-Hub/KEP tabs present, one tab's
        // item data type-checks as DatapanelGUI* but reads ASCII garbage as
        // its category (field build A landed on cat=0x454D44).  Real
        // categories are tiny (vanilla max 0x22, KEP 0x30) — ignore junk.
        if (p && *p != NULL
            && (*p)->currentCategory > maxCat && (*p)->currentCategory < 0x1000)
            maxCat = (*p)->currentCategory;
    }
    int cat = 0x31;
    if (cat <= maxCat) cat = maxCat + 1;

    // Index 5 = right after Controls, before Mods (KEP's slot choice).
    size_t index = (tabCount < 5) ? tabCount : 5;
    MyGUI::TabItem* tab =
        self->tabs->insertItemAt(index, MyGUI::UString(dcZh("Direct Control")));
    if (!tab)
    {
        DebugLog("[WASDCombat] dc_opt_tab_insert_failed");
        return;
    }
    DatapanelGUI* panel = gui->createDatapanel("dc_options", tab, true);
    if (!panel)
    {
        DebugLog("[WASDCombat] dc_opt_panel_create_failed");
        return;
    }
    panel->changeCategory(cat);
    panel->setLineSpacing(25.0f);

    int built = 0;

    // ---- KEYBINDS (first section — no leading addSpace, it renders as a
    // large empty gap; Controls-rows field finding 2026-06) ----
    panel->setLineText(dcZh("DIRECT CONTROL KEYS"), dcZh("DIRECT CONTROL KEYS"), cat, false,
                       MyGUI::Align(MyGUI::Align::Left));
    // Native press-a-key row. The game's capture flow (clickButton ->
    // OptionsWindow::setKey) is panel-agnostic; rebinds persist via the
    // existing saveOptions hook. Vanilla movement-speed controls remain only
    // on the vanilla Controls page instead of being duplicated here.
    panel->addCustomLine(new DataPanelLine_KeyConfig(
        "dc_toggle",        dcZh("Direct Control on / off"), cat));
    built += 1;
    // INI-polled hotkeys: key-picker dropdowns (see s_dcBindRows).
    for (int i = 0; i < DC_BIND_ROW_COUNT; i++)
    {
        const DcBindRow& b = s_dcBindRows[i];
        if (b.role < 0)
        {
            panel->addSpace(cat, 0.35f);
            panel->setLineText(dcZh(b.label), dcZh(b.label), cat, false, MyGUI::Align(MyGUI::Align::Left));
            built++;
            continue;
        }
        DataPanelLine_DropBox* db = panel->setLineDropBox(
            dcZh(b.label), cat, &s_uiBindVk[b.role], false, 0.7f);
        if (!db) continue;
        char one[2] = { 0, 0 };
        for (char c = 'A'; c <= 'Z'; c++)
        {
            one[0] = c;
            db->addAValue(std::string(one), (int)c);
        }
        for (char d = '0'; d <= '9'; d++)
        {
            one[0] = d;
            db->addAValue(std::string(one), (int)d);
        }
        for (int f = 0; f < 12; f++)
        {
            char fbuf[8];
            sprintf_s(fbuf, sizeof(fbuf), "F%d", f + 1);
            db->addAValue(std::string(fbuf), VK_F1 + f);
        }
        for (int k = 0; k < DC_KEY_CHOICE_COUNT; k++)
            db->addAValue(std::string(dcZh(s_dcKeyChoices[k].label)),
                          s_dcKeyChoices[k].vk);
        db->refresh();   // select the currently-bound key
        if (b.tip)
            db->setToolTip(std::string(dcZh(b.tip)), self->tooltip);
        built++;
    }

    bool firstHeader = false;   // the key groups were emitted above — every
                                // settings header gets its addSpace
    for (int i = 0; i < DC_OPT_ROW_COUNT; i++)
    {
        const DcOptRow& r = s_dcOptRows[i];
        if (!r.key)
        {
            // Section header.  No addSpace before the FIRST one — a leading
            // space rendered as a large empty gap (Controls-rows field
            // finding, 2026-06).
            if (!firstHeader)
                panel->addSpace(cat, 0.35f);
            firstHeader = false;
            panel->setLineText(dcZh(r.label), dcZh(r.label), cat, false,
                               MyGUI::Align(MyGUI::Align::Left));
            continue;
        }
        DataPanelLine* line = NULL;
        if (r.bp)
        {
            line = panel->setLineCheckbox(dcZh(r.label), r.bp, cat);
        }
        else if (r.fp)
        {
            DataPanelLine_SliderEditable* sl = panel->setLineSliderEditable(
                dcZh(r.label), cat, true, r.mn, r.mx, r.fp);
            if (sl) sl->setPrecision(r.prec);
            line = sl;
        }
        if (line)
        {
            if (r.tip)
                line->setToolTip(std::string(dcZh(r.tip)), self->tooltip);
            built++;
        }
    }

    // "Reset to defaults" button (user req 2026-09-04) — one click restores
    // every setting on this tab to the shipped defaults, for players who
    // over-tweak the camera.  Remember the panel/cat so the callback can
    // refresh the widgets.
    s_dcOptPanel = panel;
    s_dcOptCat   = cat;
    panel->addSpace(cat, 0.35f);
    DataPanelLine_Button* resetBtn =
        panel->setLineButton(dcZh("Reset visible settings to defaults"), dcZh("Reset"), cat);
    if (resetBtn && resetBtn->button)
    {
        resetBtn->button->eventMouseButtonClick += MyGUI::newDelegate(dcOnDefaultsPressed);
        built++;
    }

    // The game's tab-switch logic owns page visibility; a fresh page starts
    // hidden.  Registering the panel as the tab's item data is what lets
    // the switch logic find it (same contract as the vanilla tabs).
    tab->setVisible(false);
    self->tabs->setItemData(tab, panel);

    char tbuf[128];
    sprintf_s(tbuf, sizeof(tbuf),
              "[WASDCombat] dc_opt_tab_injected idx=%u cat=0x%X rows=%d",
              (unsigned)index, (unsigned)cat, built);
    DebugLog(tbuf);
}

// -----------------------------------------------------------------------
// Press handlers — shared by the poll thread (fallback path) and the
// native processKeys event path.  Loot-suspend gating lives here so both
// paths behave identically.
// -----------------------------------------------------------------------
static void handleTogglePress()
{
    if (s_lootUiSuspendActive)
        return;
    if (s_mode == MODE_FREE_MOVE || s_userWantsDC)
    {
        s_userWantsDC = false;
        s_userWantsFP = false;   // FP requires DC; leaving DC clears the FP intent too
        s_otsResumeZoomPending = false;
        manualAttackClearAimInput(false, true);
        manualAttackClearQueuedInput(true);
        manualAttackClearNeutralIntent();
        setMode(MODE_VANILLA);
        DebugLog("[WASDCombat] dc_user_intent_off_manual");
    }
    else
    {
        s_userWantsDC = true;
        setMode(MODE_FREE_MOVE);
        DebugLog("[WASDCombat] dc_user_intent_on");
    }
}

static void handleSelectPress()
{
    if (s_lootUiSuspendActive)
        return;
    // Only meaningful in DC; the main loop consumes the edge and switches the
    // WASD anchor to the selected/highlighted character.
    if (s_mode == MODE_FREE_MOVE)
        s_fSelectEdge = true;
}

static void handleFirstPersonPress()
{
    if (s_lootUiSuspendActive)
        return;
    // Only meaningful in DC; the main loop consumes the edge on the game thread
    // and enters/exits first-person (camera calls must run there, not here).
    if (s_mode == MODE_FREE_MOVE)
        s_fpToggleRequested = true;
}

static void handleSneakPress()
{
    if (s_lootUiSuspendActive)
        return;
    // Shift+C CHORD, first-person only (user req 2026-08-01): the sneak key alone
    // does nothing, so a bare C press can never collide with vanilla or other
    // mod uses of the key.  Final gating (live anchor, FP still active) happens
    // on the game thread where the edge is consumed.
    if (s_mode != MODE_FREE_MOVE || !(s_firstPersonActive || s_otsCamActive))
        return;
    if (!(GetAsyncKeyState(VK_SHIFT) & 0x8000))
        return;
    s_sneakToggleRequested = true;
}

// SwapShoulder (OTS): flip the over-the-shoulder camera to the other side.
// Just flips the sign applied to OtsSide; takes effect next OTS frame.
static void handleOtsShoulderPress()
{
    s_otsShoulderSign = -s_otsShoulderSign;
    DebugLog(s_otsShoulderSign > 0.0f ? "[WASDCombat] dc_ots_shoulder_right"
                                      : "[WASDCombat] dc_ots_shoulder_left");
}

static void onPress(int role)
{
    if (role == KR_TOGGLE)       handleTogglePress();
    else if (role == KR_SELECT)  handleSelectPress();
    else if (role == KR_FP)      handleFirstPersonPress();
    else if (role == KR_SNEAK)   handleSneakPress();
    else if (role == KR_OTS_SHOULDER) handleOtsShoulderPress();
    else if (role == KR_ATTACK && s_directionalCombat && s_mode == MODE_FREE_MOVE)
        s_interactionRequested = true;
}

// -----------------------------------------------------------------------
// Polling thread
// -----------------------------------------------------------------------
struct PollKey { int role; bool prev; };
static PollKey s_keys[] =
{
    { KR_FORWARD, false }, { KR_LEFT,   false },
    { KR_BACK,    false }, { KR_RIGHT,  false },
    { KR_TOGGLE,  false }, { KR_SELECT,  false },
    { KR_FP,      false },
    { KR_SNEAK,   false }, { KR_OTS_SHOULDER, false },
    { KR_BLOCK,   false }, { KR_ATTACK, false },
};
static const int NUM_KEYS = 11;

static bool isKenshiForeground()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static DWORD WINAPI PollThread(LPVOID)
{
    while (true)
    {
        Sleep(10);  // capture short mouse clicks and configurable key edges
        if (!isKenshiForeground())
        {
            if (s_mouseBlockHeld) s_blockHeld = false;
            s_mouseBlockHeld = false;
            s_recentMouseLastMs = 0;
            InterlockedExchange(&s_combatGestureDirection, CUT_DEFAULT);
            s_attackAimHeld = false;
            s_attackAimCancelled = false;
            s_attackHeld = false;
            continue;
        }
        // Movement roles are ALWAYS VK-polled (INI-configurable) — the
        // native keybind system is one-command-per-key and vanilla camera
        // owns W/S/A/D, so DC movement cannot live there.  Toggle/speed
        // polling stands down once their native commands are registered
        // (presses then arrive via the game's processKeys events).
        for (int i = 0; i < NUM_KEYS; ++i)
        {
            int role = s_keys[i].role;
            if (s_nativeCommandsRegistered
                && role == KR_TOGGLE)
                continue;
            bool down = (GetAsyncKeyState(s_bindVk[role]) & 0x8000) != 0;
            if (down == s_keys[i].prev) continue;
            s_keys[i].prev = down;
            bool wasWasd = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
            switch (role) {
                case KR_FORWARD: s_wHeld = down; break;
                case KR_LEFT:    s_aHeld = down; break;
                case KR_BACK:    s_sHeld = down; break;
                case KR_RIGHT:   s_dHeld = down; break;
                case KR_BLOCK:                                 // manual block (hold + timed press)
                    if (!s_directionalCombat) {
                        s_blockHeld = down;
                        if (down) s_blockPressMs = GetTickCount64();
                    }
                    break;
                case KR_ATTACK:                                // manual attack (press)
                    if (!s_directionalCombat) {
                        s_attackHeld = down;
                        if (down) s_attackPressMs = GetTickCount64();
                    }
                    break;
            }
            if (!wasWasd && (s_wHeld || s_aHeld || s_sHeld || s_dHeld))
                s_wasdTapStartMs = GetTickCount64();
            if (down) onPress(role);
        }
        // The same physical buttons remain vanilla UI clicks outside manual
        // FP/OTS combat. The edge flags below are also used by old DC features.
        bool mouseCombat = mouseCombatContext();
        bool lmbDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        bool rmbDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        if (s_directionalCombat && s_mode == MODE_FREE_MOVE
            && ((lmbDown && !s_lmbPrev) || (rmbDown != s_rmbPrev)))
        {
            ULONGLONG age = s_recentMouseLastMs ? GetTickCount64() - s_recentMouseLastMs : 0;
            char tb[220];
            sprintf_s(tb, sizeof(tb), "[IronwindTrace] mouse_input l=%d r=%d context=%d manual=%d ots=%d fp=%d ui=%d dir=%d mouse_age=%llu",
                      lmbDown ? 1 : 0, rmbDown ? 1 : 0, mouseCombat ? 1 : 0,
                      s_manualAttackOn ? 1 : 0, s_otsCamActive ? 1 : 0,
                      s_firstPersonActive ? 1 : 0, s_cameraUiOpen ? 1 : 0,
                      (int)combatRecentDirection(), age);
            VerbLog(tb);
        }
        if (mouseCombat)
        {
            ULONGLONG inputNow = GetTickCount64();
            bool lmbPressed = lmbDown && !s_lmbPrev;
            bool lmbReleased = !lmbDown && s_lmbPrev;
            bool rmbPressed = rmbDown && !s_rmbPrev;
            CutDirection recent = combatRecentDirection();

            bool hitReactionLocked = InterlockedCompareExchange(
                &s_hitReactionInputLock, 0, 0) != 0;
            if (hitReactionLocked)
            {
                // Original Kenshi reaction owns the body. Attacks still die at
                // this boundary. A physically held RMB is different: preserve
                // exactly one direction and re-arm it only after the native
                // reaction ends. Releasing during the reaction cancels it, so
                // this is live hold intent rather than a buffered old click.
                if (lmbPressed || lmbReleased)
                    VerbLog("[Ironwind] hit_reaction_input_dropped");
                if (rmbDown)
                {
                    if (!s_reactionGuardHeld)
                    {
                        CutDirection guard = CUT_DEFAULT;
                        bool gestureSeen = false;
                        if (!s_settingAiBlocks)
                        {
                            if (s_mouseBlockHeld && s_blockHeld
                                && s_blockDesired != CUT_DEFAULT)
                            {
                                guard = (CutDirection)s_blockDesired;
                                gestureSeen = s_blockGestureSeen;
                            }
                            else
                            {
                                CutDirection gesture = recent;
                                if (gesture == CUT_DEFAULT)
                                    gesture = (CutDirection)s_lastAttackDirection;
                                if (gesture == CUT_DEFAULT) gesture = CUT_DOWNWARD;
                                guard = combatGuardDirectionFromGesture(gesture);
                                gestureSeen = recent != CUT_DEFAULT;
                            }
                        }
                        s_reactionGuardDesired = guard;
                        s_reactionGuardGestureSeen = gestureSeen;
                        s_reactionGuardHeld = true;
                        VerbLog("[Ironwind] reaction_guard_hold_preserved");
                    }
                }
                else if (s_reactionGuardHeld)
                {
                    s_reactionGuardHeld = false;
                    s_reactionGuardDesired = CUT_DEFAULT;
                    s_reactionGuardGestureSeen = false;
                    VerbLog("[Ironwind] reaction_guard_hold_cancelled");
                }
                s_attackAimHeld = false;
                s_attackAimCancelled = true;
                s_attackAimGestureSeen = false;
                s_attackAimStartedMs = 0;
                s_attackHeld = false;
                s_rmbCombatEdgeMs = 0;
                s_mouseBlockHeld = false;
                s_blockHeld = false;
                s_blockGestureSeen = false;
            }
            else
            {
                // A release always invalidates a reaction-held request even if
                // the game thread has not yet observed the native end frame.
                if (!rmbDown)
                {
                    s_reactionGuardHeld = false;
                    s_reactionGuardDesired = CUT_DEFAULT;
                    s_reactionGuardGestureSeen = false;
                }
                if (lmbPressed)
                {
                    s_attackAimHeld = true;
                    s_attackAimCancelled = false;
                    s_attackHeld = true;
                    s_attackAimStartedMs = inputNow;
                    s_attackAimHoldLocked = false;
                    s_attackAimGestureSeen = recent != CUT_DEFAULT;
                    s_attackAimDirection = s_attackAimGestureSeen
                        ? recent : (CutDirection)s_lastAttackDirection;
                    char tb[144]; sprintf_s(tb, sizeof(tb),
                        "[Ironwind] attack_aim_begin dir=%d gesture=%d",
                        (int)s_attackAimDirection, s_attackAimGestureSeen ? 1 : 0);
                    VerbLog(tb);
                }
                if (lmbDown && s_attackAimHeld && recent != CUT_DEFAULT)
                {
                    // v38 sticky preview: the FIRST gesture of a hold selects
                    // freely (12-count deadzone as before); after that, small
                    // return strokes (recentering the hand / camera-look drift in
                    // a fresh gesture window) must not flip the shown direction
                    // before release - changing it needs a deliberate, much
                    // larger stroke in the current window (user 2026-10-20).
                    bool canChange = !s_attackAimHoldLocked
                                  || s_attackAimDirection == CUT_DEFAULT
                                  || recent == (CutDirection)s_attackAimDirection;
                    if (!canChange)
                    {
                        LONG gx = InterlockedCompareExchange(&s_recentMouseX, 0, 0);
                        LONG gy = InterlockedCompareExchange(&s_recentMouseY, 0, 0);
                        if (gx * gx + gy * gy >= 40 * 40) canChange = true;
                    }
                    if (canChange)
                    {
                        s_attackAimDirection = recent;
                        s_attackAimGestureSeen = true;
                        s_attackAimHoldLocked = true;
                    }
                }

                // A right press during the input-only aim is a feint/cancel.  The
                // same press continues into the normal guard path below, so the
                // transition is immediate without starting or truncating a clip.
                if (rmbPressed && s_attackAimHeld)
                {
                    s_attackAimHeld = false;
                    s_attackAimCancelled = true;
                    s_attackHeld = false;
                    VerbLog(s_settingAiBlocks
                        ? "[Ironwind] attack_aim_cancel mode=automatic_ai"
                        : "[Ironwind] attack_aim_cancel_to_guard");
                }

                if (lmbReleased)
                {
                    s_attackHeld = false;
                    if (s_attackAimHeld && !s_attackAimCancelled)
                    {
                        CutDirection commit = (CutDirection)s_attackAimDirection;
                        if (commit == CUT_DEFAULT) commit = recent;
                        if (commit == CUT_DEFAULT) commit = (CutDirection)s_lastAttackDirection;
                        if (commit == CUT_DEFAULT) commit = CUT_DOWNWARD;
                        s_attackDesired = commit;
                        s_lastAttackDirection = commit;
                        s_attackPressMs = inputNow;
                        char tb[176]; sprintf_s(tb, sizeof(tb),
                            "[Ironwind] attack_release_commit dir=%d held_ms=%llu",
                            (int)commit, inputNow - s_attackAimStartedMs);
                        VerbLog(tb);
                    }
                    s_attackAimHeld = false;
                    s_attackAimCancelled = false;
                    s_attackAimStartedMs = 0;
                    s_attackAimGestureSeen = false;
                }
                // In the combat layer RMB is always guard. The game thread owns
                // the edge so block state is never mutated from the poll thread.
                if (rmbPressed)
                    s_rmbCombatEdgeMs = inputNow;
                else if (!rmbDown && s_rmbPrev)
                {
                    s_mouseBlockHeld = false;
                    s_blockHeld = false;
                    s_blockGestureSeen = false;
                }
            }
        }
        else
        {
            s_reactionGuardHeld = false;
            s_reactionGuardDesired = CUT_DEFAULT;
            s_reactionGuardGestureSeen = false;
            if (s_mouseBlockHeld) s_blockHeld = false;
            s_mouseBlockHeld = false;
            s_blockGestureSeen = false;
            s_attackAimGestureSeen = false;
            s_recentMouseLastMs = 0;
            InterlockedExchange(&s_combatGestureDirection, CUT_DEFAULT);
            s_attackAimHeld = false;
            s_attackAimCancelled = false;
            s_attackHeld = false;
            if (s_mode != MODE_FREE_MOVE || !s_manualAttackOn || s_dcShutdownInProgress)
            {
                s_attackQueuedAtMs = 0;
                s_attackQueuedDirection = CUT_DEFAULT;
            }
        }

        // RMB press edge — earliest player-click signal (see global note).
        {
            if (rmbDown && !s_rmbPrev && !mouseCombat)
            {
                s_rmbPressedEdge = true;
                // Native interaction owns this click completely.  In ordinary
                // non-combat cursor contexts the old right-click focus remains.
                if (!s_interactionMenuByE)
                    s_rmbAimPressMs = GetTickCount64();
            }
            s_rmbPrev = rmbDown;
        }

        // LMB double-click edge — two left-press edges within the OS double-click
        // time gate the DC control-switch (see s_lmbDoubleClickMs).  The 50ms poll
        // reliably separates the two down-edges of a normal double-click (~200-
        // 400ms apart); a single click sets only s_lastLmbDownMs and never the
        // double-click timestamp.
        {
            if (lmbDown && !s_lmbPrev && !mouseCombat && !s_interactionMenuByE)
            {
                ULONGLONG nowLB = GetTickCount64();
                if (s_lastLmbDownMs > 0
                    && (nowLB - s_lastLmbDownMs) <= (ULONGLONG)GetDoubleClickTime())
                    s_lmbDoubleClickMs = nowLB;   // double-click
                else
                    s_lmbDoubleClickMs = 0;       // fresh single — clear any stale double
                s_lastLmbDownMs = nowLB;
            }
            s_lmbPrev = lmbDown;
        }
    }
}

// -----------------------------------------------------------------------
// HUD — disabled for reload stability
// -----------------------------------------------------------------------
// -----------------------------------------------------------------------
// World state — main thread only
// -----------------------------------------------------------------------
static Character*    s_selectedCharacter = nullptr;
static CharMovement* s_selectedMovement  = nullptr;
static Character*    s_freeMoveAnchor    = nullptr;
static bool          s_wasdWasActive     = false;
static bool          s_combatWASDLogged  = false;
static bool          s_retreatLogged     = false;

static bool          s_squadThreat         = false;
static bool          s_consciousAllyThreat = false;

// Cached movement pointer — used by charMovUpdate_hook without dereferencing
// s_freeMoveAnchor (which may be freed during a LOADGAME transition).
static CharMovement* s_anchorMovement = nullptr;

static volatile bool s_loadGuardActive      = false;
static int           s_stabilizationCountdown = 0;
static bool          s_postLoadReacquire   = false;

// Stable, read-only interoperability boundary. Other plugins may use this to
// hand an existing autonomous order back exactly when Ironwind stops owning a
// character. It exposes no internal combat state and performs no mutation.
extern "C" __declspec(dllexport) bool __cdecl
IronwindCombat_IsManualControlActive(Character* character)
{
    return character && character == s_freeMoveAnchor &&
        s_mode == MODE_FREE_MOVE && !s_dcShutdownInProgress && !s_loadGuardActive;
}

// DC pointer-loss state — entered when any load signal fires while s_userWantsDC is true.
// Injection pauses; mode stays FREE_MOVE; reacquire loop runs until pointers are valid.
// Times out to hard shutdown only if pointers stay invalid beyond the squad-loss threshold.
static bool           s_dcPtrLossActive      = false;
static ULONGLONG      s_reacquireMs          = 0;      // build 65: tick of the last post-load reacquire (stale-anchor re-poll window)
static bool           s_streamLogged         = false;  // build 67: one log per chunk stream

// Build 67: a REAL load (save / import / new game) vs a chunk STREAM.  Both raise
// isLoadingFromASaveGame (log 2026-09-30: every chunk crossing did, 0.4-8 s, and
// the character object survived all of them - same pointer before and after -
// while true loads took ~16 s and rebuilt it).  A real load sets the SaveManager
// signal before teardown (the same mechanism the NEWGAME detection uses) and/or
// drops the player object; a stream does neither.  During a stream the mod keeps
// control: WASD keeps driving and the OTS / FP camera keeps following, in all
// three views.  The caller additionally checks the anchor is still in the live
// update list (dcInUpdateList) - if it ever vanishes mid-stream, the real-load
// path takes over exactly as before.
static bool dcInUpdateList(GameWorld* gw, Character* c);   // fwd decl (defined with mainLoop_hook)
static bool      s_realLoadLatched = false;   // build 74: SaveManager signal seen (LOAD/NEW/IMPORT) - held until the load flag drops
static ULONGLONG s_loadFlagLogMs   = 0;
static bool      s_loadFlagPrev    = false;   // build 75: load flag last frame (falling-edge latch release)
static ULONGLONG s_latchSetMs      = 0;
static bool dcRealLoad()
{
    if (!ou) return true;
    if (!ou->isLoadingFromASaveGame()) return false;
    if (!ou->player) return true;
    if (s_realLoadLatched) return true;
    SaveManager* sm = SaveManager::getSingleton();
    int sig = sm ? sm->signal : 0;
    if (sig == SaveManager::LOADGAME || sig == SaveManager::NEWGAME || sig == SaveManager::IMPORTGAME) return true;
    return false;
}
static ULONGLONG      s_dcPtrLossStartedAt   = 0;  // when pointer loss began (for duration logging)
static ULONGLONG      s_dcPtrLossLastLogTick = 0;  // throttle for periodic duration log
static MoveSpeed      s_dcPreservedSpeedMode  = WALK;
static bool          s_enemyTargetingLogged = false;
static ULONGLONG     s_lastScanTick        = 0;

static ControlMode   s_fmTrackedMode = MODE_VANILLA;

// CombatClass state tracking
static bool           s_wasPrevInCombat  = false;
static ULONGLONG      s_wasdReleasedTick = 0;

// Combat engagement tracking
static Character*     s_prevAttackTarget  = nullptr;
static bool           s_prevTargetInRange = false;

// Protected animation state tracking (knockdown / get-up / stagger)
static bool           s_wasProtectedState = false;

// Instant-stop: set when WASD movement is applied, cleared on release.
static bool           s_wasdMovementApplied = false;

// Attack commitment — protects CHOP_WEAPON from early interruption

// Healing job active — set/cleared by addJob/removeJob hooks.
// True while any medical job is running on the anchor; DC suspends
// movement injection so the animation is not interrupted.
static bool           s_healingJobActive = false;
// Pending: a medical job arrived while WASD was held.
// Promoted to s_healingJobActive in pre-AI once WASD is released.
static bool           s_healingJobPending = false;

// WASD retreat state — tracks active retreat for suppression diagnostics
static bool          s_wasdRetreatActive      = false;
static bool          s_retreatCleanLogged     = false;
static ULONGLONG     s_retreatActiveStartTick  = 0;
static ULONGLONG     s_lastCombatEnterExitTick = 0;
static bool          s_combatFlickerLogged    = false;

// Post-WASD grace period — suppress combat re-entry after WASD release
static const ULONGLONG POST_WASD_GRACE_MS          = 2000;
static const float     POST_WASD_REENGAGEMENT_RANGE = 200.0f;
static bool            s_postWasdGraceActive        = false;
static ULONGLONG       s_postWasdGraceStart          = 0;
static bool            s_combatReentryAllowed        = false;

// CombatClass::_NV_go suppression — set when go() was skipped this frame
static bool            s_retreatLockGoSuppressed     = false;
// Sticky: once go() is suppressed during a WASD hold, stays true until release
static bool            s_retreatLockEverActive       = false;

// DC OWNERSHIP-HANDOFF COMBAT MODEL (user req 2026-06-21): the controlled
// character's combat AI (CombatClass::_NV_go) runs FULLY AUTONOMOUSLY whenever
// WASD is NOT held, and is SUSPENDED the moment WASD is held (movement owns the
// character) — see combatGo_hook.  No per-frame tug-of-war = no stutter/slide.

// Downed/crippled movement tracking — set each frame applyDownedMovement fires
static bool            s_wasdDownedMovementActive    = false;

// SLAVE OBEDIENCE (v1.4.1) — order-driven WASD while the anchor is an obeying
// slave (see isObeyingSlave / applySlaveMovement).
static Ogre::Vector3   s_slaveLastDir                = Ogre::Vector3::ZERO;
static Ogre::Vector3   s_slaveLastDest               = Ogre::Vector3::ZERO;
static ULONGLONG       s_slaveLastIssueMs            = 0;
static bool            s_slaveOrderActive            = false;   // a WASD move order is live
static bool            s_slaveModeLogged             = false;   // edge log per WASD press-run
static ULONGLONG       s_lastModOrderMs              = 0;       // DIAG: last playerMoveOrderDefault WE issued
static int             s_lastSlaveState              = -1;      // DIAG: isSlave() edge tracking

// Play-dead exit — once per WASD press; reset when all keys released
static bool            s_playDeadExitDone            = false;

// Combat WASD grace ("absolute movement priority", user req 2026-06-20): while
// in combat, the AI must not react/re-orient until the player has fully released
// WASD for ~half a second.  Rolling between keys (W->A->S->D) or a brief pause
// otherwise instant-stops, letting the combat AI grab the frame to square up to
// the attacker = the "movement pauses / character distracted in combat" reports.
// For this long after the last real key, keep driving the LAST direction so the
// AI never gets a re-orient frame.  Combat-only so out-of-combat stops stay crisp.
// Used by BOTH the charMovUpdate locomotion bridge AND the combatGo_hook ownership
// handoff: for this long after the last real key, movement still owns the character
// (AI stays suspended) so a key-roll doesn't hand control back mid-roll.  250ms
// covers key-rolls while letting the AI resume combat promptly after a real stop
// (in the ownership-handoff model a long grace would delay autonomous combat).
// 140 -> 500 (absolute-priority era) -> 250 (ownership-handoff model).  Tunable.
static const ULONGLONG COMBAT_WASD_BRIDGE_MS         = 250;

// Medical job suppression — set once when any medical job is blocked during a WASD hold,
// cleared on WASD release.  Prevents repeated per-frame log spam.
static bool            s_medicalJobSuppressedThisHold = false;

// Multi-enemy tracking
static int             s_retreatBlockedAttackerCount  = 0;
static int             s_retreatTargetsProcessed      = 0;
static int             s_retreatTargetsCachedSkipped  = 0;
static int             s_lastKnownEnemyCount          = 0;

// Performance: throttle step-7 job removal to once per 250 ms.
static const ULONGLONG JOB_REMOVAL_INTERVAL_MS = 250;
static ULONGLONG       s_jobRemovalLastTick     = 0;

// movement_injection_allowed throttle — emit at most once per second.
static ULONGLONG       s_movInjLogTick         = 0;
// Athletics XP bridge throttle — ticks at 1 s intervals; 0 = timer not yet armed.
static ULONGLONG       s_athleticsXpLastTick   = 0;

// AI MOTION FEED runtime state (v1.3.2) — see the doc block above
// charMovUpdate_hook.  Declared here so clearAllState can reset them (flags only —
// clearAllState must never touch CharMovement, its pointers may be stale).
static bool          s_aiFeedWrote   = false;  // fields hold fed values (restore due)
static ULONGLONG     s_aiFeedLogTick = 0;
// Mode publish (2026-08-22 disassembly finding): CombatMovementController::
// chasingModeCheck (real 0x2AE0A0, called per-frame from combatMovementUpdate)
// only engages chase-and-strike vs a target whose CharMovement movementMode
// (+0x378) == MOVE_NORMAL, officiallyStopped (+0x8) == 0 and currentSpeed > 9.0
// sustained 1.5 s (timer at controller+0x4C resets on any failed gate).  The DC
// anchor is MOVE_DIRECTION by construction → the gate fails EVERY frame → enemies
// ring-hold and never attack a moving anchor.  Publish MOVE_NORMAL (+ not-stopped)
// post-update alongside the velocity feed; restore pre-update.
static bool          s_aiFeedModePublished = false;
static bool          s_aiFeedSavedStopped  = false;
// v2 (2026-08-22 late): CharMovement::update runs on engine SUBSTEPS — multiple
// calls with odd dt per render frame (the 3x-speed outliers, and why the v1
// per-call publish got wiped: a substep restored engine state and its re-publish
// was skipped by the dt/discontinuity gates → the AI mostly saw MOVE_DIRECTION).
// v2 computes velocity ONCE per render frame in mainLoop (frame-to-frame pos
// delta — substep-immune) and publishes unconditionally on every anchor update
// call AND once at end-of-mainLoop (the frame's last word).
static float         s_aiPubSpd = 0.0f;
static Ogre::Vector3 s_aiPubVel(0.0f, 0.0f, 0.0f);
static Ogre::Vector3 s_aiPubLastPos(0.0f, 0.0f, 0.0f);
static bool          s_aiPubValid = false;
// True only when the inject path ACTUALLY drove the anchor this frame — the
// end-of-mainLoop publish keys on this, NOT on s_wasdMovementApplied: that flag
// legitimately stays set through a committed-action release (stagger/parry skips
// the instant stop), and publishing "moving, MOVE_NORMAL, not stopped" onto a
// released character kept it WALKING (field 2026-08-23: sticky WASD after
// release while being beaten).
static bool          s_aiPubDroveThisFrame = false;
// Locomotion feel: direction tracking for turn detection and grace window.
static Ogre::Vector3   s_prevWasdDir           = Ogre::Vector3::ZERO;
static ULONGLONG       s_wasdLastHeldMs        = 0;
// Camera lock: saved freecam state from before DC was activated; restored on DC exit.
static bool            s_savedFreeCameraMode       = false;
// Camera-lock suspension states — DC stays active but tracking is paused.
static bool            s_cameraLockInvSuspend      = false;  // suspended during inventory UI
static bool            s_cameraLockTurretSuspend   = false;  // suspended during turret/mounted use
static bool            s_menuSuspendActive         = false;  // suspended while ou->isPaused() (escape/options/save/load menus)

// -----------------------------------------------------------------------
// Hybrid post-WASD hold (v1.5) — Direct Control is a hybrid play style:
// vanilla point-click movement and orders work normally in V-mode; WASD
// overrides them while pressed; after WASD release the character HOLDS
// where WASD left them until the player gives a new point-click, presses
// WASD again, or toggles V off.  The hold is "WASD parked the character
// here", never "DC owns all locomotion".
//   s_wasdHoldActive: set once at the WASD release edge; cleared by a real
//     player click (playerMove dispatcher, RVA 0x7F95F0), the next WASD
//     press, V transitions, anchor switch, and clearAllState.
//   s_holdPos/s_holdPosValid: X/Z position clamp anchor — motion zeroing
//     alone is too early for indoor routing systems that write position
//     later in the frame, so while holding, the position is restored both
//     post-orig in charMovUpdate and at the end of mainLoop.  Y stays free
//     for gravity/ramp settling.  Invalidated whenever the hold is not
//     enforcing (stale anchor would teleport-snap).
//   Door suppression: door-type addJob/addOrder on the anchor are swallowed
//     ONLY while the hold is active — the window where no player intent
//     exists and stale indoor door tasks used to fire (auto-open bug).
//     Vanilla door behavior everywhere else.  MOVE_CUS_ORDERED is never
//     suppressed (DC's own disengage orders use it).
//   DO NOT hook CharBody::setCurrentAction (KenshiLib error 8 → crash).
// -----------------------------------------------------------------------
static bool           s_wasdHoldActive          = false;
// Active player point-click/order — set in the real click dispatcher
// (playerMove_hook), cleared at the WASD press edge (WASD wins), the WASD
// release edge (the release anchor-snap cancels the order anyway), V
// transitions, anchor switch, and clearAllState.  The hold may only
// enforce when this is false: a live player click always outranks the
// hold, regardless of which was set first.
static bool           s_playerPointClickActive  = false;
static Ogre::Vector3  s_holdPos                 = Ogre::Vector3::ZERO;
static bool           s_holdPosValid            = false;
static bool           s_idleHoldEngaged         = false;  // log edge tracking
static ULONGLONG      s_authGateLogTick         = 0;      // gate log heartbeat
static bool           s_authGateLastAllow       = true;   // gate log change detect
static char           s_authGateLastReason[24]  = "";
static ULONGLONG      s_doorSuppressLogTick     = 0;      // door-suppress log throttle
static ULONGLONG      s_addOrderDiagLogTick     = 0;      // addorder-during-hold diag throttle
// DC camera focus offset: saved objectCurrentlyFollowingOffset.y from DC entry; restored on DC exit.
static float           s_savedCamFollowOffY        = 0.0f;

// Performance: session cache — each attacker blocked exactly once per WASD hold.
// No TTL: valid for the entire retreat lock; cleared on WASD release.
// 256 entries covers large guard squads without repeated miss-scans.
static const int  RETREAT_CACHE_SIZE       = 256;
static Character* s_retreatSessionCache[RETREAT_CACHE_SIZE];
static int        s_retreatSessionCacheCount = 0;

// -----------------------------------------------------------------------
// clearAllState
// -----------------------------------------------------------------------
static void otsRestoreNames();   // fwd decl — defined with the OTS camera code
static void clearAllState()
{
    s_mode              = MODE_VANILLA;
    s_wHeld             = false;
    s_aHeld             = false;
    s_sHeld             = false;
    s_dHeld             = false;
    s_lmbDoubleClickMs  = 0;
    s_fSelectEdge       = false;
    s_selectedCharacter = nullptr;
    s_selectedMovement  = nullptr;
    s_freeMoveAnchor    = nullptr;
    s_anchorMovement    = nullptr;
    s_wasdWasActive     = false;
    s_combatWASDLogged  = false;
    s_retreatLogged     = false;
    s_squadThreat       = false;
    s_consciousAllyThreat = false;
    s_enemyTargetingLogged = false;
    s_lastScanTick      = 0;
    s_fmTrackedMode     = MODE_VANILLA;
    s_syncSpeedToAnchor = false;   // SPEEDSYNC-TWEAK
    s_stabilizationCountdown = 0;
    s_wasPrevInCombat   = false;
    s_wasdReleasedTick  = 0;
    s_prevAttackTarget  = nullptr;
    s_prevTargetInRange = false;
    s_wasProtectedState = false;
    s_wasdMovementApplied    = false;
    s_healingJobActive       = false;
    s_wasdRetreatActive      = false;
    s_retreatCleanLogged     = false;
    s_retreatActiveStartTick  = 0;
    s_lastCombatEnterExitTick = 0;
    s_combatFlickerLogged    = false;
    s_postWasdGraceActive      = false;
    s_postWasdGraceStart       = 0;
    s_combatReentryAllowed     = false;
    s_retreatLockGoSuppressed     = false;
    s_retreatLockEverActive       = false;
    s_blockHeld                   = false;   // manual block: drop everything on teardown
    s_mouseBlockHeld              = false;
    InterlockedExchange(&s_recentMouseX, 0);
    InterlockedExchange(&s_recentMouseY, 0);
    InterlockedExchange(&s_combatGestureDirection, CUT_DEFAULT);
    s_recentMouseStartMs          = 0;
    s_recentMouseLastMs           = 0;
    s_attackDesired               = CUT_DEFAULT;
    s_attackDesiredForSwing       = CUT_DEFAULT;
    s_blockDesired                = CUT_DEFAULT;
    manualAttackClearAimInput(false, true);
    s_blockGestureSeen            = false;
    s_rmbCombatEdgeMs             = 0;
    s_rmbCombatEdgeSeenMs         = 0;
    manualInteractionClearRuntime(true);
    s_allowTalkCursor             = false;
    s_xhairTint                   = 0;
    s_hitMarkerUntilMs            = 0;
    s_lastAttackDirection         = CUT_DOWNWARD;
    s_blockHeldPrev               = false;   // (never dereference s_blockOrderChar here -
    s_blockOrderApplied           = false;   //  the world may be gone; the order is save
    s_blockOrderChar              = nullptr; //  state the player can also clear by button)
    s_blockInjectedPressMs        = 0;
    s_autoBlockAttemptWho         = nullptr;
    s_autoBlockAttemptStartMs     = 0;
    s_autoBlockActiveTech         = nullptr;
    s_blockAcceptedPressMs        = 0;
    s_blockSeenPressMs            = 0;
    s_blockCooldownUntilMs        = 0;
    s_blockHoldStartMs            = 0;
    s_blockReleaseMs              = 0;
    s_staggerStartMs              = 0;
    manualHitReactionClearRuntime();
    s_hitReactionSerial           = 0;
    s_staggerRawSinceMs           = 0;
    s_freeBlockPressMs            = 0;
    s_freeGuardUpperIdle          = false;
    s_freeGuardCombatOwned         = false;
    s_freeBlockTech               = nullptr;
    s_freeAnimCombatMode          = false;
    s_weaponDrawAttemptMs         = 0;
    s_weaponMissingLogged         = false;
    s_weaponDrawInProgress        = false;
    s_attackHeld                  = false;
    s_attackCommitUntilMs         = 0;
    s_attackSeenPressMs           = 0;
    s_attackInjectedPressMs       = 0;
    s_attackAcceptedPressMs       = 0;
    s_attackEngagePressMs         = 0;
    manualAttackClearActiveRuntime();
    s_attackRecoveryUntilMs       = 0;
    manualAttackClearQueuedInput(true);
    for (int i = 0; i < 4; ++i) s_dirMoveAvailable[i] = -1;   // live attack-direction availability cache
    s_dirMoveCheckedMs           = 0;
    s_dirMoveAnchor              = nullptr;
    s_dirMoveWeaponType          = -2;
    s_dirMoveHands               = -1;
    s_freeAttackTech              = nullptr;
    s_freeAttackStartMs           = 0;
    s_freeAttackSeenAnim          = false;
    s_freeAttackLastProgress      = -1.0f;
    s_freeAttackLastAdvanceMs     = 0;
    s_lastFreeAttackTech          = nullptr;
    s_attackIntentTarget          = nullptr;
    manualAttackClearNeutralIntent();
    for (int wi = 0; wi < 8; ++wi)
    {
        s_attackWorldNotified[wi] = nullptr;
        s_attackNeutralContact[wi] = nullptr;
        s_victimResponseVictim[wi] = nullptr;
        s_victimResponseAttacker[wi] = nullptr;
        s_victimResponseSource[wi] = nullptr;
    }
    s_freeAttackPressMs           = 0;
    s_freeAttackImpactPassed      = false;
    s_attackIntentPressMs         = 0;
    s_manualAttackOn              = false;   // runtime state: re-toggle after a load
    s_lockTarget                  = nullptr;
    s_aimLogged                   = nullptr;
    s_aimClicked                  = false;
    s_rmbAimSeenMs                = 0;
    for (int gi = 0; gi < INCOMING_ATTACK_MAX; ++gi) s_incomingAttack[gi].who = nullptr;
    s_wasdDownedMovementActive    = false;
    s_slaveOrderActive            = false;
    s_slaveModeLogged             = false;
    s_playDeadExitDone            = false;
    // Inventory face-cam: load/teardown — the scene (and our detached node) is
    // gone, so make NO camera calls here; just drop the runtime flags/pointers.
    s_fpActive                    = false;
    s_fpNode                      = nullptr;
    s_fpCursorCaptured            = false;
    s_fpCamLocalsSaved            = false;
    s_fpHadAutoTrack              = false;
    // First-person: scene (and our detached node/skeleton) is gone — drop the
    // runtime flags/pointers only, make NO camera/skeleton/GUI calls here.
    // Restoring the grass/foliage draw-range IS safe (just writes the options
    // globals) and must happen or a teardown that bypassed exitFirstPerson would
    // leave the range permanently boosted.
    if (s_fpOptRangeSaved && options)
    {
        options->grassRange   = s_fpSavedGrassRange;
        options->foliageRange = s_fpSavedFoliageRange;
        s_fpOptRangeSaved     = false;
    }
    s_firstPersonActive           = false;
    s_otsResumeZoomPending       = false;
    s_fpToggleRequested           = false;
    s_sneakToggleRequested        = false;
    s_fpEnemyClearSmooth          = 1.0f;
    s_fpEnemyNearestDist          = -1.0f;
    s_fpSuspendedForInv           = false;
    s_fpHeadBoneHidden            = false;
    for (int hbI = 0; hbI < 2; ++hbI)        // flags only — skeleton may be gone
        s_fpHideBoneSaved[hbI]    = false;
    s_fpHairHidden                = false;
    s_fpPixelFallbackOn           = false;    // re-probe the shader when FP resumes
    s_fpHiddenBeardCount          = 0;        // names only — entities may be gone
    s_fpHeadSmoothValid           = false;
    otsRestoreNames();            // re-show name-tags if a teardown left them hidden
    s_otsRestorePending           = false;
    s_invFaceCloseStreak          = INV_FACE_CLOSE_DEBOUNCE;
    s_otsInvFaceActive            = false;
    s_otsInvFaceChar              = nullptr;
    s_otsSavedYaw                 = 0.0f;
    s_otsSavedPitch               = 0.0f;
    s_otsSavedDist                = 14.0f;
    s_medicalJobSuppressedThisHold = false;
    s_retreatBlockedAttackerCount  = 0;
    s_retreatTargetsProcessed      = 0;
    s_retreatTargetsCachedSkipped  = 0;
    s_lastKnownEnemyCount          = 0;
    s_jobRemovalLastTick           = 0;
    s_retreatSessionCacheCount     = 0;
    s_lootUiSuspendActive          = false;
    s_lootUiWasPrevOpen            = false;
    s_tradeWindowActive            = false;
    s_lootSuspendStartTick         = 0;
    s_invMoveThroughActive         = false;
    s_invMoveThroughForcedRun      = false;
    s_invMoveThroughPlayerPaused   = false;
    s_invPausedBeforeOpen          = false;
    s_invMoveThroughShownChar      = nullptr;
    s_invMoveThroughEdgeTick       = 0;
    s_invTradeCloseRequested       = false;
    s_invTradeStartValid           = false;
    s_movInjLogTick                = 0;
    s_athleticsXpLastTick          = 0;
    s_aiFeedWrote                  = false;   // flags only — never touch CharMovement here
    s_aiFeedLogTick                = 0;
    s_aiFeedModePublished          = false;
    s_aiFeedSavedStopped           = false;
    s_aiPubValid                   = false;
    s_aiPubSpd                     = 0.0f;
    s_aiPubVel                     = Ogre::Vector3::ZERO;
    s_aiPubLastPos                 = Ogre::Vector3::ZERO;
    s_aiPubDroveThisFrame          = false;
    s_prevWasdDir                  = Ogre::Vector3::ZERO;
    s_wasdLastHeldMs               = 0;
    s_savedFreeCameraMode          = false;
    s_prof_mainLoop                = 0;
    s_prof_charMove                = 0;
    s_prof_playerControl           = 0;
    s_prof_committedAct            = 0;
    s_prof_threatScan              = 0;
    s_prof_cameraLock              = 0;
    s_prof_wasdInject              = 0;
    s_prof_combatTarget            = 0;
    s_nearbyEnemyCount             = 0;
    s_chaseFlapsCount              = 0;
    s_pathfindingEnemyCount        = 0;
    s_cameraLockInvSuspend         = false;
    s_cameraLockTurretSuspend      = false;
    s_menuSuspendActive            = false;
    s_wasdHoldActive          = false;
    s_playerPointClickActive  = false;
    s_holdPos                 = Ogre::Vector3::ZERO;
    s_holdPosValid            = false;
    s_idleHoldEngaged         = false;
    s_authGateLogTick         = 0;
    s_authGateLastAllow       = true;
    s_authGateLastReason[0]   = '\0';
    s_doorSuppressLogTick     = 0;
    s_addOrderDiagLogTick     = 0;
    s_savedCamFollowOffY           = 0.0f;
    s_wasdTapStartMs               = 0;
    s_userWantsDC           = false;
    s_userWantsFP           = false;   // hard teardown clears FP intent (survivable loads preserve it)
    s_dcPtrLossActive       = false;
    s_dcPtrLossStartedAt    = 0;
    s_dcPtrLossLastLogTick  = 0;
    s_hookBlockLoggedMain    = false;
    s_hookBlockLoggedCharMov = false;
    s_hookBlockLoggedPCtrl   = false;
    s_hookBlockLoggedRemJob  = false;
    s_hookBlockLoggedAddJob  = false;
    s_shutdownWaitLogTick    = 0;
    s_healingJobPending      = false;
}

// -----------------------------------------------------------------------
// computeWASDDirection — camera-relative direction helper.
// -----------------------------------------------------------------------
static Ogre::Vector3 s_lastCamFwd = Ogre::Vector3::ZERO;   // build 63: last known camera basis (chunk loads)
static bool computeWASDDirection(bool bW, bool bA, bool bS, bool bD, Ogre::Vector3& outDir)
{
    if (!ou) return false;
    bool haveCam = ou->player && ou->player->camera;
    Ogre::Vector3 camFwd;
    if (s_firstPersonActive)
    {
        // First-person: the game camera controller is detached and stale — the
        // real view heading lives in s_fpYaw (drives mouse-look + the rendered
        // camera orientation).  Basis must match it so W follows the gaze.
        camFwd = Ogre::Vector3(-sinf(s_fpYaw), 0.0f, -cosf(s_fpYaw));
    }
    else if (s_otsCamActive)
    {
        // Detached OTS: same story — the vanilla camera's facing is stale, our
        // view heading is s_otsYaw.  Match it so WASD is camera-relative to the
        // OTS view exactly like the normal DC camera.
        camFwd = Ogre::Vector3(-sinf(s_otsYaw), 0.0f, -cosf(s_otsYaw));
    }
    else if (haveCam)
    {
        camFwd = ou->player->camera->getFacingDirection();
        s_lastCamFwd = camFwd;
    }
    else
    {
        camFwd = s_lastCamFwd;                                  // player object away (chunk load): keep the last basis
    }
    camFwd.y = 0.0f;
    float cflen = camFwd.length();
    if (cflen < 0.001f) return false;
    camFwd /= cflen;
    Ogre::Vector3 camRight(-camFwd.z, 0.0f, camFwd.x);
    Ogre::Vector3 move = Ogre::Vector3::ZERO;
    if (bW) move += camFwd;
    if (bS) move -= camFwd;
    if (bD) move += camRight;
    if (bA) move -= camRight;
    float mlen = move.length();
    if (mlen < 0.001f) return false;
    outDir = g_loco.normalizeDiagonalMovement ? (move / mlen) : move;
    return true;
}

// -----------------------------------------------------------------------
// wasdMoveLimit — the move-limit passed to setDirectMovement for WASD.
//
// Legacy behaviour forced ~99 (uncapped), which let shackled/injured/encumbered
// characters run at full speed and outrun enemies.  With the cap on (default), the
// limit is the character's REAL max run speed (CharStats::getMaxRunSpeed, which the
// game computes from stats/injuries/encumbrance) scaled by WasdSpeedMult, with an
// extra hard clamp while chained/shackled so the Rebirth shackle escape is a shuffle.
// The turn-responsiveness boost still applies briefly on direction changes for snappy
// turns.  Cap off (WasdSpeedCap=false) → the old uncapped limit.
static bool manualAttackMovementSlow()
{
    return s_mode == MODE_FREE_MOVE && s_manualAttackOn
        && (s_freeAttackTech != nullptr || s_attackActive);
}

static bool manualBlockMovementSlow()
{
    return s_mode == MODE_FREE_MOVE && s_manualAttackOn
        && s_settingManualBlock && s_blockHeldPrev;
}

static bool manualCombatMovementSlow()
{
    return manualAttackMovementSlow() || manualBlockMovementSlow();
}

static float wasdMoveLimit(bool turning)
{
    const float turnBoost = turning ? g_loco.wasdTurnResponsiveness : 1.0f;
    // A live V-mode swing or held guard keeps WASD responsive at 20% speed.
    // Apply this after the turn boost so changing direction cannot bypass it.
    const float actionScale = manualCombatMovementSlow() ? 0.20f : 1.0f;
    if (!s_settingWasdSpeedCap || !s_freeMoveAnchor)
        return 99.0f * g_loco.wasdAccelerationMultiplier * turnBoost * actionScale;

    float legit = 99.0f;
    CharStats* st = s_freeMoveAnchor->getStats();
    if (st)
    {
        float m = st->getMaxRunSpeed();      // injury / encumbrance aware
        if (m > 0.1f) legit = m;
    }
    // Shackles hard-limit real movement via a separate mechanism the direct-move
    // bypasses; when chained, clamp to a slow shuffle so the player can't sprint off.
    if (s_freeMoveAnchor->isChainedMode())
    {
        float shuffle = legit * 0.35f;
        if (shuffle > 6.0f) shuffle = 6.0f;   // absolute shuffle ceiling
        legit = shuffle;
    }
    // Sneaking (Shift+C in first-person, or the vanilla sneak button): vanilla
    // movement is capped at the stealth-skill speed; mirror it so WASD-sneak is
    // exactly as fast as the game's own sneak movement, no faster.
    if (st && s_freeMoveAnchor->isStealthMode())
    {
        float sneakMax = st->calculateMaxStealthSpeed();
        if (sneakMax > 0.1f && sneakMax < legit) legit = sneakMax;
    }
    legit *= s_settingWasdSpeedMult;

    // Per-second diagnostic so the real values can be read from the RE_Kenshi log.
    if (g_log.debugVerbose)
    {
        static ULONGLONG s_spdLogTick = 0;
        ULONGLONG t = GetTickCount64();
        if (t - s_spdLogTick >= 1000)
        {
            s_spdLogTick = t;
            char b[160];
            sprintf_s(b, sizeof(b),
                "[WASDCombat] wasd_speed maxRun=%.1f chained=%d mult=%.2f attack=%d block=%d limit=%.1f",
                st ? st->getMaxRunSpeed() : -1.0f,
                s_freeMoveAnchor->isChainedMode() ? 1 : 0,
                s_settingWasdSpeedMult, manualAttackMovementSlow() ? 1 : 0,
                manualBlockMovementSlow() ? 1 : 0,
                legit * turnBoost * actionScale);
            DebugLog(b);
        }
    }
    return legit * turnBoost * actionScale;
}

// applyPlayerMovement — halt() + setDirectMovement in camera-relative WASD.
// -----------------------------------------------------------------------
// v44: true only while vanilla owns the body through a real reaction state or
// a native stumble transition queued by _getHit(..., stumble=true).  No fixed
// duration is imposed: light hits remain light, and a real STUMBLE ends on the
// exact frame the native state machine leaves it.
static bool manualHitReactionLive(Character* ch)
{
    if (!ch || ch != s_freeMoveAnchor)
    {
        InterlockedExchange(&s_hitReactionInputLock, 0);
        return false;
    }
    CombatClass* cc = ch->getCombatClass();
    if (!cc)
    {
        s_hitReactionPending = false;
        InterlockedExchange(&s_hitReactionInputLock, 0);
        return false;
    }
    swordStateEnum state = cc->getCombatState();
    bool inNativeState = state == STUMBLE || state == REACTION_BLOCK;
    bool nativeQueued = s_hitReactionPending;
    if (inNativeState) s_hitReactionPending = false;
    bool live = inNativeState || nativeQueued;
    InterlockedExchange(&s_hitReactionInputLock, live ? 1 : 0);
    return live;
}

// V-mode reaction ownership is narrower than the character's native reaction
// state.  Leaving V must never carry our pending/input latch into a later V
// session (for example after KO auto-exit).  Re-entry follows the character's
// current native state instead of a clock: a live STUMBLE/REACTION_BLOCK keeps
// the lock, every other state starts clean.
static void manualHitReactionSyncVBoundary(Character* ch, bool entering)
{
    bool wasLocked = InterlockedCompareExchange(&s_hitReactionInputLock, 0, 0) != 0;
    if (!entering)
    {
        manualHitReactionClearRuntime();
        if (wasLocked)
            VerbLog("[WASDCombat] hit_reaction_lock_cleared_v_exit");
        return;
    }

    CombatClass* cc = ch ? ch->getCombatClass() : nullptr;
    swordStateEnum state = cc ? cc->getCombatState() : COMBAT_FINISHED;
    bool nativeLive = state == STUMBLE || state == REACTION_BLOCK;
    s_hitReactionPending = false;
    InterlockedExchange(&s_hitReactionInputLock, nativeLive ? 1 : 0);
    if (wasLocked && !nativeLive)
        VerbLog("[WASDCombat] hit_reaction_stale_lock_cleared_v_enter");
}

static bool applyPlayerMovement(bool bW, bool bA, bool bS, bool bD)
{
    CharMovement* mv = s_freeMoveAnchor ? s_freeMoveAnchor->movement : nullptr;
    if (!s_freeMoveAnchor || !mv) return false;
    if (!ou || !ou->player || !ou->player->camera) return false;
    if (manualHitReactionLive(s_freeMoveAnchor)) return false;   // v40: let the stagger play

    Ogre::Vector3 move;
    if (!computeWASDDirection(bW, bA, bS, bD, move)) return false;

    // Turn responsiveness: boost move limit on significant direction change.
    bool prevHasDir = (s_prevWasdDir.squaredLength() > 0.0001f);
    bool turning    = prevHasDir && (move.dotProduct(s_prevWasdDir) < 0.9f);
    float limit     = wasdMoveLimit(turning);

    mv->halt();
    mv->setDesiredSpeed(mv->speedOrders);
    if (manualCombatMovementSlow()) mv->setDesiredSpeed(limit);
    mv->setDirectMovement(move, limit);
    s_prevWasdDir = move;
    return true;
}

// -----------------------------------------------------------------------
// isProtectedAnimationState — returns true when V-Mode must not interfere.
// -----------------------------------------------------------------------
static bool isProtectedAnimationState(Character* ch)
{
    if (!ch) return false;
    ProneState prone = ch->getProneState();
    if (prone == PS_KO || prone == PS_PLAYING_DEAD) return true;
    if (ch->isDown())              return true;
    if (ch->isCurrentlyGettingUp) return true;
    CombatClass* cc = ch->getCombatClass();
    if (cc && cc->getCombatState() == STUMBLE) return true;
    return false;
}

// -----------------------------------------------------------------------
// isCommittedAction — returns true when the character is executing a vanilla
// action that DC must not interrupt.
//
// Narrowed scope (current pass): only used at instant_stop and
// combat_state_restored_after_wasd_release.  Not used at movement injection
// sites until movement logs confirm it is no longer over-broad.
//
// Committed action state set:
//   STARTUP_STATE    attack windup
//   CHOP_WEAPON      active swing
//   DECISION         attack recovery
//   BLOCK            active block
//   REACTION_BLOCK   parry
//   HESITATE         hesitation between attack cycles
//   STUMBLE          stagger (also caught by isProtectedAnimationState)
//   isDown / PS_KO / PS_PLAYING_DEAD / isCurrentlyGettingUp
//                    knockdown, unconscious, playing dead, get-up
//   s_healingJobActive  medical action in progress
// -----------------------------------------------------------------------
static bool isCommittedAction(Character* ch)
{
    ScopeTimer _tCA(s_prof_committedAct);
    if (!ch) return false;
    if (ch == s_freeMoveAnchor && s_freeAttackTech) return true;

#define _LOG_COMMITTED(reason) \
    if (g_log.debugVerbose) DebugLog("[WASDCombat] committed_action_true reason=" reason)

    ProneState prone = ch->getProneState();
    if (prone == PS_KO)
        { _LOG_COMMITTED("PS_KO");              return true; }
    if (prone == PS_PLAYING_DEAD)
        { _LOG_COMMITTED("PS_PLAYING_DEAD");    return true; }
    if (ch->isDown())
        { _LOG_COMMITTED("isDown");             return true; }
    if (ch->isCurrentlyGettingUp)
        { _LOG_COMMITTED("isCurrentlyGettingUp"); return true; }
    if (s_healingJobActive)
        { _LOG_COMMITTED("HEALING_JOB");        return true; }

    CombatClass* cc = ch->getCombatClass();
    // Gate the combat-STATE checks on combatModeActive (v1.8.4 lesson): a combat
    // state left STALE after a fight (e.g. recovering from a knockdown — the state
    // machine can sit in DECISION/STUMBLE with combatModeActive already false) must
    // NOT count as a committed action, or it blocks the release-stop and WASD
    // movement is delayed after you get up (field 2026-06-21).  The physical states
    // above (KO/down/getting-up/healing) stay ungated — they are real regardless.
    if (cc && cc->combatModeActive)
    {
        swordStateEnum st = cc->getCombatState();
        if (st == STUMBLE)
            { _LOG_COMMITTED("STUMBLE");        return true; }
        if (st == STARTUP_STATE)
            { _LOG_COMMITTED("STARTUP_STATE");  return true; }
        if (st == CHOP_WEAPON)
            { _LOG_COMMITTED("CHOP_WEAPON");    return true; }
        if (st == DECISION)
            { _LOG_COMMITTED("DECISION");       return true; }
        if (st == BLOCK)
            { _LOG_COMMITTED("BLOCK");          return true; }
        if (st == REACTION_BLOCK)
            { _LOG_COMMITTED("REACTION_BLOCK"); return true; }
        if (st == HESITATE)
            { _LOG_COMMITTED("HESITATE");       return true; }
    }

#undef _LOG_COMMITTED
    // Gated (v1.4.1): this fired UNCONDITIONALLY every frame in idle DC mode —
    // ~70 lines/sec of RE_Kenshi_log.txt bloat in every player session.
    if (g_log.debugLogging && g_log.verboseCommittedActionLogs)
        DebugLog("[WASDCombat] committed_action_false");
    return false;
}

// -----------------------------------------------------------------------
// isCommittedCombatClip — the character is mid-play in a committed one-shot combat
// CLIP that must finish before WASD movement takes over: their own attack swing
// (windup STARTUP_STATE / strike CHOP_WEAPON), a stagger from being hit (STUMBLE), or
// a parry (REACTION_BLOCK).  Kenshi exposes no way to abort an animation clip, so
// cutting one with movement looks broken / stutters (field 2026-06-22: stutter when
// retreating + after being hit and stumbled).  While this is true: the buffer in
// charMovUpdate HOLDS movement (no inject, no state touched) AND combatGo_hook lets
// go() RUN so the clip advances and finishes — exactly one system drives the body, no
// fighting.  The instant the clip ends, movement resumes (plain walk).  DECISION /
// BLOCK / CIRCLE / WAIT / HESITATE are NOT included — they persist or re-trigger
// attacks, so the player must be able to move/retreat through them.  Gated on
// combatModeActive (stale post-combat states must not count — v1.8.4 lesson).
// -----------------------------------------------------------------------
static bool manualAttackCommitLive();  // fwd decl (manual combat, below)
static void manualAimTick();           // fwd decl (manual combat, below)

// A chosen automatic guard is a native one-shot action, not an idle AI
// decision. Only the exact BLOCK initialisation state is admitted here. Kenshi
// does not expose the normal block clip through the Animation lookup used by
// free actions (v96 field log: clip_progress=-1 on every successful HIT_SWORD),
// so do not guess its lifetime or reset the pose on the state edge. v97 instead
// gives held guard exclusive stationary ownership in charMovUpdate_hook; the
// original animation system can then start and finish the pose undisturbed.
static bool manualAutoGuardBlockLive(Character* ch)
{
    if (!ch || ch != s_freeMoveAnchor || !s_autoBlockActiveTech) return false;
    CombatClass* cc = ch->getCombatClass();
    if (cc && cc->combatModeActive && cc->getCombatState() == BLOCK
        && cc->currentTechnique == s_autoBlockActiveTech) return true;

    s_autoBlockActiveTech = nullptr;
    char b[128];
    sprintf_s(b, sizeof(b), "[Ironwind] ai_guard_state_exit state=%d held=%d",
        cc ? (int)cc->getCombatState() : -1, s_blockHeldPrev ? 1 : 0);
    VerbLog(b);
    return false;
}

static bool isCommittedCombatClip(Character* ch)
{
    if (!ch) return false;
    // V's free swing is driven by manualFreeActionTick, not CombatClass::go.
    // Let WASD through during it.  Only an actual hit reaction owns locomotion.
    if (ch == s_freeMoveAnchor && s_mode == MODE_FREE_MOVE && s_manualAttackOn)
    {
        CombatClass* manualCC = ch->getCombatClass();
        if (!manualCC || !manualCC->combatModeActive) return false;
        swordStateEnum manualState = manualCC->getCombatState();
        return manualState == STUMBLE || manualState == REACTION_BLOCK
            || manualAutoGuardBlockLive(ch);
    }
    if (ch == s_freeMoveAnchor && (manualAttackCommitLive() || s_freeAttackTech)) return true;
    CombatClass* cc = ch->getCombatClass();
    if (!cc || !cc->combatModeActive) return false;
    swordStateEnum st = cc->getCombatState();
    return st == STARTUP_STATE || st == CHOP_WEAPON
        || st == STUMBLE       || st == REACTION_BLOCK;
}

// -----------------------------------------------------------------------
// isAnchoredToFurniture — the character is physically using a UseableStuff object
// (chair / throne / bed / crafting+research machine).  The low-level CharBody action
// for using one is OPERATE_MACHINERY (key 87) when you put them there directly, OR
// PRETEND_TO_OPERATE_MACHINERY (key 221) when they idled onto it via a toggled JOB
// (field diag 2026-06-22 — BOTH must be detected, else a job-sat character rotates in
// place after a squad-switch).  In this state the body is locked to the furniture
// node, so injecting setDirectMovement only ROTATES the model; when detected with WASD
// held we issue a real move order to detach them (see charMovUpdate).  Calls are
// header-declared (KenshiLib-linked) + null-checked.  SIT_AROUND/SIT_ON_THRONE/
// USE_BED*/REST are higher-level AI goals that never surface as the current action;
// kept as harmless belt-and-suspenders.
// -----------------------------------------------------------------------
static bool isSeatedTaskType(TaskType t)
{
    return t == OPERATE_MACHINERY
        || t == PRETEND_TO_OPERATE_MACHINERY   // job-driven idle-at-station (jobs toggled ON)
        || t == SIT_AROUND || t == SIT_ON_THRONE
        || t == USE_BED    || t == USE_BED_ORDER
        || t == REST;
}

static bool isAnchoredToFurniture(Character* ch)
{
    if (!ch) return false;
    if (ch->inSomething == IN_BED) return true;        // sleeping / lying in a bed
    CharBody* body = ch->getBody();
    if (body)
    {
        Tasker* action = body->getCurrentAction();
        if (action && isSeatedTaskType(action->key()))
            return true;
    }
    return false;
}

// -----------------------------------------------------------------------
// isUsingStationaryTurret — true when character is manning a turret/crossbow.
// When WASD is not held, V-Mode must not suppress aiming input.
// When WASD is held, turret use is cancelled (player takes movement authority).
// -----------------------------------------------------------------------
static bool isUsingStationaryTurret(Character* ch)
{
    if (!ch) return false;
    // isUsingTurret is a hand (reference to the turret building); truthy when valid.
    return (bool)(ch->isUsingTurret);
}

// -----------------------------------------------------------------------
// Retreat session cache — each enemy processed once per WASD hold, then
// immediately skipped on every subsequent call with zero overhead.
// Cleared on WASD release via s_retreatSessionCacheCount = 0.
// -----------------------------------------------------------------------
static bool retreatSessionCacheContains(Character* ch)
{
    for (int i = 0; i < s_retreatSessionCacheCount; ++i)
        if (s_retreatSessionCache[i] == ch) return true;
    return false;
}

static void retreatSessionCacheAdd(Character* ch)
{
    if (!ch) return;
    for (int i = 0; i < s_retreatSessionCacheCount; ++i)
        if (s_retreatSessionCache[i] == ch) return;
    if (s_retreatSessionCacheCount < RETREAT_CACHE_SIZE)
        s_retreatSessionCache[s_retreatSessionCacheCount++] = ch;
    // If full: silently drop — best-effort optimization
}

// -----------------------------------------------------------------------
// isDownedButMovable — true when character is downed/crippled/playing-dead but
// vanilla point-click movement still works (crawl/limp).  Hard blocks (truly
// unconscious, getting-up animation, stumble) return false.
// -----------------------------------------------------------------------
static bool isDownedButMovable(Character* ch)
{
    if (!ch) return false;
    ProneState prone = ch->getProneState();
    // PS_KO is the authoritative hard block — truly knocked out, cannot crawl.
    // Do NOT use isUnconcious() here: it returns true for PS_PLAYING_DEAD and
    // crippled characters in Kenshi even though point-click crawl still works.
    if (prone == PS_KO) return false;
    if (ch->isCurrentlyGettingUp) return false;
    // Playing-dead and crippled can crawl/limp via the point-click path.
    if (prone == PS_PLAYING_DEAD || prone == PS_CRIPPLED) return true;
    // Down but not KO and not getting up — conscious downed state.
    if (ch->isDown() && !ch->isUnconcious()) return true;
    return false;
}

// stableIndoors — isInsideBuildingLoadedInterior with 400 ms hysteresis.
// Stairwell/roof transitions (e.g. stormhouse interior -> roof) flicker the
// raw flag between floor layers; without debounce the crawl flaps between
// order mode and direct mode, each cancelling the other (field finding,
// 2026-06-12: "struggles to move smoothly through the layers").  Anchor-only
// state — DC controls one character at a time.
static bool      s_indoorEffective  = false;
static bool      s_indoorPendingVal = false;
static ULONGLONG s_indoorPendingMs  = 0;

static bool stableIndoors(CharMovement* mv)
{
    bool raw = mv->isInsideBuildingLoadedInterior();
    if (raw == s_indoorEffective)
    {
        s_indoorPendingMs = 0;
        return s_indoorEffective;
    }
    ULONGLONG now = GetTickCount64();
    if (s_indoorPendingMs == 0 || raw != s_indoorPendingVal)
    {
        s_indoorPendingVal = raw;
        s_indoorPendingMs  = now;
        return s_indoorEffective;
    }
    if (now - s_indoorPendingMs >= 400)
    {
        s_indoorEffective = raw;
        s_indoorPendingMs = 0;
        DebugLog(raw ? "[WASDCombat] dc_downed_zone_now_indoors"
                     : "[WASDCombat] dc_downed_zone_now_outdoors");
    }
    return s_indoorEffective;
}

// downedOrderDriven — ALWAYS false since v1.7.17: downed movement is
// direct-injected everywhere, identical to standing WASD.
// History of the crawl saga, so nobody resurrects the order mode:
//   - Orders indoors path-walk the interior network regardless of dest
//     ("directional keys are meaningless indoors", v1.7.13).
//   - Orders outdoors pathfind a blind 10 m dest; on rooftops/elevated
//     ground that dest lands off the structure and the pathfinder routes
//     back DOWN ("bounce off the roof layer", v1.7.16).
//   - Direct injection was proven downed-capable in v1.7.14 ("downed
//     movement feels great indoors") — the original downed stutter that
//     motivated orders was the combat-steering conflict + order-fighters,
//     both fixed independently (v1.7.6 flip, v1.7.11 gates).
// Order machinery (applyDownedMovement, stableIndoors, the step-5 order
// branch, the flag-gated stops) is retained dormant for rollback.
static bool downedOrderDriven(Character* ch)
{
    (void)ch;
    (void)&stableIndoors;   // keep dormant order machinery referenced
    return false;
}

// applyDownedMovement — issue point-click-equivalent order for downed/crippled
// characters.  Uses playerMoveOrderDefault (pathfind/crawl path) rather than
// setDirectMovement, which is only valid for standing locomotion.
static Ogre::Vector3 s_downedLastDir     = Ogre::Vector3::ZERO;
static Ogre::Vector3 s_downedLastDest    = Ogre::Vector3::ZERO;
static ULONGLONG     s_downedLastIssueMs = 0;
static Ogre::Vector3 s_crawlSamplePos    = Ogre::Vector3::ZERO;
static ULONGLONG     s_crawlSampleMs     = 0;

static void applyDownedMovement(bool bW, bool bA, bool bS, bool bD)
{
    if (!s_freeMoveAnchor || !s_freeMoveAnchor->movement) return;
    Ogre::Vector3 dir;
    if (!computeWASDDirection(bW, bA, bS, bD, dir)) return;
    float dlen = dir.length();
    if (dlen < 0.001f) return;
    dir /= dlen;   // pathfind dest needs direction only, never diagonal scaling

    Ogre::Vector3 posNow = s_freeMoveAnchor->movement->pos;
    ULONGLONG    nowDI   = GetTickCount64();

    // Diagnostic: once per second while crawling, compare actual motion
    // against the intended camera-relative direction.
    if (!s_wasdDownedMovementActive)
    {
        s_crawlSampleMs  = nowDI;
        s_crawlSamplePos = posNow;
    }
    else if (nowDI - s_crawlSampleMs >= 1000)
    {
        Ogre::Vector3 dmoved = posNow - s_crawlSamplePos;
        char buf[192];
        sprintf_s(buf, sizeof(buf),
            "[WASDCombat] dc_crawl_actual moved=(%.2f,%.2f,%.2f) want=(%.2f,%.2f,%.2f)",
            dmoved.x, dmoved.y, dmoved.z, dir.x, dir.y, dir.z);
        DebugLog(buf);
        s_crawlSampleMs  = nowDI;
        s_crawlSamplePos = posNow;
    }

    // Outdoors only — indoor downed movement is direct-injected (see
    // downedOrderDriven); v1.7.13's indoor short-hop attempt proved the
    // interior router path-walks ANY order regardless of distance.
    const float hopLen     = 100.0f;   // point-click range, 10 m
    const float approachAt = 60.0f;

    // ONE persistent order, like a point-click (per-frame re-issue
    // restarts pathfinding before it produces motion — the never-starts
    // stutter).  This only works because NOTHING else is allowed to fight
    // the order while a downed character holds keys: the press-edge
    // disengage and the post-AI standing injection are both downed-gated
    // (v1.7.11) — they were the hidden order-killers that made the
    // throttled crawl die after a few steps.  Re-issue on first press,
    // direction change, a 1.5 s refresh, or approach of the last dest.
    if (s_wasdDownedMovementActive
        && dir.dotProduct(s_downedLastDir) > 0.95f
        && nowDI - s_downedLastIssueMs < 1500
        && (s_downedLastDest - posNow).length() > approachAt)
        return;
    s_downedLastDir     = dir;
    s_downedLastIssueMs = nowDI;

    // Dest at point-click range.  v1.7.11 used 50 m, which is far enough
    // off the local navmesh that the path's first leg could head in the
    // wrong direction — the user-visible "directions are wrong".  Short
    // hops behave like the nearby point-clicks that are known good.
    Ogre::Vector3 dest = posNow + dir * hopLen;
    s_downedLastDest = dest;
    s_freeMoveAnchor->playerMoveOrderDefault(nullptr, nullptr, dest);
    {
        char buf[224];
        sprintf_s(buf, sizeof(buf),
            "[WASDCombat] dc_crawl_issue keys=%d%d%d%d dir=(%.2f,%.2f,%.2f) pos=(%.1f,%.1f,%.1f) dest=(%.1f,%.1f,%.1f)",
            bW?1:0, bA?1:0, bS?1:0, bD?1:0, dir.x, dir.y, dir.z,
            posNow.x, posNow.y, posNow.z, dest.x, dest.y, dest.z);
        DebugLog(buf);
    }
}

// =======================================================================
// OTS action camera — core implementation (ported from OTS_Project_Shelved).
// =======================================================================
static const float FP_RAD_PER_PIXEL = 0.0030f;
static const float FP_PITCH_LIMIT   = 1.45f;   // ~83 degrees, radians

static bool isKenshiForegroundMain()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// --- First-person raw mouse-look via DirectInput (KenshiFP method) ----------
// A second, NON-EXCLUSIVE BACKGROUND DirectInput mouse device reads the same
// high-rate relative stream the game does — Kenshi keeps its own input (we steal
// no registration, so right-click etc. still work).  A dedicated ~1kHz thread
// owns the reads and accumulates counts; each frame consumes the total.  This
// decouples look feel from framerate: the old GetCursorPos/SetCursorPos warp was
// sampled at the frame rate and felt sluggish-then-teleporty at high fps.  The
// To stay independent of the DirectX import libs (dxguid/dinput8 are not reliably
// on the v100 toolset's lib path), we resolve DirectInput8Create at runtime and
// define the GUIDs + mouse data format ourselves — exactly KenshiFP's approach.
typedef HRESULT (WINAPI *DI8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static const GUID DIFP_GUID_SysMouse =
    { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID DIFP_IID_IDirectInput8A =
    { 0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
// DIMOUSESTATE2 axes at offsets 0/4/8 (lX/lY/lZ).  NULL pguid = "any object of
// this type" → the device's relative X/Y/Z map onto these slots.
static DIOBJECTDATAFORMAT DIFP_odf[] = {
    { NULL, 0, DIDFT_AXIS | DIDFT_ANYINSTANCE, 0 },
    { NULL, 4, DIDFT_AXIS | DIDFT_ANYINSTANCE, 0 },
    { NULL, 8, DIDFT_AXIS | DIDFT_ANYINSTANCE, 0 },
};
static const DIDATAFORMAT DIFP_df = {
    sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_RELAXIS,
    sizeof(DIMOUSESTATE2), 3, DIFP_odf
};
static IDirectInputDevice8A* s_diMouse      = nullptr;
static bool                  s_diReady      = false;
static volatile LONG         s_diAccX       = 0;
static volatile LONG         s_diAccY       = 0;
static volatile LONG         s_diAccZ       = 0;
static HANDLE                s_diThread     = nullptr;
static volatile LONG         s_diThreadRun  = 0;

static void fpEnsureDInput()
{
    static int tried = 0;
    if (s_diReady || tried >= 600) return;   // retry through early frames, then give up
    tried++;
    HWND w = FindWindowA("OgreD3D11Wnd", nullptr);
    if (!w) w = FindWindowA("OgreD3D9Wnd", nullptr);
    if (!w) w = GetForegroundWindow();
    if (!w) return;
    if (!s_diMouse)
    {
        HMODULE dll = LoadLibraryA("dinput8.dll");
        DI8Create_t create = dll
            ? (DI8Create_t)GetProcAddress(dll, "DirectInput8Create") : nullptr;
        IDirectInput8A* di = nullptr;
        if (!create || FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                                     DIFP_IID_IDirectInput8A, (void**)&di, nullptr)) || !di)
        { tried = 600; return; }
        if (FAILED(di->CreateDevice(DIFP_GUID_SysMouse, &s_diMouse, nullptr)) || !s_diMouse)
        { di->Release(); tried = 600; return; }
        di->Release();
        s_diMouse->SetDataFormat(&DIFP_df);
    }
    s_diMouse->SetCooperativeLevel(w, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    if (SUCCEEDED(s_diMouse->Acquire()))
    {
        s_diReady = true;
        VerbLog("[WASDCombat] dc_fp_dinput_acquired");
    }
}

static DWORD WINAPI fpDInputPollThread(void*)
{
    timeBeginPeriod(1);   // 1ms Sleep granularity for this loop
    while (InterlockedCompareExchange(&s_diThreadRun, 1, 1))
    {
        if (s_diReady && s_diMouse)
        {
            DIMOUSESTATE2 st;
            if (SUCCEEDED(s_diMouse->GetDeviceState(sizeof(st), &st)))
            {
                if (st.lX) InterlockedAdd(&s_diAccX, st.lX);
                if (st.lY) InterlockedAdd(&s_diAccY, st.lY);
                if (st.lZ) InterlockedAdd(&s_diAccZ, st.lZ);
                if (st.lX || st.lY)
                {
                    ULONGLONG now = GetTickCount64();
                    if (!s_recentMouseStartMs || now - s_recentMouseStartMs > 180
                        || !s_recentMouseLastMs || now - s_recentMouseLastMs > 220)
                    {
                        // v39: only the accumulators restart here. Clearing
                        // s_combatGestureDirection too made the idle preview
                        // arrow blink at every 180 ms window boundary and
                        // dropped strokes that began right after a restart
                        // (user 2026-10-20: right arrow flickered; log showed
                        // dir=0 with fresh mouse_age at every press). The kept
                        // direction seeds the new window; the radial deadzone
                        // and the diagonal hysteresis in
                        // combatDirectionFromDelta already absorb jitter.
                        InterlockedExchange(&s_recentMouseX, 0);
                        InterlockedExchange(&s_recentMouseY, 0);
                        s_recentMouseStartMs = now;
                    }
                    if (st.lX) InterlockedAdd(&s_recentMouseX, st.lX);
                    if (st.lY) InterlockedAdd(&s_recentMouseY, st.lY);
                    LONG gx = InterlockedCompareExchange(&s_recentMouseX, 0, 0);
                    LONG gy = InterlockedCompareExchange(&s_recentMouseY, 0, 0);
                    CutDirection previous = (CutDirection)InterlockedCompareExchange(
                        &s_combatGestureDirection, CUT_DEFAULT, CUT_DEFAULT);
                    CutDirection selected = combatDirectionFromDelta(gx, gy, previous);
                    if (selected != CUT_DEFAULT)
                        InterlockedExchange(&s_combatGestureDirection, (LONG)selected);
                    s_recentMouseLastMs = now;
                }
            }
            else s_diMouse->Acquire();   // lost (alt-tab): re-acquire, skip this poll
        }
        Sleep(1);
    }
    return 0;
}

static void fpStartDInputThread()
{
    if (s_diThread) return;
    InterlockedExchange(&s_diThreadRun, 1);
    s_diThread = CreateThread(nullptr, 0, fpDInputPollThread, nullptr, 0, nullptr);
}

// Consume (and zero) the accumulated relative deltas since the last call.
static void fpTakeMouseAccum(float* dx, float* dy)
{
    *dx = (float)InterlockedExchange(&s_diAccX, 0);
    *dy = (float)InterlockedExchange(&s_diAccY, 0);
}

// otsRestoreCameraToRig — re-attach the Ogre camera to the game's rig and
// restore its transform/FOV/near-clip/auto-tracking, destroying our detached
// node.  ONLY for the inventory face-cam now (the gameplay OTS is scrapped).
// World-map arrow (user 2026-10-08: "the arrow faces the wrong direction in
// first person").  The map's camera marker follows the GAME camera's heading
// (CameraClass::yaw), which our detached FP / OTS views never touch, so it
// froze at the heading you entered with.  Mirror the view yaw into it every
// frame (same convention: forward = (-sin yaw, 0, -cos yaw), as the face-cam
// wake already relies on) and put the original back when the rig takes over.
static bool  s_rigYawSaved = false;
static float s_rigYawSave  = 0.0f;
static void dcSyncRigYaw(CameraClass* cam, float yaw)
{
    if (!cam) return;
    if (!s_rigYawSaved) { s_rigYawSave = cam->yaw; s_rigYawSaved = true; }
    yaw = fmodf(yaw, 6.2831853f);
    if (yaw >  3.1415927f) yaw -= 6.2831853f;
    if (yaw < -3.1415927f) yaw += 6.2831853f;
    cam->yaw = yaw;
}

// World-map arrow, take 2 (test 2026-10-08: third person right, FIRST PERSON
// INVERTED after the yaw sync).  The yaw is not what flips it: in FP the rig's
// center is welded to the character's ROOT while the camera sits at the eye,
// AHEAD of it, so a facing derived from camera -> center points backwards (in
// OTS the camera is behind the root, so the same math points forward).  In FP
// the player camera's getFacingDirection now returns the real view direction.
// s_mapFacingCalls counts FP calls so the log proves the map asks for it.
static volatile LONG s_mapFacingCalls = 0;
static Ogre::Vector3* (*s_getFacingOrig)(const CameraClass* self, Ogre::Vector3* ret);
static Ogre::Vector3* getFacing_hook(const CameraClass* self, Ogre::Vector3* ret)
{
    if (s_firstPersonActive && ou && ou->player && self == ou->player->camera)
    {
        float cp = cosf(s_fpPitchSm);
        *ret = Ogre::Vector3(-sinf(s_fpYawSm) * cp, sinf(s_fpPitchSm), -cosf(s_fpYawSm) * cp);   // q = yaw(Y) * pitch(X) applied to -Z
        InterlockedIncrement(&s_mapFacingCalls);
        return ret;
    }
    return s_getFacingOrig(self, ret);
}

static void otsRestoreCameraToRig(CameraClass* cam)
{
    if (!cam) return;
    if (s_rigYawSaved) { cam->yaw = s_rigYawSave; s_rigYawSaved = false; }
    Ogre::Camera* oc = cam->camera;
    if (oc)
    {
        oc->detachFromParent();
        Ogre::SceneNode* rigNode = cam->getCameraNode();
        if (rigNode)
            rigNode->attachObject(oc);
        if (s_fpCamLocalsSaved)
        {
            oc->setPosition(s_fpSavedCamPos);
            oc->setOrientation(s_fpSavedCamOri);
            oc->setFOVy(s_fpSavedFov);
            if (s_fpSavedNearClip > 0.0f)
                oc->setNearClipDistance(s_fpSavedNearClip);
        }
        if (s_fpHadAutoTrack)
            oc->setAutoTracking(true, cam->getCenterNode());
        if (s_fpNode)
        {
            oc->getSceneManager()->destroySceneNode(s_fpNode);
            s_fpNode = nullptr;
        }
    }
    s_fpNode           = nullptr;
    s_fpCamLocalsSaved = false;
    s_fpHadAutoTrack   = false;
}

// Restore the floating name-tags to the player's setting if we hid them.  Safe
// to call from any teardown path; only touches gui when names were actually
// hidden and gui is valid.
static void otsRestoreNames()
{
    if (!s_namesHidden) return;
    if (gui) gui->showNames(s_savedShowNames);
    s_namesHidden = false;
    DebugLog("[WASDCombat] dc_names_restored");
}

// exitOTS — re-attach the camera to the rig and re-track the anchor.
static void exitOTS(bool restoreCamera)
{
    if (!s_fpActive) return;
    s_fpActive = false;
    if (restoreCamera && ou && ou->player)
    {
        otsRestoreCameraToRig(ou->player->camera);
        if (s_freeMoveAnchor)
            ou->player->startTrackCharacter(s_freeMoveAnchor);
    }
    otsRestoreNames();
    s_fpNode = nullptr;
    s_invFaceRetrackUntilMs = GetTickCount64() + 4000;  // arm follow watchdog
    DebugLog("[WASDCombat] dc_cam_exited");
}

// enterOTS — detach the Ogre camera onto our own root node so we can aim it
// freely (the attached RTS camera can only look top-down, which is why the
// inventory face-cam can't face the character without detaching).  Used ONLY
// for the inventory face-cam, while the sim is paused and the character is
// standing — none of the walking/floor problems that scrapped the gameplay OTS.
static void enterOTS()
{
    if (s_fpActive) return;
    if (!ou || !ou->player || !ou->player->camera
        || !s_freeMoveAnchor || !s_freeMoveAnchor->movement) return;
    CameraClass* cam = ou->player->camera;
    Ogre::Camera* oc = cam->camera;
    if (!oc) return;

    s_otsSavedAltitude = cam->altitude;   // held constant during the face-cam
    cam->stopFollowing();
    s_fpHadAutoTrack = (oc->getAutoTrackTarget() != nullptr);
    oc->setAutoTracking(false);
    s_fpSavedCamPos    = oc->getPosition();
    s_fpSavedCamOri    = oc->getOrientation();
    s_fpSavedFov       = oc->getFOVy();
    s_fpSavedNearClip  = oc->getNearClipDistance();
    {   // DIAG (build 70): vanilla depth planes vs ours - far/near ratio = depth precision
        char cb[160];
        sprintf_s(cb, sizeof(cb), "[WASDCombat] dc_cam_planes vanilla_near=%.3f far=%.1f ours_near=%.3f fov=%.0f",
                  s_fpSavedNearClip, oc->getFarClipDistance(), s_fpNearClipFP, s_fpFovDegFP);
        VerbLog(cb);
    }
    s_fpCamLocalsSaved = true;
    oc->setFOVy(Ogre::Radian(Ogre::Degree(s_fpFovDeg)));
    oc->setNearClipDistance(s_fpNearClip);
    oc->detachFromParent();
    s_fpNode = oc->getSceneManager()->getRootSceneNode()->createChildSceneNode();
    s_fpNode->attachObject(oc);
    oc->setPosition(Ogre::Vector3::ZERO);
    oc->setOrientation(Ogre::Quaternion::IDENTITY);
    // Hide the floating name-tags for the duration of the face-cam.
    if (gui && !s_namesHidden)
    {
        s_savedShowNames = options ? options->showNames : true;
        gui->showNames(false);
        s_namesHidden = true;
        DebugLog("[WASDCombat] dc_names_hidden");
    }
    s_fpActive = true;
    DebugLog("[WASDCombat] dc_cam_entered");
}

// Own-inventory = at least one inventory window open (LIVE count, never stale)
// AND no trade/loot session (edge-latched flag, never stale).  True for a plain
// inventory (1 window) AND a backpack character (2 windows); false for any
// shop/loot/corpse trade.  See s_tradeWindowActive for the staleness rationale.
static bool isOwnInventoryOpen()
{
    return gui && !s_tradeWindowActive
        && !gui->isCharacterEditorMode()   // editor owns the camera; don't fight it
        && gui->getNumOpenInventoryWindows() >= 1;
}

// Inventory move-through eligibility (see s_invMoveThroughActive).  Manual V
// combat always keeps direct control and the user's camera when Tab opens;
// legacy DC can still opt in with InventoryFaceCam=false.  Dialogue and the
// character editor keep their own UI behavior.
static bool invMoveThroughEligible()
{
    return s_mode == MODE_FREE_MOVE
        && s_freeMoveAnchor
        && (s_manualAttackOn || !s_settingInventoryFaceCam)
        && gui && gui->isAnyInventoryWindowOpen()
        && !gui->inDialogue()                    // dialogue still pauses everything
        && !gui->isCharacterEditorMode();
}

// =======================================================================
// First-person camera — functions (ported from the FPS prototype 2026-07-22).
// Shares the detached-camera machinery (s_fpNode, saved cam locals) with the
// inventory face-cam; s_firstPersonActive is the drive-mode selector.
// =======================================================================

// fpSetHeadBoneHidden — modern-FPS head hide, v1.3.2 FINAL FORM (user decision
// 2026-08-24): SHRINK TINY AT THE NATURAL POSITION — nothing else.
//
// Saga summary: the male head is BODY-MESH skin with blended head/neck vertex
// weights (the attachment inventory proved no head entity exists; the female
// only hides "perfectly" because her modded replacement body — vanillaAppend06,
// Nude Mod HD — has clean head-only rigging).  Every attempt to RELOCATE the
// collapse (neck tuck, -500 drop, bone-group zero) moved the remnant somewhere
// VISIBLE (torso/collar) or stretched shared-weight vertices into artifacts.
// The original v1.3.0 shrink worked precisely because the tiny head stays at
// its NATURAL position — at the FP camera itself, inside the near-clip plane,
// where it cannot render.  So: 0.001 scale, manual control, bone position and
// orientation UNTOUCHED, and the TRUE eye mounts the head bone as originally
// tuned (its derived position is unaffected by scale).
//
// Bone scales partly size Kenshi characters, so the ORIGINAL scale is saved and
// restored exactly — never assumed 1.0.
static const int FP_HIDE_BONE_COUNT = 2;
static const char* const FP_HIDE_BONES[FP_HIDE_BONE_COUNT] =
    { "Bip01 Head", "Head" };
static void fpSetHeadBoneHidden(bool hide)
{
    if (hide == s_fpHeadBoneHidden) return;
    if (!s_freeMoveAnchor) { s_fpHeadBoneHidden = false; return; }
    AppearanceBase* ap = s_freeMoveAnchor->getAppearance();
    Ogre::OldSkeletonInstance* sk = ap ? ap->getSkeleton() : nullptr;
    if (!sk) { s_fpHeadBoneHidden = false; return; }
    bool any = false;
    for (int i = 0; i < FP_HIDE_BONE_COUNT; ++i)
    {
        if (!sk->hasBone(FP_HIDE_BONES[i])) continue;
        Ogre::OldBone* hb = sk->getBone(FP_HIDE_BONES[i]);
        if (!hb) continue;
        if (hide)
        {
            if (!s_fpHideBoneSaved[i])
            {
                s_fpHideBoneSavedScale[i] = hb->getScale();
                s_fpHideBoneSaved[i]      = true;
            }
            hb->setManuallyControlled(true);
            hb->setScale(Ogre::Vector3(0.001f, 0.001f, 0.001f));
            any = true;
        }
        else if (s_fpHideBoneSaved[i])
        {
            hb->setScale(s_fpHideBoneSavedScale[i]);
            hb->setManuallyControlled(false);
            s_fpHideBoneSaved[i] = false;
        }
    }
    s_fpHeadBoneHidden = hide && any;
    DebugLog(hide ? "[WASDCombat] dc_fp_head_hidden"
                  : "[WASDCombat] dc_fp_head_restored");
}

// (THE MALE HEAD REMNANT — final verdict after the full 2026-08-23→27 hunt:
// male bodies render from RUNTIME-GENERATED morph meshes ("human_male.mesh_
// morph_N", Kenshi's physique system) that carry their OWN vertex data — they
// ignore the source file's bone-assignment chunks (purifying ALL THREE
// same-named mesh candidates simultaneously changed nothing), partially ignore
// bone scale, and are not separate scene entities (exhaustive scene searches at
// head AND chest anchors found nothing).  The only remaining approach would be
// live GPU vertex-buffer editing of the generated mesh — out of scope.
// SHIPPED MITIGATION: the head-bone shrink + EyeDrop 0.06 default parks the
// lens so the remnant near-clips away.  Clean-rigged FEMALE replacer bodies
// (Nude Mod HD) vanish completely — recommend in README.)

// fpSetBeardHidden — HideHead companion (2026-08-31): HideHead hides the
// ENTIRE head.  Beards are separate attached entities with their OWN skeletons
// (2026-08 hunt): they inherit neither the head-bone shrink nor shaveHead
// (HideHair = scalp piece only), so a bearded male kept a full-size beard
// floating at the hidden head.  Walk the scene's Entity list (the v8/v17
// export-verified iterator), keep objects riding the anchor's character node
// (bone-attached entities report the char node as their parent scene node),
// skip the body, match by MESH filename ("beard", case-insensitive —
// beard##/Beard##.mesh on disk).  Only VISIBLE matches are hidden, recorded by
// name; restore re-shows recorded names only (see s_fpHiddenBeards).  No
// entity pointers are ever cached (appearance rebuilds recreate entities); the
// 1 Hz mainLoop re-assert heals rebuild resurrections.  logInventory=true also
// names EVERY non-body entity on the char node — if a beard still renders, the
// log carries the real mesh name (v10's silent-miss lesson: silence must be
// impossible).
static void fpSetBeardHidden(bool hide, bool logInventory)
{
    if (!s_freeMoveAnchor) return;
    if (!ou || !ou->player || !ou->player->camera) return;
    Ogre::Camera* ocB = ou->player->camera->camera;
    if (!ocB) return;
    Ogre::SceneManager* smB = ocB->getSceneManager();
    AppearanceBase* apB = s_freeMoveAnchor->getAppearance();
    Ogre::Entity* bodyB = apB ? apB->getBody() : nullptr;
    if (!smB || !bodyB) return;
    Ogre::SceneNode* charNodeB = bodyB->getParentSceneNode();
    if (!charNodeB) return;
    int othersB = 0;
    Ogre::SceneManager::MovableObjectIterator itB =
        smB->getMovableObjectIterator("Entity");
    while (itB.hasMoreElements())
    {
        Ogre::MovableObject* moB = itB.getNext();
        if (!moB || moB == (Ogre::MovableObject*)bodyB) continue;
        if (moB->getParentSceneNode() != charNodeB) continue;
        Ogre::Entity* entB = static_cast<Ogre::Entity*>(moB);
        const Ogre::MeshPtr& meshB = entB->getMesh();
        if (meshB.isNull()) continue;
        const Ogre::String& mnB = meshB->getName();
        char lowB[96];
        size_t nB = mnB.size() < sizeof(lowB) - 1 ? mnB.size() : sizeof(lowB) - 1;
        for (size_t iB = 0; iB < nB; ++iB)
            lowB[iB] = (char)tolower((unsigned char)mnB[iB]);
        lowB[nB] = '\0';
        // Headwear match (2026-08-31 v2): token lists replace the attachment
        // registry, which over-reached into modded CLOTHING in the field.
        // Gear tokens ride HideHeadgear; hair meshes (incl. modded hairstyles
        // like MaleHaircutsSamuraiBun — outside shaveHead AND the registry)
        // ride HideHair.  Any miss shows up by name in the charnode_ent recon.
        // "head" (not "headband") catches headwrap/headband/headgear —
        // field miss 2026-09-01: farmers_headwrap rendered.  NO bare "wrap"
        // token: it would eat the hand-wraps underwear mod.
        static const char* const FP_GEAR_TOKENS[] =
            { "beard", "hat", "helmet", "hood", "mask", "cap",
              "turban", "head", "veil", "goggle" };
        bool matchGearB = false;
        for (int tkB = 0; tkB < 10 && !matchGearB; ++tkB)
            if (strstr(lowB, FP_GEAR_TOKENS[tkB])) matchGearB = true;
        bool matchHairB = strstr(lowB, "hair") != nullptr;
        bool isBeardB = (matchGearB && s_fpHideHeadgear)
                     || (matchHairB && s_fpHideHair);
        ++othersB;
        if (logInventory)
        {
            char invB[192];
            sprintf_s(invB, sizeof(invB),
                "[WASDCombat] dc_fp_charnode_ent mesh=%s vis=%d beard=%d",
                mnB.c_str(), moB->getVisible() ? 1 : 0, isBeardB ? 1 : 0);
            VerbLog(invB);
        }
        if (!isBeardB) continue;
        if (hide)
        {
            if (!moB->getVisible()) continue;   // vanilla-hidden (helmet): not ours
            moB->setVisible(false);
            bool knownB = false;
            for (int k = 0; k < s_fpHiddenBeardCount; ++k)
                if (strcmp(s_fpHiddenBeards[k], lowB) == 0) { knownB = true; break; }
            if (!knownB && s_fpHiddenBeardCount < FP_BEARD_MAX)
                strcpy_s(s_fpHiddenBeards[s_fpHiddenBeardCount++], lowB);
            // Throttled: the per-frame re-assert finds facial attachments
            // re-shown EVERY frame — unthrottled this logged 70 lines/sec
            // (3088-line flood, field log 2026-08-31).
            static ULONGLONG s_fpHwLogTick = 0;
            ULONGLONG nowHw = GetTickCount64();
            if (logInventory || nowHw - s_fpHwLogTick >= 5000)
            {
                s_fpHwLogTick = nowHw;
                char bBuf[160];
                sprintf_s(bBuf, sizeof(bBuf),
                    "[WASDCombat] dc_fp_headwear_hidden mesh=%s", mnB.c_str());
                VerbLog(bBuf);
            }
        }
        else
        {
            for (int k = 0; k < s_fpHiddenBeardCount; ++k)
                if (strcmp(s_fpHiddenBeards[k], lowB) == 0)
                {
                    moB->setVisible(true);
                    char bBuf[160];
                    sprintf_s(bBuf, sizeof(bBuf),
                        "[WASDCombat] dc_fp_beard_restored mesh=%s", mnB.c_str());
                    DebugLog(bBuf);
                    break;
                }
        }
    }
    if (!hide) s_fpHiddenBeardCount = 0;
    if (logInventory && othersB == 0)
        VerbLog("[WASDCombat] dc_fp_charnode_ent_none");
}


// (setAttachmentsVisible-based gear hiding was tried a THIRD time 2026-08-31
// and removed the same night: on the user's modded setup the attachment
// registry contains modded CLOTHING, which vanished with the hats.  Hats,
// beards and modded hairstyles are all charnode ENTITIES — the token walk in
// fpSetBeardHidden covers every one of them precisely, by mesh name.)

// fpSetHeadMask — set/restore the head-pixel hide uniform ("dcHideHead") on the
// anchor's body material (per-character material instance; every subentity).
// (The legacy "hiddenMask" part-bit tool was removed 2026-09-21: the head has
// no part bits, proven 2026-08-31.)
// ABI: every call export-verified 2026-08-31; buffer writes go through the
// CONST inline getters + const_cast because _markDirty (used by the non-const
// getters) is NOT exported — the value lands in the per-draw upload buffer,
// and the 1 Hz re-assert covers any refresh gap.
static void fpSetHeadMask(bool apply)
{
    if (!s_freeMoveAnchor) return;
    AppearanceBase* apM = s_freeMoveAnchor->getAppearance();
    Ogre::Entity* bodyM = apM ? apM->getBody() : nullptr;
    if (!bodyM) return;
    size_t nSub = bodyM->getNumSubEntities();
    if (!apply)
    {
        // Head-pixel uniform back to 0 on every subentity we may have touched.
        if (s_fpHeadPixelApplied)
        {
            s_fpHeadPixelApplied = false;
            for (size_t si = 0; si < nSub; ++si)
            {
                Ogre::SubEntity* seR = bodyM->getSubEntity(si);
                if (!seR) continue;
                const Ogre::MaterialPtr& matR = seR->getMaterial();
                if (matR.isNull()) continue;
                Ogre::Technique* tqR = matR->getTechnique((unsigned short)0);
                Ogre::Pass* psR = tqR ? tqR->getPass((unsigned short)0) : nullptr;
                if (!psR || !psR->hasFragmentProgram()) continue;
                Ogre::GpuProgramParametersSharedPtr fpR = psR->getFragmentProgramParameters();
                if (fpR.isNull()) continue;
                const Ogre::GpuConstantDefinition* dR =
                    fpR->_findNamedConstantDefinition("dcHideHead", false);
                if (!dR) continue;
                const Ogre::GpuProgramParameters* cR = fpR.getPointer();
                *const_cast<float*>(cR->getFloatPointer(dR->physicalIndex)) = 0.0f;
            }
            VerbLog("[WASDCombat] dc_fp_headpixel_restored");
        }
        // Build 72: body cull modes back to what they were.
        if (s_fpBodyCullCount > 0)
        {
            for (size_t si = 0; si < nSub && (int)si < s_fpBodyCullCount; ++si)
            {
                Ogre::SubEntity* seC = bodyM->getSubEntity(si);
                if (!seC) continue;
                const Ogre::MaterialPtr& matC = seC->getMaterial();
                if (matC.isNull()) continue;
                Ogre::Technique* tqC = matC->getTechnique((unsigned short)0);
                Ogre::Pass* psC = tqC ? tqC->getPass((unsigned short)0) : nullptr;
                if (!psC) continue;
                psC->setCullingMode(s_fpBodyCullSaved[si]);
                psC->setManualCullingMode(s_fpBodyManualSaved[si]);
            }
            s_fpBodyCullCount = 0;
            DebugLog("[WASDCombat] dc_fp_body_solid_inside restored");
        }
        return;
    }
    bool pixelUniformFound = false;   // verdict input: the uniform EXISTS somewhere
    for (size_t si = 0; si < nSub; ++si)
    {
        Ogre::SubEntity* seM = bodyM->getSubEntity(si);
        if (!seM) continue;
        const Ogre::MaterialPtr& matM = seM->getMaterial();
        if (matM.isNull()) continue;
        Ogre::Technique* tqM = matM->getTechnique((unsigned short)0);
        Ogre::Pass* psM = tqM ? tqM->getPass((unsigned short)0) : nullptr;
        if (!psM) continue;
        // Build 72: double-sided body while in first person (saved per subentity, restored on exit).
        if (s_fpBodySolidInside && (int)si < FP_BODY_SUB_MAX && s_fpBodyCullCount <= (int)si)
        {
            s_fpBodyCullSaved[si]   = psM->getCullingMode();
            s_fpBodyManualSaved[si] = psM->getManualCullingMode();
            psM->setCullingMode(Ogre::CULL_NONE);
            psM->setManualCullingMode(Ogre::MANUAL_CULL_NONE);
            s_fpBodyCullCount = (int)si + 1;
            if (si == 0) DebugLog("[WASDCombat] dc_fp_body_solid_inside applied");
        }
        // (b) head-pixel hide via the shader-override uniform (the real fix).
        if (s_fpHeadPixelHide && psM->hasFragmentProgram())
        {
            Ogre::GpuProgramParametersSharedPtr fppM = psM->getFragmentProgramParameters();
            const Ogre::GpuConstantDefinition* dP = fppM.isNull() ? nullptr :
                fppM->_findNamedConstantDefinition("dcHideHead", false);
            if (dP)
            {
                pixelUniformFound = true;
                const Ogre::GpuProgramParameters* cP = fppM.getPointer();
                float* vP = const_cast<float*>(cP->getFloatPointer(dP->physicalIndex));
                if (*vP < 0.5f)
                {
                    *vP = 1.0f;
                    s_fpHeadPixelApplied = true;
                    char pbuf[96];
                    sprintf_s(pbuf, sizeof(pbuf),
                        "[WASDCombat] dc_fp_headpixel_applied sub=%u", (unsigned)si);
                    VerbLog(pbuf);
                }
                else if (!s_fpHeadPixelApplied)
                {
                    // Already 1.0 (a stream pseudo-load / switch dropped our flag
                    // without a restore — user 2026-09-14 "face gone" after
                    // FP/OTS/DC toggling, fixed by a reload).  ADOPT it: we own
                    // the hide while FP is active, so the exit restores it.
                    s_fpHeadPixelApplied = true;
                    char pbuf2[96];
                    sprintf_s(pbuf2, sizeof(pbuf2),
                        "[WASDCombat] dc_fp_headpixel_adopted sub=%u", (unsigned)si);
                    VerbLog(pbuf2);
                }
            }
            else
            {
                // Override shader not live (cache/race) — say so once per FP.
                static ULONGLONG s_hpMissTick = 0;
                ULONGLONG nowHp = GetTickCount64();
                if (nowHp - s_hpMissTick >= 10000)
                {
                    s_hpMissTick = nowHp;
                    DebugLog("[WASDCombat] dc_fp_headpixel_uniform_missing");
                }
            }
        }
    }
    // Verdict AFTER the subentity walk (subentities carry different materials —
    // one hit is enough, so the fallback must not fire from a single miss): the
    // uniform was found nowhere, so another mod's character.hlsl owns the slot
    // for this whole session.  Bone-shrink the head instead.  Skipped when the
    // user already runs HideHead; idempotent (the setter no-ops when hidden).
    // (was `!s_fpHeadPixelApplied` — a uniform already at 1.0 counted as
    //  "missing" and shrank the head ON TOP of the pixel hide; then the exit
    //  restored neither cleanly = the missing face.)
    if (s_fpHeadPixelHide && !pixelUniformFound && !s_fpHideHead)
    {
        if (!s_fpPixelFallbackOn)
        {
            s_fpPixelFallbackOn = true;
            DebugLog("[WASDCombat] dc_fp_headpixel_fallback_shrink");
        }
        fpSetHeadBoneHidden(true);
    }
}

// dcBodyRoot — where the anchor's BODY actually is, for camera pivots.
// CharMovement::pos is the LOGIC position: the engine parks it while the body
// is a ragdoll (KO, thrown — physics moves the mesh) and while the character is
// CARRIED (the carrier walks off with the mesh; pos stays at the pickup spot).
// Both OTS and FP orbited/sat at that parked spot (user 2026-09-13).  Carried:
// the game's own getBoneWorldPosition composes the true rendered transform, so
// the pelvis/spine bone is where the body is; ragdoll: the physics root.
// Standing/animated: plain pos (unchanged behaviour).
static Ogre::Vector3 dcBodyRoot(Character* ch)
{
    if (!ch || !ch->movement) return Ogre::Vector3::ZERO;
    Ogre::Vector3 pos = ch->movement->pos;
    if (ch->isBeingCarried())
    {
        AppearanceBase* ap = ch->getAppearance();
        Ogre::OldSkeletonInstance* sk = ap ? ap->getSkeleton() : nullptr;
        if (sk)
        {
            static const char* const BONES[] = { "Bip01 Pelvis", "Bip01 Spine", "Bip01", "Bip01 Head" };
            for (int i = 0; i < 4; ++i)
            {
                if (!sk->hasBone(BONES[i])) continue;
                Ogre::Vector3 b = ch->getBoneWorldPosition(std::string(BONES[i]));
                bool finite = (b.x == b.x) && (b.y == b.y) && (b.z == b.z)
                           && fabsf(b.x) < 1.0e7f && fabsf(b.y) < 1.0e7f && fabsf(b.z) < 1.0e7f;
                if (finite)
                {
                    static ULONGLONG s_carryLogTick = 0;
                    ULONGLONG nowC = GetTickCount64();
                    if (nowC - s_carryLogTick >= 2000)
                    {
                        s_carryLogTick = nowC;
                        char cb[192];
                        sprintf_s(cb, sizeof(cb),
                            "[WASDCombat] dc_cam_carried_root bone=%s body=(%.1f,%.1f,%.1f) pos=(%.1f,%.1f,%.1f)",
                            BONES[i], b.x, b.y, b.z, pos.x, pos.y, pos.z);
                        DebugLog(cb);
                    }
                    return b;
                }
                break;
            }
        }
    }
    if (ch->isRagdoll()) return ch->getRagdollPhysicsRootPos();
    return pos;
}

// fpGetHeadWorld — world-space position of the anchor's animated head/neck bone.
// CRITICAL: the skeleton MUST come from AppearanceBase::getSkeleton() (the game's
// own getter).  Entity::getSkeleton() on the body entity returns a different
// runtime type whose virtual calls CRASH (the v1.8.9 P-toggle crash).  The
// bone's derived position is clean MODEL space relative to the character root;
// the entity node's transform is stale/wrong-space, so world head = logic-space
// root + bone offset rotated by the character's facing yaw.
static bool fpGetHeadWorld(Ogre::Vector3& out)
{
    if (!s_freeMoveAnchor || !s_freeMoveAnchor->movement) return false;
    AppearanceBase* ap = s_freeMoveAnchor->getAppearance();
    Ogre::Entity* ent = ap ? ap->getBody() : nullptr;
    if (!ent) return false;
    Ogre::OldSkeletonInstance* sk = ap->getSkeleton();
    if (!sk) return false;

    CharMovement* mvB   = s_freeMoveAnchor->movement;
    Ogre::Vector3 root  = dcBodyRoot(s_freeMoveAnchor);   // body, not the parked logic pos
    bool bodyMode = s_freeMoveAnchor->isRagdoll() || s_freeMoveAnchor->isBeingCarried();
    (void)mvB;

    // Resolve an EXISTING mount bone.  hasBone (safe on the AppearanceBase
    // skeleton — the only skeleton whose virtual calls don't crash, per the
    // v1.8.9 lesson) both validates the skeleton and picks a name that exists,
    // so the getBoneWorldPosition call below can't hit a missing bone.
    //  * TRUE-world path: prefer the HEAD bone (KenshiFP mounts there); its world
    //    origin is unaffected by the scale-hide, and eye level is a fixed drop
    //    below it.
    //  * legacy synthetic path: base-of-neck first — it keeps animating when the
    //    head is scale-hidden, and the eye sits higher above a neck mount.
    static const char* const BONE_TRUE[]  = { "Bip01 Head", "Bip01 Neck", "Head", "Bip01 Neck1" };
    static const char* const BONE_SYNTH[] = { "Bip01 Neck", "Bip01 Head", "Bip01 Neck1", "Head" };
    static const float        BONE_SYNTH_UP[] = { 2.4f, 1.0f, 2.4f, 1.0f };
    const char* const* names = s_fpTrueBoneEye ? BONE_TRUE : BONE_SYNTH;
    const char* used = nullptr;
    int usedIdx = -1;
    for (int i = 0; i < 4 && !used; ++i)
        if (sk->hasBone(names[i])) { used = names[i]; usedIdx = i; }
    if (!used) return false;

    if (s_fpTrueBoneEye)
    {
        // KenshiFP method: the game's own Character::getBoneWorldPosition composes
        // the full skeleton + entity transform and returns the head bone's TRUE
        // world position — tracking every animation (bob, run-lean, turn) with NO
        // synthetic reconstruction and NO dependence on the view yaw, so a pure
        // pan no longer swings the eye on an arc (the old root+rotate(offset,yaw)
        // formula did).  Same coordinate space as mvB->pos (both game-world), which
        // is the space our root-child s_fpNode consumes.
        Ogre::Vector3 head = s_freeMoveAnchor->getBoneWorldPosition(std::string(used));
        float ddx = head.x - root.x, ddy = head.y - root.y, ddz = head.z - root.z;
        bool plausible = (ddx*ddx + ddy*ddy + ddz*ddz) < 30.0f * 30.0f
                      && (bodyMode || head.y > root.y - 1.0f);   // head above the feet
                                                                 // (not when slung over a shoulder)
        if (plausible)
        {
            out              = head;
            s_fpHeadAboveRoot = ddy;   // live height sample for the EyeDrop scale
            // Neck mount (head hidden): add the neck→head rise in WORLD space.
            // The first attempt routed it through s_fpBoneEyeUp, which the eye
            // compose rotates by the VIEW orientation — pitching down tilted the
            // rise forward and sank the camera into the chest (user screenshots
            // 2026-08-23, "center" view full of body mesh).
            if (strstr(used, "Neck") != nullptr) out.y += 2.4f;
            s_fpBoneEyeUp = 0.0f;   // eye level handled by EyeDrop in the true-eye path
            if (!s_fpBoneLogged)
            {
                s_fpBoneLogged = true;
                char bbuf[256];
                sprintf_s(bbuf, sizeof(bbuf),
                    "[WASDCombat] dc_fp_truebone bone=%s world=(%.1f,%.1f,%.1f)"
                    " root=(%.1f,%.1f,%.1f)",
                    used, head.x, head.y, head.z, root.x, root.y, root.z);
                VerbLog(bbuf);
            }
            return true;
        }
        // implausible (skeleton mid-load / odd rig) -> fall through to synthetic
    }

    // Legacy synthetic path: root + model-space bone offset rotated by the VIEW yaw.
    Ogre::OldBone* b = sk->getBone(used);
    if (!b) return false;
    s_fpBoneEyeUp = (strstr(used, "Neck") != nullptr) ? 2.4f
                  : (usedIdx >= 0 && !s_fpTrueBoneEye ? BONE_SYNTH_UP[usedIdx] : 1.0f);
    Ogre::Vector3 boneModel = b->_getDerivedPosition();
    float mountYaw = s_fpYawSm;   // render-smoothed view yaw (== s_fpYaw when LookSmooth=0)
    Ogre::Quaternion qBody(Ogre::Radian(mountYaw), Ogre::Vector3::UNIT_Y);
    out = root + qBody * boneModel;

    float ddx = out.x - root.x, ddy = out.y - root.y, ddz = out.z - root.z;
    bool plausible = (ddx*ddx + ddy*ddy + ddz*ddz) < 30.0f * 30.0f;
    if (!s_fpBoneLogged)
    {
        s_fpBoneLogged = true;
        char bbuf[256];
        sprintf_s(bbuf, sizeof(bbuf),
            "[WASDCombat] dc_fp_headbone_tracking bone=%s plausible=%d"
            " boneModel=(%.1f,%.1f,%.1f) mountYaw=%.2f world=(%.1f,%.1f,%.1f)",
            used, (int)plausible,
            boneModel.x, boneModel.y, boneModel.z,
            mountYaw, out.x, out.y, out.z);
        DebugLog(bbuf);
    }
    return plausible;
}

// UNIFY (2026-09-04): ONE source of truth for "does a UI/menu currently need
// the free cursor?" — used by FP (fpDriveFrame), V-OTS (cameraUpdate_hook)
// and the shared crosshair, so all three suspend/free the cursor IDENTICALLY.
// Previously each path re-derived this from a slightly different set of terms,
// which is why fixing one mode kept breaking the other.  Superset of every
// prior condition; broader = safer (never leaves the cursor locked in a menu).
static bool dcUiWantsCursor()
{
    // Pause has cursor priority.  In v102, treating a bare world pause as a
    // camera-look state kept recentering and hiding the pointer; on this Kenshi
    // build the Esc menu does not reliably clear CameraClass::controlEnabled
    // either, so both Space pause and Esc could leave the player mouse-locked.
    // One authoritative pause test here releases FP, OTS and crosshair capture
    // together.  Unpausing re-centres once through dcUpdateCrosshair's edge.
    if (ou && ou->isPaused())                             return true;
    if (s_lootUiSuspendActive)                            return true; // loot / trade
    if (ou && ou->player && ou->player->contextMenu.isVisible()) return true; // RMB interaction menu
    ManagementScreen* m = ManagementScreen::getSingleton();
    if (m && m->getVisible())                             return true; // map / factions / tech / squad
    if (gui && (gui->isStatsWindowOpen() || gui->inDialogue())) return true; // UIs
    // Tab owns the pointer whenever an inventory is open.  WASD can keep
    // controlling the character, but the camera must stop recentering the
    // mouse so items and inventory buttons remain clickable without Alt.
    if (gui && gui->isAnyInventoryWindowOpen()) return true;
    return false;
}
// (fpShowCrosshair — the old sand-coloured FP-only "+" — REMOVED in the 2026-09-04
//  unification: both modes now use the single white crosshair in dcUpdateCrosshair.)

// While the RMB hold-menu is open in first-person, tint the hovered option
// yellow-green and the rest parchment.  ContextMenuGUI::optionsList is at 0xF8
// (the class is forward-declared, so the member is read by documented offset).
static const MyGUI::Colour FP_MENU_ACCENT(0.72f, 0.86f, 0.38f, 1.0f);  // yellow-green
static const MyGUI::Colour FP_MENU_NORMAL(0.78f, 0.75f, 0.66f, 1.0f);  // parchment
static void fpTintContextMenu()
{
    if (!ou || !ou->player) return;
    ContextMenu& cm = ou->player->contextMenu;
    if (!cm.isVisible()) return;

    MyGUI::Widget* focus = MyGUI::InputManager::getInstance().getMouseFocusWidget();
    ContextMenuGUI* menus[2] = { cm.menuGUI, cm.menuGUI2 };
    for (int m = 0; m < 2; ++m)
    {
        if (!menus[m]) continue;
        MyGUI::Widget* list =
            *(MyGUI::Widget**)((char*)menus[m] + 0xF8);  // ContextMenuGUI::optionsList
        if (!list) continue;
        size_t n = list->getChildCount();
        for (size_t i = 0; i < n; ++i)
        {
            MyGUI::Widget* c = list->getChildAt(i);
            if (!c) continue;
            MyGUI::TextBox* tb = c->castType<MyGUI::TextBox>(false);
            if (!tb) continue;
            bool hovered = (focus == c);
            if (!hovered && focus)
                for (MyGUI::Widget* p = focus->getParent(); p; p = p->getParent())
                    if (p == c) { hovered = true; break; }
            tb->setTextColour(hovered ? FP_MENU_ACCENT : FP_MENU_NORMAL);
        }
    }
}

// exitFirstPerson — leave first-person: restore head/hair/crosshair, then hand
// the camera back to the game's rig via the shared otsRestoreCameraToRig helper.
static void exitFirstPerson(bool restoreCamera)
{
    if (!s_firstPersonActive) return;
    s_firstPersonActive = false;
    s_fpCursorCaptured  = false;
    if (!restoreCamera) s_rigYawSaved = false;   // camera not ours to restore (load): drop the map-arrow save
    // Restore the exact grass/foliage draw-range the boost overrode on enter.
    if (s_fpOptRangeSaved && options)
    {
        options->grassRange   = s_fpSavedGrassRange;
        options->foliageRange = s_fpSavedFoliageRange;
        s_fpOptRangeSaved     = false;
        DebugLog("[WASDCombat] dc_fp_grass_range_restored");
    }
    fpSetHeadBoneHidden(false);
    fpSetBeardHidden(false, false);
    fpSetHeadMask(false);
    s_fpPixelFallbackOn = false;   // re-probe the shader next FP enter
    if (s_fpHairHidden)
    {
        s_fpHairHidden = false;
        if (s_freeMoveAnchor)
        {
            AppearanceBase* ap = s_freeMoveAnchor->getAppearance();
            if (ap) { ap->shaveHead(false); VerbLog("[WASDCombat] dc_fp_hair_restored"); }
        }
    }
    if (restoreCamera && ou && ou->player)
    {
        otsRestoreCameraToRig(ou->player->camera);
        if (s_freeMoveAnchor)
            ou->player->startTrackCharacter(s_freeMoveAnchor);
    }
    DebugLog("[WASDCombat] dc_fp_exited");
}

// enterFirstPerson — detach the camera onto our root node and open the view at
// the anchor's eye.  Guards against the inventory face-cam already owning the
// camera (s_fpActive).  Mirrors enterOTS but sets FP FOV/near-clip and hides
// the head/hair.  Camera calls MUST run on the game thread (consumed in mainLoop).
static void enterFirstPerson()
{
    if (s_firstPersonActive || s_fpActive) return;
    if (!ou || !ou->player || !ou->player->camera
        || !s_freeMoveAnchor || !s_freeMoveAnchor->movement) return;
    // FP and the detached OTS are mutually exclusive — both detach the same
    // camera. If OTS is up (for example when P is pressed), tear it
    // down first so the camera is back on the rig before FP re-detaches it.
    if (s_otsCamActive) exitOtsCam(true);

    // Live re-tune: re-read [FirstPerson] from the INI on every entry so the
    // player can edit FOV / ForwardOffset / EyeUpAdjust / NearClip, toggle P
    // off then on, and see the new camera immediately — no game relaunch.
    {
        char cfgPath[MAX_PATH];
        getConfigPath(cfgPath, sizeof(cfgPath));
        loadFirstPersonConfig(cfgPath);
    }

    CameraClass* cam = ou->player->camera;
    Ogre::Camera* oc = cam->camera;
    if (!oc) return;

    cam->stopFollowing();   // zoom left untouched — restored view = pre-FP view
    s_fpHadAutoTrack = (oc->getAutoTrackTarget() != nullptr);
    oc->setAutoTracking(false);
    s_fpSavedCamPos    = oc->getPosition();
    s_fpSavedCamOri    = oc->getOrientation();
    s_fpSavedFov       = oc->getFOVy();
    s_fpSavedNearClip  = oc->getNearClipDistance();
    {   // DIAG (build 70): vanilla depth planes vs ours - far/near ratio = depth precision
        char cb[160];
        sprintf_s(cb, sizeof(cb), "[WASDCombat] dc_cam_planes vanilla_near=%.3f far=%.1f ours_near=%.3f fov=%.0f",
                  s_fpSavedNearClip, oc->getFarClipDistance(), s_fpNearClipFP, s_fpFovDegFP);
        VerbLog(cb);
    }
    s_fpCamLocalsSaved = true;
    oc->setFOVy(Ogre::Radian(Ogre::Degree(s_fpFovDegFP)));
    oc->setNearClipDistance(s_fpNearClipFP);
    oc->detachFromParent();
    s_fpNode = oc->getSceneManager()->getRootSceneNode()->createChildSceneNode();
    s_fpNode->attachObject(oc);
    oc->setPosition(Ogre::Vector3::ZERO);
    oc->setOrientation(Ogre::Quaternion::IDENTITY);

    // Open the view centered on the character's current facing.
    Ogre::Vector3 d = s_freeMoveAnchor->movement->direction;
    d.y = 0.0f;
    float dlen = d.length();
    if (dlen > 0.001f)
    {
        d /= dlen;
        s_fpYaw = atan2f(-d.x, -d.z);   // forward = (-sin yaw, 0, -cos yaw)
    }
    s_fpPitch = 0.0f;

    if (s_fpHideHair)
    {
        AppearanceBase* ap = s_freeMoveAnchor->getAppearance();
        if (ap) { ap->shaveHead(true); s_fpHairHidden = true; VerbLog("[WASDCombat] dc_fp_hair_hidden"); }
    }
    // Grass/foliage draw-range boost: Kenshi builds grass to a range tuned for the
    // high top-down camera, so at ground level grass only exists in a short ring
    // that pops at its edge as you turn/move.  While FP owns the view, widen the
    // range so grass is already present before it rotates into frame.  Save the
    // exact originals once (guarded) and restore on exit.  Mult 1.0 = vanilla.
    if (options && s_fpGrassRangeMult > 1.0f && !s_fpOptRangeSaved)
    {
        s_fpSavedGrassRange   = options->grassRange;
        s_fpSavedFoliageRange = options->foliageRange;
        s_fpOptRangeSaved     = true;
        options->grassRange   *= s_fpGrassRangeMult;
        options->foliageRange *= s_fpGrassRangeMult;
        DebugLog("[WASDCombat] dc_fp_grass_range_boosted");
    }
    s_fpLeanFwd         = 0.0f;
    s_fpMoveLeanSmooth  = 0.0f;
    s_fpGaitFwdSmooth   = 0.0f;
    s_fpActionClrSmooth = 0.0f;
    s_fpHaveLastFeet    = false;   // true-bone-eye feet-delta speed tracker
    s_fpMoveSpeed       = 0.0f;
    s_fpMoveFwdSmooth   = 0.0f;
    s_fpFeetTickMs      = 0;
    s_fpSmValid         = false;    // re-seed the render-smoothed view on first frame
    s_fpEnemyClearSmooth = 1.0f;    // enemy-clearance pullback starts released
    s_fpEnemyNearestDist = -1.0f;
    s_fpLastStreamValid = false;    // force a streaming teleport on the first FP frame
    s_fpBodyYaw         = s_fpYaw;   // body starts aligned with the opening view
    s_fpHeadSmoothValid = false;
    s_fpBoneLogged      = false;
    s_firstPersonActive = true;
    s_fpCursorCaptured  = false;   // first capture pass establishes the center
    if (s_fpHideHead)
        fpSetHeadBoneHidden(true);
    if (s_fpHideHeadgear || s_fpHideHair)
        fpSetBeardHidden(true, true);
    // Pixel hide is deliberately INDEPENDENT of HideHead (2026-08-31): with
    // both on they overlap harmlessly; HideHead=0 + HeadPixelHide=1 is the
    // decisive A/B — the clip acting alone on a full-size, unshrunk head.
    if (s_fpHideHead || s_fpHeadPixelHide)
        fpSetHeadMask(true);
    DebugLog("[WASDCombat] dc_fp_entered");
}

// fpDriveFrame — the per-frame first-person drive, called from cameraUpdate_hook
// AFTER the game's camera update.  Mouse-look (cursor recentered on the viewport
// center), neck-limit (turn the body when the view exceeds its facing while
// idle), then place the detached node at the head-bone eye looking along the view.
static void fpDriveFrame(CameraClass* thisptr, bool uiOpen)
{
    // Auto-exit: first-person only exists inside DC with a live anchor + no loot UI.
    if (s_mode != MODE_FREE_MOVE || !s_freeMoveAnchor
        || !s_freeMoveAnchor->movement || s_lootUiSuspendActive)
    {
        exitFirstPerson(true);
        return;
    }

    // Crosshair + pointer are owned by the shared dcUpdateCrosshair (called
    // from cameraUpdate_hook for BOTH modes) — nothing to draw here anymore.

    // Mouse-look — paused only while the cursor is genuinely needed: a menu
    // (uiOpen already folds in contextMenu.isVisible), or the interaction
    // context-menu itself.  It is NOT paused for a bare RMB HOLD (field
    // 2026-09-04: holding RMB to MOVE in FP must keep the crosshair centered;
    // the cursor frees only once the interaction menu actually appears, then
    // re-centers on close — user's "move now, but menu still frees").
    bool ctxVisible = ou->player->contextMenu.isVisible();
    if (ctxVisible)
        fpTintContextMenu();
    // uiOpen already folds in dcUiWantsCursor() (menu / loot / contextMenu /
    // pause / etc.), so it is the single suspend signal — no bare RMB hold.
    bool captureOk = !uiOpen && isKenshiForegroundMain();
    if (captureOk)
    {
        // RawMouse: ensure the 1kHz DirectInput look device is running.  Its poll
        // thread accumulates hardware deltas off the frame loop; we consume them
        // below.  Falls back to cursor-warp until the device is acquired.
        bool useRaw = s_fpRawMouse || (s_directionalCombat && s_manualAttackOn);
        if (useRaw) { fpStartDInputThread(); fpEnsureDInput(); }

        HWND fg = GetForegroundWindow();
        RECT rc;
        if (fg && GetClientRect(fg, &rc))
        {
            POINT center;
            center.x = (rc.left + rc.right) / 2;
            center.y = (rc.top + rc.bottom) / 2;
            ClientToScreen(fg, &center);

            float dx = 0.0f, dy = 0.0f;
            bool  haveDelta = false;
            if (useRaw && s_diReady)
            {
                // Framerate-independent 1kHz DirectInput deltas (raw hardware).
                fpTakeMouseAccum(&dx, &dy);
                haveDelta = s_fpCursorCaptured;    // skip the baseline frame
                SetCursorPos(center.x, center.y);  // keep the crosshair/click point centered
                s_fpCursorCaptured = true;
            }
            else
            {
                // Cursor-warp fallback: RawMouse off, or DI not yet acquired.
                POINT cur;
                if (GetCursorPos(&cur))
                {
                    if (s_fpCursorCaptured)
                    {
                        dx = (float)(cur.x - center.x);
                        dy = (float)(cur.y - center.y);
                        haveDelta = true;
                    }
                    SetCursorPos(center.x, center.y);
                    s_fpCursorCaptured = true;
                }
            }

            if (haveDelta && (dx != 0.0f || dy != 0.0f))
            {
                s_fpYaw   -= dx * FP_RAD_PER_PIXEL * s_fpSensitivityFP;
                s_fpPitch -= dy * FP_RAD_PER_PIXEL * s_fpSensitivityFP;
                if (s_fpPitch >  FP_PITCH_LIMIT) s_fpPitch =  FP_PITCH_LIMIT;
                if (s_fpPitch < -FP_PITCH_LIMIT) s_fpPitch = -FP_PITCH_LIMIT;
                // Mark active mouse-look (deadzone to ignore 1px jitter) so the
                // point-click FollowTurn yields — it only recentres after the mouse
                // has been still for FollowDelayMs.
                if (dx > 1.0f || dx < -1.0f || dy > 1.0f || dy < -1.0f)
                    s_fpLastMouseMoveMs = GetTickCount64();
            }
        }
    }
    else
    {
        s_fpCursorCaptured = false;  // re-baseline when capture resumes
        // Drain deltas accumulated while a UI owns the cursor so the view doesn't
        // jump when capture resumes.
        if (s_fpRawMouse) { float jx, jy; fpTakeMouseAccum(&jx, &jy); }
    }

    CharMovement* mvFP = s_freeMoveAnchor->movement;

    // "Actually moving" — true for WASD, point-click, AND autonomous combat/heal
    // approach movement.  currentlyMoving/currentSpeed are set by the game
    // regardless of what issued the move, so the camera can follow + compensate in
    // all of those cases, not just WASD.
    bool fpMoving = mvFP->currentlyMoving || mvFP->currentSpeed > 0.25f;

    // Body facing.
    //  * CAMERA-PRIORITY (WASD, plus a short grace after release while still
    //    moving): FORCE the body to face the view direction, so movement is strafe-
    //    relative — W walks forward, S backpedals, A/D sidestep — and the body
    //    never rotates away from the camera.  The camera yaw is NEVER touched here,
    //    so it cannot jolt.  The grace is the fix for the backpedal twitch: on the
    //    release/coast frame of an S-walk the character is briefly still facing 180°
    //    from the view, and the old idle neck-limit would SNAP the camera onto that
    //    body direction — a violent jolt.  Holding camera-priority through the coast
    //    keeps refacing the body to the view instead, so by the time we fall to idle
    //    the body is already aligned and nothing snaps.  Motion is unaffected:
    //    setDirectMovement got the world WASD vector directly, so direction here is
    //    facing only, and this hook writes late enough to survive to the frame.
    //  * point-click / autonomous move: camera follows the heading (yields to mouse).
    //  * Idle: neck-limit — turn the body toward the view if you look too far.
    bool strafeGrace = (GetTickCount64() - s_wasdLastHeldMs) < FP_STRAFE_GRACE_MS;
    if (s_frameWasdHeld || (fpMoving && strafeGrace))
    {
        // BODY FACES TRAVEL (user re-design 2026-08-24; replaces the old
        // body-faces-VIEW coupling).  While WASD drives, the body turns toward
        // the actual MOVEMENT heading; the camera looks anywhere freely.  Two
        // wins: (1) the enemy AI reads the anchor's FACING — the old view
        // coupling meant fleeing while looking backward pointed the body AT the
        // pursuers, which Kenshi's combat AI treats as a squared-up combatant
        // (ring-hold + jitter at range instead of run-down-and-attack; field
        // repro 2026-08-24).  Facing the travel direction reads as a genuine
        // fleeing back, and pursuit/attacks behave identically no matter where
        // the player looks.  (2) No more moonwalk: the walk cycle faces the way
        // the body moves.  WASD INPUT stays camera-relative (mapped elsewhere);
        // this is facing only.  On release, s_prevWasdDir zeroes and targetYaw
        // holds the last body yaw through the strafe grace — no snap.
        float targetYaw = s_fpBodyYaw;
        if (s_prevWasdDir.squaredLength() > 0.0001f)
            targetYaw = atan2f(-s_prevWasdDir.x, -s_prevWasdDir.z);
        float dyaw = targetYaw - s_fpBodyYaw;
        while (dyaw >  3.14159265f) dyaw -= 6.28318531f;
        while (dyaw < -3.14159265f) dyaw += 6.28318531f;
        s_fpBodyYaw += dyaw * FP_BODY_TURN;
        while (s_fpBodyYaw >  3.14159265f) s_fpBodyYaw -= 6.28318531f;
        while (s_fpBodyYaw < -3.14159265f) s_fpBodyYaw += 6.28318531f;
        mvFP->direction =
            Ogre::Vector3(-sinf(s_fpBodyYaw), 0.0f, -cosf(s_fpBodyYaw));
    }
    else if (fpMoving && s_fpFollowTurn > 0.0f
             && (GetTickCount64() - s_fpLastMouseMoveMs) > (ULONGLONG)s_fpFollowDelayMs)
    {
        // Point-click / combat / heal-approach movement: the CHARACTER leads (walks
        // its own path), so make the CAMERA follow — gently lerp the view yaw toward
        // the body's movement heading so you look where you're going, the mirror of
        // what WASD does.  We do NOT write mvFP->direction here (the game's pathing
        // owns it); we only turn the view.  CRUCIAL: this only runs after the mouse
        // has been STILL for FollowDelayMs — so while you are actively looking around
        // the follow stays out of the way (no fighting your mouse), then eases the
        // view back onto the path once you let go (field 2026-07-25).
        Ogre::Vector3 hd = mvFP->direction;
        hd.y = 0.0f;
        float hlen = hd.length();
        if (hlen > 0.001f)
        {
            hd /= hlen;
            float headYaw = atan2f(-hd.x, -hd.z);
            float d = headYaw - s_fpYaw;
            while (d >  3.14159265f) d -= 6.28318531f;
            while (d < -3.14159265f) d += 6.28318531f;
            s_fpYaw += d * s_fpFollowTurn;
            while (s_fpYaw >  3.14159265f) s_fpYaw -= 6.28318531f;
            while (s_fpYaw < -3.14159265f) s_fpYaw += 6.28318531f;
            s_fpBodyYaw = headYaw;   // keep body-yaw synced so a later stop won't snap
        }
    }
    else
    {
        Ogre::Vector3 bd = mvFP->direction;
        bd.y = 0.0f;
        float blen = bd.length();
        if (blen > 0.001f)
        {
            bd /= blen;
            float bodyYaw = atan2f(-bd.x, -bd.z);
            float delta   = s_fpYaw - bodyYaw;
            while (delta >  3.14159265f) delta -= 6.28318531f;
            while (delta < -3.14159265f) delta += 6.28318531f;
            if (delta > s_fpNeckLimitRad || delta < -s_fpNeckLimitRad)
            {
                mvFP->direction =
                    Ogre::Vector3(-sinf(s_fpYaw), 0.0f, -cosf(s_fpYaw));
                s_fpYaw = bodyYaw
                        + (delta > 0.0f ?  s_fpNeckLimitRad
                                        : -s_fpNeckLimitRad);
                bodyYaw = s_fpYaw;
            }
            // Keep the smoothed body yaw synced to the resting facing so the next
            // movement turn lerps from where the body actually is (no initial jump).
            s_fpBodyYaw = bodyYaw;
        }
    }

    // LookSmooth: derive a render-smoothed view from the authoritative s_fpYaw/
    // s_fpPitch (which all the control logic above wrote).  Only the VISUAL — the
    // orientation quaternion, the head-bone eye mount, and the forward vectors below
    // — uses the smoothed values; body-facing/motion already used the raw target, so
    // the character still turns crisply.  Smaller per-frame rotation delta shrinks
    // the render-thread grass re-facing mismatch → less side-to-side foliage flicker.
    // LookSmooth=0 → the smoothed value equals the raw value exactly (no change).
    if (!s_fpSmValid) { s_fpYawSm = s_fpYaw; s_fpPitchSm = s_fpPitch; s_fpSmValid = true; }
    {
        float a = 1.0f - s_fpLookSmooth;   // 1.0 = snap (off), <1 = glide
        float dyawS = s_fpYaw - s_fpYawSm;
        while (dyawS >  3.14159265f) dyawS -= 6.28318531f;
        while (dyawS < -3.14159265f) dyawS += 6.28318531f;
        s_fpYawSm += dyawS * a;
        while (s_fpYawSm >  3.14159265f) s_fpYawSm -= 6.28318531f;
        while (s_fpYawSm < -3.14159265f) s_fpYawSm += 6.28318531f;
        s_fpPitchSm += (s_fpPitch - s_fpPitchSm) * a;
    }

    Ogre::Quaternion q =
        Ogre::Quaternion(Ogre::Radian(s_fpYawSm),   Ogre::Vector3::UNIT_Y) *
        Ogre::Quaternion(Ogre::Radian(s_fpPitchSm), Ogre::Vector3::UNIT_X);

    // Enemy body-clip clearance: nearest live hostile distance was sampled this
    // frame in mainLoop (-1 = none in range).  Map it to a 0..1 offset scale —
    // 1 at/beyond EnemyClearRadius, EnemyClearMinScale at contact — and smooth
    // it so the eye eases back rather than snapping.  Both eye paths multiply
    // their forward offsets by it, so an aggressor pressing into the lens pulls
    // the eye back to the (hidden) skull instead of poking inside their model.
    {
        float enemyScale = 1.0f;
        if (s_fpEnemyClearRadius > 0.0f && s_fpEnemyNearestDist >= 0.0f
            && s_fpEnemyNearestDist < s_fpEnemyClearRadius)
        {
            float t = s_fpEnemyNearestDist / s_fpEnemyClearRadius;
            enemyScale = s_fpEnemyClearMinScale
                       + t * (1.0f - s_fpEnemyClearMinScale);
        }
        s_fpEnemyClearSmooth += (enemyScale - s_fpEnemyClearSmooth) * 0.15f;
    }

    // Eye position — primary: the ANIMATED head bone, so the camera rides the
    // neck through every pose.  Smoothed on height to damp stride bob; hard-
    // attached horizontally so a sprinting model can't outrun the camera.
    Ogre::Vector3 eye;
    Ogre::Vector3 headWorld;
    if (fpGetHeadWorld(headWorld))
    {
      if (s_fpTrueBoneEye)
      {
        // KenshiFP weld: eye = the head bone's REAL world position, Y taken RAW
        // (welded to head height — no bob smoothing/lag), plus a HORIZONTAL forward
        // push (along yaw only, so looking down does not sink the eye into the
        // chest) so the eye sits at the face rather than inside the skull.  Because
        // headWorld comes from getBoneWorldPosition and is view-independent, a pure
        // pan no longer moves the eye — the arc-swing (and the grass re-page it
        // fed) is gone.
        // Feet-delta ground speed (framerate-independent, low-passed) drives the
        // forward LEAD so the eye leads faster movement instead of trailing the
        // leaning head.  Keyed off ON-SCREEN speed, not the noisy currentMotion
        // magnitude our notes found unreliable (2026-07-24).
        {
            ULONGLONG nowF = GetTickCount64();
            float dt = (s_fpFeetTickMs > 0) ? (float)(nowF - s_fpFeetTickMs) * 0.001f : 0.0f;
            s_fpFeetTickMs = nowF;
            if (dt > 0.001f && dt < 0.25f && s_fpHaveLastFeet)
            {
                float dfx = mvFP->pos.x - s_fpLastFeetX;
                float dfz = mvFP->pos.z - s_fpLastFeetZ;
                float inst = sqrtf(dfx*dfx + dfz*dfz) / dt;
                if (inst > 400.0f) inst = 400.0f;   // reject teleport/paging jumps
                s_fpMoveSpeed += (inst - s_fpMoveSpeed) * 0.20f;
                // Vertical speed for the stair-climb pullback (+ = ascending).
                float vy = (mvFP->pos.y - s_fpLastFeetY) / dt;
                if (vy >  60.0f) vy =  60.0f;        // reject teleport/paging jumps
                else if (vy < -60.0f) vy = -60.0f;
                s_fpClimbSpeedSmooth += (vy - s_fpClimbSpeedSmooth) * 0.20f;
            }
            s_fpLastFeetX = mvFP->pos.x; s_fpLastFeetZ = mvFP->pos.z;
            s_fpLastFeetY = mvFP->pos.y; s_fpHaveLastFeet = true;
        }
        float lead = 0.0f;
        if (s_fpMoveForward > 0.0f && s_fpMoveSpeedRef > 1.0f)
        {
            float gait = s_fpMoveSpeed / s_fpMoveSpeedRef;   // 0 idle .. ~1 run
            if (gait < 0.0f) gait = 0.0f; else if (gait > 1.25f) gait = 1.25f;
            lead = s_fpMoveForward * gait;
        }
        s_fpMoveFwdSmooth += (lead - s_fpMoveFwdSmooth) * 0.15f;

        // Ascent-aware forward pullback: turn the low-passed climb speed into a
        // 0..1 ramp, shrink the forward push toward StairForwardMinScale, and lift
        // the eye by StairEyeLift so the camera clears the rising steps instead of
        // jamming into them.  Inert on flat ground (ascent01 == 0 -> scale 1, lift 0),
        // so open-ground framing and body-clip protection are unchanged.
        float ascent01 = (s_fpClimbSpeedSmooth > 0.0f)
                       ? s_fpClimbSpeedSmooth * s_fpStairForwardReduce : 0.0f;
        if (ascent01 > 1.0f) ascent01 = 1.0f;
        float ascentScale = 1.0f - ascent01 * (1.0f - s_fpStairForwardMinScale);
        float stairLift   = s_fpStairEyeLift * ascent01;

        // Keep the shared 0..1 speed factor alive in this path too — it drives
        // the MoveNearClip blend below.  It previously only updated in the legacy
        // synthetic-eye branch, which left MoveNearClip dead under TrueBoneEye
        // (the default) — found in the 2026-08-01 anti-clip audit.
        {
            float leanTarget = (s_fpMoveSpeedRef > 1.0f)
                             ? s_fpMoveSpeed / s_fpMoveSpeedRef : 0.0f;
            if (leanTarget > 1.0f) leanTarget = 1.0f;
            s_fpMoveLeanSmooth += (leanTarget - s_fpMoveLeanSmooth) * 0.12f;
        }

        float fwdScale = ascentScale * s_fpEnemyClearSmooth;
        if (g_log.debugLogging)
        {
            static ULONGLONG t = 0; ULONGLONG n = GetTickCount64();
            if (n - t >= 500) { t = n; char b[112];
                sprintf_s(b, sizeof(b),
                    "[WASDCombat] dc_fp_stair climb=%.2f scale=%.2f lift=%.2f",
                    s_fpClimbSpeedSmooth, ascentScale, stairLift);
                DebugLog(b); }
        }

        eye    = headWorld;
        // Height-proportional eye level (user req 2026-08-24): the eye BASE
        // (head bone) already tracks the character's custom height; scale the
        // fixed EyeDrop by the measured head height vs the standard-height
        // reference so short/tall characters keep the same RELATIVE eye
        // placement.  EyeUpAdjust stays absolute (user trim).
        float dropScale = s_fpHeadAboveRoot / 16.0f;
        if (dropScale < 0.5f) dropScale = 0.5f;
        if (dropScale > 2.0f) dropScale = 2.0f;
        eye.y += -(s_fpEyeDrop * dropScale) + s_fpEyeUpAdjust + stairLift;
        Ogre::Vector3 fwd(-sinf(s_fpYawSm), 0.0f, -cosf(s_fpYawSm));
        eye += fwd * ((s_fpFwdOffset + s_fpMoveFwdSmooth) * fwdScale);

        // Optional additive clearances (default 0 = inert) kept from our tuning:
        // gait-forward while jog/sprinting, and committed-action clearance.  They
        // only engage if the user opts in via the INI; both push along the view.
        {
            float gaitTarget = 0.0f;
            if (fpMoving)
            {
                MoveSpeed g = mvFP->speedOrders;
                if (g == JOG) gaitTarget = s_fpJogForward;
                else if (g == RUN || g == GROUPED) gaitTarget = s_fpRunForward;
            }
            s_fpGaitFwdSmooth += (gaitTarget - s_fpGaitFwdSmooth) * 0.10f;
            if (s_fpGaitFwdSmooth > 0.001f) eye += fwd * (s_fpGaitFwdSmooth * fwdScale);
        }
        {
            bool actionNow = s_fpActionClearFwd > 0.0f && isCommittedAction(s_freeMoveAnchor);
            float clrTarget = actionNow ? s_fpActionClearFwd : 0.0f;
            s_fpActionClrSmooth += (clrTarget - s_fpActionClrSmooth) * 0.15f;
            if (s_fpActionClrSmooth > 0.001f) eye += fwd * (s_fpActionClrSmooth * fwdScale);
        }
      }
      else
      {
        if (!s_fpHeadSmoothValid)
        {
            s_fpHeadSmooth      = headWorld;
            s_fpHeadSmoothValid = true;
        }
        else
        {
            s_fpHeadSmooth.x  = headWorld.x;
            s_fpHeadSmooth.z  = headWorld.z;
            s_fpHeadSmooth.y += (headWorld.y - s_fpHeadSmooth.y) * FP_BONE_SMOOTH;
        }
        // Speed-scaled lean compensation: at jog/sprint the model pitches
        // forward and swings the arms/chest up into view.  Ramp a smoothed 0..1
        // speed factor and add extra eye height + forward reach so the camera
        // rises above and past the leaning torso.  Zero when standing/walking,
        // so the natural upright view is untouched.  Magnitudes are INI-tuned.
        {
            float spd = mvFP->currentMotion.length();
            float leanTarget = spd * 0.04f;          // ~1.0 by jog speed
            if (leanTarget > 1.0f) leanTarget = 1.0f;
            s_fpMoveLeanSmooth += (leanTarget - s_fpMoveLeanSmooth) * 0.12f;
        }
        float leanUp  = s_fpMoveLeanUp  * s_fpMoveLeanSmooth;
        float leanFwd = s_fpMoveLeanFwd * s_fpMoveLeanSmooth;
        // Side-look clearance (user req 2026-08-23): looking toward the neck
        // limit swings the shoulder into frame (the eye pivot sits between the
        // shoulders).  Add forward reach proportional to how far sideways the
        // view is from the body facing — zero looking straight ahead, full at
        // 90°+ — so the lens clears the shoulder at the extremes.
        float sideFwd = 0.0f;
        if (s_fpSideLookFwd > 0.0f)
        {
            float relYaw = s_fpYawSm - s_fpBodyYaw;
            while (relYaw >  3.14159265f) relYaw -= 6.28318531f;
            while (relYaw < -3.14159265f) relYaw += 6.28318531f;
            float sideAmt = fabsf(sinf(relYaw));
            sideFwd = s_fpSideLookFwd * sideAmt;
        }
        eye = s_fpHeadSmooth
            + q * Ogre::Vector3(0.0f, s_fpBoneEyeUp + s_fpEyeUpAdjust + leanUp,
                                -((s_fpFwdOffset + leanFwd + sideFwd) * s_fpEnemyClearSmooth));

        // One-frame look-ahead — ONLY along the view forward.  The bone pose read
        // this frame is the previous frame's animation result; at sprint speed that
        // leaves the camera a stride behind, so feed forward velocity ahead by the
        // frame time.  Projected onto the view forward (positive only) so strafing
        // (A/D) and backpedalling (S) never shove the eye sideways or backward —
        // that lateral/back offset was the strafe "twitch".
        {
            static ULONGLONG s_fpLastTickMs = 0;
            ULONGLONG nowFF = GetTickCount64();
            float dt = (s_fpLastTickMs > 0)
                     ? (float)(nowFF - s_fpLastTickMs) * 0.001f : 0.0f;
            s_fpLastTickMs = nowFF;
            if (dt > 0.05f) dt = 0.05f;
            Ogre::Vector3 vel = mvFP->currentMotion;
            vel.y = 0.0f;
            Ogre::Vector3 viewFwd(-sinf(s_fpYawSm), 0.0f, -cosf(s_fpYawSm));
            float fwdComp = vel.dotProduct(viewFwd);
            if (fwdComp > 0.0f)
                eye += viewFwd * (fwdComp * dt);
        }

        // Gait-based forward compensation (the reliable jog/sprint fix).  Keyed
        // off the discrete gait tier (speedOrders) — NOT the noisy currentMotion
        // magnitude — and gated on ACTUAL movement (fpMoving) so it also fires for
        // point-click / combat / heal-approach running, not just WASD (that's when
        // the body was clipping through the lens).  While jogging/running the model
        // leans forward and opens a gap; push the eye forward ALONG THE VIEW — during
        // autonomous movement the view now follows the heading (FollowTurn), so the
        // push lands along the direction of travel.  WALK => 0 (walking untouched).
        {
            float gaitTarget = 0.0f;
            if (fpMoving)
            {
                MoveSpeed gait = mvFP->speedOrders;
                if (gait == JOG)      gaitTarget = s_fpJogForward;
                // RUN = solo sprint; GROUPED = squad-follow speed (group-sprint):
                // treat both as sprint so group movement gets the same fix.
                else if (gait == RUN || gait == GROUPED) gaitTarget = s_fpRunForward;
            }
            s_fpGaitFwdSmooth += (gaitTarget - s_fpGaitFwdSmooth) * 0.10f;
            if (s_fpGaitFwdSmooth > 0.001f)
            {
                Ogre::Vector3 fwdDir(-sinf(s_fpYawSm), 0.0f, -cosf(s_fpYawSm));
                eye += fwdDir * (s_fpGaitFwdSmooth * s_fpEnemyClearSmooth);
            }
        }

        // Committed-action body clearance: during attack swings, blocks, heals,
        // revives and get-ups the arms/torso/head swing hard toward the head bone
        // and clip through the lens even while standing (so the gait push above,
        // which needs movement, can't help).  When such an action is active, push
        // the eye forward along the view to sit clear of the swinging body.  Smoothed
        // so it eases in/out.  0 (default) = off — set ActionClearForward to enable.
        {
            bool actionNow = s_fpActionClearFwd > 0.0f
                          && isCommittedAction(s_freeMoveAnchor);
            float clrTarget = actionNow ? s_fpActionClearFwd : 0.0f;
            s_fpActionClrSmooth += (clrTarget - s_fpActionClrSmooth) * 0.15f;
            if (s_fpActionClrSmooth > 0.001f)
            {
                Ogre::Vector3 fwdDir(-sinf(s_fpYawSm), 0.0f, -cosf(s_fpYawSm));
                eye += fwdDir * (s_fpActionClrSmooth * s_fpEnemyClearSmooth);
            }
        }
      }   // end legacy synthetic-eye branch
    }
    else
    {
        // Fallback: root-relative neck model + speed lean compensation.
        float speed   = mvFP->currentMotion.length();
        float target  = speed * 0.04f;
        if (target > 2.5f) target = 2.5f;
        s_fpLeanFwd  += (target - s_fpLeanFwd) * 0.15f;

        Ogre::Vector3 neck = mvFP->pos;
        neck.y += (s_fpEyeHeight - FP_HEAD_LEN);
        eye = neck
            + q * Ogre::Vector3(0.0f, FP_HEAD_LEN,
                                -(s_fpFwdOffset + s_fpLeanFwd));
    }

    if (s_fpNode)
    {
        s_fpNode->setPosition(eye);
        s_fpNode->setOrientation(q);
    }

    // Near-clip management, applied every frame from the base value:
    //  * MoveNearClip — while moving fast, push the near plane OUT to slice away
    //    the arm/torso that the jog/sprint animation sweeps into the lens (blended
    //    by the 0..1 speed factor; snaps back at a stand so the close-up chest
    //    view is untouched).  Off when <= the base near-clip.
    //  * EnemyNearClip — while a hostile overlaps the lens, pull the near plane
    //    IN toward this value so whatever body part still crosses the plane
    //    slices the thinnest possible cross-section instead of opening a big
    //    see-through hole in the aggressor.  Wins over MoveNearClip (takes the
    //    minimum) because an enemy in your face matters more than your own arms.
    if (thisptr->camera)
    {
        float nc = s_fpNearClipFP;
        if (s_fpMoveNearClip > s_fpNearClipFP)
            nc += (s_fpMoveNearClip - s_fpNearClipFP) * s_fpMoveLeanSmooth;
        if (s_fpEnemyNearClip > 0.0f && s_fpEnemyNearClip < nc
            && s_fpEnemyClearMinScale < 1.0f)
        {
            float overlap01 = (1.0f - s_fpEnemyClearSmooth)
                            / (1.0f - s_fpEnemyClearMinScale);
            if (overlap01 < 0.0f) overlap01 = 0.0f;
            if (overlap01 > 1.0f) overlap01 = 1.0f;
            nc += (s_fpEnemyNearClip - nc) * overlap01;
        }
        // Down / ragdoll / getting up: ease toward DownNearClip (and back).
        {
            ProneState prD = s_freeMoveAnchor->getProneState();
            bool downNow = s_freeMoveAnchor->isRagdoll() || s_freeMoveAnchor->isDown()
                        || s_freeMoveAnchor->isCurrentlyGettingUp
                        || prD == PS_KO || prD == PS_PLAYING_DEAD;
            float tgt = downNow ? 1.0f : 0.0f;
            s_fpDownNearSmooth += (tgt - s_fpDownNearSmooth) * (downNow ? 0.35f : 0.12f);   // in fast, out gentle
            if (s_fpDownNearSmooth > 0.001f && s_fpDownNearClip < nc)
                nc += (s_fpDownNearClip - nc) * s_fpDownNearSmooth;
        }
        thisptr->camera->setNearClipDistance(nc);
    }

    // Keep the game-side rig loosely coherent (audio listener, zone/foliage
    // streaming).  teleport() is a JUMP: calling it every frame while the eye
    // moves makes the streamer re-page grass continuously → the foliage flickers
    // in/out while moving (field 2026-07-25).  Throttle it: only re-teleport once
    // the eye has moved StreamUpdateDist metres from the last streamed point, so
    // streaming stays coherent (a couple of metres of lag is invisible to zone
    // paging) without the per-frame thrash.  StreamUpdateDist=0 restores the old
    // every-frame behaviour for A/B testing.
    //   Anchor = the character ROOT (mvFP->pos), NOT the eye (fix 2026-09-18).
    // The eye sits ForwardOffset (+ gait / action clearance) AHEAD of the head
    // along the VIEW direction, so a pure left/right turn sweeps it on a 1-7 unit
    // arc; measuring that arc against StreamUpdateDist fired teleport() every
    // ~16-40 degrees of yaw while moving = the FP-only turn flicker.  The root
    // is yaw-invariant — exactly what otsCamDrive anchors to (OTS never flickered).
    Ogre::Vector3 streamRoot = mvFP->pos;
    bool doStream = !s_fpLastStreamValid || s_fpStreamDist <= 0.0f
                  || streamRoot.squaredDistance(s_fpLastStreamPos)
                     >= s_fpStreamDist * s_fpStreamDist;
    if (doStream)
    {
        thisptr->teleport(streamRoot);
        s_lastRigTeleportMs = GetTickCount64();   // DIAG
        thisptr->targetPositionY = eye.y;
        thisptr->speedY          = 0.0f;
        s_fpLastStreamPos   = streamRoot;
        s_fpLastStreamValid = true;
    }

    // THE grass-flicker fix (2026-07-25): Kenshi's foliage pager streams grass around
    // the camera's CENTER node, not the eye.  In FP the center node lagged at the
    // character's feet while we rendered from the head, so grass paged around the
    // wrong point and popped under foot.  Reconcile them each frame (after teleport,
    // which can reset the center) so the streaming anchor tracks the character.
    //   Anchor to the character ROOT (mvFP->pos), NOT the eye: the eye swings in a
    // small circle when you PAN (it sits ForwardOffset ahead of the head pivot), so
    // anchoring to it moved the streaming center during pure rotation and PagedGeometry
    // RE-SCATTERED the distant grass — the "scatter variation changes as I pan" that
    // read as flicker at speed (field 2026-07-25).  The root position is stable during
    // rotation (only moves when the character actually walks), so panning no longer
    // re-seeds the grass, while streaming still follows the character as they move.
    // Grass-paging fix — technique from linguine2552/KenshiFP (thanks!).  Kenshi's
    // foliage pager keys off the camera CENTER node.  THREE things matter, and our
    // old every-frame local setPosition got all three wrong:
    //  1. Snap ONLY WHILE MOVING.  At idle we leave the center vanilla/untouched —
    //     a stationary center does not move when you pan, so looking around while
    //     standing no longer re-scatters the grass (our old snap to the rotation-
    //     swinging eye was exactly what re-seeded it every frame you turned).
    //  2. Set the WORLD position via _setDerivedPosition (not local setPosition):
    //     the eye lives under our own node, so hand the pager the true world point.
    //  3. FORCE the derived-position recompute (_getDerivedPositionUpdated) so the
    //     SAME frame's paging pass reads the fresh center — a plain set leaves the
    //     derived value the pager reads stale, which was the residual flicker.
    // (Our camera is detached onto s_fpNode, not a child of center, so unlike
    //  KenshiFP we don't have to re-seat a child camera after moving the center.)
    // 2026-09-18: the weld point is the ROOT (streamRoot), not s_fpNode (the eye).
    // The eye swings on the view-yaw arc described above, so welding the center
    // to it re-scattered the grass on every moving turn — the very thing the
    // paragraph above says not to do.  Root = yaw-invariant, matches OTS.
    // 2026-09-20 FIX (FP + WASD save-load loop): _setDerivedPosition takes a
    // RENDER-space (scene-world) point, and streamRoot is GAME space; the two
    // differ by the root scene node's shift (see focusProject).  The old weld
    // point, s_fpNode->_getDerivedPositionUpdated(), already carried that shift;
    // the raw game-space root parked the streaming center a whole shift away
    // while moving, and the game answered by flagging a save-load every frame
    // (field 2026-09-20: WASD in FP = endless "quickload" loop).  Add the shift.
    if (s_fpFoliageCenterMode && thisptr->center && fpMoving && s_fpNode)
    {
        Ogre::Vector3 rootShift = Ogre::Vector3::ZERO;
        if (Ogre::Node* rootNode = s_fpNode->getParent())
            rootShift = rootNode->_getDerivedPosition();
        thisptr->center->_setDerivedPosition(streamRoot + rootShift);
        thisptr->center->_getDerivedPositionUpdated();
    }
}

static const int DIRECTION_ARROW_PX = 64;
// Distance from the crosshair to the visual centre of the selected arrow.
// v50 adds another clearly visible 40 px after the field test showed that the
// v49 108 -> 148 change was too subtle beside a 64 px widget.
static const int DIRECTION_ARROW_RADIUS_PX = 258;

static float directionArrowSegmentDistance(float x, float y,
                                            float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay;
    float wx = x - ax, wy = y - ay;
    float vv = vx * vx + vy * vy;
    float t = vv > 0.0001f ? (wx * vx + wy * vy) / vv : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float dx = x - (ax + vx * t), dy = y - (ay + vy * t);
    return sqrtf(dx * dx + dy * dy);
}

// Four equal-size inward arrows generated from one geometry and rotated by 90
// degrees.  v44 uses one runtime texture per direction and setImageTexture(),
// the same binding path as the already-proven weapon glow.  The v43 atlas was
// created successfully but Kenshi's MyGUI build rendered its setImageInfo()
// widgets transparent in the field.
static const char* const DIRECTION_ARROW_TEXTURES[4] = {
    "ironwind_direction_arrow_left",
    "ironwind_direction_arrow_right",
    "ironwind_direction_arrow_up",
    "ironwind_direction_arrow_down"
};

static bool directionArrowTextureCreate()
{
    if (s_directionArrowTexTried) return s_directionArrowTexOk;
    s_directionArrowTexTried = true;
    try
    {
        // Slots: left HUD points right, right HUD points left, top points down,
        // bottom points up. Every texture uses the same 40 px shaft/28 px head.
        const float dirX[4] = { 1.0f, -1.0f, 0.0f, 0.0f };
        const float dirY[4] = { 0.0f, 0.0f, 1.0f, -1.0f };
        const float centre = DIRECTION_ARROW_PX * 0.5f - 0.5f;
        for (int slot = 0; slot < 4; ++slot)
        {
            MyGUI::ITexture* tex = MyGUI::RenderManager::getInstance().createTexture(
                DIRECTION_ARROW_TEXTURES[slot]);
            if (!tex) return false;
            tex->createManual(DIRECTION_ARROW_PX, DIRECTION_ARROW_PX,
                              MyGUI::TextureUsage::Static | MyGUI::TextureUsage::Write,
                              MyGUI::PixelFormat::R8G8B8A8);
            unsigned char* pixels = (unsigned char*)tex->lock(MyGUI::TextureUsage::Write);
            if (!pixels) return false;
            float dx = dirX[slot], dy = dirY[slot];
            float px = -dy, py = dx;
            float startX = centre - dx * 20.0f, startY = centre - dy * 20.0f;
            float tipX = centre + dx * 20.0f, tipY = centre + dy * 20.0f;
            float headX = tipX - dx * 14.0f, headY = tipY - dy * 14.0f;
            float wing1X = headX + px * 14.0f, wing1Y = headY + py * 14.0f;
            float wing2X = headX - px * 14.0f, wing2Y = headY - py * 14.0f;
            for (int y = 0; y < DIRECTION_ARROW_PX; ++y)
            for (int x = 0; x < DIRECTION_ARROW_PX; ++x)
            {
                float fx = x + 0.5f, fy = y + 0.5f;
                float d = directionArrowSegmentDistance(fx, fy, startX, startY, tipX, tipY);
                float h1 = directionArrowSegmentDistance(fx, fy, tipX, tipY, wing1X, wing1Y);
                float h2 = directionArrowSegmentDistance(fx, fy, tipX, tipY, wing2X, wing2Y);
                if (h1 < d) d = h1;
                if (h2 < d) d = h2;
                // Keep the length/head dimensions; v50 retains the v49
                // and v48 at an approximately 3 px antialiased stroke.
                float alpha = 1.75f - d;
                if (alpha < 0.0f) alpha = 0.0f;
                if (alpha > 1.0f) alpha = 1.0f;
                size_t offset = ((size_t)y * DIRECTION_ARROW_PX + x) * 4;
                pixels[offset + 0] = 255;
                pixels[offset + 1] = 255;
                pixels[offset + 2] = 255;
                pixels[offset + 3] = (unsigned char)(alpha * 255.0f + 0.5f);
            }
            tex->unlock();
        }
        s_directionArrowTexOk = true;
        DebugLog("[Ironwind] direction_arrow_textures_created count=4");
        return true;
    }
    catch (...)
    {
        DebugLog("[Ironwind] direction_arrow_textures_create_failed");
        return false;
    }
}

// XHAIR-TWEAK: show/hide the fixed crosshair + game pointer.  `want` = FP or
// OTS engaged and no UI suspending it.  Runs on the UI thread (called from
// cameraUpdate_hook post-orig).  Lazily creates the two WhiteSkin bars; if
// creation throws (bad skin/layer on some setup) it degrades to leaving the
// normal cursor — never crashes, never hides the pointer without a crosshair.
// "Manual Attack" HUD label: a small fixed text box (no fade) shown while the
// mode is on. The currently selected direction is drawn: green = a native
// attack is currently eligible, white = unavailable/unchecked. Every one of
// the four directions can therefore appear. Four red slash glyphs make a
// short-lived broken-X hit marker only after the native melee callback
// reports a real hit (not a block or dodge).
static void manualAttackHudUpdate()
{
    bool want = s_manualAttackOn && s_mode == MODE_FREE_MOVE && !s_loadGuardActive;
    if (!want) s_hitMarkerUntilMs = 0;   // V exit / load guard: never carry a flash into the next session
    if (want && !s_manualHud && !s_manualHudTried)
    {
        s_manualHudTried = true;
        try
        {
            MyGUI::Gui* g = MyGUI::Gui::getInstancePtr();
            MyGUI::IntSize v = MyGUI::RenderManager::getInstance().getViewSize();
            const int W = 260, H = 18;
            if (g)
                s_manualHud = g->createWidget<MyGUI::TextBox>("Kenshi_TextboxStandardText_Small",
                                  v.width / 2 - W / 2, 34, W, H, MyGUI::Align::Default, "Top", "dc_manual_attack_hud");
            if (s_manualHud)
            {
                s_manualHud->setCaption(dcZh("Manual Attack"));
                s_manualHud->setTextAlign(MyGUI::Align::Center);
                s_manualHud->setTextColour(MyGUI::Colour::White);
                s_manualHud->setNeedMouseFocus(false);
                s_manualHud->setVisible(false);
                DebugLog("[WASDCombat] manual_attack_hud_created");
            }
        }
        catch (...) { s_manualHud = nullptr; DebugLog("[WASDCombat] manual_attack_hud_create_failed"); }
    }
    bool directionWant = want && s_directionalCombat
        && (s_firstPersonActive || s_otsCamActive) && !dcUiWantsCursor();
    if (directionWant && !s_directionHudTried)
    {
        s_directionHudTried = true;
        try
        {
            MyGUI::Gui* g = MyGUI::Gui::getInstancePtr();
            const char* names[4] = { "dc_dir_left", "dc_dir_right", "dc_dir_up", "dc_dir_down" };
            if (g && directionArrowTextureCreate())
                for (int i = 0; i < 4; ++i)
                {
                    s_directionHud[i] = g->createWidget<MyGUI::ImageBox>(
                        "ImageBox", 0, 0, DIRECTION_ARROW_PX, DIRECTION_ARROW_PX,
                        MyGUI::Align::Default, "Top", names[i]);
                    if (s_directionHud[i])
                    {
                        s_directionHud[i]->setImageTexture(DIRECTION_ARROW_TEXTURES[i]);
                        s_directionHud[i]->setAlpha(0.96f);
                        s_directionHud[i]->setNeedMouseFocus(false);
                        s_directionHud[i]->setVisible(false);
                    }
                }
            bool complete = s_directionHud[0] && s_directionHud[1]
                && s_directionHud[2] && s_directionHud[3];
            DebugLog(complete
                ? "[Ironwind] direction_hud_created style=uniform_textures widgets=4"
                : "[Ironwind] direction_hud_create_failed incomplete_widgets");
        }
        catch (...)
        {
            for (int i = 0; i < 4; ++i) s_directionHud[i] = nullptr;
            DebugLog("[Ironwind] direction_hud_create_failed");
        }
    }
    if (directionWant && !s_hitMarkerHudTried)
    {
        s_hitMarkerHudTried = true;
        try
        {
            MyGUI::Gui* g = MyGUI::Gui::getInstancePtr();
            const char* names[4] = {
                "dc_hit_marker_nw", "dc_hit_marker_ne", "dc_hit_marker_sw", "dc_hit_marker_se"
            };
            const char* captions[4] = { "\\", "/", "/", "\\" };
            if (g)
                for (int i = 0; i < 4; ++i)
                {
                    s_hitMarkerHud[i] = g->createWidget<MyGUI::TextBox>(
                        "Kenshi_TextboxStandardText_Small", 0, 0, 28, 28,
                        MyGUI::Align::Default, "Top", names[i]);
                    if (s_hitMarkerHud[i])
                    {
                        s_hitMarkerHud[i]->setCaption(captions[i]);
                        s_hitMarkerHud[i]->setTextAlign(MyGUI::Align::Center);
                        s_hitMarkerHud[i]->setTextColour(MyGUI::Colour(1.00f, 0.08f, 0.04f));
                        // Keep the four-stroke broken-X layout, but give each
                        // slash a one-pixel dark-red shoulder and a slightly
                        // heavier glyph. Widget alpha below keeps the result
                        // visibly translucent rather than a solid HUD stamp.
                        s_hitMarkerHud[i]->setTextShadow(true);
                        s_hitMarkerHud[i]->setTextShadowColour(MyGUI::Colour(0.62f, 0.02f, 0.01f));
                        try { s_hitMarkerHud[i]->setFontHeight(26); } catch (...) {}
                        s_hitMarkerHud[i]->setNeedMouseFocus(false);
                        s_hitMarkerHud[i]->setVisible(false);
                    }
                }
            DebugLog("[Ironwind] hit_marker_hud_created");
        }
        catch (...)
        {
            for (int i = 0; i < 4; ++i) s_hitMarkerHud[i] = nullptr;
            DebugLog("[Ironwind] hit_marker_hud_create_failed");
        }
    }
    if (s_manualHud && want)
    {
        MyGUI::IntSize v = MyGUI::RenderManager::getInstance().getViewSize();   // follows resolution changes
        s_manualHud->setPosition(v.width / 2 - 130, 34);
        s_manualHud->setTextColour(MyGUI::Colour::White);
        if (s_directionalCombat && s_settingAiBlocks && s_mouseBlockHeld)
            s_manualHud->setCaption(dcZh("Guard: AI"));
        else if (s_directionalCombat && !s_settingAiBlocks && s_mouseBlockHeld)
            s_manualHud->setCaption(std::string(dcZh("Guard: ")) + dcZh(combatDirectionLabel((CutDirection)s_blockDesired)));
        else if (s_directionalCombat && s_attackAimHeld)
            s_manualHud->setCaption(std::string(dcZh("Release to attack: "))
                + dcZh(combatDirectionLabel((CutDirection)s_attackAimDirection)));
        else if (s_directionalCombat && s_attackQueuedAtMs)
            s_manualHud->setCaption(std::string(dcZh("Queued: "))
                + dcZh(combatDirectionLabel((CutDirection)s_attackQueuedDirection)));
        else if (s_directionalCombat)
            s_manualHud->setCaption(std::string(dcZh("Direction: ")) + dcZh(combatDirectionLabel(combatRecentDirection())));
        else
            s_manualHud->setCaption(dcZh("Manual Attack"));
        if (!s_manualHud->getVisible()) s_manualHud->setVisible(true);
    }
    else if (s_manualHud && s_manualHud->getVisible()) s_manualHud->setVisible(false);

    bool haveDirectionHud = s_directionHud[0] && s_directionHud[1]
        && s_directionHud[2] && s_directionHud[3];
    if (haveDirectionHud)
    {
        if (directionWant)
        {
            MyGUI::IntSize v = MyGUI::RenderManager::getInstance().getViewSize();
            int cx = v.width / 2, cy = v.height / 2;
            // Keep all four boxes at one explicit visual-centre radius so a
            // later HUD-distance tune cannot make one axis drift from another.
            const int halfArrow = DIRECTION_ARROW_PX / 2;
            s_directionHud[0]->setPosition(cx - DIRECTION_ARROW_RADIUS_PX - halfArrow, cy - halfArrow);
            s_directionHud[1]->setPosition(cx + DIRECTION_ARROW_RADIUS_PX - halfArrow, cy - halfArrow);
            s_directionHud[2]->setPosition(cx - halfArrow, cy - DIRECTION_ARROW_RADIUS_PX - halfArrow);
            s_directionHud[3]->setPosition(cx - halfArrow, cy + DIRECTION_ARROW_RADIUS_PX - halfArrow);
            CutDirection selected = CUT_DEFAULT;
            bool showSelected = false;
            if (s_settingAiBlocks && s_mouseBlockHeld)
            {
                // Automatic guard has no player-selected direction to display.
                showSelected = false;
            }
            else if (!s_settingAiBlocks && s_mouseBlockHeld && s_blockGestureSeen)
            {
                selected = combatGuardDirectionFromGesture((CutDirection)s_blockDesired);
                showSelected = selected != CUT_DEFAULT;
            }
            else if (s_attackAimHeld && s_attackAimGestureSeen)
            {
                selected = (CutDirection)s_attackAimDirection;
                showSelected = selected != CUT_DEFAULT;
            }
            else if (s_attackQueuedAtMs)
            {
                selected = (CutDirection)s_attackQueuedDirection;
                showSelected = selected != CUT_DEFAULT;
            }
            else
            {
                selected = combatRecentDirection();
                showSelected = selected != CUT_DEFAULT;
            }
            // v50: show only the selected arrow, but never hide it merely
            // because that attack is unavailable. Green means available;
            // white means unavailable/unchecked. Guard uses the same colours.
            const CutDirection map[4] = { CUT_RIGHT, CUT_LEFT, CUT_DOWNWARD, CUT_THRUST };
            for (int i = 0; i < 4; ++i)
            {
                bool on = showSelected && map[i] == selected;
                if (!on)
                {
                    if (s_directionHud[i]->getVisible()) s_directionHud[i]->setVisible(false);
                    continue;
                }
                int slot = combatDirectionSlot(map[i]);
                bool available = slot >= 0 && s_dirMoveAvailable[slot] == 1;
                s_directionHud[i]->setColour(available
                    ? MyGUI::Colour(0.45f, 1.00f, 0.55f)
                    : MyGUI::Colour::White);
                if (!s_directionHud[i]->getVisible()) s_directionHud[i]->setVisible(true);
            }
        }
        else
            for (int i = 0; i < 4; ++i)
                if (s_directionHud[i]->getVisible()) s_directionHud[i]->setVisible(false);
    }

    bool haveHitMarkerHud = s_hitMarkerHud[0] && s_hitMarkerHud[1]
        && s_hitMarkerHud[2] && s_hitMarkerHud[3];
    if (haveHitMarkerHud)
    {
        ULONGLONG now = GetTickCount64();
        bool hitMarkerWant = directionWant && now < s_hitMarkerUntilMs;
        if (hitMarkerWant)
        {
            MyGUI::IntSize v = MyGUI::RenderManager::getInstance().getViewSize();
            int cx = v.width / 2, cy = v.height / 2;
            // Keep the 28px glyphs.  Pull their visual centres from ~16px to ~8px.
            s_hitMarkerHud[0]->setPosition(cx - 22, cy - 22);
            s_hitMarkerHud[1]->setPosition(cx - 6,  cy - 22);
            s_hitMarkerHud[2]->setPosition(cx - 22, cy - 6);
            s_hitMarkerHud[3]->setPosition(cx - 6,  cy - 6);
            float remaining = (float)(s_hitMarkerUntilMs - now) / (float)HIT_MARKER_VISIBLE_MS;
            float alpha = 0.22f + 0.60f * remaining;
            if (alpha > 0.82f) alpha = 0.82f;
            for (int i = 0; i < 4; ++i)
            {
                s_hitMarkerHud[i]->setAlpha(alpha);
                if (!s_hitMarkerHud[i]->getVisible()) s_hitMarkerHud[i]->setVisible(true);
            }
        }
        else
            for (int i = 0; i < 4; ++i)
                if (s_hitMarkerHud[i]->getVisible()) s_hitMarkerHud[i]->setVisible(false);
    }
}

static bool focusTargetLoaded(Character* t);   // fwd decl (manual combat, below)
static bool focusTargetValid(Character* t);    // fwd decl (manual combat, below)
static bool manualVPlayerTeamMember(Character* target);
static Character* focusNpcTargetUnderCrosshair();

// Observation UI deliberately has its own read-only target. Every relationship
// uses the same strict projected-body overlap test; combat focus, AI targets and
// interaction candidates are neither consumed nor mutated here.
static Character* s_barsObservedLogged = nullptr;
static Character* focusDisplayTarget()
{
    Character* observed = focusNpcTargetUnderCrosshair();
    if (observed != s_barsObservedLogged)
    {
        s_barsObservedLogged = observed;
        VerbLog(observed ? "[WASDCombat] bars_observed_target set"
                         : "[WASDCombat] bars_observed_target none");
    }
    return observed;
}


// Game-space point -> screen pixel.  Game -> render space via the root node's
// shift; projection with the camera's own matrices and an explicit w-divide.
// pullToCam nudges the point toward the camera so a marker floats off the body.
static bool focusProject(Ogre::Camera* cam, Ogre::Vector3 gamePos, float pullToCam, int& sx, int& sy)
{
    Ogre::Vector3 rootShift = Ogre::Vector3::ZERO;
    if (cam->getSceneManager() && cam->getSceneManager()->getRootSceneNode())
        rootShift = cam->getSceneManager()->getRootSceneNode()->_getDerivedPosition();
    Ogre::Vector3 pt = gamePos + rootShift;
    Ogre::Vector3 toCam = cam->getDerivedPosition() - pt;
    if (pullToCam > 0.0f && toCam.squaredLength() > 1e-4f) pt += toCam.normalisedCopy() * pullToCam;
    Ogre::Matrix4 vp = cam->getProjectionMatrix() * cam->getViewMatrix(true);
    Ogre::Vector4 c4 = vp * Ogre::Vector4(pt.x, pt.y, pt.z, 1.0f);
    if (c4.w <= 0.001f) return false;
    MyGUI::IntSize v = MyGUI::RenderManager::getInstance().getViewSize();
    sx = (int)(((c4.x / c4.w) * 0.5f + 0.5f) * v.width);
    sy = (int)((0.5f - (c4.y / c4.w) * 0.5f) * v.height);
    return true;
}

// A bone's game-space position, or a fallback height above the feet.
static bool focusBonePos(Character* t, const char* bone, float fallbackUp, Ogre::Vector3& out)
{
    AppearanceBase* ap = t->getAppearance();
    Ogre::OldSkeletonInstance* sk = ap ? ap->getSkeleton() : nullptr;
    if (sk && sk->hasBone(bone)) { out = t->getBoneWorldPosition(std::string(bone)); return true; }
    if (!t->movement) return false;
    out = t->movement->pos + Ogre::Vector3(0.0f, fallbackUp, 0.0f);
    return true;
}

// Unified observation-only panel for every NPC relationship. It never assigns
// combat focus or an interaction target: the crosshair must directly overlap
// the projected body, and only one closest character can win.
static Character* focusNpcTargetUnderCrosshair()
{
    Character* me = s_freeMoveAnchor;
    if (!me || !me->movement || !s_gwFrame || !ou || !ou->player
        || !ou->player->camera || !ou->player->camera->camera) return nullptr;
    Ogre::Camera* cam = ou->player->camera->camera;
    MyGUI::IntSize view = MyGUI::RenderManager::getInstance().getViewSize();
    if (view.width <= 0 || view.height <= 0) return nullptr;
    const float aimX = view.width * 0.5f, aimY = view.height * 0.5f;
    const Ogre::Vector3 mp = me->movement->pos;
    const float maxRange = s_aimRange * 2.0f;
    const float maxR2 = maxRange * maxRange;
    Character* best = nullptr;
    float bestD2 = maxR2;
    auto& chars = s_gwFrame->getCharacterUpdateList();
    for (auto it = chars.begin(); it != chars.end(); ++it)
    {
        Character* c = *it;
        if (!c || c == me || !c->movement) continue;
        Ogre::Vector3 delta = c->movement->pos - mp;
        delta.y = 0.0f;
        float d2 = delta.squaredLength();
        if (d2 < 1e-4f || d2 >= bestD2) continue;

        Ogre::Vector3 head, chest, hips;
        int hx, hy, cx, cy, px, py, feetX, feetY;
        if (!focusBonePos(c, "Bip01 Head", 18.0f, head)
            || !focusBonePos(c, "Bip01 Spine1", 11.0f, chest)
            || !focusBonePos(c, "Bip01 Pelvis", 5.0f, hips)
            || !focusProject(cam, head, 0.0f, hx, hy)
            || !focusProject(cam, chest, 0.0f, cx, cy)
            || !focusProject(cam, hips, 0.0f, px, py)
            || !focusProject(cam, c->movement->pos + Ogre::Vector3(0.0f, 1.0f, 0.0f),
                             0.0f, feetX, feetY)) continue;

        int topPx = hy;
        if (cy < topPx) topPx = cy;
        if (py < topPx) topPx = py;
        if (feetY < topPx) topPx = feetY;
        int bottomPx = hy;
        if (cy > bottomPx) bottomPx = cy;
        if (py > bottomPx) bottomPx = py;
        if (feetY > bottomPx) bottomPx = feetY;
        float top = (float)topPx - 14.0f;
        float bottom = (float)bottomPx + 18.0f;
        float halfWidth = (bottom - top) * 0.16f + 8.0f;
        if (halfWidth < 24.0f) halfWidth = 24.0f;
        if (halfWidth > 65.0f) halfWidth = 65.0f;
        float centerX = (cx + px) * 0.5f;
        if (aimY > (float)py && feetY != py)
        {
            float t = (aimY - py) / (float)(feetY - py);
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            centerX = px + (feetX - px) * t;
        }
        if (fabsf(aimX - centerX) > halfWidth || aimY < top || aimY > bottom)
            continue;
        best = c;
        bestD2 = d2;
    }
    return best;
}

static bool focusHealthBarsCreate()
{
    if (s_hbTried) return s_hbLayer[0][0] != nullptr;
    s_hbTried = true;
    try
    {
        MyGUI::Gui* g = MyGUI::Gui::getInstancePtr();
        if (!g) return false;
        static const char* const LABEL[HB_COUNT] = { "B", "H", "C", "S", "LA", "RA", "LL", "RL" };
        // Exact skins from Kenshi_MainPanel.layout. Segment lengths below use
        // the same HealthPartStatus fields as vanilla; there is no custom RGB
        // approximation or health-threshold palette.
        static const char* const LAYER_SKIN[HBL_COUNT] = {
            "Kenshi_LifeBarSkin",        // healthy organic
            "Kenshi_LifeBarRobotSkin",   // healthy Skeleton / robotic limb
            "Kenshi_LifeBarStunnedSkin", // blunt / stun damage
            "Kenshi_LifeBarHurtSkin",    // untreated cut damage
            "Kenshi_LifeBarHealedSkin",  // bandaged / repaired cut damage
            "Kenshi_LifeBarCrushedSkin", // robotic wear-and-tear cap
            "Kenshi_LifeBarSplintSkin"   // splinted usable health overlay
        };
        for (int i = 0; i < HB_COUNT; ++i)
        {
            char nb[32];
            sprintf_s(nb, sizeof(nb), "dc_hb_bg_%d", i);
            s_hbBg[i]   = g->createWidget<MyGUI::Widget>("WhiteSkin", 0, 0, HB_BAR_W, HB_H, MyGUI::Align::Default, "Top", nb);
            if (!s_hbBg[i]) { DebugLog("[WASDCombat] aim_health_bars_create_failed"); return false; }
            s_hbBg[i]->setColour(MyGUI::Colour(0.04f, 0.04f, 0.04f)); s_hbBg[i]->setAlpha(0.78f);
            s_hbBg[i]->setNeedMouseFocus(false); s_hbBg[i]->setVisible(false);
            for (int layer = 0; layer < HBL_COUNT; ++layer)
            {
                char nf[40]; sprintf_s(nf, sizeof(nf), "dc_hb_%d_layer_%d", i, layer);
                s_hbLayer[i][layer] = g->createWidget<MyGUI::Widget>(LAYER_SKIN[layer], 0, 0,
                    HB_BAR_W - 2, HB_H - 2, MyGUI::Align::Default, "Top", nf);
                if (!s_hbLayer[i][layer]) { DebugLog("[WASDCombat] aim_health_bars_create_failed"); return false; }
                s_hbLayer[i][layer]->setColour(MyGUI::Colour::White);
                s_hbLayer[i][layer]->setAlpha(0.98f);
                s_hbLayer[i][layer]->setNeedMouseFocus(false);
                s_hbLayer[i][layer]->setVisible(false);
            }
            char nl[32]; sprintf_s(nl, sizeof(nl), "dc_hb_label_%d", i);
            s_hbLabel[i] = g->createWidget<MyGUI::TextBox>("Kenshi_TextboxStandardText_Small", 0, 0, HB_LABEL_W, HB_LH,
                                                             MyGUI::Align::Default, "Top", nl);
            if (!s_hbLabel[i]) { DebugLog("[WASDCombat] aim_health_bars_create_failed"); return false; }
            s_hbLabel[i]->setCaption(dcZh(LABEL[i]));
            s_hbLabel[i]->setTextAlign(MyGUI::Align::Right | MyGUI::Align::VCenter);
            s_hbLabel[i]->setTextColour(MyGUI::Colour(0.78f, 0.78f, 0.76f));
            s_hbLabel[i]->setTextShadow(true);
            s_hbLabel[i]->setTextShadowColour(MyGUI::Colour::Black);
            try { s_hbLabel[i]->setFontHeight(14); } catch (...) {}
            s_hbLabel[i]->setNeedMouseFocus(false); s_hbLabel[i]->setVisible(false);
        }
        DebugLog("[WASDCombat] aim_health_bars_created");
        return true;
    }
    catch (...) { DebugLog("[WASDCombat] aim_health_bars_create_failed"); return false; }
}
static std::string focusAsciiLower(const std::string& value)
{
    std::string out = value;
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = (char)tolower((unsigned char)out[i]);
    return out;
}

// Resolve anatomy from stable FCS IDs and language-independent type/side data,
// never from translated display names.  Canonical IDs keep chest/stomach apart;
// type + side also supports custom races with their own limb records.
static int focusHealthSlot(const GameData* part, const MedicalSystem::HealthPartStatus& hp)
{
    if (!part) return -1;
    std::string id = focusAsciiLower(part->stringID);
    // Kenshi's canonical anatomy records use numeric FCS IDs. Keep semantic
    // fallbacks for custom anatomy records whose authors supplied named IDs.
    if (id == "32-gamedata.quack" || id.find("head") != std::string::npos
        || hp.whatAmI == MedicalSystem::HealthPartStatus::PART_HEAD) return HB_HEAD;
    if (id == "101-gamedata.quack" || id.find("chest") != std::string::npos) return HB_CHEST;
    if (id == "100-gamedata.quack" || id.find("stomach") != std::string::npos) return HB_STOMACH;
    if (id == "28-gamedata.quack") return HB_LEFT_ARM;
    if (id == "29-gamedata.quack") return HB_RIGHT_ARM;
    if (id == "30-gamedata.quack") return HB_LEFT_LEG;
    if (id == "31-gamedata.quack") return HB_RIGHT_LEG;
    if (hp.whatAmI == MedicalSystem::HealthPartStatus::PART_ARM)
        return hp.side == SIDE_LEFT ? HB_LEFT_ARM : HB_RIGHT_ARM;
    if (hp.whatAmI == MedicalSystem::HealthPartStatus::PART_LEG)
        return hp.side == SIDE_LEFT ? HB_LEFT_LEG : HB_RIGHT_LEG;
    return -1;
}

struct FocusHealthState
{
    bool present;
    bool robotic;
    float healthyEnd;    // flesh - fleshStun
    float fleshEnd;      // current flesh (end of blunt/stun segment)
    float bandagedEnd;   // flesh + bandaging
    float effectiveEnd;  // base maximum minus robotic wear
    float splintStart;
    float splintEnd;     // juryRigging overlay
};
static float focusHealthClamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

// Reconstruct the medical composition shown by Kenshi's own health panel:
// green/robot-blue live health, grey stun, red untreated cut, yellow bandaged
// cut, white splint, and the crushed wear cap. Endpoints are normalised to the
// original age/HPMult maximum so robotic wear remains visible.
static void focusHealthStates(Character* t, FocusHealthState out[HB_COUNT])
{
    memset(out, 0, sizeof(FocusHealthState) * HB_COUNT);
    if (!t) return;
    MedicalSystem* med = &t->medical;
    float maxB = med->getMaxBlood();
    if (maxB > 0.0f)
    {
        out[HB_BLOOD].present = true;
        out[HB_BLOOD].healthyEnd = focusHealthClamp01(med->blood / maxB);
    }
    for (auto it = med->status.begin(); it != med->status.end(); ++it)
    {
        GameData* gd = it->first;
        MedicalSystem::HealthPartStatus& hp = it->second;
        if (!gd) continue;
        int slot = focusHealthSlot(gd, hp);
        if (!s_hbNamesLogged)
        {
            char b[640];
            sprintf_s(b, sizeof(b), "[WASDCombat] aim_health_part id=%s name=%s type=%d side=%d slot=%d flesh=%.1f stun=%.1f bandage=%.1f rig=%.1f wear=%.1f max=%.1f robot=%d",
                      gd->stringID.c_str(), gd->name.c_str(), (int)hp.whatAmI, (int)hp.side,
                      slot, hp.flesh, hp.fleshStun, hp.bandaging, hp.juryRigging,
                      hp.wearDamage, hp._maxHealth, hp.isRobotic() ? 1 : 0);
            VerbLog(b);
        }
        if (slot < 0) continue;
        float baseMax = hp._maxHealth * hp.age * hp.HPMult;
        if (baseMax <= 0.0f) baseMax = hp._maxHealth;
        if (baseMax <= 0.0f) continue;
        float effectiveMax = baseMax - hp.wearDamage;
        if (effectiveMax < 0.0f) effectiveMax = 0.0f;
        if (effectiveMax > baseMax) effectiveMax = baseMax;
        FocusHealthState& s = out[slot];
        s.present = true;
        s.robotic = hp.isRobotic();
        s.effectiveEnd = focusHealthClamp01(effectiveMax / baseMax);
        s.healthyEnd = focusHealthClamp01((hp.flesh - hp.fleshStun) / baseMax);
        s.fleshEnd = focusHealthClamp01(hp.flesh / baseMax);
        s.bandagedEnd = focusHealthClamp01((hp.flesh + hp.bandaging) / baseMax);
        if (s.healthyEnd > s.effectiveEnd) s.healthyEnd = s.effectiveEnd;
        if (s.fleshEnd < s.healthyEnd) s.fleshEnd = s.healthyEnd;
        if (s.fleshEnd > s.effectiveEnd) s.fleshEnd = s.effectiveEnd;
        if (s.bandagedEnd < s.fleshEnd) s.bandagedEnd = s.fleshEnd;
        if (s.bandagedEnd > s.effectiveEnd) s.bandagedEnd = s.effectiveEnd;
        s.splintStart = s.healthyEnd;
        s.splintEnd = focusHealthClamp01((hp.flesh - hp.fleshStun + hp.juryRigging) / baseMax);
        if (s.splintEnd < s.splintStart) s.splintEnd = s.splintStart;
        if (s.splintEnd > s.effectiveEnd) s.splintEnd = s.effectiveEnd;
    }
    if (!s_hbNamesLogged)
    {
        s_hbNamesLogged = true;
        VerbLog("[WASDCombat] aim_health_parts_resolved");
    }
}
static void focusHealthLayerPlace(int slot, int layer, int x, int y, int startPx, int endPx, bool on)
{
    MyGUI::Widget* w = s_hbLayer[slot][layer];
    if (!w) return;
    int width = endPx - startPx;
    if (on && width > 0)
    {
        w->setCoord(x + startPx, y, width, HB_H - 2);
        w->setVisible(true);
    }
    else if (w->getVisible()) w->setVisible(false);
}
static bool focusNameTagCreate()
{
    if (s_nameTag || s_nameTagTried) return s_nameTag != nullptr;
    s_nameTagTried = true;
    try
    {
        MyGUI::Gui* g = MyGUI::Gui::getInstancePtr();
        if (!g) return false;
        s_nameTag = g->createWidget<MyGUI::TextBox>("Kenshi_TextboxStandardText_Small",
                        0, 0, 220, 18, MyGUI::Align::Default, "Top", "dc_aim_name_tag");
        if (!s_nameTag) return false;
        s_nameTag->setTextAlign(MyGUI::Align::Center);
        s_nameTag->setTextColour(MyGUI::Colour::White); // relation colour is assigned per observed NPC
        s_nameTag->setTextShadow(true);
        s_nameTag->setTextShadowColour(MyGUI::Colour::Black);
        s_nameTag->setNeedMouseFocus(false);
        s_nameTag->setVisible(false);
        DebugLog("[WASDCombat] aim_name_tag_created");
        return true;
    }
    catch (...) { s_nameTag = nullptr; DebugLog("[WASDCombat] aim_name_tag_create_failed"); return false; }
}

static bool manualInteractionHintCreate()
{
    if ((s_interactionHint && s_interactionHintBg) || s_interactionHintTried)
        return s_interactionHint && s_interactionHintBg;
    s_interactionHintTried = true;
    try
    {
        MyGUI::Gui* g = MyGUI::Gui::getInstancePtr();
        if (!g) return false;
        s_interactionHintBg = g->createWidget<MyGUI::Widget>(
            "WhiteSkin", 0, 0, 32, 28,
            MyGUI::Align::Default, "Top", "ironwind_interaction_hint_bg");
        s_interactionHint = g->createWidget<MyGUI::TextBox>(
            "Kenshi_TextboxStandardText_Small", 0, 0, 32, 28,
            MyGUI::Align::Default, "Top", "ironwind_interaction_hint");
        if (!s_interactionHint || !s_interactionHintBg) return false;
        s_interactionHintBg->setColour(MyGUI::Colour(0.04f, 0.04f, 0.035f));
        s_interactionHintBg->setAlpha(0.78f);
        s_interactionHintBg->setNeedMouseFocus(false);
        s_interactionHintBg->setVisible(false);
        s_interactionHint->setCaption("E");
        s_interactionHint->setTextAlign(MyGUI::Align::Center | MyGUI::Align::VCenter);
        s_interactionHint->setTextColour(MyGUI::Colour(1.00f, 0.84f, 0.22f));
        s_interactionHint->setTextShadow(true);
        s_interactionHint->setTextShadowColour(MyGUI::Colour::Black);
        try { s_interactionHint->setFontHeight(20); } catch (...) {}
        s_interactionHint->setNeedMouseFocus(false);
        s_interactionHint->setVisible(false);
        DebugLog("[Ironwind] interaction_hint_created");
        return true;
    }
    catch (...)
    {
        s_interactionHintBg = nullptr;
        s_interactionHint = nullptr;
        DebugLog("[Ironwind] interaction_hint_create_failed");
        return false;
    }
}

static void manualInteractionHintUpdate(Ogre::Camera* cam, bool live)
{
    bool show = live && s_directionalCombat && (s_firstPersonActive || s_otsCamActive)
        && !s_interactionMenuByE && !s_interactionOpenPending
        && !s_attackAimHeld && !s_attackActive && !manualAttackCommitLive()
        && !s_mouseBlockHeld && !s_blockHeld
        && !manualHitReactionLive(s_freeMoveAnchor)
        && !dcUiWantsCursor()
        && s_interactionTargetHandle.isValid();
    RootObject* target = show ? s_interactionTargetHandle.getRootObject() : nullptr;
    show = show && target && target->isValid();
    if (show && (s_interactionHint || manualInteractionHintCreate()))
    {
        Ogre::Vector3 marker = Ogre::Vector3::ZERO;
        if (target->getDataType() == CHARACTER)
        {
            Character* c = static_cast<Character*>(target);
            show = focusBonePos(c, "Bip01 Spine1", 11.0f, marker);
        }
        else if (target->getDataType() == BUILDING)
        {
            Building* b = static_cast<Building*>(target);
            marker = b->getAABB().mCenter;
            if (fabsf(marker.x) >= 1.0e7f || fabsf(marker.y) >= 1.0e7f
                || fabsf(marker.z) >= 1.0e7f)
                marker = b->getPosition() + Ogre::Vector3(0.0f, 2.5f, 0.0f);
        }
        else marker = target->getPosition() + Ogre::Vector3(0.0f, 2.0f, 0.0f);
        int sx = 0, sy = 0;
        show = show && focusProject(cam, marker, 0.0f, sx, sy);
        if (show)
        {
            MyGUI::IntSize view = MyGUI::RenderManager::getInstance().getViewSize();
            show = sx >= 0 && sy >= 0 && sx < view.width && sy < view.height;
            if (show)
            {
                int x = sx + 26, y = sy - 14;
                if (x + 32 >= view.width) x = sx - 58;
                if (x < 0) x = 0;
                if (y < 0) y = 0;
                if (y + 28 >= view.height) y = view.height - 29;
                s_interactionHintBg->setCoord(x, y, 32, 28);
                s_interactionHint->setCoord(x, y, 32, 28);
                if (!s_interactionHintWasVisible)
                {
                    char b[128];
                    sprintf_s(b, sizeof(b), "[Ironwind] interaction_hint_visible type=%d x=%d y=%d",
                              (int)target->getDataType(), x, y);
                    VerbLog(b);
                }
            }
        }
    }
    if (s_interactionHintBg && s_interactionHintBg->getVisible() != show)
        s_interactionHintBg->setVisible(show);
    if (s_interactionHint && s_interactionHint->getVisible() != show)
        s_interactionHint->setVisible(show);
    if (!show && s_interactionHintWasVisible)
        VerbLog("[Ironwind] interaction_hint_hidden");
    s_interactionHintWasVisible = show;
}
// Character::attackingYou - the victim is told at swing start.  Track the
// attacker (identity only) when WE are the victim.
static void (*s_attackingYouOrig)(Character* self, Character* attacker, bool so, bool doAwarenessCheck);
static void attackingYou_hook(Character* self, Character* attacker, bool so, bool doAwarenessCheck)
{
    // Direct AttackState announces its intended victim while it is being
    // initialised.  For an actor that was neutral at LMB-down, forwarding that
    // announcement here creates an incoherent half-commit: the victim can turn
    // locally hostile and retaliate even if the swing later misses, while the
    // campaign/faction layer is (correctly) still waiting for _getHit.  Defer
    // this one exact notification.  A confirmed _getHit replays the complete
    // native campaign + awareness + victim-response transaction through
    // manualAttackProvokeVictim; already-hostile combat remains untouched.
    if (s_attackDirectInitialising && s_attackActive && s_manualAttackOn
        && s_mode == MODE_FREE_MOVE && !s_dcShutdownInProgress && !s_loadGuardActive
        && self && self == s_attackNeutralIntentTarget
        && attacker && attacker == s_freeMoveAnchor && attacker != self)
    {
        VerbLog("[Ironwind] neutral_precontact_notice_deferred");
        return;
    }

    s_attackingYouOrig(self, attacker, so, doAwarenessCheck);
    // Record native incoming attacks for player-authorised automatic guard.
    // v121 deliberately has no visual consumer: the tracker is game-state only.
    if (!self || self != s_freeMoveAnchor || !attacker || attacker == self
        || s_mode != MODE_FREE_MOVE || s_dcShutdownInProgress || s_loadGuardActive) return;
    ULONGLONG now = GetTickCount64();
    int slot = -1;
    for (int i = 0; i < INCOMING_ATTACK_MAX; ++i) if (s_incomingAttack[i].who == attacker) { slot = i; break; }
    if (slot < 0) for (int i = 0; i < INCOMING_ATTACK_MAX; ++i) if (!s_incomingAttack[i].who) { slot = i; break; }
    if (slot < 0) { slot = 0; for (int i = 1; i < INCOMING_ATTACK_MAX; ++i) if (s_incomingAttack[i].startMs < s_incomingAttack[slot].startMs) slot = i; }
    s_incomingAttack[slot].who = attacker;
    s_incomingAttack[slot].startMs = now;
    s_incomingAttack[slot].chopMs = 0;
    s_incomingAttack[slot].impactMs = 0;
}
// Is any tracked swing still coming at us?  (Used by the idle pin so the AI's
// own block logic is never suppressed while a hit is on the way.)
static bool incomingAttackLive()
{
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < INCOMING_ATTACK_MAX; ++i)
    {
        IncomingAttackSlot& g = s_incomingAttack[i];
        if (!g.who || g.impactMs != 0 || now - g.startMs >= 4000
            || !focusTargetLoaded(g.who)) continue;
        CombatClass* cc = g.who->getCombatClass();
        swordStateEnum st = cc ? cc->getCombatState() : COMBAT_FINISHED;
        if (cc && cc->combatModeActive && (st == STARTUP_STATE || st == CHOP_WEAPON))
            return true;
    }
    return false;
}
// The attacker whose swing is coming at us right now (earliest live swing), or
// nullptr.  Build 61: the G block POSE is picked for this enemy rather than the
// aim focus, so a hit from behind gets the rear block technique (before, a
// front-facing pose injected for the aimed enemy could sit in the BLOCK state
// while the real swing came from behind, and vanilla could not choose a rear
// block until that clip ended).
static Character* incomingAttackAttacker()
{
    ULONGLONG now = GetTickCount64();
    Character* best = nullptr; ULONGLONG bestStart = 0;
    for (int i = 0; i < INCOMING_ATTACK_MAX; ++i)
    {
        const IncomingAttackSlot& g = s_incomingAttack[i];
        if (!g.who || g.impactMs != 0 || now - g.startMs >= 1500) continue;
        if (!focusTargetLoaded(g.who) || g.who->isDead() || g.who->isUnconcious()) continue;
        if (!best || g.startMs < bestStart) { best = g.who; bestStart = g.startMs; }
    }
    return best;
}
// First visible EditBox with text under a root widget (depth-limited).
static MyGUI::EditBox* dcFindSpeechEdit(MyGUI::Widget* w, int depth)
{
    if (!w || depth > 3 || !w->getVisible()) return nullptr;
    MyGUI::EditBox* e = w->castType<MyGUI::EditBox>(false);
    if (e && !e->getCaption().empty()) return e;
    size_t n = w->getChildCount();
    for (size_t i = 0; i < n; ++i)
    {
        MyGUI::EditBox* r = dcFindSpeechEdit(w->getChildAt(i), depth + 1);
        if (r) return r;
    }
    return nullptr;
}
// The top edge (screen y) of a visible speech bubble sitting over the enemy
// whose head projects to (sx, sy); -1 when there is none.
static int bubbleTopNear(int sx, int sy)
{
    int best = -1;
    MyGUI::Gui* g = MyGUI::Gui::getInstancePtr();
    if (!g) return -1;
    try
    {
        MyGUI::EnumeratorWidgetPtr en = g->getEnumerator();
        while (en.next())
        {
            MyGUI::Widget* root = en.current();
            if (!root || !root->getVisible() || root->getAlpha() <= 0.01f) continue;
            MyGUI::IntCoord c = root->getAbsoluteCoord();
            if (c.width <= 0 || c.height <= 0 || c.height > 160 || c.width > 520) continue;   // bubbles are small
            int cx = c.left + c.width / 2;
            if (abs(cx - sx) > 160) continue;                       // not over this head
            if (c.top < sy - 260 || c.top > sy + 80) continue;      // not near it vertically
            if (!dcFindSpeechEdit(root, 0)) continue;               // no text = not a bubble
            if (best < 0 || c.top < best) best = c.top;
        }
    }
    catch (...) { return -1; }
    return best;
}
// The tracked swing resolved (hit or block). Keep this event for automatic
// guard lifecycle diagnostics; it has no visual side effect.
static void incomingAttackImpact(Character* who)
{
    if (!who) return;
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < INCOMING_ATTACK_MAX; ++i)
        if (s_incomingAttack[i].who == who)
        {
            IncomingAttackSlot& incoming = s_incomingAttack[i];
            bool first = incoming.impactMs == 0;
            if (first) incoming.impactMs = now;
            if (first)
            {
                char b[128];
                sprintf_s(b, sizeof(b), "[WASDCombat] incoming_attack_impact ms=%llu chop_ms=%llu",
                          now - incoming.startMs, incoming.chopMs ? now - incoming.chopMs : 0ULL);
                VerbLog(b);
            }
        }
}

// Per frame (camera hook, post-orig): the NPC directly under the crosshair gets
// a relation-coloured name and compact native-colour health bars. Incoming
// attacks are only expired here; no hand/weapon marker is rendered.
static void focusMarkersUpdate(CameraClass* thisptr)
{
    bool live = s_mode == MODE_FREE_MOVE && !s_loadGuardActive && !s_dcShutdownInProgress
             && thisptr && thisptr->camera && s_freeMoveAnchor;
    Ogre::Camera* cam = live ? thisptr->camera : nullptr;
    ULONGLONG now = GetTickCount64();
    int sx, sy;

    // Read-only observed-NPC name tag and anatomy panel.
    bool tagOn = false;
    Character* tagTgt = (live && s_settingAimName) ? focusDisplayTarget() : nullptr;
    if (live && s_settingAimName && tagTgt
        && (s_nameTag || focusNameTagCreate()))
    {
        Ogre::Vector3 head;
        if (focusBonePos(tagTgt, "Bip01 Head", 17.0f, head)
            && focusProject(cam, head + Ogre::Vector3(0.0f, 3.0f, 0.0f), 0.0f, sx, sy))
        {
            if (s_nameTagFor != tagTgt)
            {
                s_nameTagFor = tagTgt;
                try { s_nameTag->setCaption(tagTgt->getName()); }
                catch (...) { s_nameTag->setCaption("?"); }
            }
            // Re-evaluate every frame: a neutral can become hostile while the
            // crosshair remains on the same character after a confirmed hit.
            const bool enemy = tagTgt->isEnemy(s_freeMoveAnchor, true)
                            || s_freeMoveAnchor->isEnemy(tagTgt, true);
            const bool friendly = !enemy && (manualVPlayerTeamMember(tagTgt)
                                           || tagTgt->isAlly(s_freeMoveAnchor, true));
            s_nameTag->setTextColour(enemy
                ? MyGUI::Colour(0.92f, 0.22f, 0.22f)
                : (friendly ? MyGUI::Colour(0.30f, 0.95f, 0.38f)
                            : MyGUI::Colour(1.00f, 0.78f, 0.18f)));
            const int W = 220, H = 18;
            int headY = sy;
            sy -= HB_RAISE;                                       // lift the whole group
            {
                // Vanilla speech text above this NPC? Step the group above it.
                int bubbleTop = bubbleTopNear(sx, headY);
                int groupBottom = sy + 1 + (HB_ROWS - 1) * HB_ROW_H + HB_LH;
                if (bubbleTop >= 0 && groupBottom + 4 > bubbleTop)
                    sy -= (groupBottom + 4 - bubbleTop);
            }
            s_nameTag->setCoord(sx - W / 2, sy - H, W, H);
            tagOn = true;
            // Compact two-row anatomy panel.  Every short strip is the actual
            // blood/body-part value; row two is exclusively the four limbs.
            if (s_hbLayer[0][0] || focusHealthBarsCreate())
            {
                FocusHealthState health[HB_COUNT];
                focusHealthStates(tagTgt, health);
                for (int i = 0; i < HB_COUNT; ++i)
                {
                    int row = i / HB_COLS, col = i % HB_COLS;
                    int cellX = sx - HB_PANEL_W / 2 + col * (HB_CELL_W + HB_GAP);
                    int rowY = sy + 1 + row * HB_ROW_H;
                    int bx = cellX + HB_LABEL_W;
                    int by = rowY + (HB_LH - HB_H) / 2;
                    s_hbBg[i]->setCoord(bx, by, HB_BAR_W, HB_H);
                    s_hbBg[i]->setVisible(health[i].present);
                    const int inner = HB_BAR_W - 2;
                    int healthyEnd = (int)(inner * health[i].healthyEnd + 0.5f);
                    int fleshEnd = (int)(inner * health[i].fleshEnd + 0.5f);
                    int bandagedEnd = (int)(inner * health[i].bandagedEnd + 0.5f);
                    int effectiveEnd = (int)(inner * health[i].effectiveEnd + 0.5f);
                    int splintStart = (int)(inner * health[i].splintStart + 0.5f);
                    int splintEnd = (int)(inner * health[i].splintEnd + 0.5f);
                    int px = bx + 1, py = by + 1;
                    if (i == HB_BLOOD)
                    {
                        focusHealthLayerPlace(i, HBL_CUT, px, py, 0, healthyEnd, health[i].present);
                        for (int layer = 0; layer < HBL_COUNT; ++layer)
                            if (layer != HBL_CUT) focusHealthLayerPlace(i, layer, px, py, 0, 0, false);
                    }
                    else
                    {
                        focusHealthLayerPlace(i, HBL_HEALTHY, px, py, 0, healthyEnd,
                                              health[i].present && !health[i].robotic);
                        focusHealthLayerPlace(i, HBL_ROBOT, px, py, 0, healthyEnd,
                                              health[i].present && health[i].robotic);
                        focusHealthLayerPlace(i, HBL_STUN, px, py, healthyEnd, fleshEnd, health[i].present);
                        focusHealthLayerPlace(i, HBL_BANDAGED, px, py, fleshEnd, bandagedEnd, health[i].present);
                        focusHealthLayerPlace(i, HBL_CUT, px, py, bandagedEnd, effectiveEnd, health[i].present);
                        focusHealthLayerPlace(i, HBL_WEAR, px, py, effectiveEnd, inner,
                                              health[i].present && health[i].robotic);
                        // Splint is an overlay, just as in the original panel;
                        // it marks usable rigged HP without deleting the wound.
                        focusHealthLayerPlace(i, HBL_SPLINT, px, py, splintStart, splintEnd, health[i].present);
                    }
                    if (s_hbLabel[i])
                    {
                        s_hbLabel[i]->setCoord(cellX, rowY, HB_LABEL_W, HB_LH);
                        s_hbLabel[i]->setVisible(health[i].present);
                    }
                }
            }
        }
    }
    if (s_nameTag && s_nameTag->getVisible() != tagOn) s_nameTag->setVisible(tagOn);
    if (!tagOn)
    {
        s_nameTagFor = nullptr;
        for (int i = 0; i < HB_COUNT; ++i)
        {
            if (s_hbBg[i]    && s_hbBg[i]->getVisible())    s_hbBg[i]->setVisible(false);
            for (int layer = 0; layer < HBL_COUNT; ++layer)
                if (s_hbLayer[i][layer] && s_hbLayer[i][layer]->getVisible()) s_hbLayer[i][layer]->setVisible(false);
            if (s_hbLabel[i] && s_hbLabel[i]->getVisible()) s_hbLabel[i]->setVisible(false);
        }
    }

    // The E prompt is anchored in world space and follows the selected native
    // interaction target.  It is intentionally separate from the top combat
    // label so people, doors and furniture read as one consistent affordance.
    manualInteractionHintUpdate(cam, live);

    // Expire incoming-attack state without rendering it. Automatic guard reads
    // the same slots, so this lifecycle must remain even though the old visual
    // cue and all of its texture/widgets/configuration were removed in v121.
    for (int i = 0; i < INCOMING_ATTACK_MAX; ++i)
    {
        IncomingAttackSlot& g = s_incomingAttack[i];
        bool drop = false;
        if (g.who)
        {
            if (!live || !focusTargetLoaded(g.who) || g.who->isDead() || g.who->isUnconcious()) drop = true;
            else
            {
                CombatClass* cc = g.who->getCombatClass();
                swordStateEnum st = cc ? cc->getCombatState() : COMBAT_FINISHED;
                bool swinging = cc && cc->combatModeActive && (st == STARTUP_STATE || st == CHOP_WEAPON);
                if (st == CHOP_WEAPON && g.chopMs == 0) g.chopMs = now;
                if (g.impactMs == 0 && !swinging && now - g.startMs > 150) g.impactMs = now;
                if (g.impactMs != 0 && now - g.impactMs > 180) drop = true;
                if (now - g.startMs > 4000) drop = true;
            }
        }
        if (drop) g.who = nullptr;
    }

}

static void dcUpdateCrosshair(bool want)
{
    MyGUI::Gui* g = MyGUI::Gui::getInstancePtr();
    if (!g) return;
    if (want && !s_xhH && !s_xhTried)
    {
        s_xhTried = true;
        try
        {
            MyGUI::IntSize v = MyGUI::RenderManager::getInstance().getViewSize();
            int cx = v.width / 2, cy = v.height / 2;
            const int L = 11, T = 1;   // arm length / thickness (px)
            s_xhH = g->createWidget<MyGUI::Widget>("WhiteSkin",
                cx - L / 2, cy - T / 2, L, T, MyGUI::Align::Center, "Top", "dc_crosshair_h");
            s_xhV = g->createWidget<MyGUI::Widget>("WhiteSkin",
                cx - T / 2, cy - L / 2, T, L, MyGUI::Align::Center, "Top", "dc_crosshair_v");
            if (s_xhH) { s_xhH->setColour(MyGUI::Colour::White); s_xhH->setNeedMouseFocus(false); s_xhH->setVisible(false); }
            if (s_xhV) { s_xhV->setColour(MyGUI::Colour::White); s_xhV->setNeedMouseFocus(false); s_xhV->setVisible(false); }
            DebugLog("[WASDCombat] dc_crosshair_created");
        }
        catch (...) { s_xhH = nullptr; s_xhV = nullptr;
            DebugLog("[WASDCombat] dc_crosshair_create_failed"); }
    }
    bool have = (s_xhH && s_xhV);
    if (!have) return;   // no crosshair → leave the normal cursor alone

    // Only a verified player-talk bubble may replace the reticle.  Neutral
    // "green"/yellow arrows stay hidden behind the "+".
    bool iconWanted = want && dcPointerIsContextual();
    bool showReticle = want && !iconWanted;
    MyGUI::Colour tint = MyGUI::Colour::White;
    if (s_xhairTint == 1) tint = MyGUI::Colour(0.95f, 0.82f, 0.18f);      // neutral
    else if (s_xhairTint == 2) tint = MyGUI::Colour(0.35f, 0.85f, 0.35f);  // ally
    s_xhH->setColour(tint);
    s_xhV->setColour(tint);
    s_xhH->setVisible(showReticle);
    s_xhV->setVisible(showReticle);

    // XHAIR-TWEAK: the crosshair IS the cursor while a look-mode is active.
    // (1) RETURN-TO-CENTER on the rising edge (want false->true) — the moment a
    //     pause/UI clears and the mode takes back over, snap the cursor (OS +
    //     MyGUI) to screen centre so it re-locks under the "+".  This is the
    //     "unpausing returns the cursor to the centre" step; because want only
    //     rises once EVERY suspending UI has closed, a UI still open at unpause
    //     correctly keeps the cursor free until it too closes (user 2026-09-06).
    // (2) HOLD it there while active — pin MyGUI's tracked pick position to
    //     centre every frame so clicks always act under the "+".
    if (want && !s_xhWasWant)
    {
        MyGUI::InputManager* im = MyGUI::InputManager::getInstancePtr();
        MyGUI::IntSize v = MyGUI::RenderManager::getInstance().getViewSize();
        if (im) im->injectMouseMove(v.width / 2, v.height / 2, 0);
        HWND fg = GetForegroundWindow();
        RECT rc;
        if (fg && GetClientRect(fg, &rc))
        {
            POINT c; c.x = (rc.left + rc.right) / 2; c.y = (rc.top + rc.bottom) / 2;
            ClientToScreen(fg, &c);
            SetCursorPos(c.x, c.y);
        }
    }
    else if (want)
    {
        MyGUI::InputManager* im = MyGUI::InputManager::getInstancePtr();
        if (im)
        {
            MyGUI::IntSize v = MyGUI::RenderManager::getInstance().getViewSize();
            im->injectMouseMove(v.width / 2, v.height / 2, 0);
        }
    }
    s_xhWasWant = want;

    MyGUI::PointerManager* pm = MyGUI::PointerManager::getInstancePtr();
    if (pm)
    {
        if (want)
        {
            // XHAIR-INDICATOR: plain arrow stays hidden behind the "+";
            // contextual pointer shows through as the interaction indicator.
            pm->setVisible(iconWanted);
            s_xhPointerHidden = true;   // we own pointer visibility while active
        }
        else if (s_xhPointerHidden) { pm->setVisible(true); s_xhPointerHidden = false; }
    }
}

// enterOtsCam — detach the Ogre camera onto our own node so the OTS orbit is
// fully ours (mirrors enterFirstPerson).  No engine rotate, no cursor sharing.
static void enterOtsCam()
{
    if (s_otsCamActive || s_fpActive || s_firstPersonActive) return;
    if (!ou || !ou->player || !ou->player->camera
        || !s_freeMoveAnchor || !s_freeMoveAnchor->movement) return;
    CameraClass* cam = ou->player->camera;
    Ogre::Camera* oc = cam->camera;
    if (!oc) return;
    cam->stopFollowing();
    // Capture the rig camera's VANILLA locals ONLY when none are held (v1.4.1
    // upside-down-exit root cause, log 2026-09-12): every zone crossing briefly
    // raises isLoadingFromASaveGame, the camera hook's scene-freeing branch
    // drops s_otsCamActive WITHOUT touching the camera (scene may be dying),
    // and the next frame re-enters here — with the camera STILL parented on
    // our old orbit node.  Capturing then saved local pos ZERO / orientation
    // IDENTITY / the OTS FOV / the blended near clip as "vanilla", and the
    // exit put exactly that onto the rig = camera at the pivot, no auto-track,
    // world upside-down until restart.  The saved values are plain numbers
    // (never pointers), so holding them across a stream — or a real load —
    // is always safe; they're released only by exitOtsCam's restore.
    if (!s_otsCamLocalsSaved)
    {
        s_otsHadAutoTrack   = (oc->getAutoTrackTarget() != nullptr);
        s_otsSavedCamPos    = oc->getPosition();
        s_otsSavedCamOri    = oc->getOrientation();
        s_otsSavedFovR      = oc->getFOVy().valueRadians();
        s_otsSavedNearClip  = oc->getNearClipDistance();
        {
            char cb[128];
            sprintf_s(cb, sizeof(cb), "[WASDCombat] dc_cam_planes_ots vanilla_near=%.3f far=%.1f", s_otsSavedNearClip, oc->getFarClipDistance());
            VerbLog(cb);
        }
        s_otsCamLocalsSaved = true;
    }
    else DebugLog("[WASDCombat] dc_otscam_reentry_keeping_saved_locals");
    oc->setAutoTracking(false);
    oc->setFOVy(Ogre::Radian(Ogre::Degree(s_otsFovDeg)));
    oc->detachFromParent();
    s_otsNode = oc->getSceneManager()->getRootSceneNode()->createChildSceneNode();
    s_otsNode->attachObject(oc);
    oc->setPosition(Ogre::Vector3::ZERO);
    oc->setOrientation(Ogre::Quaternion::IDENTITY);
    // Seed the look yaw from the character's current facing so the view opens
    // behind them (not snapped to some arbitrary heading).
    Ogre::Vector3 d = s_freeMoveAnchor->movement->direction;
    d.y = 0.0f; float dl = d.length();
    if (dl > 0.001f) { d /= dl; s_otsYaw = atan2f(-d.x, -d.z); }
    s_otsPitch = -0.25f;               // negative pitch = look DOWN (over-shoulder)
    // Returning from a wheel-entered first-person view keeps the distance the
    // wheel reached.  A fresh V/F entry still begins at the configured base.
    if (!s_otsResumeZoomPending) s_otsZoomCur = s_otsCamDist;
    s_otsResumeZoomPending = false;
    InterlockedExchange(&s_diAccZ, 0); // discard wheel movement from earlier UIs
    s_otsCollDistSm = s_otsZoomCur;
    s_otsCamDistApplied = s_otsCamDist;
    s_otsCursorCaptured = false;
    s_otsLastStreamValid = false;   // rig coherence: teleport on the first drive frame
    s_otsRootSmValid     = false;   // orbit root re-seeds on the first drive frame
    s_otsRootBlendFrames = 0;
    s_otsCamActive = true;
    DebugLog("[WASDCombat] dc_otscam_entered");
}

// exitOtsCam — re-attach the camera to the rig and restore saved locals.
static void exitOtsCam(bool restoreCamera)
{
    if (!s_otsCamActive) return;
    s_otsCamActive = false;
    if (!restoreCamera) s_rigYawSaved = false;   // camera not ours to restore (load): drop the map-arrow save
    if (restoreCamera && ou && ou->player && ou->player->camera
        && ou->player->camera->camera)
    {
        CameraClass* cam = ou->player->camera;
        Ogre::Camera* oc = cam->camera;
        if (s_rigYawSaved) { cam->yaw = s_rigYawSave; s_rigYawSaved = false; }   // map-arrow sync off
        // Snap the vanilla rig HOME to the anchor BEFORE re-attaching (v1.4.1):
        // after cross-zone OTS travel the parked rig could be a whole zone
        // away, and re-attaching there forced a huge follow snap whose look-at
        // could pass through the vertical and leave the camera ROLLED — the
        // vanilla orbit only ever steers yaw/pitch, so the view stayed upside-
        // down until a game restart (user reports 2026-09).  With the rig
        // teleported to the anchor first, the exit geometry is always local.
        if (s_freeMoveAnchor && s_freeMoveAnchor->movement)
        {
            Ogre::Vector3 home = s_freeMoveAnchor->movement->pos;
            cam->teleport(home);
            cam->targetPositionY = home.y;
            cam->speedY          = 0.0f;
        }
        oc->detachFromParent();
        Ogre::SceneNode* rig = cam->getCameraNode();
        if (rig) rig->attachObject(oc);
        if (s_otsCamLocalsSaved)
        {
            oc->setPosition(s_otsSavedCamPos);
            // Restore the saved local orientation with any ROLL stripped:
            // rebuild it from its look direction as pure yaw*pitch (the only
            // orientations the vanilla orbit can recover from).  Belt-and-
            // braces for the upside-down exit — a rolled quaternion must
            // never reach the rig.
            Ogre::Vector3 svD = s_otsSavedCamOri * Ogre::Vector3::NEGATIVE_UNIT_Z;
            float svL = svD.length();
            if (svL > 0.001f)
            {
                svD /= svL;
                float svPy = svD.y;
                if (svPy >  0.999f) svPy =  0.999f;
                if (svPy < -0.999f) svPy = -0.999f;
                float svPitch = asinf(svPy);
                float svYaw   = (fabsf(svD.x) + fabsf(svD.z) > 0.0001f)
                              ? atan2f(-svD.x, -svD.z) : 0.0f;
                oc->setOrientation(
                    Ogre::Quaternion(Ogre::Radian(svYaw),   Ogre::Vector3::UNIT_Y)
                  * Ogre::Quaternion(Ogre::Radian(svPitch), Ogre::Vector3::UNIT_X));
            }
            else oc->setOrientation(s_otsSavedCamOri);
            oc->setFOVy(Ogre::Radian(s_otsSavedFovR));
            if (s_otsSavedNearClip > 0.0f) oc->setNearClipDistance(s_otsSavedNearClip);
        }
        if (s_otsHadAutoTrack) oc->setAutoTracking(true, cam->getCenterNode());
        if (s_otsNode) { oc->getSceneManager()->destroySceneNode(s_otsNode); s_otsNode = nullptr; }
        if (s_freeMoveAnchor) ou->player->startTrackCharacter(s_freeMoveAnchor);
    }
    s_otsNode = nullptr;
    s_otsCamLocalsSaved = false;
    DebugLog("[WASDCombat] dc_otscam_exited");
}

// otsHeightScale — per-character height compensation for the OTS pivot.  The
// OtsHeight slider is authored against a STANDARD-height character; multiply it
// by (this character's head-above-root / 16.0 reference) so the shoulder pivot —
// and therefore the centered crosshair — sits at the same relative body level on
// tall/short custom characters WITHOUT the player re-tuning per character.  Same
// crash-safe skeleton access + 16.0 reference as the FP EyeDrop dropScale; NO
// side effects on FP state.  Returns 1.0 (no change) if it can't measure.
static float otsHeightScale()
{
    if (!s_freeMoveAnchor || !s_freeMoveAnchor->movement) return 1.0f;
    AppearanceBase* ap = s_freeMoveAnchor->getAppearance();
    Ogre::Entity* ent = ap ? ap->getBody() : nullptr;
    if (!ent) return 1.0f;
    Ogre::OldSkeletonInstance* sk = ap->getSkeleton();
    if (!sk) return 1.0f;
    static const char* const HEAD[] = { "Bip01 Head", "Bip01 Neck", "Head", "Bip01 Neck1" };
    const char* used = nullptr;
    for (int i = 0; i < 4 && !used; ++i)
        if (sk->hasBone(HEAD[i])) used = HEAD[i];
    if (!used) return 1.0f;
    Ogre::Vector3 head = s_freeMoveAnchor->getBoneWorldPosition(std::string(used));
    float above = head.y - s_freeMoveAnchor->movement->pos.y;
    if (above < 4.0f || above > 40.0f) return 1.0f;   // implausible → no scale
    float s = above / 16.0f;                          // 16.0 = standard-height ref (== FP)
    if (s < 0.5f) s = 0.5f;
    if (s > 2.0f) s = 2.0f;
    return s;
}

// otsCamDrive — per-frame detached OTS orbit (called post-orig, player cam).
// Owns mouse-look (reuse FP raw-mouse) + positions the camera behind the
// shoulder.  M1: no collision yet (M2 adds the traceNoActors pull-in).
static void otsCamDrive(CameraClass* thisptr, bool uiOpen, int wheelPre)
{
    if (!s_otsCamActive) return;
    if (s_mode != MODE_FREE_MOVE || !s_freeMoveAnchor || !s_freeMoveAnchor->movement)
        { exitOtsCam(true); return; }
    Ogre::Camera* oc = thisptr->camera;
    if (!oc || !s_otsNode) return;

    // --- mouse-look (we own it; suspended while a UI needs the cursor) ---
    if (!uiOpen && isKenshiForegroundMain())
    {
        bool useRaw = s_fpRawMouse || (s_directionalCombat && s_manualAttackOn);
        if (useRaw) { fpStartDInputThread(); fpEnsureDInput(); }
        HWND fg = GetForegroundWindow(); RECT rc;
        if (fg && GetClientRect(fg, &rc))
        {
            POINT center; center.x = (rc.left + rc.right) / 2;
            center.y = (rc.top + rc.bottom) / 2; ClientToScreen(fg, &center);
            float dx = 0.0f, dy = 0.0f; bool have = false;
            if (useRaw && s_diReady)
            { fpTakeMouseAccum(&dx, &dy); have = s_otsCursorCaptured;
              SetCursorPos(center.x, center.y); s_otsCursorCaptured = true; }
            else { POINT cur; if (GetCursorPos(&cur))
                   { if (s_otsCursorCaptured) { dx = (float)(cur.x - center.x);
                       dy = (float)(cur.y - center.y); have = true; }
                     SetCursorPos(center.x, center.y); s_otsCursorCaptured = true; } }
            if (have && (dx != 0.0f || dy != 0.0f))
            {
                s_otsYaw   -= dx * FP_RAD_PER_PIXEL * s_fpSensitivityFP;
                s_otsPitch -= dy * FP_RAD_PER_PIXEL * s_fpSensitivityFP;
                if (s_otsPitch >  OTS_PITCH_LIMIT) s_otsPitch =  OTS_PITCH_LIMIT;
                if (s_otsPitch < -OTS_PITCH_LIMIT) s_otsPitch = -OTS_PITCH_LIMIT;
            }
        }
    }
    else s_otsCursorCaptured = false;

    // --- zoom: base from slider, wheel steps the session zoom (we own it) ---
    if (s_otsCamDistApplied != s_otsCamDist)
    { s_otsZoomCur = s_otsCamDist; s_otsCamDistApplied = s_otsCamDist; }
    // The detached view cannot rely on CameraClass::update's wheel field: in
    // this build it is already zero by the time this hook runs.  The existing
    // nonexclusive DirectInput device sees the physical Z axis independently.
    int rawWheel = (int)InterlockedExchange(&s_diAccZ, 0);
    int wheel = !uiOpen && isKenshiForegroundMain()
        ? (rawWheel != 0 ? rawWheel : wheelPre) : 0;
    if (wheel != 0)
    {
        s_otsZoomCur -= (float)wheel / 120.0f * 3.0f;
        if (s_otsZoomCur < OTS_ZOOM_FP_ENTER) s_otsZoomCur = OTS_ZOOM_FP_ENTER;
        if (s_otsZoomCur > OTS_ZOOM_MAX) s_otsZoomCur = OTS_ZOOM_MAX;
        char zb[160];
        sprintf_s(zb, sizeof(zb),
                  "[Ironwind] ots_zoom wheel=%d raw=%d game=%d distance=%.2f",
                  wheel, rawWheel, wheelPre, s_otsZoomCur);
        VerbLog(zb);
    }
    if (wheel > 0 && s_otsZoomCur <= OTS_ZOOM_FP_ENTER)
    {
        // Only a deliberate wheel-in changes the view mode.  A wall pushing
        // the camera close does not make first-person flicker in a doorway.
        float yaw = s_otsYaw, pitch = s_otsPitch;
        enterFirstPerson();
        if (s_firstPersonActive)
        {
            s_fpYaw = yaw;
            s_fpPitch = pitch;
            s_fpBodyYaw = yaw;
            s_fpSmValid = false;
            s_fpLastMouseMoveMs = GetTickCount64();
            s_userWantsFP = true; // retain this view through ordinary streaming
            DebugLog("[Ironwind] ots_zoom_enter_first_person");
            return;
        }
    }

    // --- orbit geometry ---
    // Derive the view basis FROM the orientation quaternion so the camera's
    // look direction and its position offset can never disagree (the earlier
    // hand-rolled trig had the shoulder-right sign flipped and the pitch
    // inverted → camera ended up down by the character's foot).  q = yaw*pitch
    // exactly like FP; fwd is where the lens looks; the shoulder axis is the
    // HORIZONTAL (yaw-only) right so height/side stay level.
    Ogre::Quaternion qYaw = Ogre::Quaternion(Ogre::Radian(s_otsYaw), Ogre::Vector3::UNIT_Y);
    Ogre::Quaternion q    = qYaw * Ogre::Quaternion(Ogre::Radian(s_otsPitch), Ogre::Vector3::UNIT_X);
    Ogre::Vector3 fwd     = q * Ogre::Vector3::NEGATIVE_UNIT_Z;   // view direction
    Ogre::Vector3 rightG  = qYaw * Ogre::Vector3::UNIT_X;         // horizontal shoulder axis
    Ogre::Vector3 root = s_freeMoveAnchor->movement->pos;
    // DOWNED / KO / RAGDOLL follow (user 2026-09-13): while the body is a
    // ragdoll the engine leaves CharMovement::pos parked where the character
    // last STOOD, so the orbit sat over an empty patch of ground while the
    // body lay (or flew) somewhere else.  Orbit the physics root instead, and
    // ease across both hand-offs (ragdoll on/off can each pop the source).
    // A jump > 20 u (zone teleport, load) snaps rather than eases.
    bool ragdoll = s_freeMoveAnchor->isRagdoll() || s_freeMoveAnchor->isBeingCarried();
    if (ragdoll) root = dcBodyRoot(s_freeMoveAnchor);   // ragdoll root / carried body bone
    if (!s_otsRootSmValid || root.squaredDistance(s_otsRootSm) > 400.0f)
    {
        s_otsRootSm      = root;
        s_otsRootSmValid = true;
    }
    else if (ragdoll || s_otsRootBlendFrames > 0)
    {
        s_otsRootSm += (root - s_otsRootSm) * 0.25f;
        if (!ragdoll) --s_otsRootBlendFrames;
    }
    else s_otsRootSm = root;
    if (ragdoll) s_otsRootBlendFrames = 30;
    root = s_otsRootSm;
    // Height compensates for custom character height so the crosshair lands at
    // the same relative body level without re-tuning per character (user req
    // 2026-09-06).  Side is left unscaled — shoulder offset reads well as an
    // absolute across heights and keeps the framing the player tuned.
    float hScale = otsHeightScale();
    Ogre::Vector3 pivot = root
                        + Ogre::Vector3(0.0f, s_otsCamHeight * hScale, 0.0f)
                        + rightG * (s_otsCamSide * s_otsShoulderSign);

    // --- M2 collision: SPHERE-trace from the pivot back toward where the
    // camera wants to sit; if static world geometry blocks it, pull in to just
    // short of the surface so the lens never clips through a wall/floor.  A
    // SPHERE (radius) not a thin ray = less "sensitive" (keeps a margin, small
    // gaps don't snap it); staticOnly=true ignores dynamic props/items so only
    // real walls/terrain block; traceNoActors semantics still ignore characters.
    // OTS_CAM_MIN keeps the camera OUTSIDE the character (was 1.0 → tucked
    // inside the body).  Snap IN quickly, ease OUT so leaving a wall isn't a
    // jerk but also isn't sluggish.
    // START the trace PAST the character's own body/gear (the weapon on the
    // back sat 0.3u behind the pivot and pinned the camera to min every frame —
    // log-proven).  Only geometry beyond OTS_CAM_START (real walls) tucks the
    // camera; the character's own stuff is skipped.  OTS_CAM_MIN keeps the lens
    // outside the body when a real wall does pull it in.
    const float OTS_CAM_RADIUS = 0.4f;
    const float OTS_CAM_START   = 2.5f;     // skip the character's own body/gear
    const float OTS_CAM_MIN     = 4.5f;     // closest the lens may sit to the pivot
    // A long outdoor orbit usually passes through the building's cutaway roof.
    // Keep the player's wheel distance for outdoors, but use a useful room-size
    // orbit while inside instead of trying to place the lens beyond the room.
    CharMovement* anchorMove = s_freeMoveAnchor->movement;
    bool indoors = stableIndoors(anchorMove);
    const float OTS_INDOOR_MAX = 35.0f;
    float desiredDist = indoors && s_otsZoomCur > OTS_INDOOR_MAX
                      ? OTS_INDOOR_MAX : s_otsZoomCur;
    Ogre::Vector3 origin = pivot - fwd * OTS_CAM_START;
    Ogre::Vector3 sweep  = -fwd * (desiredDist - OTS_CAM_START);
    physHit hit(true, false, 0.0f);
    if (desiredDist > OTS_CAM_START)
        UtilityT::sphereTrace(hit, origin, sweep, OTS_CAM_RADIUS, 0xFFFFFFFF, true);
    // The interior roof is already hidden by dcDriveFloorReveal.  Its physics
    // shell can still be hit immediately behind the pivot, producing the 4.5u
    // lens distance seen in the log.  Ignore that ceiling hit only inside the
    // controlled character's building; ordinary walls and exterior geometry
    // still pull the camera in to avoid looking through them.
    Building* ownBuilding = indoors ? anchorMove->building.getBuilding() : nullptr;
    bool ownRoofHit = hit.hit && indoors && hit.normal.y < -0.55f
                   && ((ownBuilding && hit.getBuilding() == ownBuilding)
                       || hit.distance <= 0.5f);
    // A floor-facing surface inside the camera's own minimum-distance bubble
    // cannot be represented usefully by pulling the lens inward: the minimum
    // remains 4.5 units, so the old path snapped a long orbit onto the character
    // for one frame and immediately released it. This is easiest to trigger when
    // a running character stops into guard/reaction while the view is pitched
    // slightly upward. Ignore only that near-pivot upward-facing contact; a
    // floor/slope farther down the orbit and every wall still clamp normally.
    bool nearFloorHit = hit.hit && hit.normal.y > 0.55f
                     && (OTS_CAM_START + hit.distance) <= (OTS_CAM_MIN + 0.25f);
    float targetDist = desiredDist;
    if (hit.hit && !ownRoofHit && !nearFloorHit)
    {
        targetDist = OTS_CAM_START + hit.distance;      // total from the pivot
        if (targetDist < OTS_CAM_MIN) targetDist = OTS_CAM_MIN;
    }
    if (targetDist < s_otsCollDistSm) s_otsCollDistSm = targetDist; // never leave the lens beyond a wall
    else                             s_otsCollDistSm += (targetDist - s_otsCollDistSm) * 0.30f;
    Ogre::Vector3 camPos = pivot - fwd * s_otsCollDistSm;
    if (s_verboseLog)
    {
        static ULONGLONG s_lastOtsTraceMs = 0;
        ULONGLONG nowTrace = GetTickCount64();
        if (nowTrace - s_lastOtsTraceMs >= 1000)
        {
            s_lastOtsTraceMs = nowTrace;
            char tb[352];
            sprintf_s(tb, sizeof(tb), "[IronwindTrace] camera height=%.2f scale=%.2f root_y=%.2f pivot_above=%.2f lens_above=%.2f dist=%.2f target=%.2f zoom=%.2f indoor=%d hit=%d roof=%d floor_near=%d hit_dist=%.2f normal_y=%.2f pitch=%.2f ui=%d",
                      s_otsCamHeight, hScale, root.y, pivot.y - root.y,
                      camPos.y - root.y, s_otsCollDistSm, targetDist,
                      s_otsZoomCur, indoors ? 1 : 0, hit.hit ? 1 : 0,
                      ownRoofHit ? 1 : 0, nearFloorHit ? 1 : 0,
                      hit.hit ? hit.distance : -1.0f,
                      hit.hit ? hit.normal.y : 0.0f, s_otsPitch, uiOpen ? 1 : 0);
            VerbLog(tb);
        }
    }

    // Near-clip vs pull-in (user 2026-09-11, wall-hug clipping): at close
    // range the vanilla near plane slices the character open (hair/shoulder
    // shells).  Blend the near plane from the game's own value down to
    // OtsNearClip as the camera closes from OTS_NEAR_BLEND_START to
    // OTS_CAM_MIN.  Driven by the SMOOTHED distance so it tracks wall pull-in
    // AND manual wheel zoom with no pops; exitOtsCam already restores the
    // saved vanilla near clip.
    const float OTS_NEAR_BLEND_START = 10.0f;
    if (s_otsSavedNearClip > 0.0f)
    {
        float t = (s_otsCollDistSm - OTS_CAM_MIN)
                / (OTS_NEAR_BLEND_START - OTS_CAM_MIN);
        if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
        float tight = s_otsNearClipMin;
        if (tight > s_otsSavedNearClip) tight = s_otsSavedNearClip;
        oc->setNearClipDistance(tight + (s_otsSavedNearClip - tight) * t);
    }

    s_otsNode->setPosition(camPos);
    s_otsNode->setOrientation(q);   // q computed above (view basis)

    // Rig coherence — audio listener + zone/foliage streaming (v1.4.1).  Same
    // throttled teleport as FP (see fpDriveFrame): every-frame jumps make the
    // streamer re-page grass continuously, so only re-anchor once the character
    // has moved StreamUpdateDist metres.  Anchored to the character ROOT (the
    // orbit target) — stable under pure camera rotation, and it is where the
    // player's "ears" should sit in third person.
    bool otsStream = !s_otsLastStreamValid || s_fpStreamDist <= 0.0f
                   || root.squaredDistance(s_otsLastStreamPos)
                      >= s_fpStreamDist * s_fpStreamDist;
    if (otsStream)
    {
        thisptr->teleport(root);
        s_lastRigTeleportMs = GetTickCount64();   // DIAG
        thisptr->targetPositionY = root.y;
        thisptr->speedY          = 0.0f;
        s_otsLastStreamPos   = root;
        s_otsLastStreamValid = true;
    }
}

// dcDriveFloorReveal — storey visibility while OUR detached camera owns the
// view (FP and OTS).  The game derives the displayed floor from the camera's
// TRACKED character inside restrictPosition; both detached modes stop
// tracking, so without this the floor you climb to never reveals (FP: user
// 2026-07-31 thieves tower; OTS: user 2026-09-14 "fails to render the next
// floor above when walking up the stairs").  Player camera only, once per
// frame, called from cameraUpdate_hook before the mode's drive.
static void dcDriveFloorReveal()
{
    if (!ou || !ou->player) return;
    // Interior floor reveal (user 2026-07-31: shinobi thieves-tower floors did
    // NOT load in FP, unlike plain DC; HUD stayed "Floor 0" on an upper storey).
    // The game normally derives the displayed floor from the camera's TRACKED
    // character inside restrictPosition and reveals it — but FP detaches the
    // camera, stops tracking, and runs the update with controlEnabled=false, so
    // restrictPosition never runs and the player's current floor stays 0.  Drive
    // it ourselves: sync the player's current floor to the controlled character's
    // floor, then refresh storey visibility — once per frame, player camera only.
    if (s_freeMoveAnchor && s_freeMoveAnchor->movement)
    {
        int anchorFloor = s_freeMoveAnchor->movement->getCurrentFloor();
        if (anchorFloor != ou->player->getCurrentFloor())
        {
            ou->player->setCurrentFloor(anchorFloor);
            if (g_log.debugLogging)
            {
                char fbuf[80];
                sprintf_s(fbuf, sizeof(fbuf),
                    "[WASDCombat] dc_fp_floor_sync floor=%d", anchorFloor);
                DebugLog(fbuf);
            }
        }
    }
    ou->player->updateFloorVisibility(ou->player->getAllPlayerCharacters());
    // Below-floor reveal (user 2026-08-01): the character-based pass above
    // hides storeys no squad member stands on, so looking DOWN from an upper
    // floor showed black voids where the lower shells should be (and fast
    // pans flickered them as the vanilla update disagreed frame-to-frame).
    // Force the cutaway to "everything up to the anchor's floor" — the same
    // reveal restrictPosition would compute — AFTER that pass so this is the
    // frame's final word.  INI FloorRevealBelow=0 restores the old behaviour
    // (re-read on every F-enter, so it's revertible without a rebuild).
    //
    // Outside-building reveal (user 2026-08-05): vanilla only cuts a building
    // away while the tracked character is INSIDE one, but neither pass here
    // had that gate — with the whole squad outdoors the squad-based pass hid
    // every storey no one stood on and the anchor-floor clamp pinned the rest
    // to floor 0, so owned multi-floor buildings looked cut open from the
    // outside.  When the anchor is not registered in any building, the
    // frame's final word is "no cutaway" instead (identity/null check only —
    // the Building* is never dereferenced or stored).
    if (s_freeMoveAnchor && s_freeMoveAnchor->movement)
    {
        // Outdoor STRUCTURES (user 2026-09-13: standing on a swamp walkboard
        // in FP left distant owned buildings cut to floor 0 until stepping
        // off): a walkboard/platform registers the character in a Building
        // too, but it has no interior — vanilla never cuts away for those.
        // Only a building WITH an interior counts as "inside".
        // 2nd pass (user 2026-09-13): the swamp ROUND platform passed
        // hasInterior yet is just as roofless, so the gate is now the
        // engine's own "inside a LOADED interior" (isInsideBuildingLoadedInterior,
        // 400 ms debounced by stableIndoors so doorways don't flicker) — an
        // outdoor deck never loads an interior.  hasInterior stays as a
        // cheap pre-filter.  Flags logged on change to settle any other case.
        CharMovement* mvIn = s_freeMoveAnchor->movement;
        Building* bIn = mvIn->building.getBuilding();
        bool hasInt  = bIn && bIn->hasInterior();
        bool insideL = hasInt && stableIndoors(mvIn);
        {
            static int s_lastGate = -1;
            int gate = (bIn ? 1 : 0) | (hasInt ? 2 : 0) | (insideL ? 4 : 0)
                     | (mvIn->isIndoors() ? 8 : 0);
            if (gate != s_lastGate)
            {
                s_lastGate = gate;
                char gb[160];
                sprintf_s(gb, sizeof(gb),
                    "[WASDCombat] dc_fp_cutaway_gate building=%d hasInterior=%d loadedInt=%d indoors=%d floors=%d",
                    bIn ? 1 : 0, hasInt ? 1 : 0, insideL ? 1 : 0, mvIn->isIndoors() ? 1 : 0,
                    bIn ? bIn->getNumFloors() : 0);
                VerbLog(gb);
            }
        }
        if (!insideL)
            ou->player->resetFloorsVisibility();
        else if (s_fpFloorRevealBelow)
            ou->player->setFloorsVisibility(
                s_freeMoveAnchor->movement->getCurrentFloor());
    }
}

// CameraClass::update hook — runs AFTER the game's camera update, BEFORE
// render: this is the only point whose writes survive to the frame.
static void (*s_cameraUpdateOrig)(CameraClass* thisptr, bool controlEnabled);
static void cameraUpdate_hook(CameraClass* thisptr, bool controlEnabled)
{
    // OTS owns the camera: clear the vanilla MMB-rotate state before the
    // game's update sees it, so MMB does nothing while OTS is active.
    if (s_fpActive || s_firstPersonActive)
        thisptr->isRotating = false;
    // Manual combat keys vs vanilla keyboard camera-rotate (Q = rotLeft, E =
    // rotRight are direction flags this update reads): while DC is on, a
    // combat key bound to Q/E must not also spin the camera.  Only the flag
    // for a key we actually use is cleared; MMB/CTRL rotate is untouched, and
    // the vanilla keys return the moment DC is off.
    if (s_mode == MODE_FREE_MOVE && key)
    {
        if (s_bindVk[KR_BLOCK] == 'Q' || s_bindVk[KR_ATTACK] == 'Q') key->rotLeft  = false;
        if (s_bindVk[KR_BLOCK] == 'E' || s_bindVk[KR_ATTACK] == 'E') key->rotRight = false;
    }

    // (2026-09-06) The old engine-rotate OTS force (key->rotate = true) was
    // removed — OTS now drives its OWN detached camera and never uses the
    // vanilla rotate.  Nothing to force here anymore.

    // While the inventory face-cam owns the camera, force controlEnabled=false
    // into the vanilla update — this is Kenshi's OWN gate for "ignore camera
    // input" (it passes false when a UI has focus), so the wheel-zoom and
    // WASD/edge pan never run.  Those were still moving/scaling the world
    // name-tags because the vanilla update lays the tags out from the (just-
    // moved) camera DURING orig, before our post-orig altitude restore could
    // undo it (field 2026-06-17: scrolling/WASD still moved the squad names).
    // We drive the detached camera ourselves via s_fpNode, so we need nothing
    // from orig's input handling here.
    bool ctlEnabled = (s_fpActive || s_firstPersonActive) ? false : controlEnabled;

    // OTS-TWEAK v2: this frame's wheel delta — must be read BEFORE orig
    // (CameraClass::update consumes and zeroes mWheel in its own zoom).
    int otsWheelPre = key ? key->mWheel : 0;

    s_cameraUpdateOrig(thisptr, ctlEnabled);
    manualAttackHudUpdate();    // "Manual Attack" label (self-gated)
    focusMarkersUpdate(thisptr);   // target name + health bars (self-gated)

    // SCENE-FREEING — the Ogre scene is actively being torn down: only `!ou`
    // (world gone) and a save-load in progress qualify.  Touching the camera
    // then reads freed memory (v1.8.0 crash).  Stop driving + mark restore
    // PENDING; keep s_fpNode + saved locals.  Deliberately NOT gated on
    // s_dcShutdownInProgress / s_loadGuardActive: those are mod wait-states
    // that STAY true through char-creation (no player to stabilize on), which
    // would strand the camera detached and hide the new-game character preview
    // (field 2026-06-13).  Once the scene is stable they're safe to ignore.
    bool sceneFreeing = !ou || dcRealLoad();   // build 67: a chunk stream keeps OTS / FP driving
    if (!sceneFreeing && ou->isLoadingFromASaveGame()
        && !(s_freeMoveAnchor && s_gwFrame && dcInUpdateList(s_gwFrame, s_freeMoveAnchor)))
        sceneFreeing = true;                    // build 74: flag up and the anchor is not provably alive -> treat as a real load
    if (sceneFreeing)
    {
        s_otsCamDistApplied = -1.0f;
        // Detached OTS: the scene/camera is being torn down — drop our state
        // WITHOUT touching the (freed) camera or node (mirrors the FP handling
        // just below).  s_otsNode dies with the scene; recreated on next enter.
        s_otsCamActive      = false;
        s_otsNode           = nullptr;   // (an old node survives a mere zone
                                         // stream as an empty orphan — harmless)
        // s_otsCamLocalsSaved is deliberately KEPT: the saved vanilla locals are
        // plain values and are the only correct thing to restore on the exit
        // after a zone-crossing re-entry (see enterOtsCam).
        s_otsCursorCaptured = false;
        s_invFaceRetrackUntilMs = 0;
        dcUpdateCrosshair(false);    // XHAIR-TWEAK: never strand a hidden pointer
        if (s_fpActive || s_firstPersonActive)
        {
            s_fpActive          = false;
            s_firstPersonActive = false;
            s_fpCursorCaptured  = false;
            s_fpHeadBoneHidden  = false;  // skeleton torn down — restore flags moot
            s_fpBodyCullCount   = 0;      // (materials gone with the scene)
            s_fpDownNearSmooth  = 0.0f;
            for (int hbT = 0; hbT < 2; ++hbT)
                s_fpHideBoneSaved[hbT] = false;
            s_fpHairHidden      = false;
            s_otsRestorePending = true;
            if (s_fpOptRangeSaved && options)   // never leave the range boosted
            {
                options->grassRange   = s_fpSavedGrassRange;
                options->foliageRange = s_fpSavedFoliageRange;
                s_fpOptRangeSaved     = false;
            }
        }
        return;
    }

    // Scene is stable (gameplay OR char-creation/menu).  thisptr is a valid
    // camera even when ou->player is null, so restore via thisptr.
    // (1) Deferred restore from a load/shutdown that hit while OTS was active.
    if (s_otsRestorePending)
    {
        s_otsRestorePending = false;
        s_fpSuspendedForInv = false;   // a load cancels any pending FP auto-return
        otsRestoreCameraToRig(thisptr);
        if (ou && ou->player && s_freeMoveAnchor)
            ou->player->startTrackCharacter(s_freeMoveAnchor);
        otsRestoreNames();   // scene is stable again — safe to re-show names
        DebugLog("[WASDCombat] dc_cam_restored_after_load");
        return;
    }

    // ONLY the player camera drives the inventory face-cam.  Kenshi calls this
    // hook for OTHER cameras too (the inventory portrait render cameras) — if
    // those touched the detached camera it flickered detach/re-attach EVERY
    // frame (field 2026-06-16: dc_cam_entered/exited toggling).  Non-player
    // cameras just return after orig and never touch s_fpActive/the node.
    if (!ou || !ou->player || thisptr != ou->player->camera)
        return;

    // Any open UI suspends V-camera input and releases the cursor. `controlEnabled`
    // is the game's own "a UI has focus" signal; the gui checks are
    // belt-and-suspenders.
    // The world MAP and the character STATS window do NOT flip controlEnabled and
    // are not inventory/dialogue/pause — so in FP the cursor stayed pinned to
    // center inside them (field 2026-07-25).  Add them explicitly: the map lives
    // in ManagementScreen (also covers faction/tech/squad tabs), stats via the gui.
    // UNIFY (2026-09-04): single suspend signal shared with FP + the crosshair
    // (dcUiWantsCursor covers map/stats/dialogue/pause/inventory/loot/context-
    // menu); controlEnabled adds the game's own "UI has focus" catch-all.
    // Move-through keeps WASD active, while dcUiWantsCursor releases the
    // mouse to the open inventory until Tab closes it.
    bool uiOpenNow = (!controlEnabled && !s_invMoveThroughActive) || dcUiWantsCursor();
    // OTS engagement + cursor are handled by the DEDICATED DETACHED CAMERA
    // now (enterOtsCam / otsCamDrive / exitOtsCam, below) — the old
    // engine-rotate (key->rotate) path with its warp / lastMousePos / SendInput
    // flush was removed in the 2026-09-06 rebuild so OTS owns the cursor like
    // FP does.
    s_cameraUiOpen = uiOpenNow;

    // XHAIR-TWEAK (2026-09-06, user goal "just keep the cursor hidden"): drive
    // crosshair + pointer visibility off the actual V-camera state and no real
    // UI wanting the cursor. It
    // deliberately does NOT key off s_otsRotateEngaged: that flag blips off for
    // a frame during the engine's rotate re-engage (after a UI closes), which
    // let the real pointer flash back on at its last spot = the intermittent
    // "cursor stuck" the user saw. The pointer stays hidden for the whole active
    // V view (except genuine UIs), so
    // any residual engine cursor blip is never visible.  Look-drive still uses
    // its own suspend (wantRotate / fpDriveFrame) independently.
    dcUpdateCrosshair((s_firstPersonActive || s_otsCamActive) && !uiOpenNow);

    // First person is the near end of the same V-camera zoom range regardless
    // of whether it was entered by F or by scrolling inward. One outward notch
    // crosses the hysteresis gap back to OTS.
    // Always drain raw Z here so scrolling an open menu cannot trigger a view
    // switch when the menu closes.
    if (s_firstPersonActive)
    {
        int rawWheel = (int)InterlockedExchange(&s_diAccZ, 0);
        int wheel = rawWheel != 0 ? rawWheel : otsWheelPre;
        if (!uiOpenNow && isKenshiForegroundMain() && wheel < 0)
        {
            float yaw = s_fpSmValid ? s_fpYawSm : s_fpYaw;
            float pitch = s_fpSmValid ? s_fpPitchSm : s_fpPitch;
            s_otsZoomCur = OTS_ZOOM_FP_ENTER - (float)wheel / 120.0f * 3.0f;
            if (s_otsZoomCur < OTS_ZOOM_FP_EXIT) s_otsZoomCur = OTS_ZOOM_FP_EXIT;
            if (s_otsZoomCur > OTS_ZOOM_MAX) s_otsZoomCur = OTS_ZOOM_MAX;
            s_otsResumeZoomPending = true;
            s_userWantsFP = false;
            exitFirstPerson(true);
            enterOtsCam();
            if (s_otsCamActive)
            {
                s_otsYaw = yaw;
                s_otsPitch = pitch;
                DebugLog("[Ironwind] ots_zoom_exit_first_person");
            }
            else s_otsResumeZoomPending = false;
        }
    }

    // OTS: drive the DEDICATED DETACHED CAMERA (2026-09-06 rebuild) — its own
    // camera node, own mouse-look, own cursor (like FP), no engine rotate.
    // V mode owns OTS whenever first person is not active; otsCamDrive orbits
    // behind the shoulder, and V/FP teardown re-attaches the rig camera.
    {
        bool wantOts = (s_mode == MODE_FREE_MOVE)
                    && !s_fpActive && !s_firstPersonActive
                    && !s_dcShutdownInProgress;
        if (wantOts)
        {
            if (!s_otsCamActive) enterOtsCam();
            if (s_otsCamActive)
            {
                dcDriveFloorReveal();   // detached camera: drive storey reveal (as FP)
                otsCamDrive(thisptr, uiOpenNow, otsWheelPre);
                if (s_otsCamActive) dcSyncRigYaw(thisptr, s_otsYaw);   // world-map arrow
            }
        }
        else if (s_otsCamActive)
        {
            exitOtsCam(true);
        }
    }

    // Post-face-cam follow watchdog (see s_invFaceRetrackUntilMs): while the
    // window is open, if the camera's orbit pivot has drifted well away from
    // the anchor (follow frozen), do a full stopFollowing+startTrackCharacter
    // reset.  0.5 s throttle keeps the track sound from spamming; the window
    // closes itself in 4 s or the moment tracking is healthy.
    if (s_invFaceRetrackUntilMs > 0 && !s_fpActive)
    {
        ULONGLONG nowRt = GetTickCount64();
        if (nowRt >= s_invFaceRetrackUntilMs
            || s_mode != MODE_FREE_MOVE || !s_freeMoveAnchor)
        {
            s_invFaceRetrackUntilMs = 0;
        }
        else if (s_freeMoveAnchor->movement && thisptr->camera
              && nowRt - s_invFaceRetrackLastMs >= 500)
        {
            s_invFaceRetrackLastMs = nowRt;
            // Probe verdict (2026-09-03): after the face-cam exit the FOLLOW
            // is healthy (pivot tracks the anchor, follow hand correct,
            // camera on the live rig node) but the camera node's PLACEMENT
            // math is asleep — vanilla recomputes it only on input, which is
            // why any keypress "fixed" it.  Detect the real symptom (camera
            // node standing still while the anchor moves) and WAKE the
            // camera with manuallySetOrientationAndZoom — the immediate-
            // apply path the inventory face-cam itself uses — at the
            // camera's own current yaw/pitch/zoom (view direction unchanged).
            static float s_ifwCamX, s_ifwCamY, s_ifwCamZ;
            static float s_ifwAncX, s_ifwAncZ;
            static bool  s_ifwHavePrev = false;
            Ogre::SceneNode* camNodeRt = thisptr->camera->getParentSceneNode();
            Ogre::Vector3 cnRt = camNodeRt ? camNodeRt->_getDerivedPosition()
                                           : Ogre::Vector3(0, 0, 0);
            Ogre::Vector3 apRt = s_freeMoveAnchor->movement->getPosition();
            if (s_ifwHavePrev)
            {
                float cdx = cnRt.x - s_ifwCamX, cdy = cnRt.y - s_ifwCamY,
                      cdz = cnRt.z - s_ifwCamZ;
                float adx = apRt.x - s_ifwAncX, adz = apRt.z - s_ifwAncZ;
                bool camStill   = (cdx*cdx + cdy*cdy + cdz*cdz) < 0.01f;
                bool anchorMoved = (adx*adx + adz*adz) > 1.0f;
                if (camStill && anchorMoved)
                {
                    thisptr->stopFollowing();
                    ou->player->startTrackCharacter(s_freeMoveAnchor);
                    Ogre::Quaternion qRt =
                        Ogre::Quaternion(Ogre::Radian(thisptr->yaw),   Ogre::Vector3::UNIT_Y) *
                        Ogre::Quaternion(Ogre::Radian(thisptr->pitch), Ogre::Vector3::UNIT_X);
                    thisptr->manuallySetOrientationAndZoom(qRt, thisptr->altitude);
                    DebugLog("[WASDCombat] dc_cam_follow_woken");
                }
            }
            s_ifwCamX = cnRt.x; s_ifwCamY = cnRt.y; s_ifwCamZ = cnRt.z;
            s_ifwAncX = apRt.x; s_ifwAncZ = apRt.z;
            s_ifwHavePrev = true;
        }
    }

    // Inventory face-cam trigger: DETACH the player camera when the player's OWN
    // inventory opens (exactly one window), re-attach when it closes.  The
    // detach is what lets the camera face the character — the attached RTS
    // camera can only look top-down.  Safe here (sim paused, character standing)
    // unlike the scrapped gameplay OTS.
    {
        // Only TRUE teardown (shutdown / save-load) exits immediately.  Every
        // other eligibility signal — mode, anchor/movement validity AND the
        // own-inventory window count — is treated as FLAPPY and debounced: those
        // all blip for a frame or two when you switch squad members with the
        // inventory open (the window briefly closes+reopens / the anchor's
        // movement ptr churns), and a single-frame dropout used to slam the
        // streak to max → instant detach (field 2026-06-17: a ~0.1s burst of
        // enter/exit + name hide/restore on every squad switch).  Debouncing
        // the soft signals keeps the camera attached across the swap; it then
        // simply re-frames the newly selected character.
        bool hardStop  = s_dcShutdownInProgress || s_loadGuardActive;
        // Face-cam engages only when enabled in the INI AND the character is NOT
        // in combat — opening inventory mid-fight to loot/disarm an enemy must
        // leave the camera where it is (user req 2026-06-20).  The point-click
        // suppression during inventory is gated separately (s_lootUiSuspendActive),
        // so disabling the face-cam here does NOT let world-clicks move the char.
        // In-combat (with a short grace) is a HARD exit, NOT part of softOK — so a
        // single flicker-false frame can't latch the face-cam through the debounce.
        ULONGLONG nowFC      = GetTickCount64();
        bool inCombatNow     = s_freeMoveAnchor
                            && s_freeMoveAnchor->isInCombatMode(true, true);
        if (inCombatNow) s_lastInCombatMs = nowFC;
        bool inCombatRecent  = inCombatNow
                            || (s_lastInCombatMs > 0
                                && nowFC - s_lastInCombatMs < FACECAM_COMBAT_GRACE_MS);

        bool softOK    = s_settingInventoryFaceCam
                      && !invMoveThroughEligible()
                      && s_mode == MODE_FREE_MOVE
                      && s_freeMoveAnchor && s_freeMoveAnchor->movement
                      && isOwnInventoryOpen();

        bool faceCamWanted;
        if (hardStop || inCombatRecent)
        {
            faceCamWanted        = false;
            s_invFaceCloseStreak = INV_FACE_CLOSE_DEBOUNCE;
        }
        else if (softOK)
        {
            faceCamWanted        = true;
            s_invFaceCloseStreak = 0;
        }
        else
        {
            if (s_invFaceCloseStreak < INV_FACE_CLOSE_DEBOUNCE)
                s_invFaceCloseStreak++;
            // Keep the face-cam alive across a brief dropout (squad switch, or a
            // single dissenting per-frame camera call); only let it drop once
            // the close signal has persisted for the full debounce.
            faceCamWanted = s_fpActive && s_invFaceCloseStreak < INV_FACE_CLOSE_DEBOUNCE;
        }

        // First-person owns the detached camera via s_firstPersonActive (NOT
        // s_fpActive).  If the inventory face-cam now wants the camera, SUSPEND
        // first-person (hand the camera back to the rig) and remember to auto-
        // return when the inventory closes.  If nothing wants the face-cam,
        // first-person simply keeps the camera and the enter/exit below is a
        // no-op (s_fpActive stays false while FP owns the view).
        if (s_firstPersonActive && faceCamWanted)
        {
            exitFirstPerson(true);        // clears s_firstPersonActive, reattaches cam
            s_fpSuspendedForInv = true;
        }
        // Detached OTS owns the camera too — hand it back to the rig BEFORE the
        // face-cam detaches, so the face-cam saves the real rig camera state
        // (not OTS's zeroed detached pose, which corrupted the restore).  OTS
        // auto-re-engages via wantOts once the inventory closes.
        if (s_otsCamActive && faceCamWanted)
        {
            // A temporary inventory camera must never discard the distance
            // the player set with the wheel when the OTS camera re-enters.
            s_otsResumeZoomPending = true;
            exitOtsCam(true);
        }

        if (faceCamWanted && !s_fpActive && !s_firstPersonActive)
            enterOTS();
        else if (!faceCamWanted && s_fpActive)
        {
            exitOTS(true);
            if (s_fpSuspendedForInv)
            {
                s_fpSuspendedForInv = false;
                enterFirstPerson();       // inventory closed — auto-return to FP
            }
            return;
        }
    }

    // Robust FP auto-return (belt-and-suspenders).  The primary path above
    // re-enters first-person when the OTS face-cam closes, but a debounce edge or
    // squad-switch churn can clear s_fpActive on a frame where that branch does
    // not fire, stranding the player in top-down DC with the pending return lost.
    // Whenever FP was suspended for the inventory and the inventory is now closed
    // (nothing else owns the camera), re-enter first-person.
    if (s_fpSuspendedForInv && !s_fpActive && !s_firstPersonActive
        && s_mode == MODE_FREE_MOVE && !s_lootUiSuspendActive
        && s_freeMoveAnchor && s_freeMoveAnchor->movement
        && !isOwnInventoryOpen())
    {
        s_fpSuspendedForInv = false;
        enterFirstPerson();
        return;
    }

    if (!s_fpActive && !s_firstPersonActive)
        return;

    // First-person drive: place the detached camera at the anchor's head-bone
    // eye and look along the mouse view.  Runs INSTEAD of the face-cam path.
    if (s_firstPersonActive)
    {
        dcDriveFloorReveal();
        fpDriveFrame(thisptr, uiOpenNow);
        if (s_firstPersonActive) dcSyncRigYaw(thisptr, s_fpYawSm);   // world-map arrow
        {   // detailed log: is the open world map asking the camera for its facing (FP arrow fix)?
            static ULONGLONG s_mapDiagMs = 0;
            ManagementScreen* ms = ManagementScreen::getSingleton();
            ULONGLONG nowM = GetTickCount64();
            if (ms && ms->getVisible() && nowM - s_mapDiagMs >= 3000)
            {
                s_mapDiagMs = nowM;
                char mb[96];
                sprintf_s(mb, sizeof(mb), "[WASDCombat] dc_map_fp_facing_calls=%ld", InterlockedExchange(&s_mapFacingCalls, 0));
                VerbLog(mb);
            }
        }
        return;
    }

    // Anti scroll-zoom: the wheel still reaches the game's camera zoom inside
    // orig (changing altitude → the world name-tags scale).  Hold the altitude
    // at the value saved on enter so scrolling in the inventory zooms nothing.
    thisptr->altitude = s_otsSavedAltitude;

    // Mouse-look — paused while a UI needs the cursor (context menu visible,
    // pause menu, loot/trade/inventory) or the anchor is KO (free the cursor
    // to pick another squad member).
    bool ctxVisible = ou->player->contextMenu.isVisible();
    bool anchorKO   = s_freeMoveAnchor->isUnconcious();
    // Dialogue / bail-out / conversation UIs need the cursor: free it so the
    // player can click the menu instead of it being pinned to the crosshair
    // (field 2026-06-16, bail-NPC-out menu).  inDialogue covers NPC talk +
    // the prisoner/bail dialogue windows.
    bool inDialogue = (gui && gui->inDialogue());
    bool captureOk  = !s_cameraUiOpen && !ctxVisible
                   && !s_lootUiSuspendActive && !anchorKO && !inDialogue
                   && isKenshiForegroundMain();

    if (captureOk)
    {
        HWND fg = GetForegroundWindow();
        RECT rc;
        if (fg && GetClientRect(fg, &rc))
        {
            POINT center;
            center.x = (rc.left + rc.right) / 2
                     + (LONG)((rc.right - rc.left) * s_otsCrosshairOffsetX);
            center.y = (rc.top + rc.bottom) / 2;
            ClientToScreen(fg, &center);
            POINT cur;
            if (GetCursorPos(&cur))
            {
                if (s_fpCursorCaptured)
                {
                    float dx = (float)(cur.x - center.x);
                    float dy = (float)(cur.y - center.y);
                    if (dx != 0.0f || dy != 0.0f)
                    {
                        s_fpYaw   -= dx * FP_RAD_PER_PIXEL * s_fpSensitivity;
                        s_fpPitch -= dy * FP_RAD_PER_PIXEL * s_fpSensitivity;
                        if (s_fpPitch >  FP_PITCH_LIMIT) s_fpPitch =  FP_PITCH_LIMIT;
                        if (s_fpPitch < -FP_PITCH_LIMIT) s_fpPitch = -FP_PITCH_LIMIT;
                    }
                }
                SetCursorPos(center.x, center.y);
                s_fpCursorCaptured = true;
            }
        }
    }
    else
    {
        s_fpCursorCaptured = false;
    }

    CharMovement* mvFP = s_freeMoveAnchor->movement;


    // Own-inventory face-cam (ported from OTS_Project_Shelved, 2026-06-14 for
    // v1.A).  When the player opens their OWN inventory (no trade/loot party on
    // the other side), swing the shoulder camera around to a centered upper-
    // chest shot facing the selected character so worn gear is visible; restore
    // the previous shoulder view when the window closes.  Runs even under the
    // inventory pause (CameraClass::update still ticks) — which is exactly why
    // the OTS detached-node approach frames cleanly where the vanilla camera
    // could not.  Trade/loot keep the normal over-the-shoulder view.
    {
        // Own inventory (incl. a backpack character's 2-window inventory),
        // excluding any shop/loot/corpse trade.  See isOwnInventoryOpen().
        bool ownInv = isOwnInventoryOpen();
        // Face-cam target = the SELECTED (white-highlighted) character, not the
        // DC anchor: clicking portraits while inventory is open changes the
        // selection, never control.
        Character* invTarget = nullptr;
        if (ownInv)
        {
            // PRIMARY = the character whose inventory window is actually open
            // (inventoryWindowCharacter).  At count==1 this is always one of your
            // own squad, including RECRUITED MOD-NPCs (e.g. Wandering Menders'
            // Kumo) that don't report isPlayerCharacter()==true and so used to
            // fall through to the anchor — leaving the camera framing the wrong
            // character (field 2026-06-17).  Fall back to the selected player
            // char, then the DC anchor.
            Character* invChar = gui ? gui->inventoryWindowCharacter.getCharacter() : nullptr;
            if (invChar && invChar->movement)
                invTarget = invChar;
            else if (s_selectedCharacter && s_selectedCharacter->movement
                     && s_selectedCharacter->isPlayerCharacter())
                invTarget = s_selectedCharacter;
            else
                invTarget = s_freeMoveAnchor;
        }

        if (ownInv && (!s_otsInvFaceActive || s_otsInvFaceChar != invTarget))
        {
            if (!s_otsInvFaceActive)
            {
                // First open: remember the player's shoulder view.
                s_otsSavedYaw   = s_fpYaw;
                s_otsSavedPitch = s_fpPitch;
                s_otsSavedDist  = s_otsDistCur;
                DebugLog("[WASDCombat] dc_cam_inventory_face");
            }
            else
            {
                DebugLog("[WASDCombat] dc_cam_inventory_face_retargeted");
            }
            s_otsInvFaceActive = true;
            s_otsInvFaceChar   = invTarget;

            float faceYaw = s_fpYaw + 3.14159265f;   // fallback: spin around
            if (invTarget && invTarget->movement)
            {
                Ogre::Vector3 bd = invTarget->movement->direction;
                bd.y = 0.0f;
                float bl = bd.length();
                if (bl > 0.001f)
                {
                    bd /= bl;
                    faceYaw = atan2f(-bd.x, -bd.z) + 3.14159265f;
                }
            }
            s_fpYaw      = faceYaw;
            s_fpPitch    = -0.02f;                    // near-level at chest height
            s_otsDistCur = 20.0f;                     // pull in for a tight portrait
        }
        else if (!ownInv && s_otsInvFaceActive)
        {
            s_otsInvFaceActive = false;
            s_otsInvFaceChar   = nullptr;
            s_fpYaw      = s_otsSavedYaw;
            s_fpPitch    = s_otsSavedPitch;
            s_otsDistCur = s_otsSavedDist;
            DebugLog("[WASDCombat] dc_cam_inventory_face_restored");
        }
    }

    // Position the DETACHED camera to face the character's FRONT, recomputed
    // EVERY FRAME from the target's current facing — so it always faces the
    // front regardless of which way the character points, and never drifts to
    // the side on a re-open (the old edge-triggered aim was skipped on re-open
    // and left a stale yaw — field 2026-06-16).  Target = the selected (white)
    // character, falling back to the DC anchor.
    // Same target priority as the face-cam swing above: the OPEN inventory
    // window's character first (covers recruited mod-NPCs that fail
    // isPlayerCharacter()), then the selected player char, then the DC anchor.
    Character* tgt = nullptr;
    {
        Character* invChar = gui ? gui->inventoryWindowCharacter.getCharacter() : nullptr;
        if (invChar && invChar->movement)
            tgt = invChar;
        else if (s_selectedCharacter && s_selectedCharacter->movement
                 && s_selectedCharacter->isPlayerCharacter())
            tgt = s_selectedCharacter;
        else
            tgt = s_freeMoveAnchor;
    }
    if (tgt && tgt->movement && s_fpNode)
    {
        CharMovement* mvP = tgt->movement;
        Ogre::Vector3 bd = mvP->direction;
        bd.y = 0.0f;
        float bl = bd.length();
        float faceYaw = (bl > 0.001f) ? atan2f(bd.x, bd.z) : 0.0f;  // forward=-bd=front
        Ogre::Quaternion q =
            Ogre::Quaternion(Ogre::Radian(faceYaw), Ogre::Vector3::UNIT_Y) *
            Ogre::Quaternion(Ogre::Radian(-0.02f),  Ogre::Vector3::UNIT_X);
        Ogre::Vector3 pivot = mvP->pos;
        pivot.y += 12.5f;                                  // upper chest
        // q*(0,0,dist) is along the character's facing (in FRONT of them).
        Ogre::Vector3 eye = pivot + q * Ogre::Vector3(0.0f, 0.5f, 20.0f);
        s_fpNode->setPosition(eye);
        s_fpNode->setOrientation(q);
    }
}

// CameraClass::restrictPosition hook.  This call is what REFRESHES the interior
// floor visibility (data 2026-06-16: floors render iff restrictPosition runs —
// camFloor/centerBuilding are NOT the lever).  We used to skip it entirely under
// OTS because its camera clamp yanks the over-the-shoulder view back to the
// floor — but skipping it meant the storey you climbed onto never rendered until
// P off.  Now we RUN it (so the floor refreshes) and immediately re-apply the
// OTS pose we wrote this frame, so the clamp can't move the view.
static void (*s_restrictPosOrig)(CameraClass* thisptr, lektor<Character*>& objects);
static void restrictPos_hook(CameraClass* thisptr, lektor<Character*>& objects)
{
    // While the detached inventory face-cam OR first-person is active, skip the
    // RTS clamp — it would yank our free camera back to the floor.  (Interior floor
    // reveal in FP is handled separately by driving updateFloorVisibility from the
    // camera hook, since restrictPosition does not run while FP owns the camera.)
    if (s_fpActive || s_firstPersonActive)
        return;
    s_restrictPosOrig(thisptr, objects);
}

// -----------------------------------------------------------------------
// CharMovement::_NV_update hook — WASD priority with animation protection
//
// WASD held:   apply halt+MOVE_DIRECTION before original; instant-stop on release.
// WASD not held: original runs freely; protected animations are untouched.
// -----------------------------------------------------------------------
static void (*s_charMovUpdateOrig)(CharMovement* thisptr, float time);

// -----------------------------------------------------------------------
// isDoorInteractionTask — door-related TaskTypes.  Used only by the
// hold-scoped door suppression in addJob_hook / addOrder_hook.
// MOVE_CUS_ORDERED is deliberately absent (DC's own disengage orders).
// -----------------------------------------------------------------------
static bool isDoorInteractionTask(TaskType t)
{
    switch (t)
    {
    case OPEN_DOOR:
    case CLOSE_DOOR:
    case OPEN_DOOR_HERE:
    case CLOSE_DOOR_HERE:
    case LOCK_DOOR:
    case UNLOCK_DOOR:
    case LOCK_DOOR_HERE:
    case UNLOCK_DOOR_HERE:
    case PICK_LOCK:
    case BASH_DOOR:
    case MOVE_TO_BUILDING_DOOR:
    case MOVE_TO_CURRENT_LOCATION_BUILDING_DOOR:
    case MOVE_TO_BUILDING_DOOR_INSIDEPOS:
    case MOVE_TO_BUILDING_DOOR_OUTSIDEPOS:
    case OPEN_DOOR_FOR_CURRENT_LOCATION:
    case OPEN_DOOR_FOR_DESTINATION:
        return true;
    default:
        return false;
    }
}

// -----------------------------------------------------------------------
// SLAVE OBEDIENCE (v1.4.1, Workshop/Nexus reports 2026-09: "enslaved in
// Rebirth, you can simply walk out with WASD without punishment").
// While the anchor is an OBEYING slave (Character::isSlave()==IS_SLAVE —
// Rebirth, slaver caravans, ...) the vanilla slave-obedience AI job must own
// locomotion: direct MOVE_DIRECTION injection walked the character straight
// out of the camp with no order the guards could see, and the post-WASD hold
// pinned them against the AI's "get back to work" march — so the game never
// saw a disobeying slave and no punishment ever came.  In this state DC
// yields exactly like the other engine-owned states (downed, protected
// clip, furniture):
//   * no hold clamp (computeHoldDecision reason "slave"),
//   * WASD issues REAL player move orders (the very thing a right-click does,
//     which vanilla punishes) instead of injecting motion — applySlaveMovement
//     — except indoors/caged where an order can only bark: direct injection
//     stays there (the AI still owns the body between presses),
//   * the hold-scoped door-task suppression is off (guards march slaves
//     through gates).
// ESCAPING_SLAVE / EX_SLAVE keep normal direct WASD: once the character has
// broken away the obedience job no longer holds them, and the shackle speed
// cap already keeps a chained runner at a shuffle.
//
// HISTORY: 2026-09-13 the yield was briefly scoped to OUTSIDE the town walls
// (WASD free inside Rebirth); 2026-09-14 the user chose VANILLA PARITY instead
// ("I don't want to touch any of the vanilla game's systems"): the yield
// applies wherever isSlave()==IS_SLAVE, so WASD == right-click everywhere.
// -----------------------------------------------------------------------
static bool isObeyingSlave(Character* ch)
{
    return ch && ch->isSlave() == IS_SLAVE;
}

// slaveEscapeZone — obeying slave, ANYWHERE: the state in which DC yields
// (hold exemption, door suppression off, order-driven WASD).  VANILLA PARITY
// (user 2026-09-14): WASD is treated exactly like a right-click move order in
// every slave situation, so the game's own escape rules decide everything.
static bool slaveEscapeZone(Character* ch)
{
    return isObeyingSlave(ch);
}

// -----------------------------------------------------------------------
// computeHoldDecision — single authority decision for the post-WASD hold,
// shared by charMovUpdate_hook (motion gate) and the end-of-mainLoop
// position clamp.  Returns true when vanilla may move the anchor; false
// means the hold keeps the character where WASD parked them.
//
// reason=no_hold is the normal hybrid state: vanilla owns locomotion
// (point-click works) because no WASD release is pending.  The remaining
// exemptions yield the hold to systems that must move or animate the
// character (mirrors the committed-action philosophy; uses
// isProtectedAnimationState, NOT isCommittedAction, because the door
// interaction Tasker owns STARTUP_STATE and would never release the hold).
// -----------------------------------------------------------------------
static bool computeHoldDecision(Character* ch, const char** outReason)
{
    if (!ch)                           { *outReason = "no_character";   return true; }
    if (manualHitReactionLive(ch))     { *outReason = "hit_reaction";  return true; }
    if (ch == s_freeMoveAnchor && s_freeAttackTech)
                                       { *outReason = "manual_action"; return true; }
    // Holding RMB is an explicit request to guard in V mode, even if there was
    // no preceding WASD release.  Do not yield locomotion to a queued move or
    // to the vanilla defensive-combat order while the feet have no WASD input.
    if (ch == s_freeMoveAnchor && manualBlockMovementSlow()
        && !s_healingJobActive && !s_cameraLockTurretSuspend && !s_menuSuspendActive
        && !isDownedButMovable(ch) && !isProtectedAnimationState(ch)
        && !slaveEscapeZone(ch))
                                       { *outReason = "manual_guard"; return false; }
    // Tab does not hand a manually controlled V-mode character back to queued
    // jobs.  With no WASD input, park them where the inventory was opened;
    // the WASD drive path clears this anchor while movement keys are held.
    if (ch == s_freeMoveAnchor && s_manualAttackOn && invMoveThroughEligible()
        && !s_healingJobActive && !s_cameraLockTurretSuspend && !s_menuSuspendActive
        && !isDownedButMovable(ch) && !isProtectedAnimationState(ch)
        && !slaveEscapeZone(ch))
                                       { *outReason = "manual_inventory"; return false; }
    if (!s_wasdHoldActive)             { *outReason = "no_hold";        return true; }
    if (slaveEscapeZone(ch))           { *outReason = "slave";          return true; }
    if (s_playerPointClickActive)      { *outReason = "player_click";   return true; }
    if (s_healingJobActive)            { *outReason = "healing";        return true; }
    if (s_cameraLockTurretSuspend)     { *outReason = "turret";         return true; }
    if (s_menuSuspendActive)           { *outReason = "menu";           return true; }
    if (isDownedButMovable(ch))        { *outReason = "downed";         return true; }
    if (isProtectedAnimationState(ch)) { *outReason = "protected_anim"; return true; }
    if (ch->isInCombatMode(true, true)
        && (!s_postWasdGraceActive || s_combatReentryAllowed))
                                       { *outReason = "combat";         return true; }
    *outReason = "hold";
    return false;
}

// True when issuing a playerMoveOrderDefault on this character could trip the
// vanilla "I can't get out of here" pathfinding bark — the character is inside a
// building/interior OR locked in a cage / imprisoned, where the path to ANY
// destination may run through a locked door.  DC then skips its disengage /
// release move-orders and relies on direct injection + the hold-clamp instead.
// Field 2026-06-20: the bark persisted in a "locked building" because
// isInsideBuildingLoadedInterior() returns false unless the interior is loaded/
// rendered — getBuilding()/isIndoors() catch the building regardless, and
// isPrisonerFreeToGo()==false catches cages/shackles.
static bool moveOrderMayBark(Character* ch)
{
    if (!ch) return false;
    CharMovement* mv = ch->movement;
    // Gate ONLY on the indoors signals — they are all false outdoors, so this never
    // regresses the outdoor click-cancel.  isIndoors() is broader than
    // isInsideBuildingLoadedInterior() (the latter is false unless the interior is
    // loaded/rendered).  isPrisonerFreeToGo() is NOT gated on (its return for a
    // free non-prisoner is unverified; gating could skip the disengage everywhere)
    // — it is only LOGGED at the call site for diagnosis.
    return mv && (mv->isInsideBuildingLoadedInterior() || mv->isIndoors());
}

// dcSnapCancelOrder — the standard "cancel any in-flight path order by re-issuing
// a move to the CURRENT position" used by every WASD release/stop path.  Gated on
// moveOrderMayBark: indoors / locked the order pathfinds and fires the vanilla
// "I can't get out of here" bark, so it is SKIPPED — the surrounding halt() +
// motion-zero + the next-frame hold-clamp park the character without it.  Field
// 2026-06-20: the bark was log-proven to come from these ungated snap sites (the
// press-edge disengage was already gated, but the nudge-tap release, the
// structured release-stop, the menu-suspend stop and the heal-start snap were
// NOT) — DC issues NO disengage order yet still barked.
static void dcSnapCancelOrder(Character* ch)
{
    if (ch && ch->movement && !moveOrderMayBark(ch))
    {
        // Build 63 (user 2026-09-30, "180 flip on stop"): a move order to the
        // exact current position gives the pathing a zero-length path and a
        // degenerate heading, and the character can spin to a fixed direction
        // to "arrive".  Order a point a small step AHEAD along the current
        // facing instead: still a stop, and any facing correction is toward
        // where the character already looks.
        Ogre::Vector3 f = ch->movement->direction; f.y = 0.0f;
        Ogre::Vector3 dest = ch->movement->pos;
        if (f.squaredLength() > 1e-6f) dest += f.normalisedCopy() * 0.75f;
        {
            Ogre::Vector3 m = ch->movement->currentMotion; m.y = 0.0f;
            float dot = (f.squaredLength() > 1e-6f && m.squaredLength() > 1e-6f) ? f.normalisedCopy().dotProduct(m.normalisedCopy()) : 2.0f;
            char sb[96];
            sprintf_s(sb, sizeof(sb), "[WASDCombat] snap_order facing_vs_motion=%.2f", dot);   // DIAG: -1 = moving opposite to facing
            VerbLog(sb);
        }
        ch->playerMoveOrderDefault(nullptr, nullptr, dest);
        s_lastModOrderMs = GetTickCount64();   // DIAG
    }
}

// slaveOrderDriven — obeying slave OUTDOORS: WASD becomes move orders and
// direct injection is off.  Indoors/caged (moveOrderMayBark) the order could
// only bark, so direct injection stays (see isObeyingSlave).
static bool slaveOrderDriven(Character* ch)
{
    return slaveEscapeZone(ch) && !moveOrderMayBark(ch);
}

// applySlaveMovement — WASD for an obeying slave = a point-click 10 m ahead.
// Same persistent-order cadence as the (dormant) downed crawl: re-issue on
// first press, direction change, a 1.5 s refresh, or approach of the last
// dest — never per frame (that restarts pathfinding before it moves).  Runs
// pre-AI (step 5) so the obedience job and the guards see the order this
// frame exactly as they would a player's right-click.
static void applySlaveMovement(bool bW, bool bA, bool bS, bool bD)
{
    if (!s_freeMoveAnchor || !s_freeMoveAnchor->movement) return;
    Ogre::Vector3 dir;
    if (!computeWASDDirection(bW, bA, bS, bD, dir)) return;
    float dlen = dir.length();
    if (dlen < 0.001f) return;
    dir /= dlen;
    Ogre::Vector3 posNow = s_freeMoveAnchor->movement->pos;
    ULONGLONG     now    = GetTickCount64();
    const float hopLen     = 100.0f;   // point-click range, 10 m
    const float approachAt = 60.0f;
    if (s_slaveOrderActive
        && dir.dotProduct(s_slaveLastDir) > 0.95f
        && now - s_slaveLastIssueMs < 1500
        && (s_slaveLastDest - posNow).length() > approachAt)
        return;
    s_slaveLastDir     = dir;
    s_slaveLastIssueMs = now;
    Ogre::Vector3 dest = posNow + dir * hopLen;
    s_slaveLastDest    = dest;
    s_freeMoveAnchor->playerMoveOrderDefault(nullptr, nullptr, dest);
    s_lastModOrderMs   = GetTickCount64();   // DIAG
    s_slaveOrderActive = true;
    if (g_log.debugLogging)
    {
        char buf[160];
        sprintf_s(buf, sizeof(buf),
            "[WASDCombat] dc_slave_wasd_order dir=(%.2f,%.2f) dest=(%.1f,%.1f,%.1f)",
            dir.x, dir.z, dest.x, dest.y, dest.z);
        DebugLog(buf);
    }
}

// -----------------------------------------------------------------------
// AI MOTION FEED (v1.3.2) — feed the anchor's REAL velocity to the enemy AI.
//
// Under MOVE_DIRECTION the engine's speed bookkeeping is bypassed: after
// CharMovement::update the anchor's currentMotion/currentSpeed read ZERO even at
// a full sprint (slope-research empirics 2026-08-05, 459 samples).  The enemy
// combat AI reads exactly those fields on its TARGET —
// CombatMovementController::chasingModeCheck (sprint-chase vs combat-stance
// walk), CharStats::chooseAttack(..., opponentIsStationary) and
// CharMovement::predictNextPosition (attack lead) — so every enemy sees a target
// standing perfectly still that keeps being somewhere else: it drops into slow
// stance-walk positioning, aims with zero lead, and never commits a swing while
// WASD is held.  The moment the player stops, its model matches reality and
// attacks resume — the exact exploit in the Workshop report (2026-08-22):
// "enemies go into a combat animation which allows us to escape any situation".
// This is DC STARVING the AI of data vanilla would provide (Rule 4: investigate
// locomotion interference, don't suppress combat) — not the AI misbehaving.
//
// Fix, each driving frame AFTER the original update has moved the body: write
// the real per-frame velocity (pos delta / dt — self-calibrating at any game
// speed) into currentMotion/currentSpeed so AI readers see the truth.  Before
// the NEXT original call both fields are restored to the engine's expected
// post-update state (zero) so the MOVE_DIRECTION ramp integration never sees
// our values (only matters when wasdAccelerationMultiplier==1.0 — the default
// 1.25 pre-charge overwrites currentMotion pre-orig anyway).  The engine NEVER
// writes currentSpeed in this mode, so every non-driving anchor frame tears the
// feed down — else a stale speed would make enemies see a "moving" target while
// the player stands (the inverse bug).  Stationary (hold/stop) therefore reads
// zero = vanilla-correct: attacks on a stopped player work exactly as before.
// INI [Settings] EnemyPursuit=false reverts (read once at plugin load, like the
// other [Settings] keys — takes effect on the next game launch).
//
// Layout inside the hook: fields are RESTORED to zero at the top of EVERY anchor
// frame (so all early-return sub-paths — hold, furniture detach, downed, clip
// buffer — hand the engine and the AI a truthful stationary state), the position
// history is invalidated on every non-driving frame, and the WRITE happens only
// at the end of the inject path after the original has moved the body.
// -----------------------------------------------------------------------
static AnimationClassBase::SingleAnimation* manualPlayingTechnique(AnimationClass* animation,
                                                                    CombatTechniqueData* technique)
{
    if (!animation || !technique) return nullptr;
    AnimationClassBase::SingleAnimation* playing = animation->getAnimationPlaying(technique);
    if (!playing) playing = animation->getAnimationPlaying_datName(technique->animation);
    if (!playing) playing = animation->getAnimationPlaying_animName(technique->animation);
    return playing;
}

static float manualClipTimeline(AnimationClassBase::SingleAnimation* playing, float* lengthOut)
{
    if (lengthOut) *lengthOut = 0.0f;
    if (!playing || !playing->mainState) return -1.0f;
    float length = playing->mainState->getLength();
    if (!(length > 0.01f && length < 60.0f)) return -1.0f;
    if (lengthOut) *lengthOut = length;
    return playing->mainState->getTimePosition() / length;
}

static void charMovUpdate_hook(CharMovement* thisptr, float time)
{
    // Fast path: non-anchor characters exit immediately with no further work.
    // s_anchorMovement is non-volatile; this single pointer comparison is the
    // only cost for every NPC/guard CharMovement::update call.
    if (thisptr != s_anchorMovement || s_loadGuardActive || s_dcShutdownInProgress)
    {
        if (s_dcShutdownInProgress && !s_hookBlockLoggedCharMov) {
            s_hookBlockLoggedCharMov = true;
            DebugLog("[WASDCombat] dc_hooks_blocked_during_loadgame hook=charMovUpdate"); }
        s_charMovUpdateOrig(thisptr, time);
        return;
    }

    // Anchor character only from this point.  Time DC-specific work from here.
    ScopeTimer _tCM(s_prof_charMove);
    // Continuously cache the anchor's REAL speed tier every frame.  A traveling /
    // save-load frees the controlled character before the load path can read it,
    // so that path used to fall back to RUN — which overwrote the player's speed
    // with a sprint after every load ("traveling load forces sprint", user
    // 2026-07-31).  Caching the true tier here lets the reacquire restore the
    // ACTUAL speed instead of guessing.  Guarded to real tiers (< GROUPED).
    // Build 63: NOT while the player pointer is lost (chunk load) - the game
    // resets the tier during the load and the cache picked that up before the
    // reacquire restored it ("speed drops to walk between chunks", user 2026-09-30).
    if (thisptr->speedOrders < GROUPED && !s_dcPtrLossActive)
        s_dcPreservedSpeedMode = thisptr->speedOrders;

    // AI MOTION FEED restore — hand the engine (and any AI reader on a
    // non-driving frame) the expected post-update state (zero) before ANY
    // anchor path runs the original.  The default pre-charge re-seeds
    // currentMotion further down the inject path exactly as it always has.
    if (s_aiFeedWrote)
    {
        thisptr->currentMotion = Ogre::Vector3::ZERO;
        thisptr->currentSpeed  = 0.0f;
        s_aiFeedWrote = false;
    }
    if (s_aiFeedModePublished)
    {
        thisptr->movementMode      = MOVE_DIRECTION;
        thisptr->officiallyStopped = s_aiFeedSavedStopped;
        s_aiFeedModePublished      = false;
    }
    // Use frame-cached values so no volatile reads are needed inside the per-frame anchor logic.
    bool wasdHeld    = s_frameWasdHeld;
    bool inVMode     = (s_frameMode == MODE_FREE_MOVE);
    bool lootSuspend = s_frameLootSuspend;

    // A held guard owns the body and WASD is ignored. Treat the physical key as
    // absent before entering the ordinary stationary path below. The old path
    // ran the native movement update and then rewound only CharMovement::pos;
    // the rendered/root-motion body had already advanced, so the camera stayed
    // on the rewound anchor while the visible character ran away. One coherent
    // stationary owner is safer than repairing only one coordinate layer.
    if (inVMode && !lootSuspend && wasdHeld && manualBlockMovementSlow())
    {
        wasdHeld = false;
        static ULONGLONG lastGuardWasdBlockedMs = 0;
        ULONGLONG guardNow = GetTickCount64();
        if (guardNow - lastGuardWasdBlockedMs >= 1000)
        {
            lastGuardWasdBlockedMs = guardNow;
            VerbLog("[Ironwind] guard_wasd_blocked");
        }
    }

    // Combat WASD transition bridge: if we were just driving and a real key was
    // held within COMBAT_WASD_BRIDGE_MS, and the anchor is in combat, treat the
    // current no-key gap as still-driving (using the last direction) so a key
    // roll (W->A->S->D) doesn't hand the body to the AI to square up.  Does NOT
    // refresh s_wasdLastHeldMs (see the held branch), so it self-expires.
    bool combatBridge = false;
    if (!wasdHeld && inVMode && !lootSuspend
        && !manualBlockMovementSlow()
        && s_wasdMovementApplied
        && s_prevWasdDir.squaredLength() > 0.0001f)
    {
        Character* chB = thisptr->getCharacter();
        if (chB && chB->isInCombatMode(true, true) && s_wasdLastHeldMs > 0
            && (GetTickCount64() - s_wasdLastHeldMs) < COMBAT_WASD_BRIDGE_MS)
            combatBridge = true;
    }

    if (!inVMode || (!wasdHeld && !combatBridge) || lootSuspend)
    {
        // Not in WASD-drive mode for the anchor.
        if (inVMode && !wasdHeld && !lootSuspend)
        {
            Character*   chR = thisptr->getCharacter();

            // Instant stop: clear residual WASD motion before original runs.
            // Grace window: hold previous motion for wasdInputGraceMs ms after key release
            // so rapid tap/switch doesn't stutter through a stop cycle.
            if (s_wasdMovementApplied)
            {
                ULONGLONG now     = GetTickCount64();
                ULONGLONG graceMs = g_release.wasdReleaseGraceMs;
                bool inGrace      = (graceMs > 0 &&
                                     s_wasdLastHeldMs > 0 &&
                                     (now - s_wasdLastHeldMs) < graceMs);
                if ((!inGrace || manualBlockMovementSlow())
                    && (!isCommittedAction(chR) || manualBlockMovementSlow()))
                {
                    Ogre::Vector3 velPre = thisptr->currentMotion;
                    thisptr->halt();
                    thisptr->desiredMotion = Ogre::Vector3::ZERO;
                    thisptr->moveLimit     = 0.0f;
                    if (g_loco.wasdDecelerationMultiplier > 1.0f)
                        thisptr->currentMotion = Ogre::Vector3::ZERO;
                    s_wasdMovementApplied = false;
                    s_prevWasdDir         = Ogre::Vector3::ZERO;
                    // The move-to-current-pos snap cancels any in-flight path
                    // order on release.  OUTDOORS it resolves instantly; INDOORS
                    // playerMoveOrderDefault path-walks the interior network even
                    // to the current position, producing a one-frame step before
                    // the hold-clamp engages = "small movement delay when walking
                    // indoors" (field 2026-06-17).  halt()+zeroed motion above and
                    // the X/Z hold-clamp next frame stop the character cleanly, so
                    // indoors we skip the snap.
                    if (s_freeMoveAnchor && !moveOrderMayBark(s_freeMoveAnchor))
                    {
                        s_freeMoveAnchor->playerMoveOrderDefault(nullptr, nullptr, thisptr->pos);
                        VerbLog("[WASDCombat] wasd_release_preorig_anchor_snap");
                    }
                    (void)velPre;
                }
            }

            // Post-WASD hold: keep the character where WASD parked them.
            // Motion is zeroed before the original runs and the X/Z
            // position is clamped after it (indoor routing writes position
            // late in the frame).  Vanilla locomotion resumes the moment
            // the hold clears (click / WASD / V OFF) or an exemption
            // yields (healing, turret, menu, downed, protected anim,
            // combat).
            const char* holdReason = "";
            if (!computeHoldDecision(chR, &holdReason))
            {
                if (!s_holdPosValid)
                {
                    s_holdPos      = thisptr->pos;
                    s_holdPosValid = true;
                }
                thisptr->halt();
                // A completed manual swing can leave CharMovement's animation
                // override latched while the post-WASD hold takes over.  The
                // next WASD press clears it in the drive path; do the same on
                // the first truly idle frame so no extra keypress is needed.
                bool restoreIdlePose = s_manualAttackOn && !manualBlockMovementSlow()
                    && !s_freeAttackTech && !s_freeAnimCombatMode
                    && !s_attackActive;
                CombatClass* idleCC = restoreIdlePose && chR ? chR->getCombatClass() : nullptr;
                if (restoreIdlePose)
                {
                    thisptr->animationOverride = false;
                    if (idleCC && manualHitReactionLive(chR))
                        idleCC->combatModeActive = true;
                    else if (idleCC && idleCC->getCombatState() != STUMBLE
                             && idleCC->getCombatState() != REACTION_BLOCK)
                        idleCC->combatModeActive = false;
                }
                thisptr->movementMode  = MOVE_DIRECTION;   // path-following ignored
                thisptr->desiredMotion = Ogre::Vector3::ZERO;
                thisptr->moveLimit     = 0.0f;
                thisptr->currentMotion = Ogre::Vector3::ZERO;
                CombatClass* guardCC = (manualBlockMovementSlow() && chR)
                    ? chR->getCombatClass() : nullptr;
                if (guardCC && s_freeGuardUpperIdle && s_freeBlockTech)
                    guardCC->combatModeActive = true;
                if (!s_idleHoldEngaged)
                {
                    s_idleHoldEngaged = true;
                    VerbLog("[WASDCombat] dc_wasd_hold_engaged");
                }
                if (manualBlockMovementSlow())
                {
                    static ULONGLONG lastGuardHoldLogMs = 0;
                    ULONGLONG guardNow = GetTickCount64();
                    if (guardNow - lastGuardHoldLogMs >= 1000)
                    {
                        lastGuardHoldLogMs = guardNow;
                        VerbLog("[Ironwind] guard_stationary_hold");
                    }
                }
                s_charMovUpdateOrig(thisptr, time);
                if (restoreIdlePose)
                {
                    bool overrideAfter = thisptr->animationOverride;
                    thisptr->animationOverride = false;
                    if (idleCC && manualHitReactionLive(chR))
                        idleCC->combatModeActive = true;
                    else if (idleCC && idleCC->getCombatState() != STUMBLE
                             && idleCC->getCombatState() != REACTION_BLOCK)
                        idleCC->combatModeActive = false;
                    if (overrideAfter)
                        VerbLog("[Ironwind] free_action_idle_pose_restored");
                }
                if (guardCC && s_freeGuardUpperIdle && s_freeBlockTech)
                    guardCC->combatModeActive = true;
                if (manualBlockMovementSlow())
                {
                    // The guard path already halted the native controller
                    // before update. Keep the post-update position as the one
                    // authoritative body position; rewinding only this raw
                    // field split the camera anchor from the rendered root.
                    s_holdPos.x = thisptr->pos.x;
                    s_holdPos.z = thisptr->pos.z;
                }
                else
                {
                    thisptr->pos.x = s_holdPos.x;
                    thisptr->pos.z = s_holdPos.z;
                }
                thisptr->currentMotion = Ogre::Vector3::ZERO;
                return;
            }
            else if (s_idleHoldEngaged)
            {
                s_idleHoldEngaged = false;
                if (g_log.debugLogging)
                    VerbLog("[WASDCombat] dc_wasd_hold_released");
            }
        }
        s_charMovUpdateOrig(thisptr, time);
        return;
    }

    // WASD held — apply movement override.
    // Skip injection while turret-suspended, menu-suspended, or healing job is active.
    if (s_cameraLockTurretSuspend || s_menuSuspendActive)
    {
        s_charMovUpdateOrig(thisptr, time);
        return;
    }
    if (s_healingJobActive)
    {
        if (!s_medicalJobSuppressedThisHold)
        {
            s_medicalJobSuppressedThisHold = true;
            VerbLog("[WASDCombat] medical_job_blocks_wasd");
        }
        s_charMovUpdateOrig(thisptr, time);
        return;
    }
    // SLAVE OBEDIENCE: an obeying slave outdoors is driven by the pre-AI
    // slave move order (step 5) — vanilla owns the body, no halt(), no
    // setDirectMovement, no post-update re-assert (see isObeyingSlave).
    {
        Character* chS = thisptr->getCharacter();
        if (chS && slaveOrderDriven(chS))
        {
            s_wasdMovementApplied = false;
            s_charMovUpdateOrig(thisptr, time);
            return;
        }
    }
    // Downed/crippled characters OUTDOORS use playerMoveOrderDefault (issued
    // in pre-AI section 5) — halt() + setDirectMovement here would cancel
    // that order.  INDOORS they fall through to the direct-steering branch
    // below, same as standing WASD (orders path-walk interiors).
    {
        Character* chC = thisptr->getCharacter();
        if (chC && downedOrderDriven(chC))
        {
            static ULONGLONG s_crippledChMovTick = 0;
            ULONGLONG nowCC = GetTickCount64();
            if (nowCC - s_crippledChMovTick >= 2000) { s_crippledChMovTick = nowCC;
                VerbLog("[WASDCombat] dc_crippled_state_detected");
                VerbLog("[WASDCombat] dc_crippled_can_move=true");
                VerbLog("[WASDCombat] dc_crippled_using_downed_movement_path"); }

            // Same combat-steering conflict as standing WASD (v1.7.5):
            // lingering combat mode after a fight computes its own
            // movement inside update and fights the crawl order — the
            // downed character stutters and goes nowhere.  Flip the flag
            // for the integration step only; CombatClass::go still sees
            // it in the AI phase.  (A downed character cannot be mid-
            // swing, so no CHOP_WEAPON exclusion is needed here.)
            CombatClass* ccD = chC->getCombatClass();
            bool flippedD = false;
            if (ccD && ccD->combatModeActive)
            {
                ccD->combatModeActive = false;
                flippedD = true;
            }
            s_charMovUpdateOrig(thisptr, time);
            if (flippedD)
            {
                ccD->combatModeActive = true;
                static ULONGLONG s_downedSteerLogTick = 0;
                ULONGLONG nowDS = GetTickCount64();
                if (nowDS - s_downedSteerLogTick >= 1000)
                {
                    s_downedSteerLogTick = nowDS;
                    DebugLog("[WASDCombat] dc_downed_combat_steering_overridden");
                }
            }
            return;
        }
    }
    // Only a REAL key press refreshes the last-held timestamp; a bridge frame
    // must let the bridge window expire (otherwise a single tap would drive
    // forever in combat).
    if (!combatBridge)
        s_wasdLastHeldMs = GetTickCount64();
    s_holdPosValid   = false;  // WASD drives — hold anchor recaptured on next hold

    Ogre::Vector3 wasdDir;
    bool dirOk = computeWASDDirection(s_wHeld, s_aHeld, s_sHeld, s_dHeld, wasdDir);
    // Bridge frame: no live key this instant, reuse the last driven direction so
    // movement carries through the key-roll gap instead of stalling.
    if (!dirOk && combatBridge)
    {
        wasdDir = s_prevWasdDir;
        dirOk   = true;
    }

    // GET UP FROM SEAT / BED / MACHINE (user req 2026-06-22): when the character is
    // anchored to a UseableStuff object (chair / throne / bed / workstation) the body
    // is locked to the node, so setDirectMovement only spins the model in place.
    // Issue ONE player move order toward the held direction — the exact path a
    // point-click takes: it (a) gets the character up with the proper animation, and
    // (b) REPLACES the queued "use object" job so the AI doesn't re-grab the furniture
    // and snap them back when they walk past it again (field 2026-06-22).  Edge-gated
    // (once per sit; re-armed when standing).  Direct WASD injection overrides the
    // order the instant they're standing; the on-release stop cancels any remainder.
    if (dirOk)
    {
        Character* chSeat = thisptr->getCharacter();
        if (chSeat && (isAnchoredToFurniture(chSeat) || chSeat->isCurrentlyGettingUp))
        {
            chSeat->playerWantsMeToGetUp = true;
            // SELF-HEALING re-issue: send the get-up move order at most once per window
            // while anchored — NOT a latched once-per-session flag (that got stuck
            // across squad-switches / job re-sits, so a re-selected character could no
            // longer leave the chair, field 2026-06-22).  Throttled so we don't re-path
            // every frame, but always re-arms for a fresh sit / re-selected character.
            static ULONGLONG s_lastFurnitureExitMs = 0;
            ULONGLONG nowF = GetTickCount64();
            if (isAnchoredToFurniture(chSeat) && (nowF - s_lastFurnitureExitMs) > 600)
            {
                Ogre::Vector3 dest = thisptr->pos + wasdDir * 50.0f;   // ~5m ahead (≈10u/m)
                chSeat->playerMoveOrderDefault(nullptr, nullptr, dest);
                s_lastFurnitureExitMs = nowF;
                DebugLog("[WASDCombat] dc_furniture_exit_move_order");
            }
            s_wasdMovementApplied = false;   // not injecting this frame; let the get-up run
            s_prevWasdDir         = wasdDir;
            static ULONGLONG s_seatGetupTick = 0;
            ULONGLONG nowSG = GetTickCount64();
            if (nowSG - s_seatGetupTick >= 1000) { s_seatGetupTick = nowSG;
                DebugLog("[WASDCombat] dc_wasd_getup_from_furniture"); }
            s_charMovUpdateOrig(thisptr, time);
            return;
        }
    }

    // FINISH-THE-CLIP buffer (user req 2026-06-21, broadened 2026-06-22): Kenshi can't
    // abort an animation clip, so cutting one with movement looks broken/stutters.
    // BUFFER movement while a committed combat clip plays — the character's own swing
    // (windup/strike), a native automatic BLOCK, a stagger from being hit
    // (STUMBLE), or a parry (REACTION_BLOCK)
    // — and let it finish in place, THEN move.  go() is left running for these states
    // (so the clip completes + the state advances), then combatGo_hook suppresses it,
    // so no new attack chains and movement flows the instant the clip ends.  We touch
    // NO combat state here (no flip, no cut) — exactly one system drives the body, so
    // nothing fights.  Generic DECISION/BLOCK/CIRCLE/WAIT/HESITATE still yield
    // to movement; only the exact automatic-guard BLOCK technique is buffered.
    if (dirOk && (isCommittedCombatClip(thisptr->getCharacter())
                  || manualHitReactionLive(thisptr->getCharacter())))
    {
        // v40: our own free swing does not outrank a hit reaction.  The free
        // swing path returns false above for the anchor, so reaching this
        // branch with a live free swing means the vanilla machine just
        // entered a reaction (STUMBLE/REACTION_BLOCK) on top of it - buffer
        // as usual, the reaction owns the body until the cleanup waits it out.
        s_wasdMovementApplied = false;   // the clip owns the body; do not inject
        static ULONGLONG s_clipBufTick = 0;
        ULONGLONG nowSB = GetTickCount64();
        if (nowSB - s_clipBufTick >= 1000) { s_clipBufTick = nowSB;
            VerbLog("[WASDCombat] dc_wasd_buffered_combat_clip"); }
        s_charMovUpdateOrig(thisptr, time);
        return;
    }

    if (dirOk)
    {
        // Turn responsiveness: detect direction change and boost limit.
        bool prevHasDir = (s_prevWasdDir.squaredLength() > 0.0001f);
        bool turning    = prevHasDir && (wasdDir.dotProduct(s_prevWasdDir) < 0.9f);
        float limit     = wasdMoveLimit(turning);
        const bool attackSlow = manualAttackMovementSlow();
        const bool combatSlow = attackSlow || manualBlockMovementSlow();
        // A targeted swing reaches this branch only after the player's input
        // synchronously started the native AttackState. Preserve that action
        // layer while allowing the existing 20% WASD movement.
        // Automatic guard has no Ironwind free-block technique: before/after a
        // real native BLOCK it is represented by the held vanilla defensive
        // request plus the normal combat idle. Preserve that combat layer
        // during 20% WASD instead of clearing it as ordinary locomotion.
        const bool autoGuardHeldPose = manualBlockMovementSlow()
                                    && s_settingAiBlocks && s_blockHeldPrev;
        const bool manualCombatPose = (attackSlow && (s_freeAttackTech || s_attackActive))
                                   || (manualBlockMovementSlow() && s_freeGuardUpperIdle && s_freeBlockTech)
                                   || autoGuardHeldPose;

        s_wasdMovementApplied = true;
        { ULONGLONG t = GetTickCount64();
          if (t - s_movInjLogTick >= 1000) { s_movInjLogTick = t;
              if (g_log.debugVerbose)
                  DebugLog("[WASDCombat] movement_injection_allowed"); } }
        thisptr->halt();
        thisptr->animationOverride = false;
        thisptr->movementMode      = MOVE_DIRECTION;
        thisptr->setDesiredSpeed(thisptr->speedOrders);
        if (combatSlow) thisptr->setDesiredSpeed(limit);
        thisptr->setDirectMovement(wasdDir, limit);

        // Pre-charge currentMotion to jump-start acceleration ramp-up.
        // The boost is a FRACTION of desiredSpeed; for a healthy runner
        // (desiredSpeed ~999) even a 20% fraction is fast, but for an
        // injured/crippled character (desiredSpeed clamped to ~55) the same
        // fraction is a ~11-unit crawl.  In a big fight at low FPS the engine
        // often fails to integrate currentMotion up past the pre-charge that
        // same frame, so the body lurches between full speed and that crawl —
        // the "stutter, especially when injured/crippled" (field diag
        // 2026-06-13: spd=55, cur oscillating 55 -> 10 -> 0).  Floor the
        // pre-charge at the FULL desired velocity so a speed-capped character
        // always gets their (already-reduced) full speed, never a fraction.
        float accel = (g_loco.wasdAccelerationMultiplier - 1.0f)
                    * (turning ? g_loco.wasdTurnResponsiveness : 1.0f);
        if (accel > 0.0f)
        {
            float boost = accel < 1.0f ? accel : 1.0f;
            float preSpeed = thisptr->desiredSpeed * boost;
            // Floor at the FULL desired velocity so a (reduced) capped character
            // still gets their whole speed and doesn't stutter — BUT never above the
            // WASD move-limit.  Flooring at raw desiredSpeed was what set currentMotion
            // to ~999 every frame and let shackled/injured runners outrun everything
            // (the setDirectMovement limit alone didn't bind because this velocity
            // write does).  Clamp the pre-charge to `limit` so the cap actually holds.
            float floorSpeed = thisptr->desiredSpeed;
            const bool capPrecharge = s_settingWasdSpeedCap || combatSlow;
            if (capPrecharge && floorSpeed > limit) floorSpeed = limit;
            if (preSpeed < floorSpeed) preSpeed = floorSpeed;
            if (capPrecharge && preSpeed > limit) preSpeed = limit;
            thisptr->currentMotion = wasdDir * preSpeed;
        }

        s_prevWasdDir = wasdDir;

        // NORMAL WALK WHILE DRIVING (user req 2026-06-22; refined via dc_armsdown_diag):
        // WASD movement must ALWAYS use plain walk/run locomotion — never a combat-ready
        // stance.  The diagnostic proved the arms-down appears while the character is
        // TARGETED (isInCombatMode / red portrait): flipping combatModeActive off only
        // for the integration step was NOT enough — RESTORING it true afterward kept the
        // combat animation layer (and our COMBAT_FINISHED cut) active under
        // MOVE_DIRECTION, so the body stayed in the lowered-weapon "arms-down" pose every
        // frame.  Fix: while WASD is driving, leave combatModeActive CLEARED (do not
        // restore).  This is XP-safe — go() is already suppressed while driving
        // (combatGo_hook), so the character isn't attacking/earning combat XP while you
        // reposition anyway — and the AI re-establishes combat the instant you release
        // WASD (go() re-runs and re-engages).  Result: plain walk even with an enemy on
        // you; combat (and its animations) resume the moment you stop driving.
        Character*   chFlip = thisptr->getCharacter();
        CombatClass* ccFlip = chFlip ? chFlip->getCombatClass() : nullptr;
        // A free swing or held guard needs its combat animation layer while
        // the feet move.  Clearing it lets walking override the action pose.
        // During a native hit reaction FORCE the flag so vanilla owns its
        // animation layer.  v44 bases this only on _getHit/state, never time.
        if (ccFlip && (manualCombatPose
                       || manualHitReactionLive(chFlip)))
            ccFlip->combatModeActive = true;
        else if (ccFlip && ccFlip->combatModeActive)
            ccFlip->combatModeActive = false;
        Ogre::Vector3 attackMoveStart = thisptr->pos;
        s_charMovUpdateOrig(thisptr, time);

        // The native update may restore the RUN order and accelerate after our
        // pre-charge.  Bound all three live movement values again before the
        // next integration frame.  Keep the vertical component (gravity) intact.
        if (combatSlow)
        {
            float postLimit = wasdMoveLimit(false);
            thisptr->setDesiredSpeed(postLimit);
            float planar = sqrtf(thisptr->currentMotion.x * thisptr->currentMotion.x
                               + thisptr->currentMotion.z * thisptr->currentMotion.z);
            if (planar > postLimit && planar > 0.001f)
            {
                float factor = postLimit / planar;
                thisptr->currentMotion.x *= factor;
                thisptr->currentMotion.z *= factor;
            }
            if (thisptr->currentSpeed > postLimit) thisptr->currentSpeed = postLimit;
            if (s_verboseLog)
            {
                static ULONGLONG lastAttackMoveTraceMs = 0;
                ULONGLONG traceNow = GetTickCount64();
                if (traceNow - lastAttackMoveTraceMs >= 350)
                {
                    lastAttackMoveTraceMs = traceNow;
                    Ogre::Vector3 delta = thisptr->pos - attackMoveStart;
                    float frameDistance = sqrtf(delta.x * delta.x + delta.z * delta.z);
                    char b[192]; sprintf_s(b, sizeof(b),
                        "[Ironwind] combat_move attack=%d block=%d limit=%.2f desired=%.2f current=%.2f motion=%.2f frame_dist=%.3f frame_time=%.3f",
                        attackSlow ? 1 : 0, manualBlockMovementSlow() ? 1 : 0,
                        postLimit, thisptr->desiredSpeed, thisptr->currentSpeed,
                        thisptr->currentMotion.length(), frameDistance, time);
                    VerbLog(b);
                }
            }
        }


        // Post-original re-assert: the combat AI re-enables animationOverride /
        // flips movementMode away from MOVE_DIRECTION when it wants to drive the
        // body (positioning OR an action animation), which would override the
        // player's WASD movement.  Re-assert MOVE_DIRECTION unconditionally so the
        // player always wins (absolute priority, user req 2026-06-20).
        {
            Character*   chPost = thisptr->getCharacter();
            CombatClass* ccPost = chPost ? chPost->getCombatClass() : nullptr;
            if (ccPost)
            {
                if (manualCombatPose || manualHitReactionLive(chPost))
                    ccPost->combatModeActive = true;   // v41: reaction needs the layer
                // Re-assert MOVE_DIRECTION so the AI can never drag/circle/square the
                // character while keys are held (absolute priority, user req
                // 2026-06-20).  We do NOT cut the combat state here anymore: the
                // earlier per-frame `combatState = COMBAT_FINISHED` write FOUGHT the
                // combat system and produced the broken/stutter look (field
                // 2026-06-22).  Sliding is already prevented two cleaner ways — committed
                // clips (swing/stagger/parry) are BUFFERED above so they never move, and
                // for every other state combatModeActive is cleared this frame so plain
                // walk plays (no combat clip to slide).  One system drives at a time.
                {
                    thisptr->animationOverride = false;
                    thisptr->movementMode      = MOVE_DIRECTION;
                    thisptr->setDirectMovement(wasdDir, wasdMoveLimit(false));
                }
                if (g_log.debugLogging && ccPost && ccPost->combatModeActive)
                {
                    static ULONGLONG s_xpCombatLogTick = 0;
                    ULONGLONG _tcx = GetTickCount64();
                    if (_tcx - s_xpCombatLogTick >= 1000) { s_xpCombatLogTick = _tcx;
                        DebugLog("[WASDCombat] dc_xp_vanilla_combat_allowed skill=Combat"); }
                }
            }
        }

        // AI MOTION FEED publish (v2 — doc at the feed statics): the original
        // has moved the body; publish the frame-level velocity (computed once
        // per render frame in mainLoop — substep-immune) plus the fleeing-
        // character mode so enemy AI readers (chasingModeCheck / chooseAttack /
        // predictNextPosition) see a normally-moving target.  UNCONDITIONAL on
        // every driven update call: substeps each end published; the top-of-
        // frame restore hands the engine clean state before every original.
        if (s_settingEnemyPursuit)
        {
            thisptr->currentMotion     = s_aiPubVel;
            thisptr->currentSpeed      = s_aiPubSpd;
            s_aiFeedWrote              = true;
            s_aiFeedSavedStopped       = thisptr->officiallyStopped;
            thisptr->movementMode      = MOVE_NORMAL;
            thisptr->officiallyStopped = false;
            s_aiFeedModePublished      = true;
            s_aiPubDroveThisFrame      = true;
            if (g_log.debugLogging)
            {
                ULONGLONG nowAF = GetTickCount64();
                if (nowAF - s_aiFeedLogTick >= 1000) { s_aiFeedLogTick = nowAF;
                    char fbuf[96];
                    sprintf_s(fbuf, sizeof(fbuf),
                        "[WASDCombat] dc_ai_motion_feed spd=%.1f", s_aiPubSpd);
                    DebugLog(fbuf); }
            }
        }
    }
    else
    {
        // Race-case stop: snapshot said WASD held but poll thread released keys before
        // direction could be computed.  Zero stale motion fields before vanilla runs.
        if (s_wasdMovementApplied)
        {
            Character* chRace = thisptr->getCharacter();
            if (!isCommittedAction(chRace))
            {
                thisptr->halt();
                thisptr->desiredMotion = Ogre::Vector3::ZERO;
                thisptr->moveLimit     = 0.0f;
                thisptr->currentMotion = Ogre::Vector3::ZERO;
                s_prevWasdDir          = Ogre::Vector3::ZERO;
                s_wasdMovementApplied  = false;
                VerbLog("[WASDCombat] wasd_release_race_preorig_zeroed");
            }
        }
        s_charMovUpdateOrig(thisptr, time);
    }
}

// -----------------------------------------------------------------------
// MANUAL BLOCK (Build 2) - see the state block by s_settingManualBlock.
// -----------------------------------------------------------------------
static bool manualBlockArmed(Character* self)
{
    return s_settingManualBlock && !s_settingAiBlocks && s_blockHeld
        && s_blockAcceptedPressMs != 0 && s_blockPressMs == s_blockAcceptedPressMs
        && s_blockHoldStartMs != 0
        && !s_dcShutdownInProgress && !s_loadGuardActive
        && s_mode == MODE_FREE_MOVE
        && self && self == s_freeMoveAnchor;
}

// Sync the orders-panel BLOCK checkbox when the panel is showing this character
// (same precedent as the sneak toggle driving the panel's own checkbox).
static void manualBlockSyncPanel(Character* ch, bool on)
{
    OrdersPanel* op = (gui && gui->mainbar) ? gui->mainbar->ordersDataPanel : nullptr;
    if (op && op->blocksCheckbox && op->ordersCharacter.getCharacter() == ch)
        op->blocksCheckbox->setStateSelected(on);
}

// Apply / restore the vanilla DEFENSIVE_COMBAT standing order on the anchor.
static void manualBlockOrder(bool on)
{
    if (on)
    {
        if (s_blockOrderApplied || !s_freeMoveAnchor) return;
        bool prev = s_freeMoveAnchor->getStandingOrder(MessageForB::M_SET_ORDER_DEFENSIVE_COMBAT);
        if (!prev)
        {
            s_freeMoveAnchor->setStandingOrder(MessageForB::M_SET_ORDER_DEFENSIVE_COMBAT, true);
            manualBlockSyncPanel(s_freeMoveAnchor, true);
            s_blockOrderApplied = true;
            s_blockOrderChar    = s_freeMoveAnchor;
        }
        char b[96];
        sprintf_s(b, sizeof(b), "[WASDCombat] block_hold vanilla_order=%s", prev ? "already_on" : "applied");
        VerbLog(b);
    }
    else
    {
        if (!s_blockOrderApplied) { VerbLog("[WASDCombat] block_release"); return; }
        // Only touch the pointer while the world is live and it is still the
        // anchor (or the just-switched-from anchor on a live selection change).
        if (!s_dcShutdownInProgress && !s_loadGuardActive && s_blockOrderChar)
        {
            s_blockOrderChar->setStandingOrder(MessageForB::M_SET_ORDER_DEFENSIVE_COMBAT, false);
            manualBlockSyncPanel(s_blockOrderChar, false);
            VerbLog("[WASDCombat] block_release vanilla_order=restored");
        }
        else
            VerbLog("[WASDCombat] block_release vanilla_order=dropped(load)");
        s_blockOrderApplied = false;
        s_blockOrderChar    = nullptr;
    }
}

static bool verifyPatchSiteBytes(intptr_t addr, const unsigned char* expected, size_t len, const char* name);
static void nativeDefenceResolvePlayFn()
{
    if (s_defencePlayChecked) return;
    s_defencePlayChecked = true;
    intptr_t base = (intptr_t)GetModuleHandleA(NULL);
    intptr_t fn   = base + (intptr_t)DC_DEFENCE_PLAY_RVA;
    static const unsigned char PROLOGUE[16] = { 0x40,0x53,0x55,0x57,0x41,0x54,0x48,0x81,0xEC,0xC8,0x00,0x00,0x00,0x48,0xC7,0x44 };
    static const unsigned char ISDODGE_CMP[4] = { 0x80,0x7A,0x2D,0x00 };   // cmp byte ptr [rdx+0x2D],0 at +0x58
    if (verifyPatchSiteBytes(fn, PROLOGUE, 16, "native defence technique-play")
        && verifyPatchSiteBytes(fn + 0x58, ISDODGE_CMP, 4, "native defence technique-play (+0x58)"))
    {
        s_defencePlayFn = (DcTechPlayFn)fn;
        VerbLog("[WASDCombat] native_defence_play_fn verified");
    }
    else
        ErrorLog("WASDCombatPlugin: native defence technique-play function not verified on this exe - native automatic dodge playback unavailable");
}

// Build only mod-owned block copies from the vanilla defence pool (one shot).
// Dodge records are deliberately skipped: Ironwind no longer manufactures or
// injects a player dodge. A copy = the technique's bytes + its own std::string for the
// animation name; the impact list / event map internals are shared read-only
// with the original (which lives for the whole process) and never freed.
static void manualBlockBuildCopies()
{
    if (s_dcBlockBuilt) return;
    s_dcBlockBuilt = true;
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    const int*  pCount = (const int*)(base + 0x2010F88);
    CombatTechniqueData* const* const* pArr = (CombatTechniqueData* const* const*)(base + 0x2010F90);
    MEMORY_BASIC_INFORMATION mbi;
    auto readable = [&](const void* a, size_t n) -> bool {
        if (!a) return false;
        if (VirtualQuery(a, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD)) return false;
        return ((uintptr_t)a + n) <= ((uintptr_t)mbi.BaseAddress + mbi.RegionSize);
    };
    if (!readable(pCount, 4) || !readable(pArr, 8)) { DebugLog("[WASDCombat] block_copies_unreadable"); return; }
    int cnt = *pCount;
    CombatTechniqueData* const* arr = *pArr;
    if (cnt <= 0 || cnt > 400 || !readable(arr, (size_t)cnt * 8)) { DebugLog("[WASDCombat] block_copies_insane"); return; }
    const size_t objBytes = 0x100;   // > sizeof on 1.0.65 (impactPoints lektor ends ~0xC8)
    for (int i = 0; i < cnt; ++i)
    {
        CombatTechniqueData* t = arr[i];
        if (!readable(t, objBytes)) continue;
        if (!t->isBlock || t->isDodge || s_dcBlockCount >= 8) continue;
        void* mem = malloc(objBytes + 64);
        if (!mem) break;
        memcpy(mem, t, objBytes);
        CombatTechniqueData* c = (CombatTechniqueData*)mem;
        new (&c->animation) std::string(t->animation);   // own name string (deep copy)
        char b[160];
        s_dcBlock[s_dcBlockCount++] = c;
        sprintf_s(b, sizeof(b), "[WASDCombat] block_copy[%d] anim=%s dir0=%d", s_dcBlockCount - 1,
                  c->animation.c_str(), c->numImpactPoints() > 0 ? (int)c->impactPoint(0)->direction : -1);
        VerbLog(b);
    }
}

// CharStats keeps attacks in a different static list from defence techniques.
// The block copy builder above intentionally reads only the defence list.
// Attack techniques are process-owned and are never modified here.
static void manualAttackLoadPool()
{
    if (s_dcAttackBuilt) return;
    static bool unavailableLogged = false;
    // KenshiLib declares CharStats::attacks at this 1.0.65 RVA but does not
    // export its data symbol.  lektor's count and stuff sit at +8 and +16.
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    const unsigned int* pCount = (const unsigned int*)(base + 0x200EF80);
    CombatTechniqueData* const* const* pArr = (CombatTechniqueData* const* const*)(base + 0x200EF88);
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(pCount, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT
        || (mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD)) return;
    unsigned int count = *pCount;
    if (count == 0 || count > 1024)
    {
        if (!unavailableLogged)
        {
            unavailableLogged = true;
            char b[96]; sprintf_s(b, sizeof(b), "[Ironwind] attack_pool_unavailable count=%u", count);
            VerbLog(b);
        }
        return;
    }
    if (VirtualQuery(pArr, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT
        || (mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD)) return;
    CombatTechniqueData* const* attacks = *pArr;
    if (!attacks || VirtualQuery(attacks, &mbi, sizeof(mbi)) != sizeof(mbi)
        || mbi.State != MEM_COMMIT
        || (mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD)
        || (uintptr_t)attacks + count * sizeof(void*) > (uintptr_t)mbi.BaseAddress + mbi.RegionSize) return;
    for (unsigned int i = 0; i < count && s_dcAttackCount < 256; ++i)
    {
        CombatTechniqueData* t = attacks[i];
        if (!t || VirtualQuery(t, &mbi, sizeof(mbi)) != sizeof(mbi)
            || mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_NOACCESS)
            || (mbi.Protect & PAGE_GUARD)
            || (uintptr_t)t + 0x100 > (uintptr_t)mbi.BaseAddress + mbi.RegionSize) continue;
        if (t->isBlock || t->isDodge || t->animation.empty()) continue;
        s_dcAttack[s_dcAttackCount++] = t;
    }
    s_dcAttackBuilt = s_dcAttackCount > 0;
    char b[112]; sprintf_s(b, sizeof(b), "[Ironwind] attack_pool native=%u usable=%d", count, s_dcAttackCount);
    VerbLog(b);
}

// Staggered = the physical states (KO, playing dead, down, getting up) or the
// combat STUMBLE state while combat is live (a stale STUMBLE after a fight is
// not a stagger - see isCommittedAction).
static bool manualStaggered(Character* ch)
{
    if (!ch) return false;
    ProneState prone = ch->getProneState();
    if (prone == PS_KO || prone == PS_PLAYING_DEAD || ch->isDown() || ch->isCurrentlyGettingUp) return true;
    CombatClass* cc = ch->getCombatClass();
    return cc && cc->combatModeActive && cc->getCombatState() == STUMBLE;
}
// Once per frame (from manualBlockTick): stagger edges. Attack presses die;
// a physically continuous RMB hold is preserved separately and becomes a new
// guard request only on the native end edge. End = inputs count again.
static void manualStaggerTick()
{
    bool stag = s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && !s_dcShutdownInProgress
             && !s_loadGuardActive && manualStaggered(s_freeMoveAnchor);
    ULONGLONG now = GetTickCount64();
    if (stag && s_staggerRawSinceMs == 0) s_staggerRawSinceMs = now;
    if (!stag) s_staggerRawSinceMs = 0;
    // Debounced: a stagger counts once the signal has held STAGGER_DEBOUNCE_MS
    // (the stumble state flickers for a frame or two on some hits); the long-
    // stagger clock still runs from the real edge.
    if (stag && s_staggerStartMs == 0 && (now - s_staggerRawSinceMs) >= STAGGER_DEBOUNCE_MS)
    {
        s_staggerStartMs   = s_staggerRawSinceMs;
        int dropped = 0;
        if (!s_attackActive && s_attackCommitUntilMs != 0) { s_attackCommitUntilMs = 0; ++dropped; }
        if (s_attackQueuedAtMs != 0)
        {
            manualAttackClearQueuedInput(true);
            ++dropped;
        }
        manualAttackClearAimInput(true, true);
        char b[96];
        sprintf_s(b, sizeof(b), "[WASDCombat] stagger_start pending_dropped=%d", dropped);
        VerbLog(b);
    }
    else if (!stag && s_staggerStartMs != 0)
    {
        char b[96];
        sprintf_s(b, sizeof(b), "[WASDCombat] stagger_end ms=%llu", now - s_staggerStartMs);
        VerbLog(b);
        s_staggerStartMs = 0;
    }
}

// Is this Character* still a live, loaded, hostile target?  (identity pointer -
// never trusted without this check; Rule 5)
// Main thread, once per frame: V-mode owns manual combat automatically.
static void manualEnsureWeaponReady(Character* me, CharStats* stats);
static Character* manualAttackIntentPick(Character* me);
static void manualAttackAimCaptureTick();
static void manualAttackTick()
{
    bool want = s_settingManualBlock && s_mode == MODE_FREE_MOVE && s_freeMoveAnchor
             && !s_loadGuardActive && !s_dcShutdownInProgress;
    if (s_manualAttackOn != want)
    {
        s_manualAttackOn = want;
        DebugLog(want ? "[WASDCombat] manual_attack_on_with_dc"
                      : "[WASDCombat] manual_attack_off_with_dc");
        if (want)
        {
            manualHitReactionSyncVBoundary(s_freeMoveAnchor, true);
            CharStats* stats = s_freeMoveAnchor->getStats();
            manualEnsureWeaponReady(s_freeMoveAnchor, stats);
            CombatClass* cc = s_freeMoveAnchor->getCombatClass();
            if (cc && cc->animation) cc->animation->setCombatMode(true);
            VerbLog("[Ironwind] v_combat_stance_prearmed");
        }
        else
            manualHitReactionSyncVBoundary(nullptr, false);
    }
}

// Resolve a KenshiLib export to the REAL game address: the export is a
// `jmp [rip+slot]` stub whose slot KenshiLib fills at start-up for the running
// exe version (the same table GetRealAddress uses) - so this stays version-
// safe without a header declaration.
static intptr_t dcResolveKenshiLibExport(const char* mangled)
{
    HMODULE h = GetModuleHandleA("KenshiLib.dll");
    if (!h) return 0;
    unsigned char* stub = (unsigned char*)GetProcAddress(h, mangled);
    if (!stub) return 0;
    if (stub[0] == 0xFF && stub[1] == 0x25)
    {
        int disp = *(int*)(stub + 2);
        void** slot = (void**)(stub + 6 + disp);
        return (intptr_t)*slot;
    }
    return (intptr_t)stub;
}

// AttackState::_NV_initialise(state) - the ONE place a swing starts (state+8 =
// CombatClass*).  In focus mode, the AI's own swings are denied here: the
// initialiser reports failure exactly as it does when the target is out of
// reach, and no technique is chosen or played.  Direct player attacks call
// initialiseAttack(target) themselves and therefore do not need a broad
// s_attackActive exemption here.  Only the exact synchronous initializer scope
// may pass if the engine re-enters this wrapper.
static bool (*s_attackInitOrig)(void* state);
static bool attackInit_hook(void* state)
{
    CombatClass* cc = state ? *(CombatClass**)((char*)state + 8) : nullptr;
    if (cc && s_manualAttackOn && !s_attackDirectInitialising && s_mode == MODE_FREE_MOVE
        && !s_dcShutdownInProgress && !s_loadGuardActive && cc->me == s_freeMoveAnchor)
    {
        ULONGLONG nowD = GetTickCount64();
        if (nowD - s_manualSuppressLogMs >= 1000)
        {
            s_manualSuppressLogMs = nowD;
            char db[96];
            sprintf_s(db, sizeof(db), "[WASDCombat] manual_ai_attack_denied from_state=%d pending=%d",
                      s_anchorPrevState, manualAttackCommitLive() ? 1 : 0);
            VerbLog(db);
        }
        return false;
    }
    return s_attackInitOrig(state);
}

// CombatClass::setCombatState (protected; member pointer via the explicit-
// instantiation loophole) - every state transition the combat code makes goes
// through here.  Second choke point: in focus mode, an attempt to put the
// controlled character into CHOP_WEAPON that is not our own swing becomes
// WAIT_MENACINGLY (the armed ready hold).
template<typename Tag> struct DcRobResult { typedef typename Tag::type type; static type ptr; };
template<typename Tag> typename DcRobResult<Tag>::type DcRobResult<Tag>::ptr;
template<typename Tag, typename Tag::type P> struct DcRob : DcRobResult<Tag>
{
    struct Filler { Filler() { DcRobResult<Tag>::ptr = P; } };
    static Filler filler;
};
template<typename Tag, typename Tag::type P> typename DcRob<Tag, P>::Filler DcRob<Tag, P>::filler;
struct DcSetCombatStateTag { typedef void (CombatClass::*type)(swordStateEnum); };
template struct DcRob<DcSetCombatStateTag, &CombatClass::setCombatState>;
struct DcGetStateClassTag { typedef CombatState* (CombatClass::*type)(swordStateEnum); };
template struct DcRob<DcGetStateClassTag, &CombatClass::getStateClass>;
struct DcWeaponReachTag { typedef float (CombatClass::*type)(); };
template struct DcRob<DcWeaponReachTag, &CombatClass::weaponReach>;
struct DcCameraUpdateAudioTag { typedef void (CameraClass::*type)(); };
template struct DcRob<DcCameraUpdateAudioTag, &CameraClass::updateAudio>;

// V-mode character-centred audio (v89).
//
// CameraClass::updateAudio builds the same 9-float transform accepted by the
// engine's listener-submit routine: two orientation vectors followed by the
// listener position.  Detached OTS/FP cameras are deliberately real render
// cameras, so vanilla submits their remote lens position and nearby combat
// sounds fade out as the player pulls the view back.  Hook the submit routine
// itself and replace ONLY the position.  Orientation remains the rendered
// camera orientation, preserving left/right/front/back spatialisation.
struct DcAudioListenerTransform
{
    float orientationA[3];
    float orientationB[3];
    float position[3];
};
typedef int (*DcAudioListenerSubmitFn)(const DcAudioListenerTransform*, unsigned int);
static DcAudioListenerSubmitFn s_audioListenerSubmitOrig = nullptr;
static bool s_audioListenerAnchorLogged = false;
static bool (*s_characterAudioEventOrig)(Character*, const char*, SoundRange) = nullptr;
static ULONGLONG s_audioSourceBypassLogMs = 0;

// Character::audioEvent performs its own camera-distance test before an event
// is posted to Wwise.  Moving the listener in v87/v88 therefore happened too
// late for sounds that had already been rejected here.  In V mode, only the
// controlled character uses SOUNDRANGE_ALWAYS; every other character and every
// sound outside V mode retains the vanilla range gate.  The listener hook below
// then places that posted source at the player while preserving camera-facing
// spatial orientation.
static bool characterAudioEvent_hook(Character* self, const char* name, SoundRange range)
{
    bool controlled = self && self == s_freeMoveAnchor
                   && s_mode == MODE_FREE_MOVE && self->movement && s_gwFrame
                   && !s_dcShutdownInProgress && !s_loadGuardActive
                   && dcInUpdateList(s_gwFrame, self);
    if (controlled && range != SOUNDRANGE_ALWAYS)
    {
        ULONGLONG now = GetTickCount64();
        if (!s_audioSourceBypassLogMs || now - s_audioSourceBypassLogMs >= 3000)
        {
            s_audioSourceBypassLogMs = now;
            char ab[240];
            sprintf_s(ab, sizeof(ab),
                "[WASDCombat] dc_audio_source_range_bypass event=%s native_range=%d",
                name ? name : "<null>", (int)range);
            VerbLog(ab);
        }
        range = SOUNDRANGE_ALWAYS;
    }
    return s_characterAudioEventOrig(self, name, range);
}

static int audioListenerSubmit_hook(const DcAudioListenerTransform* transform,
                                    unsigned int listenerIndex)
{
    bool anchor = transform && listenerIndex == 0
               && s_mode == MODE_FREE_MOVE && s_freeMoveAnchor
               && s_freeMoveAnchor->movement && s_gwFrame
               && !s_dcShutdownInProgress && !s_loadGuardActive
               && dcInUpdateList(s_gwFrame, s_freeMoveAnchor);
    if (!anchor)
    {
        if (s_audioListenerAnchorLogged)
        {
            s_audioListenerAnchorLogged = false;
            VerbLog("[WASDCombat] dc_audio_listener_anchor_off");
        }
        return s_audioListenerSubmitOrig(transform, listenerIndex);
    }

    DcAudioListenerTransform local = *transform;
    // Use the exact coordinate space Kenshi already submits for this
    // character's Wwise emitter.  v87 derived an Ogre torso point; that hook
    // ran, but remote-camera tests still had no audible character sound.  The
    // emitter position avoids guessing axis order, origin and unit scaling.
    const AkVector& emitter = s_freeMoveAnchor->audioData.Position;
    bool emitterValid = emitter.X == emitter.X && emitter.Y == emitter.Y
                     && emitter.Z == emitter.Z
                     && fabsf(emitter.X) < 10000000.0f
                     && fabsf(emitter.Y) < 10000000.0f
                     && fabsf(emitter.Z) < 10000000.0f
                     && (fabsf(emitter.X) + fabsf(emitter.Y) + fabsf(emitter.Z)) > 0.001f;
    Ogre::Vector3 body = dcBodyRoot(s_freeMoveAnchor);
    local.position[0] = emitterValid ? emitter.X : body.x;
    local.position[1] = emitterValid ? emitter.Y : body.y;
    local.position[2] = emitterValid ? emitter.Z : body.z;

    if (!s_audioListenerAnchorLogged)
    {
        s_audioListenerAnchorLogged = true;
        char ab[320];
        sprintf_s(ab, sizeof(ab),
            "[WASDCombat] dc_audio_listener_anchor_on source=%s listener=(%.1f,%.1f,%.1f) camera=(%.1f,%.1f,%.1f) body=(%.1f,%.1f,%.1f)",
            emitterValid ? "character_audio" : "body_fallback",
            local.position[0], local.position[1], local.position[2],
            transform->position[0], transform->position[1], transform->position[2],
            body.x, body.y, body.z);
        VerbLog(ab);
    }
    return s_audioListenerSubmitOrig(&local, listenerIndex);
}

// Resolve the listener submit from the current executable's symbol-resolved
// CameraClass::updateAudio body.  This avoids a naked game RVA: the short call-
// site signature is byte-checked, and the target prologue is checked before
// MinHook sees it.  An updated executable safely disables only this feature.
static intptr_t dcResolveAudioListenerSubmit()
{
    unsigned char* updateAudio = (unsigned char*)KenshiLib::GetRealAddress(
        DcRobResult<DcCameraUpdateAudioTag>::ptr);
    if (!updateAudio) return 0;

    const unsigned char prefix[] = { 0x33, 0xD2, 0x48, 0x8D, 0x4D, 0x38, 0xE8 };
    const unsigned char suffix[] = { 0x45, 0x33, 0xC0 };
    for (size_t i = 0; i + 14 <= 0x180; ++i)
    {
        if (memcmp(updateAudio + i, prefix, sizeof(prefix)) != 0
            || memcmp(updateAudio + i + 11, suffix, sizeof(suffix)) != 0)
            continue;
        int relative = 0;
        memcpy(&relative, updateAudio + i + 7, sizeof(relative));
        unsigned char* callNext = updateAudio + i + 11;
        unsigned char* target = callNext + relative;
        const unsigned char prologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };
        if (memcmp(target, prologue, sizeof(prologue)) == 0)
            return (intptr_t)target;
    }
    return 0;
}

static void (*s_setCombatStateOrig)(CombatClass* cc, swordStateEnum state);
static void setCombatState_hook(CombatClass* cc, swordStateEnum state)
{
    swordStateEnum before = cc ? cc->getCombatState() : COMBAT_FINISHED;
    if (state == CHOP_WEAPON && cc && s_manualAttackOn && !s_attackDirectInitialising && s_mode == MODE_FREE_MOVE
        && !s_dcShutdownInProgress && !s_loadGuardActive && cc->me == s_freeMoveAnchor)
    {
        VerbLog("[WASDCombat] manual_ai_attack_state_denied");
        state = WAIT_MENACINGLY;
    }
    s_setCombatStateOrig(cc, state);
    if (cc && cc->me == s_freeMoveAnchor && s_mode == MODE_FREE_MOVE)
    {
        swordStateEnum after = cc->getCombatState();
        bool inReaction = after == STUMBLE || after == REACTION_BLOCK;
        bool wasReaction = before == STUMBLE || before == REACTION_BLOCK;
        if (inReaction)
        {
            s_hitReactionPending = false;
            InterlockedExchange(&s_hitReactionInputLock, 1);
        }
        else if (wasReaction)
        {
            s_hitReactionPending = false;
            InterlockedExchange(&s_hitReactionInputLock, 0);
            VerbLog("[WASDCombat] hit_native_reaction_state_end");
        }
    }
}

// CharStats::chooseBlock - the AI's defence choice.  In manual-attack mode the
// controlled character gets NO autonomous defence: the chooser returns nothing
// (exactly what a failed defence roll returns) and the hit lands.
// Exceptions: a G block the player asked for (accepted press, within its hold
// or perfect window), and a stagger in progress (the stumble reaction stays
// vanilla). Ironwind's manual guard pose does not use this chooser; automatic
// guard deliberately does, and may therefore receive a native block or dodge.
static bool focusTargetValid(Character* t);   // fwd decls (lock-on helpers below)
static bool focusTargetLoaded(Character* t);
static bool manualVAttackTargetLive(Character* t);
static bool focusLockLive();
static CombatTechniqueData* (*s_chooseAttackOrig)(CharStats*, float, float, CombatTechniqueData*, bool);
static CombatTechniqueData* chooseAttack_hook(CharStats* st, float range, float reach,
                                               CombatTechniqueData* previous, bool stationary)
{
    if (!s_directionalCombat || !s_manualAttackOn || !s_attackDirectInitialising || !st
        || st->me != s_freeMoveAnchor || s_attackDesiredForSwing == CUT_DEFAULT)
        return s_chooseAttackOrig(st, range, reach, previous, stationary);

    // Free-control attacks are action-first: the player's direction chooses a
    // native move even when no target is in reach. The target is only a carrier
    // that lets AttackState build its normal impact/defence context. Collect the
    // current weapon's same-direction techniques from the native pool first,
    // then supplement them by sampling the character's chooser at neutral
    // reference ranges. Pick randomly; actual contact remains entirely native
    // at the impact frame.
    CutDirection wanted = (CutDirection)s_attackDesiredForSwing;
    const float sampleRange[4] = { 0.0f, 5.0f, 12.0f, 25.0f };
    CombatTechniqueData* choices[64];
    int choiceCount = 0;
    int poolChoiceCount = 0;
    int wt = (int)st->currentWeaponType;
    for (int i = 0; i < s_dcAttackCount && choiceCount < 64; ++i)
    {
        CombatTechniqueData* candidate = s_dcAttack[i];
        if (!candidate || !(wt >= 0 && wt < 0x16 && candidate->skillTypes[wt])
            || candidate->isBlock || candidate->isDodge
            || candidate->animation.empty() || candidate->numImpactPoints() <= 0
            || candidate->impactPoint(0)->direction != wanted) continue;
        bool duplicate = false;
        for (int j = 0; j < choiceCount; ++j)
            if (choices[j] == candidate) { duplicate = true; break; }
        if (!duplicate) choices[choiceCount++] = candidate;
    }
    poolChoiceCount = choiceCount;
    for (int i = 0; i < 32; ++i)
    {
        CombatTechniqueData* candidate = s_chooseAttackOrig(
            st, sampleRange[i / 8], reach, previous, stationary);
        if (!candidate || candidate->isBlock || candidate->isDodge
            || candidate->animation.empty() || candidate->numImpactPoints() <= 0
            || candidate->impactPoint(0)->direction != wanted) continue;
        bool duplicate = false;
        for (int j = 0; j < choiceCount; ++j)
            if (choices[j] == candidate) { duplicate = true; break; }
        if (!duplicate && choiceCount < 64) choices[choiceCount++] = candidate;
    }
    if (choiceCount > 0)
    {
        int start = (int)(UtilityT::random() * choiceCount);
        if (start >= choiceCount) start = choiceCount - 1;
        CombatTechniqueData* picked = choices[start];
        if (choiceCount > 1 && picked == previous)
        {
            for (int i = 1; i < choiceCount; ++i)
            {
                CombatTechniqueData* alternate = choices[(start + i) % choiceCount];
                if (alternate != previous) { picked = alternate; break; }
            }
        }
        int slot = combatDirectionSlot(wanted);
        if (slot >= 0) s_dirMoveAvailable[slot] = 1;
        char b[256];
        sprintf_s(b, sizeof(b),
            "[Ironwind] attack_direction_native_random dir=%d choices=%d pool=%d sampled=%d actual_range=%.2f reach=%.2f unarmed=%d anim=%s",
            (int)wanted, choiceCount, poolChoiceCount, choiceCount - poolChoiceCount,
            range, reach, st->currentWeaponType == SKILL_UNARMED ? 1 : 0,
            picked->animation.c_str());
        VerbLog(b);
        return picked;
    }
    int slot = combatDirectionSlot(wanted);
    if (slot >= 0) s_dirMoveAvailable[slot] = 0;
    char b[176];
    sprintf_s(b, sizeof(b),
        "[Ironwind] attack_direction_unavailable dir=%d actual_range=%.2f reach=%.2f unarmed=%d",
        (int)wanted, range, reach,
        st->currentWeaponType == SKILL_UNARMED ? 1 : 0);
    VerbLog(b);
    // The weapon/skill combination genuinely has no same-direction native move.
    // Never substitute another direction merely to force an animation.
    return nullptr;
}
static CombatTechniqueData* (*s_chooseBlockOrig)(CharStats* st, CutDirection dir, float opponentAttackSkill,
                                                 CutOrigination from, Character* opponent);
static ULONGLONG s_manualBlockDeniedLogMs = 0;
static CombatTechniqueData* chooseBlock_hook(CharStats* st, CutDirection dir, float opponentAttackSkill,
                                             CutOrigination from, Character* opponent)
{
    ULONGLONG nowC = GetTickCount64();
    bool liveHeldRequest = (s_directionalCombat && s_mouseBlockHeld && s_blockHeld)
                        || (s_blockHeld && (nowC - s_blockHoldStartMs) <= (ULONGLONG)s_blockHoldMs);
    bool blockWanted = s_blockAcceptedPressMs != 0 && s_blockPressMs == s_blockAcceptedPressMs
                    && s_blockHoldStartMs != 0
                    && liveHeldRequest;
    // RMB is the permission boundary in both modes.  Manual mode validates the
    // player's direction later; automatic mode forwards the original chooser
    // only while RMB is held, so Kenshi chooses the defence direction without
    // gaining permission to defend (or take over combat) on its own.
    // (Log 2026-09-21: 293 denied
    // blocks = 293 aborted block entries - a twitch and a stagger each.)
    if (s_manualAttackOn && !blockWanted && s_mode == MODE_FREE_MOVE
        && st && st->me && st->me == s_freeMoveAnchor && !s_dcShutdownInProgress && !s_loadGuardActive)
    {
        CombatClass* cc = st->me->getCombatClass();
        bool stumbling = cc && cc->combatModeActive && cc->getCombatState() == STUMBLE;
        if (!stumbling)
        {
            if (nowC - s_manualBlockDeniedLogMs >= 1000)
            {
                s_manualBlockDeniedLogMs = nowC;
                VerbLog("[WASDCombat] manual_ai_block_denied");
            }
            return nullptr;
        }
    }
    CombatTechniqueData* picked = s_chooseBlockOrig(st, dir, opponentAttackSkill, from, opponent);
    if (s_manualAttackOn && s_settingAiBlocks && blockWanted
        && st && st->me && st->me == s_freeMoveAnchor)
    {
        char b[256];
        sprintf_s(b, sizeof(b),
            "[Ironwind] ai_block_choice incoming_dir=%d chosen=%d dodge=%d anim=%s",
            (int)dir, picked ? 1 : 0, picked && picked->isDodge ? 1 : 0,
            picked ? picked->animation.c_str() : "none");
        VerbLog(b);
    }
    return picked;
}

// AIM FOCUS feeds the AI's "who": CombatClassAI::chooseAttackTarget (facing,
// approach, swings) answers the aim focus for the controlled character while
// one is live; threats (blocks, dodges, stumble reactions) stay vanilla so an
// attack from anyone still gets its reaction.
static bool focusLockLive()
{
    return s_lockTarget && s_mode == MODE_FREE_MOVE && !s_dcShutdownInProgress && !s_loadGuardActive
        && focusTargetValid(s_lockTarget);
}
static Character* (*s_chooseAttackTargetOrig)(CombatClass* cc);
static Character* chooseAttackTarget_hook(CombatClass* cc)
{
    if (cc && cc->me == s_freeMoveAnchor && s_attackIntentTarget
        && s_attackIntentPressMs == s_attackAcceptedPressMs
        && (s_attackActive || manualAttackCommitLive())
        && focusTargetLoaded(s_attackIntentTarget)) return s_attackIntentTarget;
    if (cc && cc->me == s_freeMoveAnchor && focusLockLive()) return s_lockTarget;
    return s_chooseAttackTargetOrig(cc);
}

static bool manualAttackCommitLive()
{
    return s_attackCommitUntilMs != 0 && GetTickCount64() < s_attackCommitUntilMs;
}

static bool manualAttackStartDirect(CombatClass* cc, Character* foe, ULONGLONG press);
static void manualFreeActionTick();

// A single release may be buffered while a swing or its configured recovery
// owns the action layer.  The queue never survives a stagger/load and expires
// quickly, so old input cannot fire after the player has changed context.
static void manualAttackQueueTick()
{
    ULONGLONG queued = s_attackQueuedAtMs;
    if (!queued) return;
    ULONGLONG now = GetTickCount64();
    if (now - queued > ATTACK_QUEUE_MS)
    {
        manualAttackClearQueuedInput(true);
        VerbLog("[Ironwind] attack_queue_expired");
        return;
    }
    if (s_staggerStartMs != 0 || s_attackActive || s_freeAttackTech
        || now < s_attackRecoveryUntilMs)
        return;
    int direction = s_attackQueuedDirection;
    Character* queuedTarget = s_attackQueuedTarget;
    manualAttackClearQueuedInput(false);
    if (direction == CUT_DEFAULT) return;
    ULONGLONG syntheticPress = now;
    if (syntheticPress <= s_attackSeenPressMs) syntheticPress = s_attackSeenPressMs + 1;
    s_attackDesired = direction;
    s_attackPressMs = syntheticPress;
    s_attackIntentTarget = focusTargetLoaded(queuedTarget) && manualVAttackTargetLive(queuedTarget)
        ? queuedTarget : nullptr;
    s_attackIntentPressMs = s_attackIntentTarget ? syntheticPress : 0;
    char b[160]; sprintf_s(b, sizeof(b),
        "[Ironwind] attack_queue_commit dir=%d age=%llu captured_target=%d",
        direction, now - queued, s_attackIntentTarget ? 1 : 0);
    VerbLog(b);
}

// Main thread, once per frame: a fresh Attack press becomes a commit window.
static void manualAttackPressTick()
{
    ULONGLONG press = s_attackPressMs;
    if (press == 0 || press == s_attackSeenPressMs) return;
    s_attackSeenPressMs = press;
    ULONGLONG now = GetTickCount64();
    Character* pressTarget = nullptr;
    if (s_attackIntentPressMs == press && focusTargetLoaded(s_attackIntentTarget)
        && manualVAttackTargetLive(s_attackIntentTarget))
        pressTarget = s_attackIntentTarget;              // queued commit already associated it
    else if (focusTargetLoaded(s_attackAimCapturedTarget)
        && manualVAttackTargetLive(s_attackAimCapturedTarget))
        pressTarget = s_attackAimCapturedTarget;         // live reticle at LMB-down
    const char* why = nullptr;
    if (!s_settingManualBlock || s_mode != MODE_FREE_MOVE || !s_freeMoveAnchor
        || s_dcShutdownInProgress || s_loadGuardActive)          why = "not_dc";
    else if (!s_manualAttackOn)                                   why = "manual_off";  // E is a manual-attack control
    else if (manualHitReactionLive(s_freeMoveAnchor))             why = "native_reaction";
    else if (s_staggerStartMs != 0)                               why = "staggered";   // dropped, never buffered
    else if (s_attackActive || s_freeAttackTech || now < s_attackRecoveryUntilMs)
    {
        bool repeat = s_attackAcceptedPressMs != 0 && press >= s_attackAcceptedPressMs
                   && press - s_attackAcceptedPressMs < (ULONGLONG)s_inputRepeatGuardMs;
        if (repeat) why = "repeat";
        else
        {
            int direction = s_attackDesired;
            if (direction == CUT_DEFAULT) direction = s_lastAttackDirection;
            s_attackQueuedDirection = direction;
            s_attackQueuedAtMs = press;
            s_attackQueuedTarget = pressTarget;
            manualAttackClearCapturedTarget();
            char b[144]; sprintf_s(b, sizeof(b),
                "[Ironwind] attack_queued dir=%d captured_target=%d",
                direction, s_attackQueuedTarget ? 1 : 0);
            VerbLog(b);
            return;
        }
    }
    else if (s_attackAcceptedPressMs != 0 && press >= s_attackAcceptedPressMs
             && press - s_attackAcceptedPressMs < (ULONGLONG)s_inputRepeatGuardMs) why = "repeat";  // accidental double-tap
    else if (s_freeAttackTech)                                  why = "free_swing_in_flight";
    if (why)
    {
        CombatClass* ccTrace = (s_freeMoveAnchor && !s_loadGuardActive && !s_dcShutdownInProgress)
                             ? s_freeMoveAnchor->getCombatClass() : nullptr;
        char b[160];
        sprintf_s(b, sizeof(b), "[WASDCombat] attack_press_ignored reason=%s manual=%d cc_active=%d mode=%d",
                  why, s_manualAttackOn ? 1 : 0, ccTrace && ccTrace->combatModeActive ? 1 : 0, (int)s_mode);
        VerbLog(b);
        manualAttackClearCapturedTarget();
        return;
    }
    s_attackIntentTarget = pressTarget;
    s_attackIntentPressMs = pressTarget ? press : 0;
    manualAttackClearCapturedTarget();
    s_attackCommitUntilMs   = press + (ULONGLONG)s_attackWindowMs;
    s_attackAcceptedPressMs = press;
    char b[128];
    sprintf_s(b, sizeof(b), "[WASDCombat] attack_commit window=%.0f moving=%d captured_target=%d",
              s_attackWindowMs, (s_wHeld || s_aHeld || s_sHeld || s_dHeld) ? 1 : 0,
              pressTarget ? 1 : 0);
    VerbLog(b);
}

// Once per frame from mainLoop: hold/release edges drive the vanilla order.
static void manualBlockTick()
{
    // WORLD-LIVE GATE (crash 2026-09-21, dump: AV in manualStaggered +0x20 =
    // the virtual getProneState on a FREED anchor, 0.4 s into a quickload).
    // This tick runs at the top of mainLoop, BEFORE its safety gate, so it must
    // never touch the anchor unless the world is live: on a save-load the game
    // nulls ou->player first and frees the characters a few frames later, and
    // the flag may lag behind - both signals are required here.  Nothing in
    // this tick needs to run during a load (the gate + clearAllState reset the
    // manual-combat state) - drop out whole.
    if (!ou || !ou->player || ou->isLoadingFromASaveGame() || s_dcPtrLossActive) return;   // build 74: no anchor deref while ANY load flag is up
    manualAttackTick();
    manualAimTick();
    manualAttackAimCaptureTick();
    manualStaggerTick();
    manualAttackQueueTick();
    manualAttackPressTick();
    // AiBlocksInManual changes only who chooses the defence direction. RMB is
    // still the player's hold-to-guard request in both modes. A mode transition
    // retires the old mode's token once so stale manual directions/perfect
    // windows cannot leak into the automatic chooser; the player then re-presses
    // RMB under the new mode.
    static int lastAiBlockMode = -1;
    int aiBlockMode = s_settingAiBlocks ? 1 : 0;
    if (aiBlockMode != lastAiBlockMode)
    {
        if (lastAiBlockMode != -1)
        {
            if (s_blockOrderApplied) manualBlockOrder(false);
            s_blockHeld = false;
            s_mouseBlockHeld = false;
            s_blockGestureSeen = false;
            s_blockHeldPrev = false;
            s_blockPressMs = 0;
            s_blockSeenPressMs = 0;
            s_blockAcceptedPressMs = 0;
            s_blockInjectedPressMs = 0;
            s_blockHoldStartMs = 0;
            s_blockReleaseMs = 0;
            s_blockCooldownUntilMs = 0;
        }
        lastAiBlockMode = aiBlockMode;
        VerbLog(aiBlockMode
            ? "[Ironwind] block_mode mode=automatic_ai"
            : "[Ironwind] block_mode mode=manual_directional");
    }
    // Directional and automatic guards live exactly while RMB is held.  The
    // old non-directional keyboard fallback alone retains its hold budget and
    // cooldown so existing INI-based DIY layouts keep their behaviour.
    ULONGLONG nowT = GetTickCount64();
    ULONGLONG pressT = s_blockPressMs;
    if (pressT != 0 && pressT != s_blockSeenPressMs)
    {
        s_blockSeenPressMs = pressT;
        // RESUME (input forgiveness): the block is still live (release + press
        // fell inside one frame) or was let go within the repeat guard, and the
        // hold budget is unspent -> the SAME block continues: no cooldown, the
        // budget still runs from the first press, and the
        // pose is not re-injected.  A fumbled double-tap no longer drops the
        // guard for BlockCooldownMs (the "got hit while I was blocking" case).
        bool budgetLeft = s_blockHoldStartMs != 0 && pressT >= s_blockHoldStartMs
                       && (pressT - s_blockHoldStartMs) < (ULONGLONG)s_blockHoldMs;
        bool resume = !s_directionalCombat && budgetLeft
                   && (s_blockHeldPrev
                       || (s_blockReleaseMs != 0 && pressT >= s_blockReleaseMs
                           && (pressT - s_blockReleaseMs) <= (ULONGLONG)s_inputRepeatGuardMs));
        if (!s_manualAttackOn)
            VerbLog("[WASDCombat] block_press_ignored reason=manual_off");  // G is a manual-combat control (user 2026-09-29)
        else if (manualHitReactionLive(s_freeMoveAnchor))
            VerbLog("[WASDCombat] block_press_ignored reason=native_reaction");
        else if (s_staggerStartMs != 0)
            VerbLog("[WASDCombat] block_press_ignored reason=staggered");   // dropped, never buffered
        else if (s_settingAiBlocks)
        {
            // Automatic guard has no manual direction or cooldown. Its
            // lifetime is exactly this RMB hold; chooseBlock_hook
            // delegates each incoming defence choice to Kenshi.
            s_blockAcceptedPressMs = pressT;
            s_blockHoldStartMs = pressT;
            s_blockInjectedPressMs = 0;
            s_blockCooldownUntilMs = 0;
            VerbLog("[Ironwind] block_press_accepted mode=automatic_ai");
        }
        else if (resume)
        {
            s_blockAcceptedPressMs = pressT;
            s_blockInjectedPressMs = pressT;    // keep the pose of the first press
            VerbLog("[WASDCombat] block_press_resumed");
        }
        else if (nowT < s_blockCooldownUntilMs)
            VerbLog("[WASDCombat] block_press_ignored reason=cooldown");
        else
        {
            s_blockAcceptedPressMs = pressT;
            s_blockHoldStartMs     = pressT;
        }
        s_blockReleaseMs = 0;
    }
    bool pressLive = s_blockAcceptedPressMs != 0 && s_blockPressMs == s_blockAcceptedPressMs
                   && s_blockHoldStartMs != 0
                   && ((s_directionalCombat && s_mouseBlockHeld)
                       || (nowT - s_blockHoldStartMs) <= (ULONGLONG)s_blockHoldMs);
    bool held = s_settingManualBlock && s_manualAttackOn && s_blockHeld && pressLive && s_mode == MODE_FREE_MOVE
             && !s_dcShutdownInProgress && !s_loadGuardActive && s_freeMoveAnchor;
    // Staggered: the BLOCK order is not applied (anti-cheese) - it comes on
    // the moment the stagger ends if the key is still down.
    if (held)
    {
        if (manualHitReactionLive(s_freeMoveAnchor)) held = false;
    }
    // Anchor switched (F) while held: restore the old one, re-apply on the new.
    if (held && s_blockOrderApplied && s_blockOrderChar != s_freeMoveAnchor)
    {
        manualBlockOrder(false);
        s_blockHeldPrev = false;
    }
    if (held != s_blockHeldPrev)
    {
        s_blockHeldPrev = held;
        if (held) s_holdPosValid = false;  // capture the actual guard-press location
        // Manual mode exposes its selected direction and the vanilla defensive
        // order immediately. Automatic mode is permission only: pressing RMB
        // must not force any idle/standing pose. It remains stationary via the
        // movement gate, then enters native BLOCK only when a real incoming
        // swing produces one original chooseBlock result.
        if (!s_settingAiBlocks)
            manualBlockOrder(held);
        else
        {
            if (!held && s_blockOrderApplied) manualBlockOrder(false);
            VerbLog(held ? "[Ironwind] ai_guard_request armed=1 pose=unchanged"
                         : "[Ironwind] ai_guard_request armed=0 pose=unchanged");
        }
        if (!held)
        {
            s_blockCooldownUntilMs = (s_settingAiBlocks || s_directionalCombat) ? 0
                                   : nowT + (ULONGLONG)s_blockCooldownMs;   // released or expired
            s_blockReleaseMs       = nowT;                                   // a re-press inside the repeat guard resumes
        }
    }
}

// Pick a mod-owned block copy for the pose.  If the foe is mid-swing, match its
// current impact direction converted into our frame (what the vanilla chooser
// matches a block against); otherwise any block.
static CombatTechniqueData* manualBlockPick(Character* me, Character* foe, int* wantOut)
{
    if (s_dcBlockCount == 0) return nullptr;
    int want = -1;
    CombatClass* fc = foe ? foe->getCombatClass() : nullptr;
    CombatTechniqueData* ft = fc ? fc->currentTechnique : nullptr;
    if (fc && fc->combatModeActive && ft && !ft->isBlock && ft->numImpactPoints() > 0)
    {
        int n = ft->numImpactPoints();
        int sec = fc->currentComboSection; if (sec < 0) sec = 0; if (sec >= n) sec = n - 1;
        CombatTechniqueData::ImpactPoint* ip = ft->impactPoint(sec);
        if (ip) want = (int)me->convertCutDirection(ip->direction, foe);
    }
    if (wantOut) *wantOut = want;
    if (want >= 0)
        for (int i = 0; i < s_dcBlockCount; ++i)
        {
            CombatTechniqueData* c = s_dcBlock[i];
            if (c->numImpactPoints() > 0 && (int)c->impactPoint(0)->direction == want) return c;
        }
    int idx = (int)(UtilityT::random() * (float)s_dcBlockCount); if (idx >= s_dcBlockCount) idx = s_dcBlockCount - 1;
    return s_dcBlock[idx];
}

// --- lock-on + directional aim ------------------------------------------------------
static bool focusTargetLoaded(Character* t)
{
    if (!t || !s_gwFrame) return false;
    auto& chars = s_gwFrame->getCharacterUpdateList();
    for (auto it = chars.begin(); it != chars.end(); ++it) if (*it == t) return true;
    return false;
}

// Kenshi reports isUnconcious()==true for more than a true knockout. In
// particular PS_CRIPPLED (broken-leg crawl) and PS_PLAYING_DEAD characters can
// still be deliberately attacked by the original combat system. Keep them in
// target selection, while a real PS_KO or dead character remains ineligible.
// This chooses only the native AttackState carrier; it never grants contact or
// damage, which remain owned by Kenshi's attack zone and verdict path.
static bool manualVAttackTargetLive(Character* t)
{
    if (!t || t->isDead()) return false;
    ProneState prone = t->getProneState();
    if (prone == PS_KO) return false;
    if (prone == PS_CRIPPLED || prone == PS_PLAYING_DEAD) return true;
    return !t->isUnconcious();
}

static bool focusTargetValid(Character* t)
{
    return focusTargetLoaded(t) && s_freeMoveAnchor && manualVAttackTargetLive(t)
        && t->isEnemy(s_freeMoveAnchor, true);
}

// The view yaw the player is aiming with (the mod's own look state - the
// camera object's orientation is not reliable frame to frame).  false = no
// aimed view (plain DC camera).
static bool manualAimYaw(float& yawOut)
{
    if (s_firstPersonActive) { yawOut = s_fpYawSm; return true; }
    if (s_otsCamActive)      { yawOut = s_otsYaw;  return true; }
    return false;
}
static bool focusProject(Ogre::Camera* cam, Ogre::Vector3 gamePos, float pullToCam, int& sx, int& sy);
static bool focusBonePos(Character* t, const char* bone, float fallbackUp, Ogre::Vector3& out);

// Directional aim: the live hostile nearest to the view ray within reach and
// inside the aim cone (angle first, distance second).  nullptr = nobody aimed at.
static Character* manualAimPick(Character* me, float range)
{
    float yaw;
    if (!me || !me->movement || !s_gwFrame || !manualAimYaw(yaw)) return nullptr;
    const Ogre::Vector3 mp = me->movement->pos;
    const float fx = -sinf(yaw), fz = -cosf(yaw);           // view forward (fwd = (-sin, 0, -cos))
    const float cosCone = cosf(s_aimConeDeg * 0.0174533f);
    const float maxR2 = range * range;
    Character* best = nullptr; float bestScore = 1e9f;
    auto& chars = s_gwFrame->getCharacterUpdateList();
    for (auto it = chars.begin(); it != chars.end(); ++it)
    {
        Character* c = *it;
        if (!c || c == me || !c->movement) continue;
        float dx = c->movement->pos.x - mp.x, dz = c->movement->pos.z - mp.z;
        float d2 = dx * dx + dz * dz;
        if (d2 < 1e-4f || d2 >= maxR2) continue;
        float d = sqrtf(d2), cosA = (dx * fx + dz * fz) / d;
        if (cosA < cosCone) continue;
        if (!manualVAttackTargetLive(c) || !c->isEnemy(me, true)) continue;
        float score = (1.0f - cosA) * 100.0f + d * 0.05f;   // angle first, distance second
        if (score < bestScore) { bestScore = score; best = c; }
    }
    return best;
}

static bool manualVPlayerTeamMember(Character* target)
{
    if (!target) return false;
    if (target->isPlayerCharacter()) return true;
    // Recruited mod characters can report false from isPlayerCharacter().
    // PlayerInterface's roster is the authoritative backstop for squad safety.
    if (!ou || !ou->player) return false;
    const lektor<Character*>& pcs = ou->player->playerCharacters;
    for (auto it = pcs.begin(); it != pcs.end(); ++it)
        if (*it == target) return true;
    return false;
}

static bool manualVTargetAllowed(Character* me, Character* target)
{
    // Faction allies outside the player's squad remain deliberate attack
    // targets when the reticle covers them; only the player's team is safe.
    return me && target && target != me && !manualVPlayerTeamMember(target);
}

static bool manualUnarmedCombat(CharStats* stats)
{
    return stats && stats->currentWeaponType == SKILL_UNARMED;
}

// Use the exact world-space reach calculated by Kenshi's combat class.  The raw
// weapon-data length returned by getCurrentWeaponLength() is not in movement
// world units (v52 selected no target at ordinary melee spacing), while the
// longestReachingAttack member is ABI-stale (v51 climbed from 2269 to 4519).
// The upper bound is only a corruption guard, not a gameplay reach clamp.
// Kenshi intentionally reports zero here while fighting unarmed: martial-arts
// techniques carry their own native range data and the original chooser still
// accepts that zero.  It is therefore a valid chooser argument, not a broken
// weapon accessor and never a reason to disable the unarmed move set.
static float manualOriginalMeleeReach(Character* me)
{
    CharStats* stats = me ? me->getStats() : nullptr;
    CombatClass* cc = me ? me->getCombatClass() : nullptr;
    if (!stats || !cc) return 0.0f;
    float reach = (cc->*DcRobResult<DcWeaponReachTag>::ptr)();
    if (reach == 0.0f && manualUnarmedCombat(stats)) return 0.0f;
    if (!(reach > 0.0f && reach <= 250.0f))
    {
        static ULONGLONG lastInvalidLogMs = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastInvalidLogMs >= 1000)
        {
            lastInvalidLogMs = now;
            char b[176]; sprintf_s(b, sizeof(b),
                "[Ironwind] weapon_reach_invalid combat=%.2f weapon_length=%.2f stale_field=%.2f weapon=%d",
                reach, stats->getCurrentWeaponLength(), stats->longestReachingAttack,
                (int)stats->currentWeaponType);
            VerbLog(b);
        }
        return 0.0f;
    }
    return reach;
}

// A positive reach is required for equipped melee weapons.  Empty-hand combat
// is the one native exception: pass Kenshi's own zero through to chooseAttack
// so martial-arts skill, injuries, encumbrance and technique requirements stay
// authoritative instead of inventing a weapon-like range in the plugin.
static bool manualAttackChooserReach(Character* me, float& reach)
{
    CharStats* stats = me ? me->getStats() : nullptr;
    reach = manualOriginalMeleeReach(me);
    return reach > 0.0f || manualUnarmedCombat(stats);
}

// The long-lived aim focus is useful for name/health UI and deliberate RMB
// locks, but in a crowd it can remain on a centred enemy 30-60 units away
// while another hostile is directly in front of the blade.  For a fresh LMB
// only, find the distance-first hostile inside the same view cone.  This is
// target ownership for native AttackState initialisation, never a contact or
// damage test; the original attack zone remains authoritative.
static Character* manualMeleeForwardHostilePick(Character* me, float range,
                                                 float coneDegrees,
                                                 float* distanceOut)
{
    if (distanceOut) *distanceOut = -1.0f;
    float yaw;
    if (!me || !me->movement || !s_gwFrame || range <= 0.0f
        || !manualAimYaw(yaw)) return nullptr;
    const Ogre::Vector3 mp = me->movement->pos;
    const float fx = -sinf(yaw), fz = -cosf(yaw);
    const float cosCone = cosf(coneDegrees * 0.0174533f);
    const float maxR2 = range * range;
    Character* best = nullptr;
    float bestDistance = -1.0f, bestScore = 1e9f;
    auto& chars = s_gwFrame->getCharacterUpdateList();
    for (auto it = chars.begin(); it != chars.end(); ++it)
    {
        Character* c = *it;
        if (!c || c == me || !c->movement || !manualVAttackTargetLive(c)
            || !c->isEnemy(me, true)) continue;
        float dx = c->movement->pos.x - mp.x, dz = c->movement->pos.z - mp.z;
        float d2 = dx * dx + dz * dz;
        if (d2 < 1e-4f || d2 >= maxR2) continue;
        float d = sqrtf(d2);
        float cosA = (dx * fx + dz * fz) / d;
        if (cosA < cosCone) continue;
        // Distance leads, with enough centre bias that two similarly close
        // enemies still follow the crosshair instead of flickering by range.
        float score = d + (1.0f - cosA) * range;
        if (score < bestScore)
        {
            bestScore = score;
            bestDistance = d;
            best = c;
        }
    }
    if (distanceOut) *distanceOut = bestDistance;
    return best;
}

// A no-focus LMB must not decide whether the swing can ever deal damage.  If
// the directional search found nobody, use the nearest hostile only as the
// native AttackState's execution carrier.  This helper is deliberately
// direction-blind: it neither turns the player nor authorises a hit.  Kenshi's
// original attack zone still decides which bodies the weapon actually reaches,
// including bodies other than this carrier.
static Character* manualNearestHostileCarrier(Character* me, float range,
                                               float* distanceOut)
{
    if (distanceOut) *distanceOut = -1.0f;
    if (!me || !me->movement || !s_gwFrame || range <= 0.0f) return nullptr;
    const Ogre::Vector3 mp = me->movement->pos;
    const float maxR2 = range * range;
    Character* best = nullptr;
    float best2 = maxR2;
    auto& chars = s_gwFrame->getCharacterUpdateList();
    for (auto it = chars.begin(); it != chars.end(); ++it)
    {
        Character* c = *it;
        if (!c || c == me || !c->movement || !manualVAttackTargetLive(c)
            || !c->isEnemy(me, true)
            || !manualVTargetAllowed(me, c)) continue;
        float dx = c->movement->pos.x - mp.x, dz = c->movement->pos.z - mp.z;
        float d2 = dx * dx + dz * dz;
        if (d2 < 1e-4f || d2 >= best2) continue;
        best2 = d2;
        best = c;
    }
    if (best && distanceOut) *distanceOut = sqrtf(best2);
    return best;
}

// Intent is only aim/focus selection: the native combat state machine owns
// range, attack-zone, contact, blocking and damage for both hostile focused and
// neutral unprovoked orders.  Selection may use the weapon data length as a
// broad "who did the player mean" radius; it never grants damage.  V mode never
// targets the squad.
static Character* manualAttackIntentPick(Character* me)
{
    float yaw;
    if (!me || !me->movement || !s_gwFrame || !manualAimYaw(yaw)) return nullptr;
    Character* aimedHostile = (s_lockTarget && focusTargetValid(s_lockTarget))
        ? s_lockTarget : manualAimPick(me, s_aimRange);
    const Ogre::Vector3 mp = me->movement->pos;
    float aimedHostileDistance = aimedHostile && aimedHostile->movement
        ? mp.distance(aimedHostile->movement->pos) : -1.0f;
    float forwardHostileDistance = -1.0f;
    Character* forwardHostile = manualMeleeForwardHostilePick(
        me, s_aimRange, s_aimConeDeg, &forwardHostileDistance);
    Character* nativeHostile = aimedHostile;
    // An RMB click is an explicit lock. Automatic sticky focus is only
    // replaced when the forward candidate is materially closer, avoiding
    // target churn between two enemies standing at almost the same range.
    bool hostileRetargeted = false;
    if (!s_aimClicked && forwardHostile
        && (!aimedHostile || aimedHostileDistance < 0.0f
            || (aimedHostileDistance > forwardHostileDistance + 8.0f
                && aimedHostileDistance > forwardHostileDistance * 1.35f)))
    {
        nativeHostile = forwardHostile;
        hostileRetargeted = nativeHostile != aimedHostile;
    }
    if (hostileRetargeted)
    {
        char rb[176]; sprintf_s(rb, sizeof(rb),
            "[Ironwind] attack_hostile_retarget aimed_dist=%.1f forward_dist=%.1f",
            aimedHostileDistance, forwardHostileDistance);
        VerbLog(rb);
    }
    const float reach = manualOriginalMeleeReach(me);
    CharStats* stats = me->getStats();
    const bool unarmed = manualUnarmedCombat(stats);
    float intentReach = reach;
    const float weaponLength = stats ? stats->getCurrentWeaponLength() : 0.0f;
    if (weaponLength > intentReach && weaponLength <= 250.0f)
        intentReach = weaponLength;
    // A broken helper reach disables only the neutral/free path. Hostile target
    // eligibility remains entirely native and must not depend on this accessor.
    // Unarmed combat has no weapon reach by design.  It may capture a neutral
    // only when the reticle actually covers the body, inside the ordinary aim
    // radius.  That supplies an intent carrier; native AttackState still owns
    // martial-arts contact range and cannot be extended by this selection.
    if (intentReach <= 0.0f && !unarmed) return nativeHostile;
    const float selectionReach = intentReach > 0.0f ? intentReach : s_aimRange;
    // Exact reticle coverage identifies WHO the player deliberately attacked;
    // it is not a range verdict. Give that actor to native AttackState anywhere
    // inside the ordinary aim range and let Kenshi decide whether the technique
    // actually reaches. The approximate neutral arc remains weapon-reach bound
    // so an off-centre bystander cannot become a distant execution carrier.
    const float preciseMaxR2 = s_aimRange * s_aimRange;
    const float neutralArcMaxR2 = selectionReach * selectionReach;
    const float fx = -sinf(yaw), fz = -cosf(yaw);
    const float cosNeutralArc = cosf(25.0f * 0.0174533f);
    Ogre::Camera* cam = ou && ou->player && ou->player->camera
        ? ou->player->camera->camera : nullptr;
    MyGUI::IntSize view = MyGUI::RenderManager::getInstance().getViewSize();
    const float aimX = view.width * 0.5f, aimY = view.height * 0.5f;
    Character* precise = nullptr;
    Character* neutralArc = nullptr;
    float preciseScore = 1e9f, neutralArcScore = 1e9f;
    int preciseCandidates = 0, neutralArcCandidates = 0;
    int nearbyCharacters = 0;
    int rawNearby = 0, squadExcluded = 0, factionAlliesNearby = 0;
    float nearestCharacter = -1.0f;
    float nearestNonhostileMiss = 1e9f;
    float nearestNonhostileDx = -1.0f, nearestNonhostileDy = -1.0f;
    auto& chars = s_gwFrame->getCharacterUpdateList();
    for (auto it = chars.begin(); it != chars.end(); ++it)
    {
        Character* c = *it;
        if (!c || c == me || !c->movement || !manualVAttackTargetLive(c)) continue;
        float dx = c->movement->pos.x - mp.x, dz = c->movement->pos.z - mp.z;
        float d2 = dx * dx + dz * dz;
        if (d2 < 120.0f * 120.0f)
        {
            ++rawNearby;
            if (manualVPlayerTeamMember(c)) ++squadExcluded;
            else if (c->isAlly(me, true)) ++factionAlliesNearby;
        }
        if (!manualVTargetAllowed(me, c)) continue;
        if (d2 < 120.0f * 120.0f)
        {
            ++nearbyCharacters;
            float distance = sqrtf(d2);
            if (nearestCharacter < 0.0f || distance < nearestCharacter)
                nearestCharacter = distance;
        }
        if (d2 < 1e-4f) continue;
        float d = sqrtf(d2), cosA = (dx * fx + dz * fz) / d;
        bool enemy = c->isEnemy(me, true);
        // The neutral helper requires the reticle to overlap the projected body,
        // not merely lie near its chest. Hostile contact is never decided here.
        // chest. A height-scaled width also works for large close characters.
        if (!enemy && d2 < preciseMaxR2 && cam && view.width > 0 && view.height > 0)
        {
            Ogre::Vector3 head, chest, hips;
            int hx, hy, cx, cy, px, py, feetX, feetY;
            if (focusBonePos(c, "Bip01 Head", 18.0f, head)
                && focusBonePos(c, "Bip01 Spine1", 11.0f, chest)
                && focusBonePos(c, "Bip01 Pelvis", 5.0f, hips)
                && focusProject(cam, head, 0.0f, hx, hy)
                && focusProject(cam, chest, 0.0f, cx, cy)
                && focusProject(cam, hips, 0.0f, px, py)
                && focusProject(cam, c->movement->pos + Ogre::Vector3(0.0f, 1.0f, 0.0f),
                                0.0f, feetX, feetY))
            {
                // Include the legs and a small screen-space margin.  The
                // previous head-to-pelvis box missed visibly aimed targets
                // by only a few pixels horizontally or below the pelvis.
                int topPx = hy < cy ? hy : cy;
                if (py < topPx) topPx = py;
                if (feetY < topPx) topPx = feetY;
                int bottomPx = hy > cy ? hy : cy;
                if (py > bottomPx) bottomPx = py;
                if (feetY > bottomPx) bottomPx = feetY;
                float top = (float)topPx - 14.0f;
                float bottom = (float)bottomPx + 18.0f;
                float bodyHeight = bottom - top;
                float halfWidth = bodyHeight * 0.16f + 8.0f;
                if (halfWidth < 24.0f) halfWidth = 24.0f;
                if (halfWidth > 65.0f) halfWidth = 65.0f;
                float centerX = (cx + px) * 0.5f;
                if (aimY > (float)py && feetY != py)
                {
                    float t = (aimY - py) / (float)(feetY - py);
                    if (t < 0.0f) t = 0.0f;
                    if (t > 1.0f) t = 1.0f;
                    centerX = px + (feetX - px) * t;
                }
                float missX = fabsf(aimX - centerX) - halfWidth;
                float missY = aimY < top ? top - aimY : (aimY > bottom ? aimY - bottom : 0.0f);
                if (missX < 0.0f) missX = 0.0f;
                if (missX + missY < nearestNonhostileMiss)
                {
                    nearestNonhostileMiss = missX + missY;
                    nearestNonhostileDx = missX;
                    nearestNonhostileDy = missY;
                }
                if (fabsf(aimX - centerX) <= halfWidth && aimY >= top && aimY <= bottom)
                {
                    ++preciseCandidates;
                    float screenScore = fabsf(aimX - centerX) / halfWidth + d * 0.02f;
                    if (screenScore < preciseScore)
                    { preciseScore = screenScore; precise = c; }
                }
            }
        }
        // The approximate neutral arc depends on a verified weapon reach.
        // Empty hands require explicit body coverage to avoid selecting a
        // bystander at an arbitrary plugin-defined martial-arts distance.
        if (!enemy && d2 < neutralArcMaxR2
            && intentReach > 0.0f && cosA >= cosNeutralArc)
        {
            ++neutralArcCandidates;
            // Angle first, then distance. This stays narrow so an off-centre
            // bystander is not selected when shoulder parallax misses the body.
            float score = (1.0f - cosA) * 150.0f + d * 0.1f;
            if (score < neutralArcScore)
            { neutralArcScore = score; neutralArc = c; }
        }
    }
    // An explicitly covered neutral wins. Otherwise combat focus wins before
    // the parallax helper, so a nearby bystander cannot steal an enemy swing.
    Character* best = precise ? precise : (nativeHostile ? nativeHostile : neutralArc);
    char b[432];
    sprintf_s(b, sizeof(b), "[Ironwind] attack_pick mode=%s hostile=%d precise=%d candidates=%d/%d/%d dist=%.1f neutral_miss=%.0f/%.0f nearby=%d raw=%d squad=%d allies=%d nearest=%.1f unarmed=%d",
              precise ? "neutral_reticle" : (nativeHostile ? "native_hostile" : (neutralArc ? "neutral_arc" : "none")),
              best && best->isEnemy(me, true) ? 1 : 0, precise ? 1 : 0,
              nativeHostile ? 1 : 0, preciseCandidates, neutralArcCandidates,
              best && best->movement ? mp.distance(best->movement->pos) : -1.0f,
              nearestNonhostileDx, nearestNonhostileDy, nearbyCharacters,
              rawNearby, squadExcluded, factionAlliesNearby, nearestCharacter,
              unarmed ? 1 : 0);
    VerbLog(b);
    return best;
}

// Capture deliberate intent on the first game-thread frame of LMB-down, before
// the directional mouse stroke also rotates the OTS/FP view.  v56 field data
// repeatedly had a neutral 7-8 units away but no release-time candidate after
// the stroke moved the projected body 70-150 px off the reticle.  The pointer
// is still revalidated on release and at impact; this changes target ownership,
// not weapon reach or the final contact test.
static void manualAttackAimCaptureTick()
{
    if (s_attackAimCancelled)
    {
        manualAttackClearCapturedTarget();
        return;
    }
    ULONGLONG started = s_attackAimStartedMs;
    if (!s_attackAimHeld || !started || started == s_attackAimCapturedStartMs) return;
    s_attackAimCapturedStartMs = started;
    s_attackAimCapturedTarget = manualAttackIntentPick(s_freeMoveAnchor);
    Character* target = s_attackAimCapturedTarget;
    bool neutralOrigin = false;
    if (!target)
    {
        manualAttackClearNeutralIntent();
    }
    else if (target == s_attackNeutralIntentTarget)
    {
        // initialiseAttack can mutate relations even when it returns false.
        // Preserve the LMB-down provenance while the player keeps aiming at
        // the same actor; only a different selection replaces it.
        neutralOrigin = true;
    }
    else
    {
        neutralOrigin = !target->isEnemy(s_freeMoveAnchor, true)
            && !s_freeMoveAnchor->isEnemy(target, true);
        s_attackNeutralIntentTarget = neutralOrigin ? target : nullptr;
    }
    char b[192];
    sprintf_s(b, sizeof(b),
        "[Ironwind] attack_press_target captured=%d hostile=%d neutral_origin=%d",
        target ? 1 : 0,
        target && target->isEnemy(s_freeMoveAnchor, true) ? 1 : 0,
        neutralOrigin ? 1 : 0);
    VerbLog(b);
}

// Nearby interaction target selection.  It deliberately stays independent of
// attack intent: E may address a person, loose inventory item or world building
// while LMB remains entirely owned by manual combat. Characters reuse the strict
// projected-body observer; loose items and buildings use local world queries plus
// projected bounds. The final action is still validated by Kenshi's native
// ContextMenu, so this layer never invents an order.
static int s_interactionPickSeen = 0;
static float s_interactionPickNearestPx = -1.0f;
static float s_interactionPickSelectedDist = -1.0f;
static lektor<RootObject*> s_interactionBuildingScratch;
static lektor<RootObject*> s_interactionItemScratch;
static lektor<RootObject*> s_interactionWeaponScratch;
static lektor<RootObject*> s_interactionArmourScratch;

static bool manualInteractionBuildingEligible(Building* b)
{
    if (!b || !b->isValid() || !b->getVisible() || b->isDestroyed()) return false;
    return b->isDoor() || b->isFurniture()
        || b->getMouseCursor() != DEFAULT_CURSOR
        || b->getDefaultTask() != NULL_TASK;
}

static bool manualInteractionLooseItemEligible(RootObject* root)
{
    if (!root || !root->isValid() || !root->typeIsAnItem()) return false;
    Item* item = static_cast<Item*>(root);
    return item->onGround() && !item->isInInventory
        && item->getVisible() && !item->isDisabled();
}

static bool manualInteractionRootValid(RootObject* target, Character* me)
{
    if (!target || !target->isValid() || target == me) return false;
    itemType type = target->getDataType();
    if (type == CHARACTER)
    {
        Character* c = static_cast<Character*>(target);
        if (!c->movement) return false;
        // A conscious live enemy remains the LMB/RMB combat subject.  E is for
        // social, rescue, loot/carry and world interactions.
        return !c->isEnemy(me, true) || c->isDead() || c->getProneState() == PS_KO;
    }
    if (type == BUILDING)
        return manualInteractionBuildingEligible(static_cast<Building*>(target));
    return manualInteractionLooseItemEligible(target);
}

static bool manualInteractionAabbScreenMiss(Ogre::Camera* cam, const Ogre::Aabb& box,
                                            float aimX, float aimY, float halo,
                                            float tolerance, float& missOut)
{
    const Ogre::Vector3& c = box.mCenter;
    const Ogre::Vector3& h = box.mHalfSize;
    if (fabsf(c.x) >= 1.0e7f || fabsf(c.y) >= 1.0e7f || fabsf(c.z) >= 1.0e7f
        || fabsf(h.x) >= 1.0e5f || fabsf(h.y) >= 1.0e5f || fabsf(h.z) >= 1.0e5f)
        return false;
    Ogre::Vector3 points[9] = {
        c,
        c + Ogre::Vector3(-h.x, -h.y, -h.z), c + Ogre::Vector3(-h.x, -h.y,  h.z),
        c + Ogre::Vector3(-h.x,  h.y, -h.z), c + Ogre::Vector3(-h.x,  h.y,  h.z),
        c + Ogre::Vector3( h.x, -h.y, -h.z), c + Ogre::Vector3( h.x, -h.y,  h.z),
        c + Ogre::Vector3( h.x,  h.y, -h.z), c + Ogre::Vector3( h.x,  h.y,  h.z)
    };
    float left = 1.0e9f, right = -1.0e9f, top = 1.0e9f, bottom = -1.0e9f;
    int projected = 0;
    for (int i = 0; i < 9; ++i)
    {
        int sx, sy;
        if (!focusProject(cam, points[i], 0.0f, sx, sy)) continue;
        if (sx < left) left = (float)sx;
        if (sx > right) right = (float)sx;
        if (sy < top) top = (float)sy;
        if (sy > bottom) bottom = (float)sy;
        ++projected;
    }
    if (!projected) return false;
    left -= halo; right += halo; top -= halo; bottom += halo;
    float dx = aimX < left ? left - aimX : (aimX > right ? aimX - right : 0.0f);
    float dy = aimY < top ? top - aimY : (aimY > bottom ? aimY - bottom : 0.0f);
    missOut = dx + dy;
    return dx <= tolerance && dy <= tolerance;
}

static void manualInteractionConsiderItems(lektor<RootObject*>& items,
                                           Ogre::Camera* cam,
                                           const Ogre::Vector3& mp,
                                           float aimX, float aimY, float maxR2,
                                           RootObject*& best, float& bestScore)
{
    for (auto it = items.begin(); it != items.end(); ++it)
    {
        RootObject* root = *it;
        if (!manualInteractionLooseItemEligible(root)) continue;
        Ogre::Vector3 p = root->getPosition();
        Ogre::Vector3 delta = p - mp; delta.y = 0.0f;
        float d2 = delta.squaredLength();
        if (d2 >= maxR2) continue;
        ++s_interactionPickSeen;

        // Ground props can be only a few pixels wide.  Prefer their projected
        // physical bounds with a small visual halo; use the exact world centre
        // only when Kenshi exposes no usable AABB.  This remains a reticle hit,
        // never a nearest-item magnet.
        float miss = 0.0f;
        bool covered = manualInteractionAabbScreenMiss(
            cam, root->getAABB(), aimX, aimY, 8.0f, 12.0f, miss);
        if (!covered)
        {
            int sx, sy;
            if (!focusProject(cam, p, 0.0f, sx, sy)) continue;
            float dx = fabsf(sx - aimX), dy = fabsf(sy - aimY);
            if (dx > 24.0f || dy > 24.0f) continue;
            miss = dx + dy;
        }
        if (s_interactionPickNearestPx < 0.0f || miss < s_interactionPickNearestPx)
            s_interactionPickNearestPx = miss;
        float score = miss + sqrtf(d2) * 0.08f;
        if (s_interactionTargetHandle.isValid()
            && s_interactionTargetHandle.getRootObject() == root) score -= 0.15f;
        if (score < bestScore)
        {
            bestScore = score;
            best = root;
            s_interactionPickSelectedDist = sqrtf(d2);
        }
    }
}

static RootObject* manualInteractionPick(Character* me)
{
    s_interactionPickSeen = 0;
    s_interactionPickNearestPx = -1.0f;
    s_interactionPickSelectedDist = -1.0f;
    if (!me || !me->movement || !s_gwFrame || !ou || !ou->player
        || !ou->player->camera) return nullptr;
    Ogre::Camera* cam = ou->player->camera->camera;
    if (!cam) return nullptr;
    MyGUI::IntSize view = MyGUI::RenderManager::getInstance().getViewSize();
    if (view.width <= 0 || view.height <= 0) return nullptr;
    const float aimX = view.width * 0.5f, aimY = view.height * 0.5f;
    const Ogre::Vector3 mp = me->movement->pos;
    const float maxR2 = 30.0f * 30.0f;
    RootObject* best = nullptr;
    float bestScore = 1e9f;

    // Character interaction uses the exact same projected-body overlap as the
    // visible name/health panel.  Therefore a person can never receive E merely
    // for being closer while standing hundreds of pixels away from the reticle.
    Character* observed = focusNpcTargetUnderCrosshair();
    if (manualInteractionRootValid(observed, me))
    {
        Ogre::Vector3 delta = observed->movement->pos - mp;
        delta.y = 0.0f;
        float d2 = delta.squaredLength();
        if (d2 < maxR2)
        {
            ++s_interactionPickSeen;
            s_interactionPickNearestPx = 0.0f;
            bestScore = sqrtf(d2) * 0.08f;
            best = observed;
            s_interactionPickSelectedDist = sqrtf(d2);
        }
    }
    if (best) return best;

    // Preserve Kenshi's own exact picker when it has a valid loose item.
    RootObject* nativeTarget = ou->player->mouseRightTargetSet
        ? ou->player->mouseRightTarget : nullptr;
    if (manualInteractionLooseItemEligible(nativeTarget))
    {
        Ogre::Vector3 p = nativeTarget->getPosition();
        Ogre::Vector3 delta = p - mp; delta.y = 0.0f;
        float d2 = delta.squaredLength();
        int nx, ny;
        if (d2 < maxR2 && focusProject(cam, p + Ogre::Vector3(0.0f, 1.0f, 0.0f), 0.0f, nx, ny)
            && fabsf(nx - aimX) <= 45.0f && fabsf(ny - aimY) <= 45.0f)
        {
            float miss = fabsf(nx - aimX) + fabsf(ny - aimY);
            float score = miss + sqrtf(d2) * 0.08f;
            if (score < bestScore)
            {
                bestScore = score;
                best = nativeTarget;
                s_interactionPickSelectedDist = sqrtf(d2);
            }
        }
    }
    if (best) return best;

    // Detached over-shoulder/first-person cameras do not reliably populate
    // mouseRightTarget because the native cursor is hidden.  Query only the
    // immediate 30-unit neighbourhood and require the reticle to cover the
    // projected item.  Weapons and armour are separate Kenshi data types even
    // though all three are Item instances and use the same native menu.
    s_interactionItemScratch.clear();
    s_interactionWeaponScratch.clear();
    s_interactionArmourScratch.clear();
    s_gwFrame->getObjectsWithinSphere(s_interactionItemScratch, mp, 30.0f,
                                      ITEM, 64, me);
    s_gwFrame->getObjectsWithinSphere(s_interactionWeaponScratch, mp, 30.0f,
                                      WEAPON, 64, me);
    s_gwFrame->getObjectsWithinSphere(s_interactionArmourScratch, mp, 30.0f,
                                      ARMOUR, 64, me);
    manualInteractionConsiderItems(s_interactionItemScratch, cam, mp,
                                   aimX, aimY, maxR2, best, bestScore);
    manualInteractionConsiderItems(s_interactionWeaponScratch, cam, mp,
                                   aimX, aimY, maxR2, best, bestScore);
    manualInteractionConsiderItems(s_interactionArmourScratch, cam, mp,
                                   aimX, aimY, maxR2, best, bestScore);
    if (best) return best;

    // Buildings are not in the character update list.  Query only a small
    // local sphere while V mode is active, then select by projected AABB centre.
    s_interactionBuildingScratch.clear();
    s_gwFrame->getObjectsWithinSphere(s_interactionBuildingScratch, mp, 30.0f,
                                      BUILDING, 128, me);
    for (auto it = s_interactionBuildingScratch.begin();
         it != s_interactionBuildingScratch.end(); ++it)
    {
        RootObject* root = *it;
        Building* b = root ? static_cast<Building*>(root) : nullptr;
        if (!manualInteractionBuildingEligible(b)) continue;
        const Ogre::Aabb& box = b->getAABB();
        Ogre::Vector3 p = box.mCenter;
        Ogre::Vector3 delta = p - mp; delta.y = 0.0f;
        float d2 = delta.squaredLength();
        if (d2 >= maxR2) continue;
        ++s_interactionPickSeen;
        float miss = 0.0f;
        if (!manualInteractionAabbScreenMiss(cam, box, aimX, aimY,
                                             18.0f, 24.0f, miss)) continue;
        if (s_interactionPickNearestPx < 0.0f || miss < s_interactionPickNearestPx)
            s_interactionPickNearestPx = miss;
        float score = miss + sqrtf(d2) * 0.08f;
        if (s_interactionTargetHandle == b) score -= 0.15f;
        if (score < bestScore)
        {
            bestScore = score;
            best = b;
            s_interactionPickSelectedDist = sqrtf(d2);
        }
    }
    return best;
}

// The native context menu is a hold-RMB UI.  Opening it directly without a
// matching held input lets PlayerInterface close it on the following update.

// Vanilla player talk: the NPC owns the dialogue package and receives
// EV_PLAYER_TALK_TO_ME with the player as the other party.  Squad members
// keep the full order menu so medic, inventory and follow stay one click away.
static bool manualHasPlayerTalk(Character* me, Character* target)
{
    if (!me || !target || target->isDead()) return false;
    if (target->isEnemy(me, true)) return false;
    if (!target->hasDialogue() || !target->dialogue) return false;
    return target->dialogue->hasDialogueEvent(EV_PLAYER_TALK_TO_ME);
}

static void manualInteractionTargetTick()
{
    if (s_interactionMenuByE || s_interactionOpenPending)
    {
        s_allowTalkCursor = false;
        s_xhairTint = 0;
        return;
    }
    bool allow = false;
    int tint = 0;
    RootObject* selected = nullptr;
    if (s_mode == MODE_FREE_MOVE && s_directionalCombat
        && (s_firstPersonActive || s_otsCamActive) && s_freeMoveAnchor
        && !s_dcShutdownInProgress && !s_loadGuardActive
        && !s_cameraUiOpen && !dcUiWantsCursor() && ou && ou->player)
    {
        RootObject* t = manualInteractionPick(s_freeMoveAnchor);
        if (manualInteractionRootValid(t, s_freeMoveAnchor))
        {
            selected = t;
            if (t->getDataType() == CHARACTER)
            {
                Character* c = static_cast<Character*>(t);
                allow = manualHasPlayerTalk(s_freeMoveAnchor, c);
                tint = (c->isAlly(s_freeMoveAnchor, true)
                        || manualVPlayerTeamMember(c)) ? 2 : 1;
            }
            else tint = 1;
        }
    }
    RootObject* previous = s_interactionTargetHandle.isValid()
        ? s_interactionTargetHandle.getRootObject() : nullptr;
    if (selected != previous)
    {
        if (selected) s_interactionTargetHandle = selected;
        else s_interactionTargetHandle.setNull();
        char b[192];
        sprintf_s(b, sizeof(b),
            "[Ironwind] interact_target selected=%d type=%d distance=%.1f seen=%d miss_px=%.1f",
            selected ? 1 : 0, selected ? (int)selected->getDataType() : -1,
            selected ? s_interactionPickSelectedDist : -1.0f,
            s_interactionPickSeen, s_interactionPickNearestPx);
        VerbLog(b);
    }
    s_allowTalkCursor = allow;
    s_xhairTint = tint;
}

static void manualInteractionScheduleResume(const char* reason)
{
    s_interactionMenuByE = false;
    s_interactionOpenPending = false;
    s_interactionPendingHandle.setNull();
    if (ou && ou->player)
        ou->player->contextMenu.rightMouseActivated = false;
    if (s_interactionPauseOwned)
        s_interactionResumeAfterMs = GetTickCount64() + 120;
    char b[160];
    sprintf_s(b, sizeof(b), "[Ironwind] interact_menu_closed reason=%s pause_owned=%d",
              reason ? reason : "unknown", s_interactionPauseOwned ? 1 : 0);
    VerbLog(b);
}

static bool manualInteractionNestedUiVisible()
{
    ManagementScreen* ms = ManagementScreen::getSingleton();
    return s_lootUiSuspendActive
        || (ms && ms->getVisible())
        || (gui && (gui->inDialogue() || gui->isAnyInventoryWindowOpen()
                   || gui->isStatsWindowOpen()));
}

// Restore only a pause that this E transaction created.  If the chosen native
// order opened dialogue/inventory/map, ownership transfers to that original UI
// instead of Ironwind unpausing underneath it.
static void manualInteractionPauseTick()
{
    if (!s_interactionPauseOwned || !ou) return;
    if (!ou->isPaused())
    {
        s_interactionPauseOwned = false;
        s_interactionResumeAfterMs = 0;
        return;
    }
    if (s_interactionOpenPending || s_interactionMenuByE
        || (ou->player && ou->player->contextMenu.isVisible())) return;
    ULONGLONG now = GetTickCount64();
    if (!s_interactionResumeAfterMs || now < s_interactionResumeAfterMs) return;
    if (manualInteractionNestedUiVisible())
    {
        s_interactionPauseOwned = false;
        s_interactionResumeAfterMs = 0;
        VerbLog("[Ironwind] interact_pause_transferred_to_native_ui");
        return;
    }
    ou->userPause(false);
    s_interactionPauseOwned = false;
    s_interactionResumeAfterMs = 0;
    VerbLog("[Ironwind] interact_pause_released");
}

// One-shot E interaction.  The request is prepared in the world loop, then
// opened inside playerControl_hook so Kenshi observes a held-RMB lifecycle in
// the same input phase as its original context menu.  We never invoke an option
// ourselves; the native ContextMenuGUI owns selection and every resulting task.
static void manualInteractionActionTick()
{
    bool valid = s_mode == MODE_FREE_MOVE && s_directionalCombat
        && s_manualAttackOn && s_freeMoveAnchor
        && !s_dcShutdownInProgress && !s_loadGuardActive
        && !s_dcPtrLossActive;
    if (s_interactionOpenPending && !valid)
    {
        manualInteractionScheduleResume("context_lost_before_open");
    }
    if (s_interactionMenuByE)
    {
        bool visible = valid && ou && ou->player
            && ou->player->contextMenu.isVisible();
        if (!visible)
        {
            if (ou && ou->player && ou->player->contextMenu.isVisible() && s_gwFrame)
                s_gwFrame->hideContextMenu();
            manualInteractionScheduleResume("native_close");
        }
    }
    if (!s_interactionRequested) return;
    s_interactionRequested = false;

    if ((s_interactionMenuByE || s_interactionOpenPending) && ou && ou->player)
    {
        if (s_gwFrame) s_gwFrame->hideContextMenu();
        manualInteractionScheduleResume("e_toggle");
        return;
    }
    if (!valid)
    {
        VerbLog("[Ironwind] interact_mode_denied reason=context");
        return;
    }
    if (s_cameraUiOpen || dcUiWantsCursor())
    {
        VerbLog("[Ironwind] interact_mode_denied reason=ui");
        return;
    }
    bool busy = s_attackAimHeld || s_attackActive || manualAttackCommitLive()
        || s_freeAttackTech
        || s_mouseBlockHeld || s_blockHeld
        || manualHitReactionLive(s_freeMoveAnchor);
    if (busy)
    {
        VerbLog("[Ironwind] interact_mode_denied reason=committed_action");
        return;
    }
    // TargetTick ran immediately before this action in the same game frame.
    // Use exactly the object whose E prompt the player saw; repicking here used
    // to switch to a different, nearer character between the prompt and press.
    RootObject* target = s_interactionTargetHandle.isValid()
        ? s_interactionTargetHandle.getRootObject() : nullptr;
    if (!manualInteractionRootValid(target, s_freeMoveAnchor))
    {
        VerbLog("[Ironwind] interact_no_target");
        return;
    }
    s_interactionTargetHandle = target;
    // Never carry a combat edge into the native menu.
    s_rmbCombatEdgeSeenMs = s_rmbCombatEdgeMs;
    s_rmbCombatEdgeMs = 0;
    s_attackAimHeld = false;
    s_attackAimCancelled = false;
    s_attackHeld = false;
    s_mouseBlockHeld = false;
    s_blockHeld = false;
    s_blockGestureSeen = false;
    s_interactionPendingHandle = target;
    s_interactionOpenPending = true;
    char b[224];
    sprintf_s(b, sizeof(b),
        "[Ironwind] interact_menu_requested type=%d distance=%.1f",
        (int)target->getDataType(), s_interactionPickSelectedDist);
    VerbLog(b);
}

// Runs at the top of the game-thread frame, before the block press is consumed.
// Outside the explicit interaction layer RMB is unconditionally guard: hold
// duration can never reinterpret the press as a menu or dialogue request.
static void manualConsumeCombatRmb()
{
    // A held guard survives only as physical intent while vanilla owns a hit
    // reaction. Recreate one fresh accepted press on the exact first game
    // frame where the native state is no longer STUMBLE/REACTION_BLOCK. The
    // captured direction is reused; no new mouse gesture is required.
    if (s_reactionGuardHeld)
    {
        bool physicalHeld = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        if (!physicalHeld)
        {
            s_reactionGuardHeld = false;
            s_reactionGuardDesired = CUT_DEFAULT;
            s_reactionGuardGestureSeen = false;
        }
        else if (ou && ou->player && !ou->isLoadingFromASaveGame()
            && !s_dcPtrLossActive && !s_dcShutdownInProgress && !s_loadGuardActive
            && mouseCombatContext() && s_freeMoveAnchor && !dcUiWantsCursor()
            && !manualHitReactionLive(s_freeMoveAnchor))
        {
            ULONGLONG rearm = GetTickCount64();
            if (!s_settingAiBlocks)
            {
                CutDirection guard = (CutDirection)s_reactionGuardDesired;
                if (guard == CUT_DEFAULT)
                    guard = combatGuardDirectionFromGesture((CutDirection)s_lastAttackDirection);
                if (guard == CUT_DEFAULT) guard = CUT_DOWNWARD;
                s_blockDesired = guard;
                s_blockGestureSeen = s_reactionGuardGestureSeen;
            }
            else
                s_blockGestureSeen = false;
            s_mouseBlockHeld = true;
            s_blockHeld = true;
            s_blockPressMs = rearm;
            s_reactionGuardHeld = false;
            s_reactionGuardDesired = CUT_DEFAULT;
            s_reactionGuardGestureSeen = false;
            char rb[144];
            sprintf_s(rb, sizeof(rb),
                "[Ironwind] reaction_guard_rearmed mode=%s dir=%d",
                s_settingAiBlocks ? "automatic_ai" : "manual_directional",
                s_settingAiBlocks ? (int)CUT_DEFAULT : (int)s_blockDesired);
            VerbLog(rb);
        }
    }

    ULONGLONG edge = s_rmbCombatEdgeMs;
    if (edge && edge != s_rmbCombatEdgeSeenMs)
    {
        s_rmbCombatEdgeSeenMs = edge;
        if (!ou || !ou->player || ou->isLoadingFromASaveGame() || s_dcPtrLossActive
            || s_dcShutdownInProgress || s_loadGuardActive
            || !mouseCombatContext() || !s_freeMoveAnchor || dcUiWantsCursor())
            return;
        if ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) == 0) return;
        CutDirection recent = combatRecentDirection();
        CutDirection guard = recent;
        CutDirection gesture = CUT_DEFAULT;
        if (!s_settingAiBlocks)
        {
            s_blockGestureSeen = recent != CUT_DEFAULT;
            if (guard == CUT_DEFAULT) guard = (CutDirection)s_lastAttackDirection;
            if (guard == CUT_DEFAULT) guard = CUT_DOWNWARD;
            gesture = guard;
            guard = combatGuardDirectionFromGesture(gesture);
            s_blockDesired = guard;
            s_mouseBlockHeld = true;
            s_blockHeld = true;
            s_blockPressMs = edge;
        }
        else
        {
            // The player still owns the WHEN: holding RMB requests defence.
            // Direction stays unset so the original chooser owns the HOW.
            s_mouseBlockHeld = true;
            s_blockHeld = true;
            s_blockPressMs = edge;
            s_blockGestureSeen = false;
        }

        char b[192];
        sprintf_s(b, sizeof(b), "[Ironwind] interact_rmb_block mode=%s gesture_dir=%d guard_dir=%d gesture=%d social_armed=0",
                  s_settingAiBlocks ? "automatic_ai" : "manual_directional",
                  (int)gesture, (int)guard, s_blockGestureSeen ? 1 : 0);
        VerbLog(b);
    }
}

// The foe every manual action uses: the LOCKED target, else whoever the view is
// aimed at.  nullptr lets the caller fall back to the AI's own target.
static Character* manualFoeResolve(CombatClass* cc, Character* me)
{
    if (s_lockTarget && focusTargetValid(s_lockTarget)) return s_lockTarget;
    return manualAimPick(me, s_aimRange);
}

// Aim focus with a FREE cursor: the live hostile whose chest projects nearest
// to the mouse cursor, within AimHoverPx on screen and AimRange in the world.
static Character* manualAimHoverPick(Character* me, Ogre::Camera* cam, float radiusPx, float range)
{
    if (!me || !me->movement || !s_gwFrame || !cam) return nullptr;
    MyGUI::InputManager* im = MyGUI::InputManager::getInstancePtr();
    if (!im) return nullptr;
    MyGUI::IntPoint mp = im->getMousePosition();
    const Ogre::Vector3 me3 = me->movement->pos;
    const float maxR2 = range * range;
    Character* best = nullptr; float bestD2 = radiusPx * radiusPx;
    auto& chars = s_gwFrame->getCharacterUpdateList();
    for (auto it = chars.begin(); it != chars.end(); ++it)
    {
        Character* c = *it;
        if (!c || c == me || !c->movement) continue;
        float dx = c->movement->pos.x - me3.x, dz = c->movement->pos.z - me3.z;
        if (dx * dx + dz * dz >= maxR2) continue;
        if (!manualVAttackTargetLive(c) || !c->isEnemy(me, true)) continue;
        Ogre::Vector3 chest; int sx, sy;
        if (!focusBonePos(c, "Bip01 Spine1", 11.0f, chest) || !focusProject(cam, chest, 0.0f, sx, sy)) continue;
        float px = (float)(sx - mp.left), py = (float)(sy - mp.top);
        float d2 = px * px + py * py;
        if (d2 < bestD2) { bestD2 = d2; best = c; }
    }
    return best;
}

// Main thread, once per frame (world-live gated by manualBlockTick).  The aim
// focus follows the MANUAL COMBAT toggle (Z): while on, whoever you point at is
// re-asserted as the AI's current target.  Never touches the camera.
static void manualAimTick()
{
    bool want = s_manualAttackOn && s_mode == MODE_FREE_MOVE && s_freeMoveAnchor
             && !s_loadGuardActive && !s_dcShutdownInProgress
             && !s_freeMoveAnchor->isUnconcious() && !s_freeMoveAnchor->isDown();
    ULONGLONG now = GetTickCount64();
    if (!want)
    {
        if (s_lockTarget) { s_lockTarget = nullptr; s_aimLogged = nullptr; VerbLog("[WASDCombat] aim_focus off"); }
        s_aimClicked = false;
        return;
    }
    bool cursorFree = !(s_firstPersonActive || s_otsCamActive) || dcUiWantsCursor();
    Ogre::Camera* cam = (ou && ou->player && ou->player->camera) ? ou->player->camera->camera : nullptr;
    // RIGHT-CLICK: instant focus at any distance (in combat only; outside combat
    // the click stays a plain vanilla order).
    ULONGLONG rc = s_rmbAimPressMs;
    if (rc != 0 && rc != s_rmbAimSeenMs)
    {
        s_rmbAimSeenMs = rc;
        if (s_freeMoveAnchor->isInCombatMode(true, true))
        {
            float radius = s_aimHoverPx < 48.0f ? 48.0f : s_aimHoverPx;
            Character* cpick = cursorFree ? manualAimHoverPick(s_freeMoveAnchor, cam, radius, 600.0f)
                                          : manualAimPick(s_freeMoveAnchor, 600.0f);
            if (cpick)
            {
                s_lockTarget = cpick; s_aimClicked = true;
                VerbLog("[WASDCombat] aim_focus click");
            }
        }
    }
    if (now - s_aimEvalMs >= 50)                       // 20 Hz is plenty for a pointer
    {
        s_aimEvalMs = now;
        Character* pick = nullptr;
        if (cursorFree) pick = manualAimHoverPick(s_freeMoveAnchor, cam, s_aimHoverPx, s_aimRange);
        else if (!s_aimClicked) pick = manualAimPick(s_freeMoveAnchor, s_aimRange);   // the cone never overrides a clicked focus
        if (pick && pick != s_lockTarget) s_aimClicked = false;                       // pointed straight at someone else
        // STICKY (log 2026-09-21: 352 set / 228 none, a change every ~1 s = the AI
        // re-facing and re-pathing all fight): the focus only SWITCHES when a
        // different enemy is actually pointed at; it is kept while the current
        // one is still a live enemy within 1.5x the aim range, and released only
        // when that stops being true.
        if (pick) s_lockTarget = pick;
        else if (s_lockTarget)
        {
            bool keep = focusTargetValid(s_lockTarget) && s_lockTarget->movement && s_freeMoveAnchor->movement;
            if (keep && !s_aimClicked)                 // a clicked focus keeps at any distance
            {
                float dx = s_lockTarget->movement->pos.x - s_freeMoveAnchor->movement->pos.x;
                float dz = s_lockTarget->movement->pos.z - s_freeMoveAnchor->movement->pos.z;
                float lim = s_aimRange * 1.5f;
                keep = dx * dx + dz * dz <= lim * lim;
            }
            if (!keep) { s_lockTarget = nullptr; s_aimClicked = false; }
        }
    }
    if (s_lockTarget && !focusTargetValid(s_lockTarget)) { s_lockTarget = nullptr; s_aimClicked = false; }
    if (s_lockTarget != s_aimLogged)
    {
        s_aimLogged = s_lockTarget;
        DebugLog(s_lockTarget ? "[WASDCombat] aim_focus set" : "[WASDCombat] aim_focus none");
    }
    CombatClass* cc = s_freeMoveAnchor->getCombatClass();
    if (cc && s_attackIntentTarget && s_attackIntentPressMs == s_attackAcceptedPressMs
        && (s_attackActive || manualAttackCommitLive())
        && focusTargetLoaded(s_attackIntentTarget))
    {
        if (cc->currentTarget != s_attackIntentTarget) cc->currentTarget = s_attackIntentTarget;
    }
    else if (cc && s_lockTarget && cc->currentTarget != s_lockTarget)
        cc->currentTarget = s_lockTarget;   // AI target = the aim focus
}

// Block POSE on a G press (user 2026-09-18: a press during the enemy's swing
// blocked without the pose, or the AI's own block came too late). The manual
// block pool supplies a block technique (the
// DEFENSIVE order and the perfect window keep the chooser open for us), then
// the changeState(BLOCK) writes; blockState starts a block technique's clip
// itself (it only pre-checks progress for dodges).  The perfect-block verdict
// still guarantees the block inside the window; outside it, vanilla judges the
// pose by direction and timing exactly as the AI's own block.
static Character* manualAttackFindFoe(CombatClass* cc, Character* me);   // fwd decl
static void manualBlockInject(CombatClass* cc)
{
    if (s_settingAiBlocks) return;
    ULONGLONG press = s_blockPressMs;
    if (press == 0 || press == s_blockInjectedPressMs || press != s_blockAcceptedPressMs) return;
    ULONGLONG now = GetTickCount64();
    if (now - press > 300 && !s_blockHeld) { s_blockInjectedPressMs = press; return; }
    if (!cc->combatModeActive)
    {
        static ULONGLONG s_loggedBlockWaitPress = 0;
        if (s_loggedBlockWaitPress != press)
        {
            s_loggedBlockWaitPress = press;
            VerbLog("[IronwindTrace] block_inject_wait cc_active=0");
        }
        return;
    }                                    // wait for combat (short)
    s_blockInjectedPressMs = press;
    swordStateEnum st = cc->getCombatState();
    if (st == STUMBLE || s_attackActive) return;
    if (st == BLOCK && cc->currentTechnique) return;                      // already blocking
    Character* me = cc->me;
    CharStats* stats = me ? me->getStats() : nullptr;
    if (!stats) return;
    manualBlockBuildCopies();
    Character* foe = incomingAttackAttacker();         // build 61: whoever is swinging at us (any direction)
    bool fromSwing = foe != nullptr;
    if (!foe) foe = manualAttackFindFoe(cc, me);       // else the aimed / current / biggest-threat enemy
    if (!foe || !foe->getStats()) { VerbLog("[WASDCombat] block_inject_no_target"); return; }
    // Use block-only copies, never a dodge record. The chooser needs the exact
    // incoming cut and returned nothing 54 times in the field.
    int want = -1;
    CombatTechniqueData* t = nullptr;
    t = manualBlockPick(me, foe, &want);
    if (!t) { VerbLog("[WASDCombat] block_inject_no_technique"); return; }
    cc->currentTechnique = t;
    cc->blockingTarget   = foe;
    cc->techniqueIntegrityCheckTimer = stats->calculateTechniqueInegrityCheckTimer();
    cc->stateTimer   = 0.2f;
    cc->nextMove     = BLOCK;
    cc->combatState  = BLOCK;
    char b[160];
    sprintf_s(b, sizeof(b), "[WASDCombat] block_inject anim=%s want=%d state_was=%d press_age=%llu from_swing=%d",
              t->animation.c_str(), want, (int)st, now - press, fromSwing ? 1 : 0);
    VerbLog(b);
}

// Automatic guard is still player-authorised: RMB must be held and the native
// attackingYou notification must identify a currently live enemy swing.  The
// original CharStats::chooseBlock performs the skill/weapon/direction choice;
// we only deliver its one result to the native BLOCK state.  v86 merely let the
// AI decision frame run while the player was otherwise pinned in WAIT, which
// often never reached chooseBlock before impact and produced no guard at all.
static void manualAutoBlockInject(CombatClass* cc)
{
    if (!s_settingAiBlocks || !s_blockHeldPrev || !cc || !cc->me
        || cc->me != s_freeMoveAnchor || !s_chooseBlockOrig) return;
    if (s_attackActive || manualHitReactionLive(cc->me)) return;
    swordStateEnum state = cc->getCombatState();
    if (state == STUMBLE || state == REACTION_BLOCK) return;
    if (state == BLOCK && cc->currentTechnique) return;

    ULONGLONG now = GetTickCount64();
    Character* foe = nullptr;
    ULONGLONG swingStart = 0;
    for (int i = 0; i < INCOMING_ATTACK_MAX; ++i)
    {
        const IncomingAttackSlot& g = s_incomingAttack[i];
        if (!g.who || g.impactMs != 0 || now - g.startMs >= 4000
            || !focusTargetLoaded(g.who) || g.who->isDead() || g.who->isUnconcious()) continue;
        CombatClass* attackerCombat = g.who->getCombatClass();
        swordStateEnum attackerState = attackerCombat
            ? attackerCombat->getCombatState() : COMBAT_FINISHED;
        if (!attackerCombat || !attackerCombat->combatModeActive
            || (attackerState != STARTUP_STATE && attackerState != CHOP_WEAPON)) continue;
        if (!foe || g.startMs < swingStart)
        {
            foe = g.who;
            swingStart = g.startMs;
        }
    }
    if (!foe || swingStart == 0) return;
    if (foe == s_autoBlockAttemptWho && swingStart == s_autoBlockAttemptStartMs) return;

    CombatClass* attackerCombat = foe->getCombatClass();
    CombatTechniqueData* attack = attackerCombat ? attackerCombat->currentTechnique : nullptr;
    if (!attack || attack->isBlock || attack->numImpactPoints() <= 0) return;
    int section = attackerCombat->currentComboSection;
    if (section < 0) section = 0;
    if (section >= attack->numImpactPoints()) section = attack->numImpactPoints() - 1;
    CombatTechniqueData::ImpactPoint* impact = attack->impactPoint(section);
    CharStats* defenderStats = cc->me->getStats();
    CharStats* attackerStats = foe->getStats();
    if (!impact || !defenderStats || !attackerStats) return;

    // Spend the swing before calling game code. A null choice is a legitimate
    // native failed defence and must not be rerolled on the following frame.
    s_autoBlockAttemptWho = foe;
    s_autoBlockAttemptStartMs = swingStart;
    float versus = attackerStats->getMeleeAttack();
    if (versus < 0.0f) versus = 0.0f;
    CutOrigination from = cc->me->getAttackOriginationDirection(foe);
    CutDirection incoming = cc->me->convertCutDirection(impact->direction, from);
    CombatTechniqueData* picked = s_chooseBlockOrig(
        defenderStats, impact->direction, versus, from, foe);

    if (!picked)
    {
        char miss[176];
        sprintf_s(miss, sizeof(miss),
            "[Ironwind] ai_block_attempt chosen=0 incoming_dir=%d from=%d versus=%.1f",
            (int)incoming, (int)from, versus);
        VerbLog(miss);
        return;
    }

    cc->currentTechnique = picked;
    cc->blockingTarget = foe;
    bool played = false;
    if (picked->isDodge && cc->animation)
    {
        nativeDefenceResolvePlayFn();
        if (s_defencePlayFn)
        {
            std::string suffix;
            float speed = defenderStats->blockSpeed * picked->animSpeedMultiplier;
            s_defencePlayFn((void*)cc->animation, picked, speed, &suffix);
            played = true;
        }
    }
    cc->techniqueIntegrityCheckTimer = defenderStats->calculateTechniqueInegrityCheckTimer();
    // Automatic RMB is pose-neutral until this real incoming swing. Re-enter
    // the combat animation layer only now, immediately before native BLOCK.
    // This replaces the old press-time combat idle / defensive standing order.
    if (cc->animation) cc->animation->setCombatMode(true);
    cc->combatModeActive = true;
    cc->stateTimer = 0.2f;
    cc->nextMove = BLOCK;
    cc->combatState = BLOCK;
    s_autoBlockActiveTech = picked;

    char ok[288];
    sprintf_s(ok, sizeof(ok),
        "[Ironwind] ai_block_attempt chosen=1 incoming_dir=%d from=%d versus=%.1f dodge=%d explicit_play=%d anim=%s",
        (int)incoming, (int)from, versus, picked->isDodge ? 1 : 0,
        played ? 1 : 0, picked->animation.c_str());
    VerbLog(ok);
}

// Resolve a foe for the attack; startupState needs a current target or it ends combat.
static Character* manualAttackFindFoe(CombatClass* cc, Character* me)
{
    if (manualAttackCommitLive() && s_attackAcceptedPressMs)
    {
        if (s_attackIntentPressMs != s_attackAcceptedPressMs)
        {
            s_attackIntentPressMs = s_attackAcceptedPressMs;
            s_attackIntentTarget = manualAttackIntentPick(me);
            if (s_attackIntentTarget)
                VerbLog(s_attackIntentTarget->isEnemy(me, true)
                    ? "[Ironwind] attack_aim_target hostile=1"
                    : "[Ironwind] attack_aim_target hostile=0");
        }
        if (focusTargetLoaded(s_attackIntentTarget) && manualVAttackTargetLive(s_attackIntentTarget)
            && (s_mode != MODE_FREE_MOVE || manualVTargetAllowed(me, s_attackIntentTarget)))
            return s_attackIntentTarget;
        // Free-control swings are not target-gated. If the reticle did not
        // capture anyone, a hostile in the broad forward hemisphere may carry
        // the native AttackState so its real attack zone can decide incidental
        // contact. This never selects neutrals or squad members and never grants
        // damage by itself; empty space still falls through to a visual air swing.
        float carrierDistance = -1.0f;
        Character* carrier = manualMeleeForwardHostilePick(
            me, s_aimRange, 80.0f, &carrierDistance);
        if (carrier && manualVTargetAllowed(me, carrier))
        {
            char cb[128];
            sprintf_s(cb, sizeof(cb),
                "[Ironwind] attack_incidental_carrier dist=%.1f", carrierDistance);
            VerbLog(cb);
            return carrier;
        }
        // The old forward-only carrier made target acquisition at the release
        // frame a permanent damage gate: five v79 swings became visual-only
        // even though the reticle acquired an enemy while the blade was moving.
        // A nearby hostile now keeps the original AttackState alive; the real
        // attack zone, block/dodge verdict and victim _getHit remain untouched.
        // The v79 visual-only samples had their nearest hostile at 44-50 while
        // the reticle AimRange is 40.  Use the same broad 2x range already used
        // by display/threat fallbacks.  This still cannot extend weapon reach:
        // it only supplies AttackState's required owner and its native zone may
        // (and usually will) report zero contacts at that distance.
        carrier = manualNearestHostileCarrier(me, s_aimRange * 2.0f, &carrierDistance);
        if (carrier)
        {
            char cb[144];
            sprintf_s(cb, sizeof(cb),
                "[Ironwind] attack_incidental_carrier_anywhere dist=%.1f", carrierDistance);
            VerbLog(cb);
            return carrier;
        }
        return nullptr;
    }
    Character* foe = manualFoeResolve(cc, me);       // lock, else the enemy you are looking at
    if (!foe && focusTargetValid(cc->currentTarget)) foe = cc->currentTarget;
    if (!foe) { Character* t = cc->getBiggestThreat(cc->threats, 0.0f); if (focusTargetValid(t)) foe = t; }
    if (!foe) { Character* t = cc->_getAttackTarget().getCharacter(); if (focusTargetValid(t)) foe = t; }
    if (!foe && s_gwFrame && me->movement)
    {
        const Ogre::Vector3 mp = me->movement->pos;
        const float reach = manualOriginalMeleeReach(me);
        const float maxR2 = reach * reach;
        float best2 = -1.0f;
        auto& chars = s_gwFrame->getCharacterUpdateList();
        for (auto it = chars.begin(); it != chars.end(); ++it)
        {
            Character* c = *it;
            if (!c || c == me || !c->movement) continue;
            float dx = c->movement->pos.x - mp.x, dz = c->movement->pos.z - mp.z;
            float d2 = dx * dx + dz * dz;
            if (d2 >= maxR2 || (best2 >= 0.0f && d2 >= best2)) continue;
            if (!manualVAttackTargetLive(c)) continue;
            if (!c->isEnemy(me, true)) continue;
            best2 = d2; foe = c;
        }
    }
    return foe;
}

// The free-action animation is ours, but the active technique is also part of
// Kenshi's native attacker state.  Defenders inspect it after attackingYou()
// to choose and time their ordinary block.  Clear only the pointer we own.
static void manualFreeAttackClearTechnique(CombatClass* cc)
{
    if (cc && s_freeAttackTech && cc->currentTechnique == s_freeAttackTech)
    {
        cc->currentTechnique = nullptr;
        cc->currentTechniqueHasFinished = false;
    }
    s_freeAttackTech = nullptr;
}

// V controls can animate before Kenshi has entered its target-driven combat
// state.  These no-target clips are visual actions only: native combat still
// decides all hits, defence and XP when a hostile is present.
static void manualEnsureWeaponReady(Character* me, CharStats* stats)
{
    if (!me || !stats) return;
    CharacterHuman* human = me->isHuman();
    if (!human || human->weaponInHands) return;
    Inventory* inv = me->getInventory();
    if (!inv || !inv->hasWeaponEquipped())
    {
        if (!s_weaponMissingLogged)
        {
            s_weaponMissingLogged = true;
            VerbLog(manualUnarmedCombat(stats)
                ? "[Ironwind] weapon_ready unarmed"
                : "[Ironwind] weapon_ready no_equipped_weapon");
        }
        return;
    }
    s_weaponMissingLogged = false;
    Weapon* weapon = me->getThePreferredWeapon();
    if (!weapon) weapon = inv->getPrimaryWeapon();
    if (!weapon) weapon = inv->getSecondaryWeapon();
    if (!weapon) return;
    std::string sectionName;
    lektor<InventorySection*>& sections = inv->getAllSections();
    for (unsigned int i = 0; i < sections.size(); ++i)
        if (sections[i] && sections[i]->hasItem(weapon)) { sectionName = sections[i]->name; break; }
    s_weaponDrawInProgress = true;
    bool drawn = me->drawWeapon(weapon, sectionName);
    s_weaponDrawInProgress = false;
    char b[192]; sprintf_s(b, sizeof(b),
        "[Ironwind] weapon_draw attempted=1 ok=%d section=%d hand_after=%d unarmed_after=%d type=%d current=%d",
        drawn ? 1 : 0, sectionName.empty() ? 0 : 1,
        human->weaponInHands ? 1 : 0, stats->isUnarmed() ? 1 : 0,
        (int)weapon->getCategory(), (int)stats->currentWeaponType);
    VerbLog(b);
}

static void (*s_sheatheWeaponOrig)(CharacterHuman* me);
static void sheatheWeapon_hook(CharacterHuman* me)
{
    if (me && me == s_freeMoveAnchor && s_mode == MODE_FREE_MOVE && s_manualAttackOn
        && !s_loadGuardActive && !s_dcShutdownInProgress && !s_weaponDrawInProgress)
    {
        Inventory* inv = me->getInventory();
        if (me->weaponInHands && inv && inv->hasWeaponEquipped()
            && !me->isUnconcious() && !me->isDown() && !me->isRagdoll()
            && !s_lootUiSuspendActive)
        {
            static ULONGLONG lastLogMs = 0;
            ULONGLONG now = GetTickCount64();
            if (now - lastLogMs > 1000)
            {
                lastLogMs = now;
                VerbLog("[Ironwind] human_auto_sheathe_suppressed weapon_in_hands=1");
            }
            return;
        }
    }
    s_sheatheWeaponOrig(me);
}

// Free actions do not enter CombatClass's STARTUP/BLOCK state machine.  Read
// the SingleAnimation that Kenshi is actually advancing; completion or
// removal of that clip ends the action.  The watchdog only releases a clip
// that never appeared or has frozen, and is not the intended action duration.
static bool manualFreeClipFinished(AnimationClass* animation, CombatTechniqueData* technique,
                                   bool& seen, float& previousProgress, ULONGLONG startMs, float speed,
                                   ULONGLONG* lastAdvanceMs = nullptr)
{
    if (!technique) return false;
    AnimationClassBase::SingleAnimation* playing = manualPlayingTechnique(animation, technique);
    if (playing)
    {
        ULONGLONG now = GetTickCount64();
        float length = 0.0f;
        float progress = manualClipTimeline(playing, &length);
        if (progress >= 0.0f)
        {
            // The old instance of the same move can remain at frame 1.0.
            if (progress < 0.25f && !playing->mainState->hasEnded()) seen = true;
            if (seen)
            {
                float actualSpeed = speed > 0.1f ? speed : 1.0f;
                ULONGLONG earliestEnd = (ULONGLONG)(length * 1000.0f / actualSpeed);
                bool ended = playing->mainState->hasEnded() || progress >= 1.0f
                          || playing->currentFrameTime01 >= 0.98f
                          || (previousProgress > 0.45f && previousProgress - progress > 0.35f);
                if (progress > previousProgress + 0.001f)
                {
                    previousProgress = progress;
                    if (lastAdvanceMs) *lastAdvanceMs = now;
                }
                // This is a freeze guard, not an action timer: a slow clip is
                // valid as long as its real Ogre timeline keeps advancing.
                if (lastAdvanceMs && *lastAdvanceMs && progress > 0.05f && progress < 0.98f
                    && playing->weight > 0.10f && !playing->mainState->hasEnded()
                    && now - *lastAdvanceMs > 1500)
                {
                    char b[144]; sprintf_s(b, sizeof(b),
                        "[Ironwind] free_animation_watchdog stalled progress=%.2f ms=%llu",
                        progress, now - *lastAdvanceMs);
                    VerbLog(b);
                    return true;
                }
                if (ended && now - startMs >= earliestEnd) return true;
            }
        }
    }
    else if (seen && previousProgress > 0.45f) return true;
    // A clip which never acquired an Ogre timeline has not actually started.
    // Release that failed action promptly; live clips retain the long guard.
    ULONGLONG watchdog = previousProgress < 0.0f ? 3000 : 15000;
    if (startMs && GetTickCount64() - startMs > watchdog)
    {
        VerbLog("[Ironwind] free_animation_watchdog no_complete_clip");
        return true;
    }
    return false;
}

// v38: recompute the per-direction attack availability for the current
// anchor + weapon. Mirrors the free-attack commit eligibility exactly (pool
// scan first, chooser sampling as the fallback for directions the pool
// missed), so the HUD can distinguish directions a release can actually use.
// Game thread only; self-gated to ~600 ms and to anchor/weapon changes.
// Unknown (-1) is rendered white like an unavailable direction, and real
// commits overwrite the cache with ground truth.
static void manualRefreshDirMoveAvailability()
{
    if (!s_freeMoveAnchor || s_dcShutdownInProgress || s_loadGuardActive) return;
    if (!s_manualAttackOn || !s_directionalCombat) return;
    Character* me = s_freeMoveAnchor;
    CharStats* stats = me->getStats();
    if (!stats) return;
    CharacterHuman* human = me->isHuman();
    int wt = (int)stats->currentWeaponType;
    int hands = human && human->weaponInHands ? 1 : 0;
    ULONGLONG now = GetTickCount64();
    if (me == s_dirMoveAnchor && wt == s_dirMoveWeaponType && hands == s_dirMoveHands
        && now - s_dirMoveCheckedMs < 600)
        return;
    // v39: no pool requirement. The RVA-based pool may be empty, so chooser
    // sampling remains the fallback for capability display. Direction colour
    // describes the weapon/skill move set, never target distance.
    s_dirMoveAnchor = me;
    s_dirMoveWeaponType = wt;
    s_dirMoveHands = hands;
    s_dirMoveCheckedMs = now;
    bool poolHit[4] = { false, false, false, false };
    for (int i = 0; i < s_dcAttackCount; ++i)
    {
        CombatTechniqueData* t = s_dcAttack[i];
        if (!(wt >= 0 && wt < 0x16 && t->skillTypes[wt])) continue;
        if (t->isBlock || t->isDodge || t->animation.empty()) continue;
        if (t->numImpactPoints() > 0)
        {
            int slot = combatDirectionSlot((CutDirection)(int)t->impactPoint(0)->direction);
            if (slot >= 0) poolHit[slot] = true;
        }
    }
    bool chooseHit[4] = { false, false, false, false };
    if (s_chooseAttackOrig)
    {
        float reach = 0.0f;
        if (manualAttackChooserReach(me, reach))
        {
            const float sampleRange[4] = { 0.0f, 5.0f, 12.0f, 25.0f };
            for (int i = 0; i < 32; ++i)
            {
                CombatTechniqueData* t = s_chooseAttackOrig(stats, sampleRange[i / 8], reach,
                                                              nullptr, false);
                if (!t || t->isBlock || t->isDodge || t->animation.empty()) continue;
                if (t->numImpactPoints() > 0)
                {
                    int slot = combatDirectionSlot((CutDirection)(int)t->impactPoint(0)->direction);
                    if (slot >= 0) chooseHit[slot] = true;
                }
            }
        }
    }
    for (int i = 0; i < 4; ++i)
        s_dirMoveAvailable[i] = (poolHit[i] || chooseHit[i]) ? 1 : 0;
    // Log only when the picture changes (or once every 5 s while it holds) -
    // the 600 ms refresh itself spammed ~1.7 lines/s in the field (v40 log).
    static int s_lastLoggedKey = -1;
    static ULONGLONG s_lastLoggedMs = 0;
    int key = (poolHit[0] ? 1 : 0) | (poolHit[1] ? 2 : 0) | (poolHit[2] ? 4 : 0)
            | (poolHit[3] ? 8 : 0) | (chooseHit[0] ? 16 : 0) | (chooseHit[1] ? 32 : 0)
            | (chooseHit[2] ? 64 : 0) | (chooseHit[3] ? 128 : 0)
            | (wt << 8) | (hands << 16)
            | (manualUnarmedCombat(stats) ? (1 << 17) : 0);
    if (key != s_lastLoggedKey || now - s_lastLoggedMs >= 5000)
    {
        s_lastLoggedKey = key;
        s_lastLoggedMs = now;
        char b[176]; sprintf_s(b, sizeof(b),
            "[Ironwind] dir_move_avail pool=%d%d%d%d choose=%d%d%d%d weapon=%d hands=%d unarmed=%d",
            poolHit[0] ? 1 : 0, poolHit[1] ? 1 : 0, poolHit[2] ? 1 : 0, poolHit[3] ? 1 : 0,
            chooseHit[0] ? 1 : 0, chooseHit[1] ? 1 : 0, chooseHit[2] ? 1 : 0, chooseHit[3] ? 1 : 0,
            wt, hands, manualUnarmedCombat(stats) ? 1 : 0);
        VerbLog(b);
    }
}

static void manualFreeActionTick()
{
    manualRefreshDirMoveAvailability();
    if (!s_freeMoveAnchor || s_dcShutdownInProgress || s_loadGuardActive) return;
    CombatClass* cc = s_freeMoveAnchor->getCombatClass();
    if (!cc || !cc->animation) return;
    Character* me = cc->me;
    CharStats* stats = me ? me->getStats() : nullptr;
    if (!stats) return;
    ULONGLONG now = GetTickCount64();
    // v42: do not replay a free swing/guard over the native hit-reaction entry
    // after the original loop has just processed the impact.  The verdict hook
    // drops our action bookkeeping; the native reaction now exclusively owns
    // the combat animation layer until its state and short entry window end.
    if (manualHitReactionLive(me)) return;
    bool pendingAttack = s_attackPressMs && s_attackPressMs == s_attackAcceptedPressMs
                      && s_attackPressMs != s_attackInjectedPressMs && manualAttackCommitLive();
    bool pendingBlock = !s_settingAiBlocks
                     && s_blockPressMs && s_blockPressMs == s_blockAcceptedPressMs
                     && s_blockPressMs != s_freeBlockPressMs && s_blockHeld;
    if (s_freeGuardUpperIdle && (s_settingAiBlocks || !s_blockHeld || !s_manualAttackOn
        || s_mode != MODE_FREE_MOVE))
    {
        cc->animation->setCombatModeUpperIdle(false);
        if (s_freeGuardCombatOwned && cc->getCombatState() != STUMBLE
            && cc->getCombatState() != REACTION_BLOCK)
            cc->combatModeActive = false;
        s_freeGuardCombatOwned = false;
        s_freeGuardUpperIdle = false;
        s_freeBlockTech = nullptr;
        VerbLog("[Ironwind] free_block_release");
    }
    if (s_freeAttackTech && !s_freeAttackImpactPassed
        && s_freeAttackPressMs == s_attackAcceptedPressMs)
    {
        AnimationClassBase::SingleAnimation* clip = manualPlayingTechnique(cc->animation, s_freeAttackTech);
        float clipLength = 0.0f;
        float progress = manualClipTimeline(clip, &clipLength);
        CombatTechniqueData::ImpactPoint* impact = s_freeAttackTech->numImpactPoints() > 0
            ? s_freeAttackTech->impactPoint(0) : nullptr;
        float impactAt = impact ? impact->impactAnimationFrame : 0.50f;
        if (impactAt < 0.15f || impactAt > 0.95f) impactAt = 0.50f;
        float speed = s_freeAttackSpeed > 0.1f ? s_freeAttackSpeed : 1.0f;
        ULONGLONG earliestImpact = (ULONGLONG)(clipLength * impactAt * 600.0f / speed);
        if (s_freeAttackSeenAnim && progress >= impactAt && clipLength > 0.0f
            && now - s_freeAttackStartMs >= earliestImpact)
        {
            s_freeAttackImpactPassed = true;
            VerbLog("[Ironwind] free_attack_impact_passed damage=0");
        }
    }
    // RMB may replace the recovery of a free swing once its impact frame has
    // passed.  Never cancel startup or the damaging part of the arc: doing so
    // would yield an instant, visually fake hit (or lose the hit entirely).
    if (!s_settingAiBlocks && s_freeAttackTech && s_freeAttackImpactPassed && s_blockHeld
        && s_blockPressMs == s_blockAcceptedPressMs
        && s_blockPressMs >= s_freeAttackStartMs
        && s_blockPressMs != s_freeBlockPressMs)
    {
        AnimationClassBase::SingleAnimation* clip =
            manualPlayingTechnique(cc->animation, s_freeAttackTech);
        float clipLength = 0.0f;
        float progress = manualClipTimeline(clip, &clipLength);
        char b[160];
        sprintf_s(b, sizeof(b),
            "[Ironwind] attack_guard_cancel elapsed=%llu progress=%.2f weight=%.2f",
            now - s_freeAttackStartMs, progress, clip ? clip->weight : -1.0f);
        VerbLog(b);
        cc->animation->endCombatAnimation();
        manualFreeAttackClearTechnique(cc);
        s_freeAttackLastAdvanceMs = 0;
        s_attackCommitUntilMs = 0;
        s_attackRecoveryUntilMs = 0;
    }
    if (manualFreeClipFinished(cc->animation, s_freeAttackTech, s_freeAttackSeenAnim,
                               s_freeAttackLastProgress, s_freeAttackStartMs, s_freeAttackSpeed,
                               &s_freeAttackLastAdvanceMs))
    {
        char b[112]; sprintf_s(b, sizeof(b), "[Ironwind] free_attack_done elapsed=%llu",
                               now - s_freeAttackStartMs);
        VerbLog(b);
        manualFreeAttackClearTechnique(cc);
        s_freeAttackLastAdvanceMs = 0;
        s_attackRecoveryUntilMs = now + (ULONGLONG)s_attackRecoveryMs;
    }
    bool freeAttackLive = s_freeAttackTech != nullptr;
    bool freeBlockLive = s_freeGuardUpperIdle && s_freeBlockTech;
    if (freeAttackLive || freeBlockLive)
    {
        cc->animation->setCombatMode(true);
        s_freeAnimCombatMode = true;
        if (freeAttackLive)
        {
            cc->animation->runCombatAnimation(s_freeAttackTech, s_freeAttackSpeed, std::string());
            if (s_verboseLog)
            {
                static ULONGLONG lastAttackClipTraceMs = 0;
                if (now - lastAttackClipTraceMs >= 500)
                {
                    lastAttackClipTraceMs = now;
                    AnimationClassBase::SingleAnimation* clip =
                        manualPlayingTechnique(cc->animation, s_freeAttackTech);
                    float length = 0.0f;
                    float progress = manualClipTimeline(clip, &length);
                    char b[200];
                    sprintf_s(b, sizeof(b),
                        "[Ironwind] free_attack_clip progress=%.2f weight=%.2f combat=%d moving=%d override=%d",
                        progress, clip ? clip->weight : -1.0f,
                        cc->combatModeActive ? 1 : 0,
                        (s_wHeld || s_aHeld || s_sHeld || s_dHeld) ? 1 : 0,
                        me->movement && me->movement->animationOverride ? 1 : 0);
                    VerbLog(b);
                }
            }
        }
        else if (cc->getCombatState() != STUMBLE
              && cc->getCombatState() != REACTION_BLOCK)
        {
            // Block techniques have zero animation weight when CombatClass's
            // combat flag is down (confirmed in the v22 free_block_clip log).
            // WASD clears that flag during movement, so restore it for the
            // manual guard after movement has been integrated this frame.
            if (!cc->combatModeActive)
            {
                cc->combatModeActive = true;
                s_freeGuardCombatOwned = true;
            }
            cc->animation->runCombatAnimation(s_freeBlockTech, s_freeBlockSpeed, std::string());
            if (s_verboseLog)
            {
                static ULONGLONG lastGuardTraceMs = 0;
                if (now - lastGuardTraceMs >= 500)
                {
                    lastGuardTraceMs = now;
                    AnimationClassBase::SingleAnimation* clip =
                        manualPlayingTechnique(cc->animation, s_freeBlockTech);
                    float length = 0.0f;
                    float progress = manualClipTimeline(clip, &length);
                    char b[192]; sprintf_s(b, sizeof(b),
                        "[Ironwind] free_block_clip anim=%s found=%d progress=%.2f weight=%.2f combat=%d",
                        s_freeBlockTech->animation.c_str(), clip ? 1 : 0, progress,
                        clip ? clip->weight : -1.0f, cc->combatModeActive ? 1 : 0);
                    VerbLog(b);
                }
            }
        }
    }
    else if (s_freeAnimCombatMode)
    {
        swordStateEnum state = cc->getCombatState();
        // The moving-swing path deliberately keeps combatModeActive true so
        // the swing remains visible over WASD.  That flag used to make this
        // cleanup skip endCombatAnimation(), leaving a stale combat layer for
        // the next movement input.  Finish our clip regardless of the flag,
        // but wait for a real stagger/parry reaction to finish first.
        if (!manualHitReactionLive(me)
            && (!cc->combatModeActive || (state != STUMBLE && state != REACTION_BLOCK)))
        {
            bool combatWasActive = cc->combatModeActive;
            cc->animation->endCombatAnimation();
            cc->animation->setCombatMode(s_manualAttackOn && s_mode == MODE_FREE_MOVE);
            char b[128]; sprintf_s(b, sizeof(b),
                "[Ironwind] free_action_anim_cleared combat_was=%d state=%d",
                combatWasActive ? 1 : 0, (int)state);
            VerbLog(b);
            s_freeAnimCombatMode = false;
            manualFreeAttackClearTechnique(cc);
        }
    }
    if (!s_manualAttackOn || s_mode != MODE_FREE_MOVE) return;
    if (!pendingAttack && !pendingBlock) return;
    manualBlockBuildCopies();
    manualAttackLoadPool();
    Character* foe = manualAttackFindFoe(cc, me);
    ULONGLONG ap = s_attackPressMs;
    if (ap && ap == s_attackAcceptedPressMs && ap != s_attackInjectedPressMs
        && manualAttackCommitLive() && !s_attackActive && !s_freeAttackTech)
    {
        // Draw once for this accepted attack, never on an idle timer or while a
        // swing is playing. The old 750-ms loop restarted clips.
        if (s_weaponDrawAttemptMs != ap)
        {
            s_weaponDrawAttemptMs = ap;
            manualEnsureWeaponReady(me, stats);
        }
        bool useFreeSwing = foe == nullptr;
        if (foe && s_attackEngagePressMs != ap)
        {
            s_attackEngagePressMs = ap;
            // Start the exact native AttackState synchronously from this input.
            // No focused task is created: the AI never owns approach, timing,
            // facing, retries, or a follow-up.  A rejected native initializer
            // falls back to the existing visual-only air swing below.
            useFreeSwing = !manualAttackStartDirect(cc, foe, ap);
        }
        if (useFreeSwing)
        {
            CharacterHuman* human = me->isHuman();
            Inventory* inv = me->getInventory();
            if (human && inv && inv->hasWeaponEquipped() && !human->weaponInHands)
            {
                static ULONGLONG loggedWaitPress = 0;
                if (loggedWaitPress != ap)
                {
                    loggedWaitPress = ap;
                    VerbLog("[Ironwind] free_attack_wait_for_weapon_in_hand");
                }
                if (now - ap < 1500)
                    s_attackCommitUntilMs = now + 100;
                else
                {
                    s_attackInjectedPressMs = ap;
                    s_attackCommitUntilMs = 0;
                    VerbLog("[Ironwind] free_attack_cancelled_weapon_not_drawn");
                }
                return;
            }
            CombatTechniqueData* pick = nullptr;
            int wt = (int)stats->currentWeaponType;
            int wanted = (int)s_attackDesired;
            CombatTechniqueData* poolDirectional[64];
            int poolDirectionalCount = 0;
            for (int i = 0; i < s_dcAttackCount && poolDirectionalCount < 64; ++i)
            {
                CombatTechniqueData* t = s_dcAttack[i];
                bool skill = wt >= 0 && wt < 0x16 && t->skillTypes[wt];
                bool dir = t->numImpactPoints() > 0 && (int)t->impactPoint(0)->direction == wanted;
                if (skill && dir && !t->isBlock && !t->isDodge && !t->animation.empty())
                    poolDirectional[poolDirectionalCount++] = t;
            }
            if (poolDirectionalCount)
            {
                int start = (int)(UtilityT::random() * poolDirectionalCount);
                if (start >= poolDirectionalCount) start = poolDirectionalCount - 1;
                for (int i = 0; i < poolDirectionalCount; ++i)
                {
                    CombatTechniqueData* t = poolDirectional[(start + i) % poolDirectionalCount];
                    if (t != s_lastFreeAttackTech || i == poolDirectionalCount - 1)
                    { pick = t; break; }
                }
            }
            // The global pool used above contains defence techniques on this
            // build.  Ask the game's weapon-aware chooser for an attack when
            // the pool has no swing, trying several rolls for the aimed cut.
            if (!pick && s_chooseAttackOrig)
            {
                float reach = 0.0f;
                bool chooserEligible = manualAttackChooserReach(me, reach);
                const float sampleRange[4] = { 0.0f, 5.0f, 12.0f, 25.0f };
                CombatTechniqueData* choices[32];
                int choiceCount = 0;
                for (int i = 0; chooserEligible && i < 32; ++i)
                {
                    CombatTechniqueData* t = s_chooseAttackOrig(stats, sampleRange[i / 8], reach,
                                                                  nullptr, false);
                    if (!t || t->isBlock || t->isDodge || t->animation.empty()) continue;
                    bool duplicate = false;
                    for (int j = 0; j < choiceCount; ++j)
                        if (choices[j] == t) { duplicate = true; break; }
                    if (!duplicate) choices[choiceCount++] = t;
                }
                // Retain Kenshi's weapon/skill eligibility, but rotate among
                // the distinct moves it actually offered for this character.
                CombatTechniqueData* directional[32]; int directionalCount = 0;
                for (int i = 0; i < choiceCount; ++i)
                    if (choices[i]->numImpactPoints() > 0
                        && (int)choices[i]->impactPoint(0)->direction == wanted)
                        directional[directionalCount++] = choices[i];
                CombatTechniqueData** candidates = directional;
                int candidateCount = directionalCount;
                if (candidateCount)
                {
                    int start = (int)(UtilityT::random() * candidateCount);
                    if (start >= candidateCount) start = candidateCount - 1;
                    for (int i = 0; i < candidateCount; ++i)
                    {
                        CombatTechniqueData* t = candidates[(start + i) % candidateCount];
                        if (t != s_lastFreeAttackTech || i == candidateCount - 1) { pick = t; break; }
                    }
                }
                char b[144]; sprintf_s(b, sizeof(b),
                    "[Ironwind] attack_eligible unique=%d directional=%d weapon=%d unarmed=%d",
                    choiceCount, directionalCount, wt,
                    manualUnarmedCombat(stats) ? 1 : 0);
                VerbLog(b);
            }
            s_attackInjectedPressMs = ap;
            s_attackCommitUntilMs = 0;
            if (pick)
            {
                s_lastFreeAttackTech = pick;
                s_attackDesiredForSwing = wanted;
                {   // v38 ground truth: this direction just proved playable
                    int slot = combatDirectionSlot((CutDirection)wanted);
                    if (slot >= 0) s_dirMoveAvailable[slot] = 1;
                }
                std::string suffix;
                float speed = stats->attackSpeed * pick->animSpeedMultiplier;
                if (speed < 0.1f) speed = 0.1f;
                cc->animation->endCombatAnimation();
                cc->animation->setCombatMode(true);
                s_freeAnimCombatMode = true;
                cc->animation->startCombatAnimation(pick, speed, std::string());
                s_freeAttackTech = pick;
                cc->currentTechnique = pick;
                cc->currentTechniqueHasFinished = false;
                if (me->movement) cc->attackStartPos = me->movement->pos;
                cc->calculatedTargetsInAttackZoneThisFrame = false;
                s_freeAttackPressMs = ap;
                s_freeAttackImpactPassed = false;
                s_freeAttackSpeed = speed;
                s_freeAttackStartMs = now;
                AnimationClassBase::SingleAnimation* started = manualPlayingTechnique(cc->animation, pick);
                float clipLength = 0.0f;
                float clipProgress = manualClipTimeline(started, &clipLength);
                s_freeAttackSeenAnim = started && started->currentFrameTime01 >= 0.0f
                    && started->currentFrameTime01 < 0.25f;
                s_freeAttackLastProgress = -1.0f;
                s_freeAttackLastAdvanceMs = now;
                // A free swing now means empty space only.  Its impact point is
                // retained solely so RMB cannot cancel before the visual blade
                // has crossed the damaging phase; it never applies damage.
                AnimationData* ad = cc->animation->getAnimationData(pick->animation);
                char b[384]; sprintf_s(b, sizeof(b), "[Ironwind] free_attack_start anim=%s dir=%d weapon=%d hand=%d reach=%.2f weapon_length=%.2f speed=%.2f timeline=%.2f/%.2f frame01=%.2f found=%d combat=%d action=%d",
                                       pick->animation.c_str(), wanted, wt, human && human->weaponInHands ? 1 : 0,
                                       manualOriginalMeleeReach(me), stats->getCurrentWeaponLength(), speed,
                                       clipProgress, clipLength,
                                       started ? started->currentFrameTime01 : -1.0f, ad ? 1 : 0,
                                       ad ? (int)ad->isCombatMode.key : -1, ad && ad->isAction ? 1 : 0);
                VerbLog(b);
            }
            else
            {
                // No technique for this direction: record it so v48 keeps
                // the arrow white, but do NOT lock the HUD or the input:
                // the next stroke can retarget instantly (user 2026-10-20).
                int slot = combatDirectionSlot((CutDirection)wanted);
                if (slot >= 0) s_dirMoveAvailable[slot] = 0;
                char b[144]; sprintf_s(b, sizeof(b), "[Ironwind] free_attack_no_technique pool=%d reach=%.1f weapon=%d unarmed=%d",
                                       s_dcAttackCount, manualOriginalMeleeReach(me), wt,
                                       manualUnarmedCombat(stats) ? 1 : 0);
                VerbLog(b);
            }
        }
    }

    ULONGLONG bp = s_blockPressMs;
    // A native swing and a free guard must never own the combat animation layer
    // at the same time. v82 kept CHOP alive for the visual recovery, but this
    // path could start `blk right` over that live attack. Releasing RMB then
    // crashed vanilla AnimationClass::endCombatAnimation at exe+0x5B357A.
    // Before impact, retain the block press without cancelling damage. After
    // impact, leave CHOP first and return; the next main-loop frame confirms
    // attack ownership is gone before the existing guard path may play.
    if (!s_settingAiBlocks && bp && bp == s_blockAcceptedPressMs && bp != s_freeBlockPressMs
        && s_blockHeld && s_attackActive)
    {
        if (!s_attackNativeImpactFinished)
        {
            static ULONGLONG lastImpactWaitPress = 0;
            if (lastImpactWaitPress != bp)
            {
                lastImpactWaitPress = bp;
                VerbLog("[Ironwind] attack_guard_wait_for_impact");
            }
            return;
        }

        swordStateEnum beforeGuardHandoff = cc->getCombatState();
        if (beforeGuardHandoff == CHOP_WEAPON && s_setCombatStateOrig)
        {
            s_attackGuardHandoffPending = true;
            s_setCombatStateOrig(cc, DECISION);
            swordStateEnum afterGuardHandoff = cc->getCombatState();
            if (afterGuardHandoff == STARTUP_STATE || afterGuardHandoff == CHOP_WEAPON)
                s_attackGuardHandoffPending = false;
            char gb[176];
            sprintf_s(gb, sizeof(gb),
                "[Ironwind] attack_guard_handoff before=%d after=%d accepted=%d",
                (int)beforeGuardHandoff, (int)afterGuardHandoff,
                s_attackGuardHandoffPending ? 1 : 0);
            VerbLog(gb);
        }
        else if (beforeGuardHandoff != STARTUP_STATE
              && beforeGuardHandoff != CHOP_WEAPON)
        {
            s_attackGuardHandoffPending = true;
            VerbLog("[Ironwind] attack_guard_handoff state_already_exited");
        }
        return;
    }
    if (!s_settingAiBlocks && bp && bp == s_blockAcceptedPressMs && bp != s_freeBlockPressMs
        && s_blockHeld
        && !s_attackActive && !s_freeAttackTech)
    {
        s_freeBlockPressMs = bp;
        CombatTechniqueData* pick = nullptr;
        for (int i = 0; i < s_dcBlockCount; ++i)
        {
            CombatTechniqueData* t = s_dcBlock[i];
            if (t->numImpactPoints() > 0 && (int)t->impactPoint(0)->direction == (int)s_blockDesired)
            { pick = t; break; }
        }
        if (!pick && s_dcBlockCount) pick = s_dcBlock[0];
        if (pick)
        {
            std::string suffix;
            float speed = stats->blockSpeed * pick->animSpeedMultiplier;
            if (speed < 0.1f) speed = 0.1f;
            cc->animation->setCombatModeUpperIdle(true);
            s_freeGuardUpperIdle = true;
            if (!cc->combatModeActive)
            {
                cc->combatModeActive = true;
                s_freeGuardCombatOwned = true;
            }
            cc->animation->setCombatMode(true);
            s_freeAnimCombatMode = true;
            cc->animation->startCombatAnimation(pick, speed, std::string());
            s_freeBlockTech = pick;
            s_freeBlockSpeed = speed;
            char b[160]; sprintf_s(b, sizeof(b), "[Ironwind] free_block_play anim=%s dir=%d", pick->animation.c_str(), (int)s_blockDesired);
            VerbLog(b);
        }
        else VerbLog("[Ironwind] free_block_no_technique");
    }
}

static bool manualAttackMarkWorldNotified(Character* victim);

static bool manualAttackQueueVictimResponse(Character* victim, Character* attacker,
                                            const char* source)
{
    if (!victim || !attacker) return false;
    for (int i = 0; i < 8; ++i)
    {
        if (s_victimResponseVictim[i] == victim)
        {
            s_victimResponseAttacker[i] = attacker;
            s_victimResponseSource[i] = source;
            return true;
        }
    }
    for (int i = 0; i < 8; ++i)
    {
        if (!s_victimResponseVictim[i])
        {
            s_victimResponseVictim[i] = victim;
            s_victimResponseAttacker[i] = attacker;
            s_victimResponseSource[i] = source;
            return true;
        }
    }
    VerbLog("[Ironwind] attack_victim_response_queue_full");
    return false;
}

// Restore the victim-side half of vanilla's focused/unprovoked task layer only
// after the victim's authoritative _getHit callback. Direct AttackState must
// never give the controlled player an AI combat task, but a neutral character
// actually hit by the player still needs the native awareness, campaign and
// retaliation entry points. The retaliation order is queued until the current
// original game loop returns so the first victim does not observe the old
// neutral relation. The task remains entirely on the VICTIM, so it cannot
// time, move or chain the player's swing. A click, attack start, miss or block
// never calls this helper.
static void manualAttackProvokeVictim(Character* victim, Character* attacker,
                                      bool wasNeutral, const char* source)
{
    if (!victim || !attacker || victim == attacker
        || manualVPlayerTeamMember(victim)
        || !manualAttackMarkWorldNotified(victim)) return;

    bool deferred = false;
    if (wasNeutral)
    {
        victim->notifyTheCampaignOfAnAttack(attacker);
        deferred = manualAttackQueueVictimResponse(victim, attacker, source);
    }
    victim->attackingYou(attacker, true, true);
    if (!deferred) victim->attackTarget(attacker);

    CombatClass* victimCombat = victim->getCombatClass();
    char b[256];
    sprintf_s(b, sizeof(b),
        "[Ironwind] attack_victim_provoked source=%s was_neutral=%d deferred=%d victim_enemy=%d attacker_enemy=%d target_set=%d",
        source ? source : "unknown", wasNeutral ? 1 : 0,
        deferred ? 1 : 0,
        victim->isEnemy(attacker, true) ? 1 : 0,
        attacker->isEnemy(victim, true) ? 1 : 0,
        victimCombat && victimCombat->currentTarget == attacker ? 1 : 0);
    VerbLog(b);
}

// Start exactly one engine-owned AttackState from the accepted player input.
// This bypasses CombatClassAI's focused-order decision layer but retains the
// native technique chooser, attack-zone construction, hostile-victim
// notification, animation, block/dodge verdict, damage, armour and XP paths.
// A neutral-origin victim's notification is committed only by _getHit.
static bool manualAttackStartDirect(CombatClass* cc, Character* foe, ULONGLONG press)
{
    if (!cc || !cc->animation || !s_setCombatStateOrig || !foe || !cc->me
        || press == 0 || press != s_attackAcceptedPressMs
        || press == s_attackInjectedPressMs || !manualAttackCommitLive()) return false;
    Character* me = cc->me;
    if (s_mode != MODE_FREE_MOVE || me != s_freeMoveAnchor
        || !manualVTargetAllowed(me, foe) || manualHitReactionLive(me)
        || s_attackActive) return false;

    swordStateEnum before = cc->getCombatState();
    if (before == STUMBLE || before == REACTION_BLOCK) return false;

    // Consume the press before entering game code so a re-entrant update cannot
    // initialise it twice.  The direct-initialising flag is the only permission
    // granted to the attack-direction chooser and the two anti-AI choke points.
    s_attackInjectedPressMs = press;
    s_attackCommitUntilMs = 0;
    s_attackDesiredForSwing = s_attackDesired;
    s_attackActive = true;
    s_attackSawChop = false;
    s_attackObservedUnfinished = false;
    s_attackStateRecovered = false;
    s_attackNativeImpactFinished = false;
    s_attackGuardHandoffPending = false;
    s_attackActiveSinceMs = GetTickCount64();
    s_attackAdvancedFrameSerial = 0;
    s_attackAdvanceLogMs = 0;
    s_attackNativeContacts = 0;
    s_attackNativeHits = 0;
    s_attackNativeBlocks = 0;
    s_attackNativeDodges = 0;
    for (int wi = 0; wi < 8; ++wi)
    {
        s_attackWorldNotified[wi] = nullptr;
        s_attackNeutralContact[wi] = nullptr;
    }
    s_attackDirectInitialising = true;

    Character* previousTarget = cc->currentTarget;
    bool previousCombatMode = cc->combatModeActive;
    bool previousTechniqueFinished = cc->currentTechniqueHasFinished;
    cc->currentTarget = foe;
    cc->combatModeActive = true;
    cc->animation->setCombatMode(true);

    // Direct control deliberately skips the AI DECISION/WAIT frames which, in
    // vanilla, consume the previous technique's completion flag. Leaving that
    // stale true caused every attack after the first to enter CHOP and be
    // mistaken for complete on its first (15-32 ms) update. A fresh native
    // initialisation must begin unfinished just as the ordinary AI path does.
    cc->currentTechniqueHasFinished = false;

    CombatState* nativeState = (cc->*DcRobResult<DcGetStateClassTag>::ptr)(CHOP_WEAPON);
    AttackState* attackState = nativeState ? static_cast<AttackState*>(nativeState) : nullptr;
    bool started = attackState && attackState->initialiseAttack(foe);
    if (started)
    {
        // initialiseAttack has already selected and started the native technique.
        // Enter only its CHOP state; no AI DECISION/WAIT/approach frame exists.
        cc->nextMove = CHOP_WEAPON;
        s_setCombatStateOrig(cc, CHOP_WEAPON);
        s_attackSawChop = cc->getCombatState() == CHOP_WEAPON;
        s_attackObservedUnfinished = !cc->currentTechniqueHasFinished;
    }
    s_attackDirectInitialising = false;

    float dist = (me->movement && foe->movement)
        ? me->movement->pos.distance(foe->movement->pos) : -1.0f;
    char b[320];
    sprintf_s(b, sizeof(b),
        "[Ironwind] attack_direct_native started=%d state_before=%d state_after=%d dir=%d dist=%.1f enemy=%d target_slave=%d prone=%d down=%d unconscious=%d press_age=%llu",
        started ? 1 : 0, (int)before, (int)cc->getCombatState(), s_attackDesiredForSwing,
        dist, me->isEnemy(foe, true) ? 1 : 0, (int)foe->isSlave(),
        (int)foe->getProneState(), foe->isDown() ? 1 : 0, foe->isUnconcious() ? 1 : 0,
        GetTickCount64() - press);
    VerbLog(b);

    if (!started)
    {
        cc->currentTarget = previousTarget;
        cc->combatModeActive = previousCombatMode;
        cc->currentTechniqueHasFinished = previousTechniqueFinished;
        manualAttackClearActiveRuntime();
        return false;
    }
    return true;
}

static void manualAttackFinish(CombatClass* cc, bool playerMoving, const char* reason)
{
    ULONGLONG now = GetTickCount64();
    swordStateEnum st = cc ? cc->getCombatState() : COMBAT_FINISHED;
    char b[320];
    sprintf_s(b, sizeof(b),
        "[WASDCombat] attack_done state=%d ms=%llu swung=%d cm=%d tgt=%d moving=%d contacts=%d hits=%d blocks=%d dodges=%d reason=%s",
        (int)st, now - s_attackActiveSinceMs, s_attackSawChop ? 1 : 0,
        cc && cc->combatModeActive ? 1 : 0, cc && cc->currentTarget ? 1 : 0,
        playerMoving ? 1 : 0, s_attackNativeContacts, s_attackNativeHits,
        s_attackNativeBlocks, s_attackNativeDodges,
        reason ? reason : "state_exit");
    VerbLog(b);
    bool completedChop = s_attackSawChop;
    manualAttackClearActiveRuntime();
    s_attackCommitUntilMs = 0;      // movement is yours the frame the native attempt ends
    if (completedChop)
        s_attackRecoveryUntilMs = now + (ULONGLONG)s_attackRecoveryMs;
    // No focused order exists in the direct path.  The next input is eligible
    // immediately after the native state exits (subject to configured recovery).
    VerbLog("[Ironwind] attack_direct_control_restored");
}

// Every observer of a state exit must pass through the same recovery gate.
// v80 put this check only in the post-main-loop lifecycle pass, but field logs
// showed manualAttackTrack inside CombatClass::go could finish the attack first
// (0/234/329 ms), so the later gate never saw it.  State alone is insufficient:
// recover once only when the exact native technique still owns a live,
// unfinished Ogre clip.  No elapsed-time estimate participates.
static bool manualAttackStillSwingingOrRecover(CombatClass* cc,
                                               const char* source)
{
    if (!cc) return false;
    swordStateEnum st = cc->getCombatState();
    // STARTUP is shared by two unrelated native transitions: our attack entry
    // and a victim's STARTUP -> STUMBLE hand-off.  Once _getHit has claimed the
    // character, the reaction must win before STARTUP is classified as part of
    // the outgoing swing.  v89 did this check below the STARTUP/CHOP test, so a
    // hit received during CHOP could leave both ownership latches alive: the
    // attack waited for STARTUP to finish while the queued STUMBLE waited for
    // the attack owner to yield.
    if (manualHitReactionLive(cc->me)) return false;
    if (s_attackGuardHandoffPending
        && st != STARTUP_STATE && st != CHOP_WEAPON)
        return false;
    if (st == STARTUP_STATE || st == CHOP_WEAPON) return true;

    AnimationClassBase::SingleAnimation* clip = nullptr;
    float clipLength = 0.0f, progress = -1.0f;
    if (cc->animation && cc->currentTechnique)
    {
        clip = manualPlayingTechnique(cc->animation, cc->currentTechnique);
        progress = manualClipTimeline(clip, &clipLength);
    }
    bool unfinished = cc->currentTechnique && !cc->currentTechniqueHasFinished;
    bool visualRecovery = s_attackNativeImpactFinished && clip
        && progress >= 0.0f && progress < 0.98f;
    if (!s_attackStateRecovered && (unfinished || visualRecovery) && s_setCombatStateOrig
        && progress >= 0.0f && progress < 0.98f)
    {
        s_attackStateRecovered = true;
        cc->combatModeActive = true;
        cc->nextMove = CHOP_WEAPON;
        s_setCombatStateOrig(cc, CHOP_WEAPON);
        swordStateEnum restored = cc->getCombatState();
        char rb[240];
        sprintf_s(rb, sizeof(rb),
            "[Ironwind] attack_native_state_recovered source=%s state_was=%d state_now=%d progress=%.3f length=%.3f",
            source ? source : "unknown", (int)st, (int)restored,
            progress, clipLength);
        VerbLog(rb);
        return restored == STARTUP_STATE || restored == CHOP_WEAPON;
    }

    if (unfinished || visualRecovery)
    {
        char eb[240];
        sprintf_s(eb, sizeof(eb),
            "[Ironwind] attack_native_early_exit source=%s state=%d technique_finished=%d impact_finished=%d recovered=%d clip=%d progress=%.3f",
            source ? source : "unknown", (int)st,
            cc->currentTechniqueHasFinished ? 1 : 0,
            s_attackNativeImpactFinished ? 1 : 0,
            s_attackStateRecovered ? 1 : 0, clip ? 1 : 0, progress);
        VerbLog(eb);
    }
    return false;
}

// The direct initializer enters CHOP synchronously.  The swing is over when the
// native machine leaves STARTUP/CHOP_WEAPON. There is deliberately no Ironwind
// duration timer: weapon and skill animation speed remain authoritative.
static void manualAttackTrack(CombatClass* cc, bool playerMoving)
{
    if (!s_attackActive) return;
    swordStateEnum st = cc->getCombatState();
    if (st == CHOP_WEAPON) s_attackSawChop = true;
    if (!manualAttackStillSwingingOrRecover(cc, "combat_track"))
    {
        st = cc->getCombatState();
        const char* reason = manualHitReactionLive(cc->me) ? "hit_reaction"
                           : (st == TARGET_PATHFINDING || st == TARGET_PATHFINDING_STARTUP)
                           ? "unexpected_pathfinding" : "state_exit";
        manualAttackFinish(cc, playerMoving, reason);
    }
}

// Execute one frame of the native combat base state without executing
// CombatClassAI::update/decisionState. initialiseAttack() has already chosen the
// vanilla technique, built its target context and started its animation.
// CombatClass::update dispatches CHOP directly to CombatClass::attackState,
// which owns animation sections, attack-zone checks, block/dodge verdicts,
// damage and XP.  CombatClassAI::update normally performs the separate
// CHOP -> DECISION hand-off after the technique reports completion.  We cannot
// run that whole AI update (it also owns targeting, approach and follow-up
// attacks), so the direct path reproduces only that native completion hand-off
// below. AttackState::update is only the AI-side target/movement helper; v68
// called it directly and left the real attack executor untouched.
//
// combatGo_hook is the preferred call site. The main-loop lifecycle pass calls
// the same helper as a fallback because a direct attack has no AI task and the
// engine can therefore stop scheduling CombatClass::go. The frame serial makes
// those two paths mutually exclusive, so native timers and hit checks cannot be
// advanced twice in one frame.
static bool manualAttackAdvanceNativeState(CombatClass* cc, float frameTime,
                                           const char* source)
{
    if (!s_attackActive || !cc || !s_combatBaseUpdateNative
        || cc->me != s_freeMoveAnchor
        || manualHitReactionLive(cc->me)
        || cc->getCombatState() != CHOP_WEAPON) return false;
    if (s_attackAdvancedFrameSerial == s_manualMainFrameSerial) return true;

    // `currentTechniqueHasFinished` marks the end of Kenshi's damaging attack
    // phase, not the end of the visible clip.  v81 transitioned to DECISION at
    // this edge, truncating Great sword low slash around 812 ms while the same
    // clip's real timeline lasts about 1562 ms.  Once all native impact work is
    // finished, stop calling attackState (no duplicate contacts) and let the
    // existing Ogre clip advance naturally to its own end.
    if (s_attackNativeImpactFinished)
    {
        s_attackAdvancedFrameSerial = s_manualMainFrameSerial;
        AnimationClassBase::SingleAnimation* clip =
            manualPlayingTechnique(cc->animation, cc->currentTechnique);
        float clipLength = 0.0f;
        float progress = manualClipTimeline(clip, &clipLength);
        if (clip && progress >= 0.0f && progress < 0.98f)
        {
            ULONGLONG nowRecovery = GetTickCount64();
            if (nowRecovery - s_attackAdvanceLogMs >= 500)
            {
                s_attackAdvanceLogMs = nowRecovery;
                char rb[224];
                sprintf_s(rb, sizeof(rb),
                    "[Ironwind] attack_native_recovery_clip source=%s progress=%.3f length=%.3f state=%d",
                    source ? source : "unknown", progress, clipLength,
                    (int)cc->getCombatState());
                VerbLog(rb);
            }
            return true;
        }
        if (s_setCombatStateOrig)
        {
            s_setCombatStateOrig(cc, DECISION);
            char fb[192];
            sprintf_s(fb, sizeof(fb),
                "[Ironwind] attack_native_finish_transition source=clip_finished progress=%.3f clip=%d",
                progress, clip ? 1 : 0);
            VerbLog(fb);
        }
        return true;
    }

    CombatState* nativeState = (cc->*DcRobResult<DcGetStateClassTag>::ptr)(CHOP_WEAPON);
    if (!nativeState || nativeState->combat != cc)
    {
        ULONGLONG nowBad = GetTickCount64();
        if (nowBad - s_attackAdvanceLogMs >= 500)
        {
            s_attackAdvanceLogMs = nowBad;
            VerbLog("[Ironwind] attack_native_advance_invalid");
        }
        return false;
    }

    s_attackAdvancedFrameSerial = s_manualMainFrameSerial;
    swordStateEnum before = cc->getCombatState();
    float combatTimerBefore = cc->stateTimer;
    float attackTimerBefore = nativeState->stateTimer;
    int comboBefore = cc->currentComboSection;
    bool finishedBefore = cc->currentTechniqueHasFinished;
    if (!finishedBefore) s_attackObservedUnfinished = true;
    cc->frameTIME = frameTime;
    s_combatBaseUpdateNative(cc, frameTime);
    swordStateEnum after = cc->getCombatState();

    // The flag ends native impact evaluation, but the visible recovery frames
    // may still be playing.  Latch that phase and stop advancing attackState;
    // transition only after the exact clip reaches its real end.  If no live
    // clip can be observed, retain the old immediate native cleanup as a safe
    // fallback rather than inventing a duration.
    if (after == CHOP_WEAPON && s_attackObservedUnfinished
        && cc->currentTechniqueHasFinished
        && s_setCombatStateOrig)
    {
        AnimationClassBase::SingleAnimation* clip =
            manualPlayingTechnique(cc->animation, cc->currentTechnique);
        float clipLength = 0.0f;
        float progress = manualClipTimeline(clip, &clipLength);
        if (clip && progress >= 0.0f && progress < 0.98f)
        {
            s_attackNativeImpactFinished = true;
            char ib[208];
            sprintf_s(ib, sizeof(ib),
                "[Ironwind] attack_native_impact_finished progress=%.3f length=%.3f",
                progress, clipLength);
            VerbLog(ib);
        }
        else
        {
            s_setCombatStateOrig(cc, DECISION);
            after = cc->getCombatState();
            char fb[192];
            const char* finishSource = !clip ? "technique_finished_no_clip"
                : progress >= 0.98f ? "technique_finished_clip_complete"
                : "technique_finished_clip_invalid";
            sprintf_s(fb, sizeof(fb),
                "[Ironwind] attack_native_finish_transition source=%s progress=%.3f clip=%d",
                finishSource, progress, clip ? 1 : 0);
            VerbLog(fb);
        }
    }

    ULONGLONG now = GetTickCount64();
    if (after != before || now - s_attackAdvanceLogMs >= 500)
    {
        s_attackAdvanceLogMs = now;
        char b[320];
        sprintf_s(b, sizeof(b),
            "[Ironwind] attack_native_advance source=%s before=%d after=%d dt=%.4f combat_timer=%.3f->%.3f attack_timer=%.3f->%.3f combo=%d->%d tech=%d target=%d finished=%d->%d",
            source ? source : "unknown", (int)before, (int)after, frameTime,
            combatTimerBefore, cc->stateTimer, attackTimerBefore,
            nativeState->stateTimer, comboBefore, cc->currentComboSection,
            cc->currentTechnique ? 1 : 0, cc->currentTarget ? 1 : 0,
            finishedBefore ? 1 : 0,
            cc->currentTechniqueHasFinished ? 1 : 0);
        VerbLog(b);
    }
    return true;
}

// CombatClassAI::update is the outer owner of the controlled character's
// combat state. v78 intercepted go(), but field logs still showed ten direct
// CHOPs being replaced by DECISION before go() was reached (0-328 ms, no
// incoming hit). While one player-authorised CHOP is live, run only the native
// CombatClass base state and skip this AI decision layer altogether. Hit
// reactions remain outside this branch and therefore stay fully vanilla.
static void (*s_combatAiUpdateOrig)(CombatClassAI*, float);
static void combatAiUpdate_hook(CombatClassAI* thisptr, float frameTime)
{
    if (!s_dcShutdownInProgress && !s_loadGuardActive
        && s_mode == MODE_FREE_MOVE && s_manualAttackOn
        && thisptr && thisptr->me == s_freeMoveAnchor
        && s_attackActive && !manualHitReactionLive(thisptr->me)
        && thisptr->getCombatState() == CHOP_WEAPON)
    {
        s_retreatLockGoSuppressed = true;
        manualAttackAdvanceNativeState(thisptr, frameTime, "combat_update");
        return;
    }
    s_combatAiUpdateOrig(thisptr, frameTime);
}

// CombatClass::_iHitYouAreYouHit - the block VERDICT Character::hitByMeleeAttack
// consults (returns HIT_SWORD = blocked, HIT_MISSED = dodged, else hit).  Runs on
// the game thread from the ATTACKER's impact check.  Inside the perfect window we
// answer HIT_SWORD for the controlled character and skip the original (its only
// entry side effect, the attacker notify, is repeated by the caller anyway; its
// crime-witness path belongs to the "hit landed" outcome).  Everything else -
// including the HELD skill block, which vanilla resolves from the block
// technique currently playing - forwards untouched.
static HitMaterialType (*s_iHitOrig)(CombatClass* cc, CutDirection dir, Damages& damage, Character* who);
static HitMaterialType iHitYouAreYouHit_inner(CombatClass* cc, CutDirection dir, Damages& damage, Character* who);   // v40 fwd (wrapper below)
static HitMaterialType iHitYouAreYouHit_hook(CombatClass* cc, CutDirection dir, Damages& damage, Character* who)
{
    Character* preVictim = cc ? cc->me : nullptr;
    bool neutralAtIntent = preVictim && preVictim == s_attackNeutralIntentTarget;
    bool neutralNow = preVictim && who && who == s_freeMoveAnchor
        && !preVictim->isEnemy(s_freeMoveAnchor, true)
        && !s_freeMoveAnchor->isEnemy(preVictim, true);
    bool outgoingNeutralContact = preVictim && who && who == s_freeMoveAnchor
        && s_attackActive && !manualVPlayerTeamMember(preVictim)
        && (neutralNow || neutralAtIntent);
    if (outgoingNeutralContact)
    {
        for (int ni = 0; ni < 8; ++ni)
        {
            if (s_attackNeutralContact[ni] == preVictim) break;
            if (!s_attackNeutralContact[ni])
            {
                s_attackNeutralContact[ni] = preVictim;
                break;
            }
        }
    }
    HitMaterialType result = iHitYouAreYouHit_inner(cc, dir, damage, who);
    Character* self = cc ? cc->me : nullptr;
    bool victimCombatRestored = false;
    if (outgoingNeutralContact && result != HIT_SWORD && result != HIT_MISSED
        && cc && !cc->combatModeActive)
    {
        // A neutral victim reached the original HIT_FLESH verdict, but without
        // the vanilla unprovoked/focused order their CombatClass can still be
        // inactive. Character::hitByMeleeAttack then abandons the path before
        // _getHit, producing contact with no damage or retaliation. Activate
        // only the victim's native combat layer here; the caller still owns
        // damage, armour, _getHit and the confirmed-hit aggression rule.
        cc->combatModeActive = true;
        victimCombatRestored = true;
        VerbLog("[Ironwind] outgoing_hit_victim_combat_restored");
    }
    // All targeted swings use the native state machine. This is a contact
    // verdict, not yet proof that Character::_getHit actually ran: v79 recorded
    // one HIT_FLESH candidate that never entered the victim path. Count contact,
    // block and dodge here; confirmed hits and the red marker are owned by the
    // later getHit_hook.
    if (who && who == s_freeMoveAnchor && s_attackActive)
    {
        ++s_attackNativeContacts;
        const char* outcome = "hit";
        if (result == HIT_SWORD)
        {
            ++s_attackNativeBlocks;
            outcome = "blocked";
        }
        else if (result == HIT_MISSED)
        {
            ++s_attackNativeDodges;
            outcome = "dodged";
        }
        else
        {
            outcome = "hit_candidate";
        }
        float victimDistance = self && self->movement && who->movement
            ? self->movement->pos.distance(who->movement->pos) : -1.0f;
        char vb[240];
        CombatClass* attackerCombat = who->getCombatClass();
        sprintf_s(vb, sizeof(vb),
            "[Ironwind] attack_native_verdict outcome=%s result=%d intended=%d dist=%.1f neutral_origin=%d victim_combat=%d restored=%d",
            outcome, (int)result,
            attackerCombat && self == attackerCombat->currentTarget ? 1 : 0,
            victimDistance, outgoingNeutralContact ? 1 : 0,
            cc && cc->combatModeActive ? 1 : 0, victimCombatRestored ? 1 : 0);
        VerbLog(vb);
    }
    // A confirmed hit on the controlled character (not blocked, not dodged)
    // immediately cancels Ironwind-owned actions.  Reaction duration is NOT
    // decided here: the subsequent native _getHit(..., stumble) call is the
    // authority in v44.
    if (self && self == s_freeMoveAnchor && result != HIT_SWORD && result != HIT_MISSED)
    {
        // The verdict hook runs before Character::hitByMeleeAttack calls the
        // victim's _getHit(..., stumble). End only Ironwind's current free
        // action here, then let vanilla install its own flinch/stumble layer.
        // v42 cleared bookkeeping immediately but left the visible swing pose
        // alive until the 750 ms window expired (field: 744/761 ms late).
        bool freeActionInterrupted = s_freeAnimCombatMode
            && (s_freeAttackTech || s_freeBlockTech);
        if (freeActionInterrupted && cc->animation)
        {
            cc->animation->setCombatModeUpperIdle(false);
            cc->animation->endCombatAnimation();
            s_freeAnimCombatMode = false;
            if (self->movement) self->movement->animationOverride = false;
            VerbLog("[Ironwind] hit_free_action_interrupted before_vanilla_reaction");
        }
        // V-mode idle/free actions can deliberately leave CombatClass inactive.
        // Character::hitByMeleeAttack checks that flag before calling _getHit;
        // restoring it here (after the native hit verdict, before that caller
        // continues) keeps an in-flight free swing from swallowing the native
        // flinch/stumble path. This is state restoration, not a timed lock:
        // getHit_hook still follows vanilla's own `stumble` decision.
        if (!cc->combatModeActive)
        {
            cc->combatModeActive = true;
            VerbLog("[WASDCombat] hit_reaction_combat_restored");
        }
        ++s_hitReactionSerial;
        bool yielded = s_freeAttackTech || s_freeBlockTech
                    || s_attackQueuedAtMs != 0 || s_attackAimHeld;
        if (cc->animation && s_freeGuardUpperIdle)
            cc->animation->setCombatModeUpperIdle(false);
        manualFreeAttackClearTechnique(cc);
        s_freeAttackStartMs = 0;
        s_freeAttackSeenAnim = false;
        s_freeAttackLastProgress = -1.0f;
        s_freeAttackLastAdvanceMs = 0;
        s_freeBlockTech = nullptr;
        s_freeGuardUpperIdle = false;
        s_freeGuardCombatOwned = false;
        s_freeAttackImpactPassed = false;
        s_attackCommitUntilMs = 0;
        manualAttackClearQueuedInput(true);
        s_attackSeenPressMs = s_attackPressMs;
        if (s_interactionMenuByE)
        {
            if (s_gwFrame) s_gwFrame->hideContextMenu();
            manualInteractionScheduleResume("hit");
        }
        s_rmbCombatEdgeSeenMs = s_rmbCombatEdgeMs;
        s_rmbCombatEdgeMs = 0;
        manualAttackClearAimInput(true, true);
        char rb[192];
        sprintf_s(rb, sizeof(rb),
            "[WASDCombat] hit_confirmed_pre_gethit serial=%u state=%d combat=%d wasd=%d yielded=%d",
            s_hitReactionSerial, (int)cc->getCombatState(), cc->combatModeActive ? 1 : 0,
            (s_wHeld || s_aHeld || s_sHeld || s_dHeld) ? 1 : 0, yielded ? 1 : 0);
        VerbLog(rb);
    }
    return result;
}

// CombatClass::_getHit is the original game's authoritative reaction choice.
// The `stumble` argument has already been calculated by Character's native
// damage path.  A true value is allowed one native go() transition and then
// STUMBLE/REACTION_BLOCK own movement for exactly as long as vanilla keeps the
// state.  A false value receives no artificial Ironwind lock.
static void (*s_getHitOrig)(CombatClass* cc, CutDirection dir,
                            const Damages& damage, RootObject* who, bool stumble);
static bool manualAttackMarkWorldNotified(Character* victim)
{
    if (!victim) return false;
    for (int i = 0; i < 8; ++i)
        if (s_attackWorldNotified[i] == victim) return false;
    for (int i = 0; i < 8; ++i)
        if (!s_attackWorldNotified[i])
        {
            s_attackWorldNotified[i] = victim;
            return true;
        }
    // An attack zone hitting more than eight distinct neutral characters is
    // exceptional. World notification is safer than silently suppressing the
    // ninth victim; duplicate callbacks remain bounded by the native impacts.
    return true;
}
static bool manualAttackWasNeutralAtContact(Character* victim)
{
    if (!victim) return false;
    for (int i = 0; i < 8; ++i)
        if (s_attackNeutralContact[i] == victim) return true;
    return false;
}
static void getHit_hook(CombatClass* cc, CutDirection dir,
                        const Damages& damage, RootObject* who, bool stumble)
{
    bool outgoingPlayerHit = cc && cc->me && cc->me != s_freeMoveAnchor
        && who && who == s_freeMoveAnchor && s_attackActive;
    bool outgoingNeutralHit = outgoingPlayerHit && !manualVPlayerTeamMember(cc->me)
        && manualAttackWasNeutralAtContact(cc->me);
    bool controlled = cc && cc->me && cc->me == s_freeMoveAnchor
        && s_mode == MODE_FREE_MOVE && s_manualAttackOn
        && !s_dcShutdownInProgress && !s_loadGuardActive;
    int beforeState = cc ? (int)cc->getCombatState() : -1;
    int beforeNext = cc ? (int)cc->nextMove : -1;
    if (controlled && stumble)
    {
        s_hitReactionPending = true;
        InterlockedExchange(&s_hitReactionInputLock, 1);
        // V movement normally clears this flag.  Restore it only because the
        // native hit explicitly requested a stumble, not for a timed window.
        cc->combatModeActive = true;
    }

    // The user requires confirmed-hit aggression: targeting, LMB release,
    // AttackState start, a miss and a native block must not provoke a neutral.
    // Neutrality was captured immediately before the native contact verdict;
    // this authoritative _getHit callback proves that the damage path accepted
    // the hit. Deliberate and incidental victims therefore share one rule.
    if (outgoingNeutralHit)
    {
        manualAttackProvokeVictim(cc->me, s_freeMoveAnchor, true, "confirmed_gethit");
        if (cc->me == s_attackNeutralIntentTarget)
            manualAttackClearNeutralIntent();
    }

    s_getHitOrig(cc, dir, damage, who, stumble);

    // The verdict hook runs before Character::hitByMeleeAttack continues into
    // the victim's native hit path. Only this callback proves that the hit was
    // accepted. Drive the counter and red X here so a verdict candidate that is
    // abandoned later cannot produce false feedback. Damage, armour, body-part
    // selection and reaction remain entirely native.
    if (outgoingPlayerHit)
    {
        ++s_attackNativeHits;
        s_hitMarkerUntilMs = GetTickCount64() + HIT_MARKER_VISIBLE_MS;
        VerbLog("[Ironwind] hit_marker_trigger source=native_gethit");
        float victimDistance = cc->me->movement && s_freeMoveAnchor->movement
            ? cc->me->movement->pos.distance(s_freeMoveAnchor->movement->pos) : -1.0f;
        char ob[224];
        sprintf_s(ob, sizeof(ob),
            "[Ironwind] attack_native_gethit victim_dist=%.1f damage=%.2f cut=%.2f blunt=%.2f pierce=%.2f stumble=%d state=%d/%d",
            victimDistance, damage.total(), damage.cut, damage.blunt, damage.pierce,
            stumble ? 1 : 0, (int)cc->getCombatState(), (int)cc->nextMove);
        VerbLog(ob);
    }

    if (controlled && cc)
    {
        swordStateEnum afterState = cc->getCombatState();
        bool inNativeState = afterState == STUMBLE || afterState == REACTION_BLOCK;
        bool queuedNativeState = cc->nextMove == STUMBLE || cc->nextMove == REACTION_BLOCK;
        if (inNativeState) s_hitReactionPending = false;
        else s_hitReactionPending = stumble; // one real go() opportunity; no clock
        bool live = inNativeState || s_hitReactionPending;
        InterlockedExchange(&s_hitReactionInputLock, live ? 1 : 0);

        char rb[256];
        sprintf_s(rb, sizeof(rb),
            "[WASDCombat] hit_native_reaction serial=%u stumble=%d before=%d/%d after=%d/%d queued=%d combat=%d lock=%d",
            s_hitReactionSerial, stumble ? 1 : 0, beforeState, beforeNext,
            (int)afterState, (int)cc->nextMove, queuedNativeState ? 1 : 0,
            cc->combatModeActive ? 1 : 0, live ? 1 : 0);
        VerbLog(rb);
    }
}

static HitMaterialType iHitYouAreYouHit_inner(CombatClass* cc, CutDirection dir, Damages& damage, Character* who)
{
    Character* self = cc ? cc->me : nullptr;
    if (self && self == s_freeMoveAnchor) incomingAttackImpact(who); // close native incoming event
    // DIAG (build 56): every hit on the controlled character, with the attacker's
    // technique - to find the two-strike moves and tune the block windows.
    if (self && self == s_freeMoveAnchor)
    {
        static ULONGLONG s_lastHitMs = 0; static Character* s_lastHitWho = nullptr;
        ULONGLONG nowD = GetTickCount64();
        CombatClass* acc = who ? who->getCombatClass() : nullptr;
        CombatTechniqueData* at = acc ? acc->currentTechnique : nullptr;
        char hb[224];
        sprintf_s(hb, sizeof(hb), "[WASDCombat] hit_on_me anim=%s pts=%d dir=%d since_last=%llu same_attacker=%d manual=%d hold=%d",
                  at ? at->animation.c_str() : "?", at ? (int)at->impactPoints.size() : -1, (int)dir,
                  s_lastHitMs ? nowD - s_lastHitMs : 0ULL, (who && who == s_lastHitWho) ? 1 : 0,
                  s_manualAttackOn ? 1 : 0, s_blockOrderApplied ? 1 : 0);
        VerbLog(hb);
        s_lastHitMs = nowD; s_lastHitWho = who;
    }
    // A mouse guard without a selected or matching direction cannot use a
    // native block technique to parry this impact. Keep dodge outcomes intact.
    if (self && self == s_freeMoveAnchor && s_directionalCombat && s_mouseBlockHeld
        && manualBlockArmed(self) && (!cc->currentTechnique || !cc->currentTechnique->isDodge)
        && !combatDirectionMatches((CutDirection)s_blockDesired, dir))
    {
        CombatTechniqueData* keep = cc->currentTechnique;
        cc->currentTechnique = nullptr;
        HitMaterialType r = s_iHitOrig(cc, dir, damage, who);
        cc->currentTechnique = keep;
        return r == HIT_SWORD ? HIT_FLESH : r;
    }
    if (manualBlockArmed(self))
    {
        ULONGLONG now   = GetTickCount64();
        ULONGLONG origin = s_blockHoldStartMs;
        CutDirection wanted = (CutDirection)s_blockDesired;
        bool directionMatched = combatDirectionMatches(wanted, dir);
        // A held upper guard is already an explicit answer to a downward cut.
        // Do not let CharMovement::direction (which can still contain the last
        // movement tangent) randomly veto that correct manual input.  Rear
        // downward is the same native attack direction for this purpose.
        bool overheadArcBypass = wanted == CUT_DOWNWARD && directionMatched;
        bool rear = (dir == CUT_REAR_DOWNWARD || dir == CUT_REAR_LEFT || dir == CUT_REAR_RIGHT);
        bool front = !rear;
        if (!overheadArcBypass && front && who && who->movement && self->movement)
        {
            Ogre::Vector3 f  = self->movement->direction;  f.y  = 0.0f;
            Ogre::Vector3 to = who->movement->pos - self->movement->pos; to.y = 0.0f;
            if (f.squaredLength() > 1e-6f && to.squaredLength() > 1e-6f)
                front = f.normalisedCopy().dotProduct(to.normalisedCopy()) > 0.34f;   // ~140 deg arc
        }
        bool swinging  = cc->combatModeActive && cc->getCombatState() == CHOP_WEAPON;
        bool reacting = manualHitReactionLive(self);
        bool directionalReady = s_directionalCombat && s_mouseBlockHeld;
        bool keyboardReady = !s_directionalCombat
                          && s_freeGuardUpperIdle && s_freeBlockTech
                          && (now - origin) <= (ULONGLONG)s_blockHoldMs;
        // Vanilla can resolve a block while its visible pose is still entering.
        // Match that semantic: an accepted held directional request is ready
        // immediately; the pose is feedback, not a second hidden timing gate.
        if ((directionalReady || keyboardReady) && !s_freeAttackTech
            && (overheadArcBypass || front) && !swinging && !reacting
            && (!s_directionalCombat
                || directionMatched))
        {
            char b[128];
            sprintf_s(b, sizeof(b), "[Ironwind] directional_guard_block dir=%d press_age=%llu pose=%d overhead_arc_bypass=%d",
                      (int)dir, now - origin,
                      (s_freeGuardUpperIdle && s_freeBlockTech) ? 1 : 0,
                      overheadArcBypass ? 1 : 0);
            VerbLog(b);
            return HIT_SWORD;   // native caller still owns sound, spark and XP
        }
    }
    HitMaterialType rv = s_iHitOrig(cc, dir, damage, who);
    if (self && self == s_freeMoveAnchor)
    {
        char vb[96];
        sprintf_s(vb, sizeof(vb), "[WASDCombat] hit_vanilla_verdict result=%d", (int)rv);   // 5 = blocked (HIT_SWORD)
        VerbLog(vb);
    }
    return rv;
}

// initCombatMode_hook and youKnowImAttacking_hook removed —
// DC no longer blocks enemy combat entry or attack notifications.
// The combat AI runs freely; DC is a movement overlay only.

// -----------------------------------------------------------------------
// combatGo_hook — CombatClass::_NV_go, the per-frame combat AI decision.
// DC OWNERSHIP-HANDOFF MODEL (user req 2026-06-21): clean, exclusive ownership of
// the controlled character's locomotion, NO per-frame tug-of-war (that was the
// root cause of all the combat stutter/slide).
//   - WASD held (+ a short COMBAT_WASD_BRIDGE_MS grace for key-rolls): MOVEMENT
//     owns the character — SKIP go() so the AI takes no action and never steers,
//     and WASD has uncontested control = smooth, instant, no stutter.  Any clip
//     already mid-play is dropped to the run anim by the charMovUpdate cutoff =
//     no slide.
//   - WASD released: the AI owns the character — go() runs FULLY AUTONOMOUSLY
//     (vanilla combat: block/dodge/attack/position), resuming instantly.
// The handoff happens once at the press/release edge, never per-frame.  Only the
// anchor (thisptr->me == s_freeMoveAnchor, me @0x188) in DC; everything else is
// untouched.  Hooking go() is safe (proven in the Focus-Mode line; the door-era
// crash was setCurrentAction, NOT go).
// -----------------------------------------------------------------------
static void (*s_combatGoOrig)(CombatClass* thisptr, float frameTime);
static void manualHitReactionReconcileAfterGo(CombatClass* cc,
    unsigned int reactionSerial, int stateBefore, const char* source)
{
    if (!cc || cc->me != s_freeMoveAnchor) return;

    if (s_hitReactionPending)
    {
        swordStateEnum afterState = cc->getCombatState();
        bool inNativeState = afterState == STUMBLE || afterState == REACTION_BLOCK;
        bool queuedNativeState = cc->nextMove == STUMBLE
                              || cc->nextMove == REACTION_BLOCK;
        if (inNativeState)
        {
            s_hitReactionPending = false;
            InterlockedExchange(&s_hitReactionInputLock, 1);
        }
        else if (!queuedNativeState)
        {
            // _getHit requested stumble, but vanilla consumed its transition
            // without entering a reaction. Follow that native result rather
            // than inventing a duration or forcing a state.
            s_hitReactionPending = false;
            InterlockedExchange(&s_hitReactionInputLock, 0);
            char nb[160];
            sprintf_s(nb, sizeof(nb),
                "[WASDCombat] hit_native_reaction_not_entered serial=%u source=%s",
                reactionSerial, source ? source : "unknown");
            VerbLog(nb);
        }
    }

    if (reactionSerial)
    {
        static unsigned int lastSerial = 0;
        static int lastAfter = -1;
        int after = (int)cc->getCombatState();
        if (reactionSerial != lastSerial || after != lastAfter)
        {
            lastSerial = reactionSerial;
            lastAfter = after;
            char rb[208];
            sprintf_s(rb, sizeof(rb),
                "[WASDCombat] hit_reaction_go serial=%u before=%d after=%d combat=%d source=%s",
                reactionSerial, stateBefore, after,
                cc->combatModeActive ? 1 : 0, source ? source : "unknown");
            VerbLog(rb);
        }
    }
}

static void combatGo_hook(CombatClass* thisptr, float frameTime)
{
    unsigned int reactionTraceSerial = 0;
    int reactionStateBefore = -1;
    if (!s_dcShutdownInProgress && !s_loadGuardActive
        && s_mode == MODE_FREE_MOVE && thisptr && thisptr->me
        && thisptr->me == s_freeMoveAnchor)
    {
        // In V mode free actions own air swings and guard; a target press starts
        // one native AttackState directly. Keep the decision layer from
        // reacquiring movement or starting an unrequested follow-up. A genuine
        // hit reaction still needs go(); an active native swing advances its
        // AttackState below without entering CombatClassAI::update.
        bool reactionLive = manualHitReactionLive(thisptr->me);
        if (reactionLive)
        {
            reactionTraceSerial = s_hitReactionSerial;
            reactionStateBefore = (int)thisptr->getCombatState();
            if (s_hitReactionPending)
                s_hitReactionAdvancedFrameSerial = s_manualMainFrameSerial;
        }
        // Automatic defence needs the original AI decision frame only when the
        // player is holding an accepted RMB request AND a live attack is
        // incoming. Outside that intersection V mode still owns locomotion and
        // suppresses autonomous defence, attacks and follow-ups.
        bool aiDefenceRequested = s_settingAiBlocks && s_blockHeldPrev && incomingAttackLive();
        bool guardDecisionRequested = (!s_settingAiBlocks && s_blockHeld)
                                   || aiDefenceRequested;
        if (s_manualAttackOn)
        {
            swordStateEnum state = thisptr->getCombatState();
            // Do not mistake the native victim entry STARTUP -> STUMBLE for an
            // AI attack startup. v44 overwrote nextMove=STUMBLE with WAIT here,
            // so the reaction animation could play while the combat state and
            // input lock returned to DECISION on the same frame.
            if (!reactionLive && !s_attackActive && !manualAttackCommitLive()
                && (state == STARTUP_STATE || state == CHOP_WEAPON
                || state == TARGET_PATHFINDING_STARTUP || state == TARGET_PATHFINDING))
            {
                thisptr->nextMove = WAIT_MENACINGLY;
                thisptr->combatState = WAIT_MENACINGLY;
            }
            if (state != STUMBLE && state != REACTION_BLOCK && !reactionLive
                && !manualAttackCommitLive() && !s_attackActive
                && !guardDecisionRequested
                && !manualAutoGuardBlockLive(thisptr->me))
            {
                s_retreatLockGoSuppressed = true;
                static ULONGLONG lastManualGoLog = 0;
                ULONGLONG nowManualGo = GetTickCount64();
                if (nowManualGo - lastManualGoLog >= 1000)
                {
                    lastManualGoLog = nowManualGo;
                    VerbLog("[Ironwind] manual_combat_ai_suppressed");
                }
                return;
            }
        }
        // A direct CHOP owns only the original attack execution state. Calling
        // the full go() here would also run AI decision/movement and can hand the
        // next strike back to the AI. Advance the native state once and return.
        // mainLoop's fallback covers frames where the engine does not call go().
        if (!reactionLive && s_attackActive
            && thisptr->getCombatState() == CHOP_WEAPON)
        {
            s_retreatLockGoSuppressed = true;
            manualAttackAdvanceNativeState(thisptr, frameTime, "combat_go");
            if (!(s_wHeld || s_aHeld || s_sHeld || s_dHeld)
                && thisptr->me->movement)
                thisptr->me->movement->halt();
            return;
        }
        bool wasdHeld     = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
        bool movementOwns = wasdHeld
            || (s_wasdLastHeldMs > 0
                && (GetTickCount64() - s_wasdLastHeldMs) < COMBAT_WASD_BRIDGE_MS);
        // (Manual block: the pose is vanilla's own - the DEFENSIVE_COMBAT standing
        //  order is applied while the key is held; nothing to request here.)
        if (s_settingManualBlock)
        {
            // MANUAL ATTACK mode: a swing intent the AI raised on its own is turned
            // into the armed ready stance before the startup state initialises it.
            // A player-authorised direct attack is already in CHOP and does not
            // require the AI's STARTUP decision.
            if (s_manualAttackOn && !s_attackActive && thisptr->combatModeActive
                && thisptr->getCombatState() == STARTUP_STATE && thisptr->nextMove == CHOP_WEAPON)
            {
                thisptr->nextMove    = WAIT_MENACINGLY;
                thisptr->combatState = WAIT_MENACINGLY;
            }
            // IDLE PIN (build 48; log 2026-09-21: the AI's swing was refused in 289
            // separate seconds = it decided to attack, entered STARTUP, got refused
            // by the initialiser and fell back, several times a second, all fight -
            // a startup-animation twitch that was most of the "stutter").  The
            // decision runs its startup inside the same update, so the STARTUP
            // catch above never sees it: pin the DECISION state itself to the
            // armed ready hold with a short dwell, so the AI never decides at all
            // while nothing of ours is pending.  Untouched: a block on the way
            // (nextMove BLOCK / wantsToBlock), an incoming swing (the AI's own
            // block logic must run - the incoming tracker knows), our swing
            // (pending or in flight), or a stagger.
            if (s_manualAttackOn && thisptr->combatModeActive && !s_attackActive
                && !manualAttackCommitLive()
                && !reactionLive
                && thisptr->getCombatState() == DECISION
                && thisptr->nextMove != BLOCK && !thisptr->wantsToBlock
                && !aiDefenceRequested)                       // build 49: WaitingStateAI hands back to DECISION every
                                                                // frame an attack slot is free, and enemy swings are
                                                                // "incoming" most of a melee - with AI blocks OFF the
                                                                // exemption only let the refused-startup twitch back in
            {
                thisptr->nextMove    = WAIT_MENACINGLY;
                thisptr->combatState = WAIT_MENACINGLY;
                thisptr->stateTimer  = 0.4f;
                ULONGLONG nowPin = GetTickCount64();
                if (nowPin - s_manualPinLogMs >= 1000)
                {
                    s_manualPinLogMs = nowPin;
                    VerbLog("[WASDCombat] manual_ai_idle_pinned");
                }
            }
            if (focusLockLive() && thisptr->currentTarget != s_lockTarget)
                thisptr->currentTarget = s_lockTarget;          // the AI thinks about the lock only
            // The native hit reaction has exclusive action ownership.  Input
            // resumes on the exact native state exit.
            if (!reactionLive)
            {
                manualAttackTrack(thisptr, movementOwns);
                s_anchorPrevState = (int)thisptr->getCombatState();   // DIAG for manual_ai_attack_denied
                if (aiDefenceRequested)
                    manualAutoBlockInject(thisptr);
                else if (!movementOwns)
                    manualBlockInject(thisptr);
            }
        }
        // With V enabled, neutral idle belongs to the player.  Do not let an
        // old attack order resume pathfinding after the requested swing ends.
        if (s_manualAttackOn && !s_attackActive && !manualAttackCommitLive()
            && !guardDecisionRequested
            && !reactionLive
            && !isCommittedCombatClip(thisptr->me))
        {
            s_retreatLockGoSuppressed = true;
            return;
        }
        // Let go() RUN while a committed combat clip is mid-play (swing / stagger /
        // parry) so it FINISHES and the state advances (the charMovUpdate buffer holds
        // movement meanwhile); suppress it the instant the clip is done so no NEW attack
        // chains and movement takes over.  Suppressing only OUTSIDE the clip also means
        // we never freeze the state machine mid-clip (the buffer would otherwise stick
        // forever — e.g. a stagger that never advances).
        if (movementOwns && !reactionLive && !s_attackActive && !manualAttackCommitLive()
            && !aiDefenceRequested
            && !isCommittedCombatClip(thisptr->me))
        {
            s_retreatLockGoSuppressed = true;   // WASD owns locomotion; AI stands down
            return;                             // skip the combat decision entirely
        }
    }
    s_retreatLockGoSuppressed = false;
    s_combatGoOrig(thisptr, frameTime);
    // BLOCK often returns to DECISION in the same call that starts its visual
    // technique. Reconcile against the real clip after original go(); the
    // helper restores guard-ready only when that clip has actually ended.
    if (thisptr && thisptr->me == s_freeMoveAnchor && s_autoBlockActiveTech)
        (void)manualAutoGuardBlockLive(thisptr->me);
    manualHitReactionReconcileAfterGo(thisptr, reactionTraceSerial,
        reactionStateBefore, "engine");
    if (thisptr && thisptr->me == s_freeMoveAnchor && s_mode == MODE_FREE_MOVE
        && s_manualAttackOn && s_attackActive
        && !(s_wHeld || s_aHeld || s_sHeld || s_dHeld)
        && thisptr->me->movement)
        thisptr->me->movement->halt();  // no AI walk-in during a player swing
}

// -----------------------------------------------------------------------
// Main-thread hook — GameWorld::mainLoop_GPUSensitiveStuff
//
// Execution order:
//   1.  Safety gate (load-guard)
//   2.  Selection tracking
//   3.  V-Mode transition
//   4.  HUD + vanilla movement-speed synchronisation
//   5.  Pre-AI WASD
//   6.  s_mainLoopOrig (AI + CharMovement::update + CombatClass::go)
//   7.  Post-AI combat job suppression
//   8.  Periodic squad-threat scan
//   9.  Post-AI WASD re-application + instant stop
// -----------------------------------------------------------------------
static void (*s_mainLoopOrig)(GameWorld* thisptr, float time);

// Build 65: is this character in the game's live update list?  (After a load the
// selection handle can still name the OLD object for a while - it reads as
// unconscious and WASD is refused until the player re-selects.)
static bool dcInUpdateList(GameWorld* gw, Character* c)
{
    if (!gw || !c) return false;
    auto& chars = gw->getCharacterUpdateList();
    for (auto it = chars.begin(); it != chars.end(); ++it) if (*it == c) return true;
    return false;
}

static void manualVictimResponseClearSlot(int i)
{
    s_victimResponseVictim[i] = nullptr;
    s_victimResponseAttacker[i] = nullptr;
    s_victimResponseSource[i] = nullptr;
}

// Complete a neutral victim's retaliation only after the original main loop
// has returned from the AttackState/impact stack that raised the aggression.
// This is event/order sequencing, not an artificial combat delay. Cached
// Character identities are always checked against the live update list before
// dereference; refusal by the victim's original rules is logged and respected.
static void manualVictimResponseTick(GameWorld* gw)
{
    if (!gw || s_dcShutdownInProgress || s_loadGuardActive) return;
    for (int i = 0; i < 8; ++i)
    {
        Character* victim = s_victimResponseVictim[i];
        Character* attacker = s_victimResponseAttacker[i];
        const char* source = s_victimResponseSource[i];
        if (!victim) continue;
        if (!attacker || victim == attacker
            || !dcInUpdateList(gw, victim) || !dcInUpdateList(gw, attacker))
        {
            VerbLog("[Ironwind] attack_victim_response_dropped reason=not_live");
            manualVictimResponseClearSlot(i);
            continue;
        }
        if (manualVPlayerTeamMember(victim) || victim->isDead()
            || victim->getProneState() == PS_KO || attacker->isDead())
        {
            VerbLog("[Ironwind] attack_victim_response_dropped reason=invalid_actor");
            manualVictimResponseClearSlot(i);
            continue;
        }

        // Repeat awareness after the world/campaign update, then issue exactly
        // one victim-side combat task. Do not force temporary-enemy memory:
        // slaves, guards and other special identities retain vanilla refusal.
        victim->attackingYou(attacker, true, true);
        victim->attackTarget(attacker);
        CombatClass* victimCombat = victim->getCombatClass();
        char b[256];
        sprintf_s(b, sizeof(b),
            "[Ironwind] attack_victim_response source=%s victim_enemy=%d attacker_enemy=%d target_set=%d",
            source ? source : "unknown",
            victim->isEnemy(attacker, true) ? 1 : 0,
            attacker->isEnemy(victim, true) ? 1 : 0,
            victimCombat && victimCombat->currentTarget == attacker ? 1 : 0);
        VerbLog(b);
        manualVictimResponseClearSlot(i);
    }
}

// Reconcile the direct native state once per main-thread frame as well as from
// combatGo_hook.  This catches a state exit even when the engine stops calling
// CombatClass::go; the real native state remains the sole duration authority.
static void manualAttackLifecycleTick(GameWorld* gw, float frameTime)
{
    if (!s_attackActive || !gw || !s_freeMoveAnchor
        || !dcInUpdateList(gw, s_freeMoveAnchor)) return;
    CombatClass* cc = s_freeMoveAnchor->getCombatClass();
    if (!cc) return;
    manualAttackAdvanceNativeState(cc, frameTime, "main_fallback");
    swordStateEnum st = cc->getCombatState();
    if (st == CHOP_WEAPON) s_attackSawChop = true;
    bool playerMoving = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
    if (!manualAttackStillSwingingOrRecover(cc, "main_lifecycle"))
    {
        st = cc->getCombatState();
        manualAttackFinish(cc, playerMoving,
            manualHitReactionLive(cc->me) ? "hit_reaction_main"
            : (st == TARGET_PATHFINDING || st == TARGET_PATHFINDING_STARTUP)
                ? "unexpected_pathfinding_main" : "state_exit_main");
    }
}

// Compatibility fallback for combat controllers that queue a native victim
// reaction but do not schedule the controlled character's go() afterwards.
// This is deliberately narrower than attack advancement: it runs only for the
// exact native victim transition STARTUP -> STUMBLE/REACTION_BLOCK, at most
// once per world frame, and calls the original go implementation. Native
// state and animation exit remain the sole duration authority.
static void manualHitReactionLifecycleTick(GameWorld* gw, float frameTime)
{
    if (!s_hitReactionPending || !s_combatGoOrig || !gw
        || s_dcShutdownInProgress || s_loadGuardActive
        || s_mode != MODE_FREE_MOVE || !s_manualAttackOn
        || !s_freeMoveAnchor || !dcInUpdateList(gw, s_freeMoveAnchor)
        || s_hitReactionAdvancedFrameSerial == s_manualMainFrameSerial)
        return;

    CombatClass* cc = s_freeMoveAnchor->getCombatClass();
    if (!cc) return;

    swordStateEnum state = cc->getCombatState();
    if (state == STUMBLE || state == REACTION_BLOCK)
    {
        s_hitReactionPending = false;
        InterlockedExchange(&s_hitReactionInputLock, 1);
        return;
    }

    bool queuedNativeState = cc->nextMove == STUMBLE
                          || cc->nextMove == REACTION_BLOCK;
    if (!queuedNativeState)
    {
        s_hitReactionPending = false;
        InterlockedExchange(&s_hitReactionInputLock, 0);
        char nb[160];
        sprintf_s(nb, sizeof(nb),
            "[WASDCombat] hit_native_reaction_not_entered serial=%u source=main_reconcile",
            s_hitReactionSerial);
        VerbLog(nb);
        return;
    }

    // _getHit's verified entry shape. Do not run an AI decision from any other
    // state merely because an unrelated mod left a reaction enum in nextMove.
    if (state != STARTUP_STATE) return;

    unsigned int reactionSerial = s_hitReactionSerial;
    int stateBefore = (int)state;
    s_hitReactionAdvancedFrameSerial = s_manualMainFrameSerial;
    cc->combatModeActive = true;
    s_combatGoOrig(cc, frameTime);

    char fb[208];
    sprintf_s(fb, sizeof(fb),
        "[WASDCombat] hit_reaction_fallback_advance serial=%u before=%d after=%d next=%d",
        reactionSerial, stateBefore, (int)cc->getCombatState(), (int)cc->nextMove);
    VerbLog(fb);
    manualHitReactionReconcileAfterGo(cc, reactionSerial, stateBefore,
        "main_fallback");
}

static void mainLoop_hook(GameWorld* thisptr, float time)
{
    ++s_manualMainFrameSerial;
    if (s_manualMainFrameSerial == 0) ++s_manualMainFrameSerial;
    s_gwFrame = thisptr;  // manual combat: current world for validated target scans
    // Build 74 (quickload froze 2026-10-01, 6501 per-frame AVs): the SaveManager
    // signal is set when a load is REQUESTED and consumed before the loading
    // flag goes up, so reading it only while the flag is up (build 67) never saw
    // it and a quickload was classed as a chunk stream.  Latch it every frame;
    // release when the flag drops.  Plus a 4 Hz state line while any flag is up.
    {
        SaveManager* smL = SaveManager::getSingleton();
        int sigL = smL ? smL->signal : 0;
        if (sigL == SaveManager::LOADGAME || sigL == SaveManager::NEWGAME || sigL == SaveManager::IMPORTGAME)
        {
            if (!s_realLoadLatched)
            {
                char lb[64]; sprintf_s(lb, sizeof(lb), "[WASDCombat] dc_real_load_latched signal=%d", sigL); DebugLog(lb);
            }
            s_realLoadLatched = true;
            s_latchSetMs = GetTickCount64();
        }
        bool flagNow = ou && ou->isLoadingFromASaveGame();
        if (s_loadFlagPrev && !flagNow) s_realLoadLatched = false;                 // load finished: release
        if (s_realLoadLatched && !flagNow && GetTickCount64() - s_latchSetMs > 10000) s_realLoadLatched = false;   // signal with no load: expire
        s_loadFlagPrev = flagNow;
        if (flagNow)
        {
            ULONGLONG nowL = GetTickCount64();
            if (nowL - s_loadFlagLogMs >= 250)
            {
                s_loadFlagLogMs = nowL;
                char fb[160];
                sprintf_s(fb, sizeof(fb), "[WASDCombat] dc_load_flag signal=%d latched=%d player=%d anchor=%d in_list=%d",
                          sigL, s_realLoadLatched ? 1 : 0, ou->player ? 1 : 0, s_freeMoveAnchor ? 1 : 0,
                          (s_freeMoveAnchor && dcInUpdateList(thisptr, s_freeMoveAnchor)) ? 1 : 0);
                VerbLog(fb);
            }
        }
    }
    manualInteractionTargetTick(); // nearby native-interactable object -> attached E prompt
    manualInteractionActionTick(); // E queues that target's original context menu
    manualInteractionPauseTick();  // release only the pause created by this E transaction
    manualConsumeCombatRmb();       // RMB is always guard outside a visible menu
    manualBlockTick();    // manual combat: block hold/release edges (self-gated, cheap)
    // Hard-shutdown: all DC hook logic suppressed while true.
    // The six-condition stabilization countdown runs inside this block so it
    // advances even while the shutdown flag is held.
    if (s_dcShutdownInProgress)
    {
        if (!s_hookBlockLoggedMain) {
            s_hookBlockLoggedMain = true;
            DebugLog("[WASDCombat] dc_hooks_blocked_during_loadgame hook=mainLoop"); }

        if (ou && ou->player) s_mainLoopOrig(thisptr, time);

        // Six-condition check: runs once per frame after load finishes.
        // Condition 1: load finished (loadSig=false) — implied by the outer if.
        // Condition 2: player valid.
        // Conditions 3-4: selected character and movement pointers valid.
        // Condition 5: 60-frame window with all above held without lapse.
        // Condition 6: no stale anchor from old save remains.
        if (s_loadGuardActive && ou && ou->player && !dcRealLoad())
        {
            Character* chStab  = ou->player->selectedCharacter.getCharacter();
            bool chValid       = (chStab != nullptr);
            bool mvValid       = chValid && (chStab->movement != nullptr);
            bool noStaleAnchor = (s_freeMoveAnchor == nullptr);
            bool allClear      = chValid && mvValid && noStaleAnchor;

            if (allClear)
            {
                if (s_stabilizationCountdown == 0)
                {
                    s_stabilizationCountdown = 60;
                    DebugLog("[WASDCombat] reload_stabilization_started");
                }
                s_stabilizationCountdown--;
            }
            else
            {
                s_stabilizationCountdown = 0; // reset if any condition lapses
            }

            {
                ULONGLONG _nowW = GetTickCount64();
                if (_nowW - s_shutdownWaitLogTick >= 1000) { s_shutdownWaitLogTick = _nowW;
                    char _wbuf[256];
                    sprintf_s(_wbuf, sizeof(_wbuf),
                        "[WASDCombat] dc_loadgame_waiting_for_safe_reacquire "
                        "player=valid character=%s movement=%s anchor_clear=%s frames=%d/60",
                        chValid ? "valid" : "invalid",
                        mvValid ? "valid" : "invalid",
                        noStaleAnchor ? "yes" : "no",
                        allClear ? (60 - s_stabilizationCountdown) : 0);
                    DebugLog(_wbuf);
                }
            }

            if (allClear && s_stabilizationCountdown == 0)
            {
                s_loadGuardActive        = false;
                s_postLoadReacquire      = true;
                s_stabilizationCountdown = 0;
                DebugLog("[WASDCombat] dc_loadgame_safe_reacquire_complete");
                s_dcShutdownInProgress   = false;
                DebugLog("[WASDCombat] dc_shutdown_flag_cleared_after_safe_reacquire");
                DebugLog("[WASDCombat] reload_complete_reacquire_started");
            }
        }
        return;
    }

    // Profiling: initialize QPC frequency once; time every mainLoop invocation.
    if (!s_profInited)
    {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        s_profFreq         = freq.QuadPart;
        s_profInited       = true;
        s_prof_windowStart = GetTickCount64();
    }
    ScopeTimer _tML(s_prof_mainLoop);

    // 1. Safety gate.
    {
        bool ouNull  = (ou == nullptr);
        bool loadSig = ouNull || !ou->player || dcRealLoad();
        bool forceReal = false;                   // build 75: set when the anchor vanished mid-"stream"
        // Build 67: chunk STREAM (flag up, no real-load signal, player present):
        // keep control while the anchor is still in the live update list.
        if (!loadSig && ou->isLoadingFromASaveGame())
        {
            if (s_freeMoveAnchor && !dcInUpdateList(thisptr, s_freeMoveAnchor))
            {
                loadSig = true; forceReal = true;                 // anchor gone mid-stream: real-load handling (log 2026-10-01: the branch below re-asked dcRealLoad and took the microload path = freed-anchor deref every frame)
                static ULONGLONG s_vanishLogMs = 0;
                ULONGLONG nowV = GetTickCount64();
                if (nowV - s_vanishLogMs >= 1000) { s_vanishLogMs = nowV; DebugLog("[WASDCombat] dc_stream_anchor_vanished (falling back to the load path)"); }
            }
            else if (!s_streamLogged)
            {
                s_streamLogged = true;
                SaveManager* smS = SaveManager::getSingleton();
                char sb[112];
                sprintf_s(sb, sizeof(sb), "[WASDCombat] dc_stream_load_control_kept signal=%d anchor=%d", smS ? smS->signal : -1, s_freeMoveAnchor ? 1 : 0);
                DebugLog(sb);
            }
        }
        else if (!ou || !ou->isLoadingFromASaveGame()) s_streamLogged = false;

        if (loadSig)
        {
            if (ouNull)
            {
                // Game world gone — full hard shutdown regardless of user intent.
                if (!s_dcShutdownInProgress)
                {
                    s_dcShutdownInProgress = true;
                    DebugLog("[WASDCombat] dc_loadgame_shutdown_begin");
                    if (s_userWantsDC || s_mode == MODE_FREE_MOVE)
                    {
                        DebugLog("[WASDCombat] dc_hard_shutdown_reason=LOADGAME");
                        s_userWantsDC = false;
                    }
                    s_healingJobActive             = false;
                    s_medicalJobSuppressedThisHold = false;
                    s_freeMoveAnchor    = nullptr;
                    s_selectedCharacter = nullptr;
                    DebugLog("[WASDCombat] dc_loadgame_clear_anchor");
                    s_anchorMovement    = nullptr;
                    s_selectedMovement  = nullptr;
                    s_prevAttackTarget  = nullptr;
                    DebugLog("[WASDCombat] dc_loadgame_clear_movement");
                    s_savedFreeCameraMode    = false;
                    s_cameraLockInvSuspend   = false;
                    s_cameraLockTurretSuspend = false;
                    s_savedCamFollowOffY     = 0.0f;
                    DebugLog("[WASDCombat] dc_loadgame_clear_camera");
                    s_dcPtrLossActive = false;
                    s_loadGuardActive = true;
                    clearAllState();
                    DebugLog("[WASDCombat] dc_loadgame_old_state_cleared");
                    DebugLog("[WASDCombat] load_guard_enabled");
                    DebugLog("[WASDCombat] dc_loadgame_shutdown_complete");
                }
                return;
            }

            // ou is valid from here.
            if (s_userWantsDC)
            {
                // CRASH FIX (2026-06-13): a REAL save-load (isLoadingFromASaveGame)
                // frees the controlled character while ou->player may still be
                // valid.  The old preserve path below dereferences
                // s_freeMoveAnchor->movement (for speedOrders AND in the anchorOk
                // check), which reads freed memory and crashed when reloading
                // mid-capture.  A real save-load is NOT a chunk microload (those
                // only null ou->player without the load flag): drop the runtime
                // anchor IMMEDIATELY without touching it, skip ALL DC processing,
                // and run the game's loop so the load proceeds.  s_userWantsDC is
                // preserved, so the reacquire path restores DC once the load
                // completes.  Never deref the anchor while a save-load is active.
                if (dcRealLoad() || forceReal)
                {
                    if (!s_dcPtrLossActive)
                    {
                        s_dcPtrLossActive      = true;
                        s_dcPtrLossStartedAt   = GetTickCount64();
                        s_dcPtrLossLastLogTick = 0;
                        // Speed is NOT forced here any more.  charMovUpdate_hook
                        // caches the anchor's real speed tier every frame, so the
                        // reacquire restores the player's ACTUAL speed instead of a
                        // hard-coded RUN ("traveling load forces sprint", user
                        // 2026-07-31).  The anchor is freed on a save-load, so we must
                        // rely on that cached value here rather than deref it.
                        DebugLog("[WASDCombat] dc_realload_anchor_dropped_preserving_intent");
                    }
                    s_freeMoveAnchor    = nullptr;
                    s_anchorMovement    = nullptr;
                    s_selectedCharacter = nullptr;
                    s_selectedMovement  = nullptr;
                    if (ou->player) s_mainLoopOrig(thisptr, time);
                    return;
                }

                // DC intended — preserve through a chunk microload (player null,
                // no save-load flag).  Characters are NOT freed here, so the
                // anchor deref below is safe.
                if (!s_dcPtrLossActive)
                {
                    s_dcPtrLossActive      = true;
                    s_dcPtrLossStartedAt   = GetTickCount64();
                    s_dcPtrLossLastLogTick = 0;
                    if (s_freeMoveAnchor && s_freeMoveAnchor->movement && s_freeMoveAnchor->movement->speedOrders < GROUPED)
                        s_dcPreservedSpeedMode = s_freeMoveAnchor->movement->speedOrders;
                    {
                        char lb[96];
                        sprintf_s(lb, sizeof(lb), "[WASDCombat] dc_pointer_loss_preserving_user_intent speed=%d", (int)s_dcPreservedSpeedMode);
                        VerbLog(lb);
                    }
                    if (!ou->player)
                        DebugLog("[WASDCombat] dc_pointer_loss_preserved_microload");
                    if (ou->player && ou->player->isTrackingCharacter())
                        DebugLog("[WASDCombat] camera_lock_lost_during_long_stream");
                }

                // No timeout — pointer loss of any duration only pauses injection.
                // Reacquire loop runs indefinitely until pointer is valid or a true hard-shutdown fires.

                // Pointer-validity guard: skip injection if anchor or player is invalid.
                bool anchorAlive = (s_freeMoveAnchor != nullptr) && (s_freeMoveAnchor->movement != nullptr);
                bool anchorOk = (ou->player != nullptr) && anchorAlive;
                if (!anchorOk)
                {
                    // Build 63: the characters are NOT freed on a chunk microload,
                    // so the per-character movement hook keeps driving WASD while
                    // the player object is away (before: injection stopped and the
                    // character halted for the whole load).  Only the player-
                    // dependent DC processing below is skipped.
                    s_anchorMovement = anchorAlive ? s_freeMoveAnchor->movement : nullptr;
                    if (g_log.debugLogging)
                    {
                        ULONGLONG nowL = GetTickCount64();
                        if (nowL - s_dcPtrLossLastLogTick >= 500)
                        {
                            s_dcPtrLossLastLogTick = nowL;
                            char dlbuf[80];
                            sprintf_s(dlbuf, sizeof(dlbuf),
                                "[WASDCombat] dc_pointer_loss_duration_ms=%llu",
                                nowL - s_dcPtrLossStartedAt);
                            DebugLog(dlbuf);
                        }
                    }
                    if (ou->player) s_mainLoopOrig(thisptr, time);
                    return;
                }

                // All pointers valid — restore cache and fall through to DC processing.
                s_anchorMovement = s_freeMoveAnchor->movement;
                // Fall through — injection resumes normally in steps 5 and 9.
            }
            else
            {
                // DC not intended — normal load guard path.
                if (!s_loadGuardActive)
                {
                    if (!s_dcShutdownInProgress)
                    {
                        s_dcShutdownInProgress = true;
                        DebugLog("[WASDCombat] dc_loadgame_shutdown_begin");
                    }
                    s_healingJobActive             = false;
                    s_medicalJobSuppressedThisHold = false;
                    s_freeMoveAnchor               = nullptr;
                    s_anchorMovement               = nullptr;
                    s_selectedCharacter            = nullptr;
                    s_selectedMovement             = nullptr;
                    s_prevAttackTarget             = nullptr;
                    s_savedFreeCameraMode          = false;
                    s_cameraLockInvSuspend         = false;
                    s_cameraLockTurretSuspend      = false;
                    s_savedCamFollowOffY           = 0.0f;
                    s_loadGuardActive = true;
                    clearAllState();
                    DebugLog("[WASDCombat] dc_loadgame_old_state_cleared");
                    DebugLog("[WASDCombat] load_guard_enabled");
                }
                if (ou && ou->player) s_mainLoopOrig(thisptr, time);
                return;
            }
        }
        else if (s_dcPtrLossActive)
        {
            // loadSig cleared — run reacquire sequence.
            Character* chR = ou->player->selectedCharacter.getCharacter();
            // Build 65 (log 2026-09-30: after a 2.2 s chunk load the reacquired
            // selection read UNCONSCIOUS for 13 s and every WASD press was refused
            // until the user re-selected by double-click = a stale object): only
            // accept a character that is in the live update list; else stay in
            // pointer-loss and retry next frame.
            if (chR && s_userWantsDC && !dcInUpdateList(thisptr, chR))
            {
                ULONGLONG nowDf = GetTickCount64();
                if (nowDf - s_dcPtrLossLastLogTick >= 1000)
                {
                    s_dcPtrLossLastLogTick = nowDf;
                    char db[128];
                    sprintf_s(db, sizeof(db), "[WASDCombat] dc_reacquire_deferred ptr=%p not_in_update_list loss_ms=%llu",
                              (void*)chR, nowDf - s_dcPtrLossStartedAt);
                    DebugLog(db);
                }
                s_mainLoopOrig(thisptr, time);
                return;
            }
            s_dcPtrLossActive = false;
            if (chR && s_userWantsDC)
            {
                s_reacquireMs = GetTickCount64();
                {
                    char rb[192];
                    sprintf_s(rb, sizeof(rb), "[WASDCombat] dc_reacquire_after_long_stream speed_now=%d preserved=%d loss_ms=%llu ptr=%p unc=%d prone=%d dead=%d",
                              chR->movement ? (int)chR->movement->speedOrders : -1, (int)s_dcPreservedSpeedMode,
                              GetTickCount64() - s_dcPtrLossStartedAt, (void*)chR,
                              chR->isUnconcious() ? 1 : 0, (int)chR->getProneState(), chR->isDead() ? 1 : 0);
                    DebugLog(rb);
                }
                s_freeMoveAnchor = chR;
                s_anchorMovement = chR->movement;
                s_dcPtrLossLastLogTick = 0;
                if (chR->movement && s_dcPreservedSpeedMode < GROUPED)
                {
                    chR->movement->setDesiredSpeedOrders(s_dcPreservedSpeedMode);
                    chR->movement->setDesiredSpeed(s_dcPreservedSpeedMode);
                }
                if (s_mode != MODE_FREE_MOVE)
                    s_mode = MODE_FREE_MOVE;
                if (ou->player->isTrackingCharacter())
                {
                    DebugLog("[WASDCombat] camera_lock_restore_skipped_already_locked");
                }
                else
                {
                    ou->player->startTrackCharacter(s_freeMoveAnchor);
                    DebugLog("[WASDCombat] camera_lock_restored_after_long_stream");
                }
                DebugLog("[WASDCombat] dc_restored_after_long_stream");
            }
        }

        // s_loadGuardActive here means the shutdown countdown was handled inside
        // the s_dcShutdownInProgress block above (DC-shutdown path).  If we reach
        // this point with s_loadGuardActive still true it means the flag was set
        // without s_dcShutdownInProgress (should not occur after this fix), so
        // clear it safely to avoid being stuck.
        if (s_loadGuardActive)
        {
            s_loadGuardActive        = false;
            s_postLoadReacquire      = true;
            s_stabilizationCountdown = 0;
            DebugLog("[WASDCombat] reload_complete_reacquire_started");
        }
    }

    // NEWGAME detection — mainLoop signal poll.
    // Reachable only after safety gate confirms ou and ou->player are valid.
    // sm->signal == NEWGAME (0x4) is set by SaveManager::newGame() before any world teardown.
    // LOADGAME/Continue set signal == LOADGAME (0x2) — cannot produce a false positive here.
    // Gated on DC being active: no-op when DC was never enabled this session.
    // Does NOT set s_dcShutdownInProgress or s_loadGuardActive; LOADGAME machinery handles those
    // after world teardown begins normally.
    {
        SaveManager* sm = SaveManager::getSingleton();
        if (sm && sm->signal == SaveManager::NEWGAME
            && (s_mode == MODE_FREE_MOVE || s_userWantsDC || s_freeMoveAnchor != nullptr))
        {
            DebugLog("[WASDCombat] dc_newgame_signal_seen");
            DebugLog("[WASDCombat] dc_newgame_detection_source=mainloop_signal_poll");
            DebugLog("[WASDCombat] dc_newgame_soft_shutdown_begin");

            // Destroy user intent first so no subsequent path can re-enter DC.
            s_userWantsDC = false;
            s_userWantsFP = false;   // new game = hard teardown; never re-enter FP into it

            // Tear down OTS while the anchor + camera are still valid: exitOTS
            // re-attaches the DETACHED camera to the rig (otherwise char
            // creation shows no character — the camera stays orphaned on our
            // node) and restores the view-floor; also drop the face-cam state
            // (it otherwise breaks in the new game).  Field 2026-06-15:
            // new-game-while-in-a-save left the OTS camera/face-cam stranded.
            if (s_fpActive) exitOTS(true);
            if (s_firstPersonActive) exitFirstPerson(true);
            // The DETACHED third-person camera too (2026-09-06 rebuild has its
            // own flag; user 2026-09-14: new game started in OTS showed no
            // characters in creation and a stuck crosshair until DC was
            // toggled). Re-attach while the camera is valid and release the
            // crosshair/pointer.
            if (s_otsCamActive) exitOtsCam(true);
            dcUpdateCrosshair(false);
            s_fpSuspendedForInv = false;
            s_otsInvFaceActive = false;
            s_otsInvFaceChar   = nullptr;

            // Stop camera tracking and restore freecam state before clearing pointers.
            if (ou->player)
            {
                ou->player->stopTrackCharacter();
                if (ou->player->camera)
                {
                    ou->player->camera->setFreeCameraMode(s_savedFreeCameraMode);
                    ou->player->camera->objectCurrentlyFollowingOffset.y = s_savedCamFollowOffY;
                }
            }
            // Force mode and tracked-mode to VANILLA together so step-3 exit-transition
            // does not re-run stopTrackCharacter or show "Direct Control Disabled".
            s_mode          = MODE_VANILLA;
            s_fmTrackedMode = MODE_VANILLA;
            DebugLog("[WASDCombat] dc_newgame_forced_vanilla_mode");

            // Clear anchor and selection pointers before world teardown can free them.
            s_freeMoveAnchor    = nullptr;
            s_anchorMovement    = nullptr;
            s_selectedCharacter = nullptr;
            s_selectedMovement  = nullptr;
            s_prevAttackTarget  = nullptr;

            // Clear camera suspension state.
            s_savedFreeCameraMode     = false;
            s_savedCamFollowOffY      = 0.0f;
            s_cameraLockInvSuspend    = false;
            s_cameraLockTurretSuspend = false;
            s_menuSuspendActive       = false;

            // Clear WASD and movement state.
            s_wHeld               = false;
            s_aHeld               = false;
            s_sHeld               = false;
            s_dHeld               = false;
            s_wasdWasActive       = false;
            s_wasdMovementApplied = false;
            s_prevWasdDir         = Ogre::Vector3::ZERO;
            s_wasdTapStartMs      = 0;
            s_wasdLastHeldMs      = 0;
            s_frameMode           = MODE_VANILLA;
            s_frameWasdHeld       = false;

            // Clear loot UI suspension.
            s_lootUiSuspendActive  = false;
            s_lootUiWasPrevOpen    = false;
            s_lootSuspendStartTick = 0;

            // Clear healing and committed-action flags.
            s_healingJobActive             = false;
            s_healingJobPending            = false;
            s_medicalJobSuppressedThisHold = false;

            // Clear micro-load state — NEWGAME is not a micro-load.
            s_dcPtrLossActive      = false;
            s_dcPtrLossStartedAt   = 0;
            s_dcPtrLossLastLogTick = 0;

            DebugLog("[WASDCombat] dc_newgame_old_state_cleared");
            DebugLog("[WASDCombat] dc_newgame_soft_shutdown_complete");
        }
    }

    // Loot UI suspension — detect any open inventory window.
    // s_mode is untouched; all V-Mode processing suspends while any inventory is open.
    // Diagnostic logs identify which specific inventory type was detected.
    {
        bool anyInvOpen      = gui && gui->isAnyInventoryWindowOpen();
        int  numInvOpen      = gui ? gui->getNumOpenInventoryWindows() : 0;
        bool npcFieldOpen    = gui && gui->inventoryWindowNPC.getCharacter()       != nullptr;
        bool charFieldOpen   = gui && gui->inventoryWindowCharacter.getCharacter() != nullptr;
        bool traderFieldOpen = gui && gui->inventoryWindowTrader.getCharacter()    != nullptr;
        bool tradeAOpen      = gui && gui->tradeA.getCharacter()                   != nullptr;
        bool tradeBOpen      = gui && gui->tradeB.getCharacter()                   != nullptr;


        // Pre-open pause memory: pause state from the last frame with NO
        // inventory window open — by definition the state from BEFORE any open
        // edge, where Kenshi's auto-pause cannot have applied yet.  True at an
        // open edge = the pause is the PLAYER'S own (2026-08-31 fix: opening an
        // inventory while manually paused force-unpaused the game — the
        // open-edge grace misread the pre-existing pause as the auto-pause;
        // the latch could never be set because it only latches DURING a
        // move-through session).
        if (ou && gui && !gui->isAnyInventoryWindowOpen())
            s_invPausedBeforeOpen = ou->isPaused();

        // Move-through eligibility (InventoryFaceCam=false): keep DC live + game
        // running instead of suspending.  Evaluated each frame.
        bool moveThrough = invMoveThroughEligible();

        // Suspension trigger: any open inventory window.
        // showTradeWindow_hook may have already set s_lootUiSuspendActive before
        // the window was visible; this block confirms open/close state and manages
        // s_lootUiWasPrevOpen for edge detection.
        if (moveThrough)
        {
            // --- Inventory MOVE-THROUGH: DC stays live, game keeps running ---
            // Lift any early suspend the trade hook set, and un-suspend the camera
            // lock so movement + tracking continue while the inventory UI is open.
            if (s_lootUiSuspendActive)
            {
                s_lootUiSuspendActive  = false;
                s_lootSuspendStartTick = 0;
            }
            if (s_cameraLockInvSuspend)
            {
                s_cameraLockInvSuspend = false;
                if (s_freeMoveAnchor && ou->player)
                    ou->player->startTrackCharacter(s_freeMoveAnchor);
            }
            // Kenshi auto-pauses on inventory open and again when the shown
            // inventory switches to another squad member.  Defeat ONLY those
            // auto-pauses (grace window after each open/switch edge) so injected
            // WASD physically moves the character.  A pause appearing OUTSIDE
            // the grace is the player's own — respect it, including across
            // subsequent inventory switches, until the player unpauses (user
            // req 2026-08-05: manual pause must work in move-through mode).
            Character* mtShownChar = gui->inventoryWindowCharacter.getCharacter();
            bool mtOpenEdge   = !s_invMoveThroughActive;
            bool mtSwitchEdge = s_invMoveThroughActive
                                && mtShownChar != s_invMoveThroughShownChar;
            s_invMoveThroughShownChar = mtShownChar;   // identity only
            // Open edge with a PRE-EXISTING pause: the player paused before the
            // inventory existed, so it cannot be the auto-pause — latch it as
            // theirs up front, which also blocks the grace re-arm below.
            if (mtOpenEdge && s_invPausedBeforeOpen && ou && ou->isPaused())
            {
                s_invMoveThroughPlayerPaused = true;
                DebugLog("[WASDCombat] inv_move_through_preexisting_pause_respected");
            }
            if (mtOpenEdge || mtSwitchEdge)
            {
                // A paused player switching inventories stays paused: no grace
                // re-arm, so the auto-pause (a no-op on a paused game) is never
                // "defeated" out from under them.
                if (!s_invMoveThroughPlayerPaused)
                    s_invMoveThroughEdgeTick = GetTickCount64();
                if (mtSwitchEdge)
                    DebugLog("[WASDCombat] inv_move_through_switch_edge");
            }
            if (ou && ou->isPaused())
            {
                if (!s_invMoveThroughPlayerPaused
                    && s_invMoveThroughEdgeTick != 0
                    && GetTickCount64() - s_invMoveThroughEdgeTick
                           <= INV_MT_AUTOPAUSE_GRACE_MS)
                {
                    ou->userPause(false);              // Kenshi's auto-pause
                    s_invMoveThroughForcedRun = true;
                }
                else if (!s_invMoveThroughPlayerPaused)
                {
                    s_invMoveThroughPlayerPaused = true;
                    DebugLog("[WASDCombat] inv_move_through_player_pause_respected");
                }
            }
            else if (s_invMoveThroughPlayerPaused)
            {
                s_invMoveThroughPlayerPaused = false;  // player unpaused
                DebugLog("[WASDCombat] inv_move_through_player_unpause");
            }
            if (!s_invMoveThroughActive)
            {
                s_invMoveThroughActive = true;
                if (s_manualAttackOn)
                {
                    s_wasdHoldActive = true;
                    s_playerPointClickActive = false;
                    s_holdPosValid = false;
                    char b[128]; sprintf_s(b, sizeof(b),
                        "[Ironwind] inventory_control_retained zoom=%.2f",
                        s_otsZoomCur);
                    VerbLog(b);
                }
                DebugLog("[WASDCombat] inv_move_through_begin");
            }
            s_lootUiWasPrevOpen = true;   // track open so the close edge is clean

            // Merchant trade window: vanilla won't close it on distance, so close
            // it ourselves once the controlled character has WALKED far enough from
            // where the trade opened.  Corpse/own loot close natively, so only the
            // trader window is handled here.  Skip player-squad trades (trader is
            // your own squadmate) — those must NOT auto-close.
            Character* trader = gui->inventoryWindowTrader.getCharacter();
            if (trader && !trader->isPlayerCharacter() && !s_invTradeCloseRequested)
            {
                Ogre::Vector3 ap = s_freeMoveAnchor->getPosition();
                if (!s_invTradeStartValid)
                {
                    s_invTradeAnchorStart = ap;   // spot where the trade opened
                    s_invTradeStartValid  = true;
                }
                else
                {
                    float dx = ap.x - s_invTradeAnchorStart.x;
                    float dz = ap.z - s_invTradeAnchorStart.z;
                    if (dx * dx + dz * dz > INV_TRADE_AUTOCLOSE_DIST_SQ)
                    {
                        gui->closeTradeWindow();
                        s_invTradeCloseRequested = true;
                        DebugLog("[WASDCombat] inv_trade_autoclosed_distance");
                    }
                }
            }
        }
        else if (s_invMoveThroughActive)
        {
            // --- Move-through session ending: inventory closed, dialogue began,
            //     DC toggled off, or face-cam re-enabled.  Leave the game running
            //     (player owns pause now); just clear the move-through state. ---
            s_invMoveThroughActive    = false;
            s_invMoveThroughForcedRun = false;
            s_invMoveThroughPlayerPaused = false;
            s_invMoveThroughShownChar    = nullptr;
            s_invMoveThroughEdgeTick     = 0;
            s_invTradeCloseRequested  = false;
            s_invTradeStartValid      = false;
            s_tradeWindowActive       = false;
            s_lootUiWasPrevOpen       = false;
            s_lootUiSuspendActive     = false;
            s_lootSuspendStartTick    = 0;
            if (s_manualAttackOn)
            {
                char b[128]; sprintf_s(b, sizeof(b),
                    "[Ironwind] inventory_control_restored zoom=%.2f",
                    s_otsZoomCur);
                VerbLog(b);
            }
            DebugLog("[WASDCombat] inv_move_through_end");
        }
        else if (anyInvOpen && !s_lootUiWasPrevOpen)
        {
            // Window is now confirmed open.  Hook may have already set suspension;
            // if not (non-trade path such as showInventoryNPC), set it now.
            if (!s_lootUiSuspendActive)
            {
                s_lootUiSuspendActive  = true;
                s_lootSuspendStartTick = GetTickCount64();
            }
            // NOTE: do NOT re-derive s_tradeWindowActive from the npc/trader/
            // tradeA/tradeB fields here — they stay STALE after a trade closes
            // (field 2026-06-17: tradeA=1 tradeB=1 persisted across later own-
            // inventory opens, re-flagging them as trades and killing the face-
            // cam).  showTradeWindow_hook is the authoritative trade latch; it
            // fires for shop AND corpse/loot, and we clear it on the close edge.
            s_lootUiWasPrevOpen = true;
            DebugLog("[WASDCombat] loot_ui_open_suspend_vmode");
            if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && !s_cameraLockInvSuspend)
            {
                s_cameraLockInvSuspend = true;
                DebugLog("[WASDCombat] camera_lock_suspended_inventory_open");
            }
        }
        else if (!anyInvOpen && s_lootUiWasPrevOpen)
        {
            // Window closed — trade/loot session is over: clear the trade latch
            // immediately (before the suspension debounce) so the next OWN
            // inventory isn't mis-classified.
            s_tradeWindowActive = false;
            // Window closed — enforce debounce before releasing suspension.
            ULONGLONG elapsed = GetTickCount64() - s_lootSuspendStartTick;
            if (elapsed >= LOOT_SUSPEND_DEBOUNCE_MS)
            {
                s_lootUiSuspendActive = false;
                s_lootUiWasPrevOpen   = false;
                DebugLog("[WASDCombat] loot_ui_closed_restore_vmode");
                if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && s_cameraLockInvSuspend && ou->player)
                {
                    s_cameraLockInvSuspend = false;
                    DebugLog("[WASDCombat] camera_lock_restored_after_inventory");
                    ou->player->startTrackCharacter(s_freeMoveAnchor);
                }
            }
            else
            {
            }
        }
        else if (s_lootUiSuspendActive && !s_lootUiWasPrevOpen && s_lootSuspendStartTick > 0)
        {
            // Hook fired but window never appeared (cancelled interaction).
            // Release after 1 s to avoid a stuck suspension.
            ULONGLONG elapsed = GetTickCount64() - s_lootSuspendStartTick;
            if (elapsed >= 1000)
            {
                s_lootUiSuspendActive  = false;
                s_lootSuspendStartTick = 0;
                s_tradeWindowActive    = false;   // cancelled before any window opened
            }
        }
    }

    // DC anchor self-heal: after a reload the anchor can be lost while DC stays
    // on (s_mode FREE_MOVE) until the player manually toggles DC off/on (field
    // 2026-06-15).  Re-grab it from the live selection so DC (and the inventory
    // face-cam, which needs the anchor) stays available without that dance.
    if (s_mode == MODE_FREE_MOVE && !s_lootUiSuspendActive && !s_freeMoveAnchor
        && !s_dcShutdownInProgress && !s_loadGuardActive && ou && ou->player)
    {
        Character* sel = s_selectedCharacter;
        if (sel && sel->movement && sel->isPlayerCharacter())
        {
            s_freeMoveAnchor = sel;
            s_anchorMovement = sel->movement;
            ou->player->startTrackCharacter(s_freeMoveAnchor);
            DebugLog("[WASDCombat] dc_anchor_reacquired_selfheal");
        }
    }

    // Safety: the inventory face-cam manages its own enter/exit in
    // cameraUpdate_hook; if it is somehow left active here with no inventory
    // open, drop straight back to the vanilla camera.
    if (s_fpActive && !isOwnInventoryOpen())
        exitOTS(true);

    // Native command registration happens HERE, not in the loadConfig hook:
    // the game loads its keyboard config before RE_Kenshi loads plugins, so
    // that hook never fires on a normal launch (field finding, log-proven).
    if (!s_nativeCommandsRegistered)
        registerNativeCommands(key);

    // Movement flags come from the poll thread (always — see the native
    // keybind block comment).  Only the toggle persistence watchdog
    // runs here: it saves rebinds even if the options menu never calls
    // saveOptions, and logs whether rebinds actually reach Command::bound.
    if (s_nativeCommandsRegistered)
        watchNativeBindChanges();

    // Frame snapshot — read all volatile input state once before any per-character
    // hooks run inside s_mainLoopOrig.  Hooks use s_frame* instead of re-reading
    // volatiles, eliminating memory fence overhead for 100+ hook calls per frame.
    s_frameMode        = s_mode;
    s_frameWasdHeld    = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
    s_frameLootSuspend = s_lootUiSuspendActive;

    // 2. Selection tracking.
    {
        Character*    ch = ou->player->selectedCharacter.getCharacter();
        CharMovement* mv = ch ? ch->movement : nullptr;
        if (ch != s_selectedCharacter || mv != s_selectedMovement)
        {
            s_selectedCharacter = ch;
            s_selectedMovement  = mv;
        }

        if (s_postLoadReacquire && ch)
        {
            s_postLoadReacquire = false;
            DebugLog("[WASDCombat] reload_reacquire_complete");
            if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor)
            {
                DebugLog("[WASDCombat] camera_lock_retarget_after_load");
                ou->player->startTrackCharacter(s_freeMoveAnchor);
            }
        }
    }

    // 3. V-Mode transition.
    { LONGLONG _clStart = qpcNow();
    {
        ControlMode curMode = s_mode;
        bool curInVM  = (curMode         == MODE_FREE_MOVE);
        bool prevInVM = (s_fmTrackedMode == MODE_FREE_MOVE);

        if (curInVM && !prevInVM && !s_lootUiSuspendActive)
        {
            s_freeMoveAnchor      = s_selectedCharacter;
            s_anchorMovement      = s_freeMoveAnchor ? s_freeMoveAnchor->movement : nullptr;
            s_wasdWasActive       = false;
            s_combatWASDLogged    = false;
            s_retreatLogged       = false;
            s_squadThreat         = false;
            s_consciousAllyThreat = false;
            s_enemyTargetingLogged = false;
            s_lastScanTick        = 0;
            // Fresh V-mode = full vanilla until the first WASD release; an
            // in-flight point-click order continues normally.
            s_wasdHoldActive         = false;
            s_playerPointClickActive = false;
            s_holdPosValid           = false;
            s_idleHoldEngaged        = false;
            s_manualAttackOn         = s_settingManualBlock && (s_freeMoveAnchor != nullptr);
            s_recentMouseLastMs      = 0;
            if (s_manualAttackOn && s_freeMoveAnchor)
            {
                manualEnsureWeaponReady(s_freeMoveAnchor, s_freeMoveAnchor->getStats());
                CombatClass* readyCombat = s_freeMoveAnchor->getCombatClass();
                if (readyCombat && readyCombat->animation)
                    readyCombat->animation->setCombatMode(true);
                CharacterHuman* readyHuman = s_freeMoveAnchor->isHuman();
                char readyLog[96]; sprintf_s(readyLog, sizeof(readyLog),
                    "[Ironwind] v_combat_stance_prearmed hand=%d",
                    readyHuman && readyHuman->weaponInHands ? 1 : 0);
                VerbLog(readyLog);
            }

            if (s_freeMoveAnchor && ou->player->camera)
            {
                s_savedFreeCameraMode = ou->player->camera->isFreeCameraMode();
                DebugLog("[WASDCombat] camera_mode_saved");
                ou->player->camera->setFreeCameraMode(false);
                ou->player->startTrackCharacter(s_freeMoveAnchor);
                s_savedCamFollowOffY = ou->player->camera->objectCurrentlyFollowingOffset.y;
                DebugLog("[WASDCombat] camera_lock_enabled_dc");
            }
            ou->showPlayerAMessage(dcZh("Direct Control Enabled"), false);
        }
        else if (!curInVM && prevInVM && !s_lootUiSuspendActive)
        {
            // V owns only Ironwind's bookkeeping, never the native reaction or
            // animation duration.  Relinquish an in-flight direct attack before
            // dropping the anchor so a hit/menu/KO boundary cannot carry the
            // stale attack latch into the next V session.  The CombatClass state
            // itself is intentionally untouched and continues under vanilla.
            if (s_attackActive)
            {
                CombatClass* leavingCombat = s_freeMoveAnchor
                    ? s_freeMoveAnchor->getCombatClass() : nullptr;
                manualAttackFinish(leavingCombat, false, "v_mode_exit");
                VerbLog("[Ironwind] attack_v_boundary_released");
            }
            s_attackCommitUntilMs = 0;
            manualAttackClearQueuedInput(true);
            manualAttackClearAimInput(false, true);
            s_autoBlockAttemptWho = nullptr;
            s_autoBlockAttemptStartMs = 0;

            // DC turned off — drop the OTS camera first (re-attach to the rig
            // while the anchor is still valid), then restore vanilla camera.
            if (s_fpActive) exitOTS(true);
            if (s_firstPersonActive) exitFirstPerson(true);
            s_otsResumeZoomPending = false;
            // The DETACHED third-person camera too (2026-09-06 rebuild has its
            // own flag; user 2026-09-14: new game started in OTS showed no
            // characters in creation and a stuck crosshair until DC was
            // toggled). Re-attach while the camera is valid and release the
            // crosshair/pointer.
            if (s_otsCamActive) exitOtsCam(true);
            dcUpdateCrosshair(false);
            s_fpSuspendedForInv = false;
            ou->player->stopTrackCharacter();
            if (ou->player->camera)
            {
                ou->player->camera->setFreeCameraMode(s_savedFreeCameraMode);
                ou->player->camera->objectCurrentlyFollowingOffset.y = s_savedCamFollowOffY;
            }
            s_savedFreeCameraMode     = false;
            s_savedCamFollowOffY      = 0.0f;
            s_cameraLockInvSuspend    = false;
            s_cameraLockTurretSuspend = false;
            // Restore vanilla hold-to-rotate state so the camera cannot remain
            // rotating in vanilla mode.
            if (key) key->rotate      = false;
            DebugLog("[WASDCombat] camera_lock_disabled_restore_freecam");
            s_freeMoveAnchor        = nullptr;
            s_anchorMovement      = nullptr;
            s_wasdWasActive       = false;
            s_combatWASDLogged    = false;
            s_retreatLogged       = false;
            s_squadThreat         = false;
            s_consciousAllyThreat = false;
            s_enemyTargetingLogged = false;
            s_lastScanTick        = 0;
            s_wasdHoldActive         = false;
            s_playerPointClickActive = false;
            s_holdPosValid           = false;
            s_idleHoldEngaged        = false;
            ou->showPlayerAMessage(dcZh("Direct Control Disabled"), false);
        }
        else if (curInVM && s_freeMoveAnchor && !s_lootUiSuspendActive)
        {
            Character* sel = s_selectedCharacter;
            // Switch WASD control to a different squad member without leaving DC,
            // two ways: DOUBLE-CLICK their portrait (user req 2026-06-20) or press
            // F while they are selected/highlighted (user req 2026-06-28).  A single
            // click only selects (vanilla), so other NPCs can be inspected/ordered
            // without stealing control (Sentient Sands compat).  Honor a poll-thread
            // double-click within the last ~600ms or a pending F edge, then consume
            // so the switch fires exactly once.
            ULONGLONG dcMs       = s_lmbDoubleClickMs;
            bool      dcSwitch   = (dcMs > 0 && (GetTickCount64() - dcMs) <= 600);
            bool      fSwitch    = s_fSelectEdge;
            bool      wantSwitch = dcSwitch || fSwitch;
            if (wantSwitch && sel && sel != s_freeMoveAnchor && sel->isPlayerCharacter())
            {
                s_lmbDoubleClickMs    = 0;   // consumed
                s_fSelectEdge         = false;
                // First-person head/hair hide is PER-CHARACTER: it was applied to the
                // OUTGOING anchor's own skeleton/appearance.  Restore it on the old
                // anchor BEFORE the pointer moves, or that character keeps a shrunk
                // head (user 2026-07-31: "head of the previous controlled character
                // will still be shrunk") and shaved hair.  fpSetHeadBoneHidden always
                // acts on the CURRENT s_freeMoveAnchor, so the order must be
                // restore-old -> switch -> re-hide-new.
                if (s_firstPersonActive && s_fpHeadBoneHidden)
                    fpSetHeadBoneHidden(false);                 // acts on OLD anchor
                if (s_firstPersonActive && (s_fpHideHeadgear || s_fpHideHair))
                    fpSetBeardHidden(false, false);             // OLD anchor's headwear back
                if (s_firstPersonActive && (s_fpHideHead || s_fpHeadPixelHide))
                    fpSetHeadMask(false);                       // OLD anchor's mask re-derived
                if (s_firstPersonActive && s_fpHairHidden)
                {
                    AppearanceBase* apOld = s_freeMoveAnchor->getAppearance();
                    if (apOld) apOld->shaveHead(false);
                    s_fpHairHidden = false;
                    DebugLog("[WASDCombat] dc_fp_hair_restored_on_switch");
                }
                s_freeMoveAnchor      = sel;
                s_anchorMovement      = sel->movement;
                s_combatWASDLogged    = false;
                s_retreatLogged       = false;
                s_squadThreat         = false;
                s_consciousAllyThreat = false;
                s_enemyTargetingLogged = false;
                s_lastScanTick          = 0;
                // A newly controlled character has not been WASD-parked;
                // its vanilla orders continue until the first release.
                s_wasdHoldActive         = false;
                s_playerPointClickActive = false;
                s_holdPosValid           = false;
                s_idleHoldEngaged        = false;
                DebugLog(fSwitch ? "[WASDCombat] camera_lock_retarget_selection_fkey"
                                 : "[WASDCombat] camera_lock_retarget_selection_doubleclick");
                ou->player->startTrackCharacter(s_freeMoveAnchor);
                // Re-apply the FP head/hair hide to the NEW anchor so first-person
                // still hides the now-controlled character's own head/hair.
                if (s_firstPersonActive && (s_fpHideHead || s_fpPixelFallbackOn))
                    fpSetHeadBoneHidden(true);                  // acts on NEW anchor
                if (s_firstPersonActive && (s_fpHideHeadgear || s_fpHideHair))
                    fpSetBeardHidden(true, true);               // NEW anchor's headwear hidden
                if (s_firstPersonActive && (s_fpHideHead || s_fpHeadPixelHide))
                    fpSetHeadMask(true);                        // NEW anchor's pixel mask
                if (s_firstPersonActive && s_fpHideHair)
                {
                    AppearanceBase* apNew = s_freeMoveAnchor->getAppearance();
                    if (apNew) { apNew->shaveHead(true); s_fpHairHidden = true;
                        DebugLog("[WASDCombat] dc_fp_hair_hidden_on_switch"); }
                }
            }
            else if (fSwitch)
            {
                // F pressed with no valid target (same char, non-player, or no
                // selection) — consume the edge so it cannot fire on a later frame.
                s_fSelectEdge = false;
            }
        }
        s_fmTrackedMode = curMode;
    }

    // First-person toggle (P): consume the poll-thread edge here on the GAME
    // thread, where the camera + anchor are valid (the poll thread must never
    // touch the camera).  Enter only in DC with a live anchor and when the
    // inventory face-cam does not already own the camera; exiting is always
    // allowed.  A manual toggle also cancels a pending suspend-for-inventory
    // auto-return so the camera doesn't snap back to first-person on close.
    if (s_fpToggleRequested)
    {
        s_fpToggleRequested = false;
        s_otsResumeZoomPending = false;
        if (s_firstPersonActive)
        {
            s_userWantsFP = false;         // user turned FP off — clear the persistent intent
            exitFirstPerson(true);
        }
        else if (s_fpSuspendedForInv)
        {
            s_userWantsFP = false;         // cancelling the auto-return = user wants FP off
            s_fpSuspendedForInv = false;   // cancel the pending auto-return
        }
        else if (s_mode == MODE_FREE_MOVE && !s_fpActive && !s_lootUiSuspendActive
                 && s_freeMoveAnchor && s_freeMoveAnchor->movement)
        {
            s_userWantsFP = true;          // user turned FP on — persist through loads
            enterFirstPerson();
        }
    }

    // FP LOAD-PERSISTENCE self-heal: if the user wants first-person but a load/stream
    // dropped it, re-enter once the scene + DC anchor are stable again.  enterFirstPerson
    // self-guards + is idempotent; gated so it never fights the inventory face-cam suspend,
    // the OTS restore, a menu, or a still-loading scene.  This is what makes FP survive
    // chunk loads (user 2026-07-29).
    if (s_userWantsFP && !s_firstPersonActive && !s_fpActive && !s_fpSuspendedForInv
        && !s_otsRestorePending && s_mode == MODE_FREE_MOVE
        && !s_dcShutdownInProgress && !s_loadGuardActive
        && ou && ou->player && ou->player->camera && !dcRealLoad()
        && s_freeMoveAnchor && s_freeMoveAnchor->movement)
    {
        DebugLog("[WASDCombat] fp_reentered_after_load");
        enterFirstPerson();
    }

    // Sneak toggle (Shift+C while first-person, user req 2026-08-01): consume the
    // poll-thread edge here on the game thread.  Drive the game's OWN sneak
    // button handler (OrdersPanel::toggleStealth — the exact path a mouse click
    // on the SNEAK button takes), so the UI checkbox, the standing order, and
    // the character's stealth state all stay in sync (user 2026-08-01: direct
    // setStealthMode toggled sneak but left the SNEAK button visually off).
    // Use the same OrdersPanel path as the vanilla sneak control. Falls
    // back to the raw state switch if the panel isn't showing the anchor (no UI
    // to sync in that case).  Stealth skill, detection, and XP all run vanilla
    // (Rule 1: DC invokes the system, it does not reimplement it); WASD speed
    // while sneaking is capped to the real stealth speed in wasdMoveLimit.
    // Sneak deliberately persists across FP/DC exit — it is vanilla state the
    // player can also clear via the sneak button.
    if (s_sneakToggleRequested)
    {
        s_sneakToggleRequested = false;
        if ((s_firstPersonActive || s_otsCamActive) && s_freeMoveAnchor && s_freeMoveAnchor->movement)
        {
            OrdersPanel* op = (gui && gui->mainbar) ? gui->mainbar->ordersDataPanel : nullptr;
            if (op && op->stealthCheckBox
                && op->ordersCharacter.getCharacter() == s_freeMoveAnchor)
            {
                op->toggleStealth(op->stealthCheckBox);
            }
            else
            {
                s_freeMoveAnchor->setStealthMode(!s_freeMoveAnchor->isStealthMode());
            }
            char sbuf[80];
            sprintf_s(sbuf, sizeof(sbuf),
                "[WASDCombat] dc_fp_sneak_toggle on=%d viaPanel=%d",
                s_freeMoveAnchor->isStealthMode() ? 1 : 0,
                (op && op->stealthCheckBox) ? 1 : 0);
            DebugLog(sbuf);
        }
    }

    // Enemy body-clip clearance sampling (first-person combat, 2026-08-01): find
    // the nearest LIVE hostile to the anchor each frame; fpDriveFrame turns it
    // into an eye pullback + near-clip tighten so an aggressor pressing into the
    // camera can't slice the view open.  Cost control: skipped entirely unless FP
    // is active AND the 5s threat scan saw enemies (or the anchor is in combat);
    // inside the loop a coarse squared-distance cull runs before any game call,
    // so the per-frame work is a few subtractions per loaded character.
    s_fpEnemyNearestDist = -1.0f;
    if (s_firstPersonActive && s_fpEnemyClearRadius > 0.0f
        && s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && s_freeMoveAnchor->movement
        && (s_nearbyEnemyCount > 0
            || s_freeMoveAnchor->isInCombatMode(true, true)))
    {
        const Ogre::Vector3 anchorPos = s_freeMoveAnchor->movement->pos;
        const float cullR  = s_fpEnemyClearRadius * 2.0f;  // coarse pre-cull ring
        const float cullR2 = cullR * cullR;
        float best2 = -1.0f;
        auto& scanChars = thisptr->getCharacterUpdateList();
        for (auto it = scanChars.begin(); it != scanChars.end(); ++it)
        {
            Character* c = *it;
            if (!c || c == s_freeMoveAnchor || !c->movement) continue;
            float dx = c->movement->pos.x - anchorPos.x;
            float dz = c->movement->pos.z - anchorPos.z;
            float d2 = dx * dx + dz * dz;
            if (d2 >= cullR2) continue;
            if (best2 >= 0.0f && d2 >= best2) continue;
            if (c->isDead() || c->isUnconcious()) continue;   // bodies underfoot: loot freely
            if (!c->isEnemy(s_freeMoveAnchor, true)) continue;
            best2 = d2;
        }
        if (best2 >= 0.0f)
            s_fpEnemyNearestDist = sqrtf(best2);
    }

    // DIAG (2026-09-14): STEEP-WALK telemetry.  User: after minutes of OTS travel
    // the character walks up any mountain (also once at 3x on 09-12); a quick
    // save+load clears it.  Sample the anchor every 250 ms; when the path grade
    // exceeds the engine's own ~0.7 wall while moving, dump the state that could
    // explain a bypassed physics controller.  1 s throttle, DC anchor only.
    if (s_freeMoveAnchor && s_freeMoveAnchor->movement && ou && ou->player)
    {
        static Ogre::Vector3 s_swLast(0.0f, 0.0f, 0.0f);
        static ULONGLONG     s_swLastMs = 0, s_swLogMs = 0;
        static bool          s_swValid = false;
        ULONGLONG nowW = GetTickCount64();
        if (!s_swValid || nowW - s_swLastMs >= 250)
        {
            Ogre::Vector3 pW = s_freeMoveAnchor->movement->pos;
            if (s_swValid)
            {
                float dx = pW.x - s_swLast.x, dz = pW.z - s_swLast.z, dy = pW.y - s_swLast.y;
                float h  = sqrtf(dx * dx + dz * dz);
                if (h > 2.0f && h < 200.0f && dy / h > 0.75f && nowW - s_swLogMs >= 1000)
                {
                    s_swLogMs = nowW;
                    // (getCameraNode() sits at the world origin — its distance was
                    //  just |pos|; measure the ORBIT CENTRE and the real lens instead.)
                    float centerDist = -1.0f, camDist = -1.0f;
                    bool  following  = false;
                    CameraClass* camW = ou->player->camera;
                    if (camW)
                    {
                        centerDist = camW->getCenter().distance(pW);
                        if (camW->camera) camDist = camW->camera->getDerivedPosition().distance(pW);
                        following = camW->getFollowObject().isValid();
                    }
                    CharMovement* mvW = s_freeMoveAnchor->movement;
                    char wb[300];
                    sprintf_s(wb, sizeof(wb),
                        "[WASDCombat] dc_steep_walk grade=%.2f h=%.1f dy=%.1f ots=%d fp=%d indoors=%d bld=%d ragdoll=%d mode=%d stopped=%d loading=%d centerDist=%.0f camDist=%.0f following=%d pursuit=%d sinceTeleMs=%llu wasd=%d",
                        dy / h, h, dy, s_otsCamActive ? 1 : 0, s_firstPersonActive ? 1 : 0,
                        mvW->isIndoors() ? 1 : 0, mvW->building.getBuilding() ? 1 : 0,
                        s_freeMoveAnchor->isRagdoll() ? 1 : 0, (int)mvW->movementMode,
                        mvW->officiallyStopped ? 1 : 0,
                        ou->isLoadingFromASaveGame() ? 1 : 0, centerDist, camDist, following ? 1 : 0,
                        s_settingEnemyPursuit ? 1 : 0,
                        s_lastRigTeleportMs ? (unsigned long long)(nowW - s_lastRigTeleportMs) : 0ULL,
                        s_frameWasdHeld ? 1 : 0);
                    DebugLog(wb);
                }
            }
            s_swLast = pW; s_swLastMs = nowW; s_swValid = true;
        }
    }

    // AI MOTION FEED v2 — frame-level velocity, computed ONCE per render frame
    // from the anchor's mainLoop-to-mainLoop pos delta (CharMovement::update
    // substeps make per-update-call deltas unreliable; this one is not).
    // Consumed by the publish in charMovUpdate_hook and the end-of-frame publish
    // at the bottom of this hook.
    {
        bool drivingAF = (s_mode == MODE_FREE_MOVE) && s_freeMoveAnchor
                       && s_freeMoveAnchor->movement && s_wasdMovementApplied
                       && s_settingEnemyPursuit;
        if (drivingAF)
        {
            const Ogre::Vector3 pNow = s_freeMoveAnchor->movement->pos;
            if (s_aiPubValid && time > 0.0001f)
            {
                Ogre::Vector3 v = (pNow - s_aiPubLastPos) / time;
                float sp = v.length();
                if (sp < 400.0f) { s_aiPubVel = v; s_aiPubSpd = sp; }
                else if (g_log.debugLogging)
                {
                    // Gated + throttled (was unconditional per-frame: 3,019
                    // lines in one run-heavy OTS session, 09-12).  Numbers
                    // included so a debug log can settle the `time` units.
                    static ULONGLONG s_aiDiscLogTick = 0;
                    ULONGLONG nowD = GetTickCount64();
                    if (nowD - s_aiDiscLogTick >= 1000) { s_aiDiscLogTick = nowD;
                        char dbuf[128];
                        sprintf_s(dbuf, sizeof(dbuf),
                            "[WASDCombat] dc_ai_motion_feed_reseed_discontinuity sp=%.1f dt=%.5f",
                            sp, time);
                        DebugLog(dbuf); }
                }
            }
            s_aiPubLastPos = pNow;
            s_aiPubValid   = true;
        }
        else
        {
            s_aiPubValid = false;
            s_aiPubSpd   = 0.0f;
            s_aiPubVel   = Ogre::Vector3::ZERO;
        }
    }

    // Turret/mounted-use camera-lock suspension.
    // Detects enter/exit of isUsingStationaryTurret to suspend movement injection
    // and camera-lock updates while mounted, then restores exactly once on exit.
    if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && !s_lootUiSuspendActive && ou->player)
    {
        bool atTurret = isUsingStationaryTurret(s_freeMoveAnchor);
        if (atTurret && !s_cameraLockTurretSuspend)
        {
            s_cameraLockTurretSuspend = true;
            DebugLog("[WASDCombat] camera_lock_suspended_turret_enter");
        }
        else if (!atTurret && s_cameraLockTurretSuspend)
        {
            s_cameraLockTurretSuspend = false;
            DebugLog("[WASDCombat] camera_lock_restored_after_turret_exit");
            ou->player->startTrackCharacter(s_freeMoveAnchor);
        }
    }
    // Menu/pause suspension — movement injection only; camera lock and anchor preserved throughout.
    // Detects transition into/out of engine pause (escape menu, options, save, load).
    // Explicitly excludes inventory-triggered auto-pause: loot UI suspension owns that path.
    // On false→true: one-time movement stop using the same fields as the structured release-stop path.
    // No stop on subsequent frames while suspended; no camera change on either edge.
    if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor)
    {
        bool inventoryPausing = s_lootUiSuspendActive || (gui && gui->isAnyInventoryWindowOpen());
        bool menuPausedNow    = ou->isPaused() && !inventoryPausing;
        if (menuPausedNow && !s_menuSuspendActive)
        {
            s_menuSuspendActive = true;
            DebugLog("[WASDCombat] dc_menu_suspend_begin");
            CharMovement* mvM = s_freeMoveAnchor->movement;
            // Only kill momentum / cancel the path order if the character was
            // actively WASD-driving.  A character standing on a player-issued move
            // order has NO WASD momentum, and pausing must leave that order intact
            // — wiping it here was the "move order gets removed when the game
            // pauses while direct controlled" bug (user 2026-07-31).  WASD driving
            // owns no player order (it snaps to current pos on release), so stopping
            // it loses nothing.
            bool hadWasdMomentum = s_wasdMovementApplied
                                 || s_prevWasdDir.squaredLength() > 0.0001f;
            if (mvM && hadWasdMomentum && !isDownedButMovable(s_freeMoveAnchor))
            {
                s_prevWasdDir = Ogre::Vector3::ZERO;
                dcSnapCancelOrder(s_freeMoveAnchor);
                mvM->halt();
                mvM->desiredMotion = Ogre::Vector3::ZERO;
                mvM->moveLimit     = 0.0f;
                if (g_release.wasdReleaseDecelerationMultiplier > 1.0f)
                    mvM->currentMotion = Ogre::Vector3::ZERO;
                s_wasdMovementApplied = false;
                VerbLog("[WASDCombat] dc_menu_suspend_stop_applied");
            }
            else
            {
                VerbLog("[WASDCombat] dc_menu_suspend_stop_skipped_no_wasd_momentum");
            }
        }
        else if (!menuPausedNow && s_menuSuspendActive)
        {
            s_menuSuspendActive = false;
            DebugLog("[WASDCombat] dc_menu_suspend_end");
        }
        // Diagnostic: ou->isPaused() is true but inventory auto-pause is the cause.
        // Fires once per inventory-pause event; resets when the condition clears.
        { static bool s_skipLogged = false;
          if (ou->isPaused() && inventoryPausing && !s_menuSuspendActive)
          { if (!s_skipLogged) { s_skipLogged = true;
                DebugLog("[WASDCombat] dc_menu_suspend_skipped_inventory_pause"); } }
          else { s_skipLogged = false; } }
    }
    // Per-frame DC camera focus offset — raises focus toward chest at close zoom, tapers to zero at medium/far.
    // Skipped while the detached OTS camera owns the view (!s_otsCamActive) —
    // OTS runs its own camera, so this vanilla-follow-offset write is moot then.
    if (s_mode == MODE_FREE_MOVE && g_dcCam.dcCameraCloseZoomChestOffset && !s_fpActive && !s_firstPersonActive &&
        !s_otsCamActive &&
        s_freeMoveAnchor && !s_lootUiSuspendActive && ou->player && ou->player->camera)
    {
        CameraClass*  cam    = ou->player->camera;
        Ogre::Vector3 camPos = cam->getCameraPos();
        Ogre::Vector3 ctr    = cam->getCenter();
        float dist = (camPos - ctr).length();
        // Full effect at/inside typical min-zoom distance (~15 units), gone by 5.5 m.
        const float zoomClose = 15.0f, zoomFar = 55.0f;
        float t   = (dist - zoomClose) / (zoomFar - zoomClose);
        float scl = 1.0f - (t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t));
        cam->objectCurrentlyFollowingOffset.y = s_savedCamFollowOffY + g_dcCam.dcCameraFocusOffsetY * scl;
    }
    s_prof_cameraLock += qpcNow() - _clStart; }   // end camera-lock timer

    // 4. Vanilla walk/jog/run cycle synchronisation. The original
    // cycle_run_speed command has already updated the OrdersPanel; only mirror
    // that selection to the directly controlled anchor. X/Z remain exclusively
    // Kenshi's speed_3/speed_1 game-time commands.
    if (s_syncSpeedToAnchor && !s_lootUiSuspendActive)
    {
        s_syncSpeedToAnchor = false;
        if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && s_freeMoveAnchor->movement)
        {
            OrdersPanel* op = (gui && gui->mainbar) ? gui->mainbar->ordersDataPanel : nullptr;
            if (op)
            {
                MoveSpeed ns = MoveSpeed(int((unsigned char)op->speedImageNamesIdx));
                if (ns < GROUPED)
                {
                    s_freeMoveAnchor->movement->setDesiredSpeedOrders(ns);
                    s_freeMoveAnchor->movement->setDesiredSpeed(ns);
                }
                DebugLog(ns == WALK ? "[WASDCombat] speed_walk"
                       : ns == JOG  ? "[WASDCombat] speed_jog"
                                    : "[WASDCombat] speed_run");
            }
        }
    }

    // 5. Pre-AI WASD application.
    if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && s_freeMoveAnchor->movement && !s_lootUiSuspendActive)
    {
        // Promote deferred healing job once WASD is released.
        if (s_healingJobPending && !(s_wHeld || s_aHeld || s_sHeld || s_dHeld))
        {
            s_healingJobPending = false;
            s_healingJobActive  = true;
            VerbLog("[WASDCombat] dc_heal_resumed_after_wasd_release");
        }
        // Mirror: an ACTIVE heal yields to a held WASD, exactly as a vanilla
        // point-click moves the patient out of treatment (medic re-paths and
        // resumes on release via the promotion above).  This is also the cure
        // for a STUCK heal flag: the auto-heal job stays queued through a
        // knockdown/KO so removeJob never fires to clear s_healingJobActive
        // (field 2026-06-13: ~90 s of medical_job_blocks_wasd after the
        // character went unconscious, WASD pinned to a single disengage step).
        // Demoting to pending on any WASD hold guarantees the player can
        // always move, and the heal resumes the instant they stop.
        else if (s_healingJobActive && (s_wHeld || s_aHeld || s_sHeld || s_dHeld))
        {
            s_healingJobActive  = false;
            s_healingJobPending = true;
            VerbLog("[WASDCombat] dc_heal_yielded_to_wasd");
        }

        bool bW = s_wHeld, bA = s_aHeld, bS = s_sHeld, bD = s_dHeld;
        if (!(bW || bA || bS || bD))
        {
            s_playDeadExitDone       = false;
        }
        if (!(bW || bA || bS || bD) && s_slaveOrderActive)
        {
            // Slave WASD order live with keys up: the step-9 release snap
            // (dcSnapCancelOrder) has already re-ordered a move to the current
            // position = stop; just drop our bookkeeping so the next press
            // re-issues from scratch.
            s_slaveOrderActive = false;
            s_slaveModeLogged  = false;
        }
        if (!(bW || bA || bS || bD) && s_wasdDownedMovementActive)
        {
            // Keys are up but a persistent crawl order is live — cancel it
            // at the current position NOW (pre-AI), before pathfinding can
            // advance it another frame.  Level-triggered: catches any
            // release the step-9 edge stop might miss.  Crawl = keys held,
            // release = stay put.
            CharMovement* mvDC = s_freeMoveAnchor->movement;
            s_freeMoveAnchor->playerMoveOrderDefault(nullptr, nullptr, mvDC->pos);
            mvDC->halt();
            s_wasdDownedMovementActive = false;
            DebugLog("[WASDCombat] dc_downed_crawl_cancelled_on_release");
        }
        if (bW || bA || bS || bD)
        {
            // WASD breaks playing-dead exactly like a vanilla move order:
            // drop the prone state once per press and let the game decide
            // whether the character can actually stand — if not, they stay
            // crippled/downed and the crawl order below carries them.
            if (!s_playDeadExitDone
                && s_freeMoveAnchor->getProneState() == PS_PLAYING_DEAD)
            {
                s_freeMoveAnchor->setProneState(PS_NORMAL);
                s_playDeadExitDone = true;
                DebugLog("[WASDCombat] dc_play_dead_exit_on_wasd");
            }
            if (slaveOrderDriven(s_freeMoveAnchor))
            {
                // SLAVE OBEDIENCE: WASD = real move orders (see isObeyingSlave).
                if (!s_slaveModeLogged)
                {
                    s_slaveModeLogged = true;
                    DebugLog("[WASDCombat] dc_slave_wasd_order_mode");
                }
                applySlaveMovement(bW, bA, bS, bD);
            }
            else if (isDownedButMovable(s_freeMoveAnchor) && !downedOrderDriven(s_freeMoveAnchor))
            {
                // Downed — direct injection (charMovUpdate steering + step-9)
                // handles it like standing WASD; everywhere since v1.7.17.
                // Only job here: cancel a leftover crawl order from the
                // dormant order mode (flag can't be set anymore — safety).
                if (s_wasdDownedMovementActive)
                {
                    s_freeMoveAnchor->playerMoveOrderDefault(
                        nullptr, nullptr, s_freeMoveAnchor->movement->pos);
                    s_wasdDownedMovementActive = false;
                    DebugLog("[WASDCombat] dc_downed_crawl_switch_to_direct_indoors");
                }
            }
            else if (isDownedButMovable(s_freeMoveAnchor))
            {
                applyDownedMovement(bW, bA, bS, bD);
                s_wasdDownedMovementActive = true;
                static ULONGLONG s_downMoveTick = 0;
                ULONGLONG t = GetTickCount64();
                if (t - s_downMoveTick >= 2000) { s_downMoveTick = t;
                    ProneState prone = s_freeMoveAnchor->getProneState();
                    if (prone == PS_PLAYING_DEAD)   DebugLog("[WASDCombat] playing_dead_state_detected");
                    else if (prone == PS_CRIPPLED)  DebugLog("[WASDCombat] crippled_state_detected");
                    else                            DebugLog("[WASDCombat] downed_state_detected");
                    VerbLog("[WASDCombat] dc_crippled_state_detected");
                    VerbLog("[WASDCombat] dc_crippled_can_move=true");
                    VerbLog("[WASDCombat] dc_crippled_using_downed_movement_path");
                    DebugLog("[WASDCombat] wasd_downed_movement_attempt");
                    DebugLog("[WASDCombat] pointclick_movement_allowed_while_downed");
                    DebugLog("[WASDCombat] wasd_downed_movement_uses_pointclick_path");
                    DebugLog("[WASDCombat] wasd_downed_destination_created");
                    DebugLog("[WASDCombat] wasd_downed_destination_source_wasd");
                    if (s_freeMoveAnchor->isCrippled())
                    {
                        DebugLog("[WASDCombat] wasd_crippled_movement_allowed");
                        DebugLog("[WASDCombat] wasd_crippled_movement_success");
                    } }
            }
            else if (s_freeMoveAnchor->isUnconcious())
            {
                static ULONGLONG s_koTick = 0;
                ULONGLONG t = GetTickCount64();
                // Build 65: within 10 s of a post-load reacquire an "unconscious"
                // anchor may be the stale object - re-read the selection and switch
                // when it names a different, loaded, conscious character.
                if (s_reacquireMs != 0 && t - s_reacquireMs <= 10000 && ou && ou->player)
                {
                    Character* sel = ou->player->selectedCharacter.getCharacter();
                    if (sel && sel != s_freeMoveAnchor && sel->movement && sel->isPlayerCharacter()
                        && dcInUpdateList(thisptr, sel) && !sel->isUnconcious())
                    {
                        char rb[128];
                        sprintf_s(rb, sizeof(rb), "[WASDCombat] dc_reacquire_anchor_replaced old=%p new=%p", (void*)s_freeMoveAnchor, (void*)sel);
                        DebugLog(rb);
                        s_freeMoveAnchor = sel;
                        s_anchorMovement = sel->movement;
                        ou->player->startTrackCharacter(sel);
                    }
                }
                if (t - s_koTick >= 2000) { s_koTick = t;
                    char kb[192];
                    sprintf_s(kb, sizeof(kb), "[WASDCombat] wasd_true_unconscious_movement_blocked ptr=%p prone=%d dead=%d in_list=%d since_reacquire_ms=%llu",
                              (void*)s_freeMoveAnchor, (int)s_freeMoveAnchor->getProneState(), s_freeMoveAnchor->isDead() ? 1 : 0,
                              dcInUpdateList(thisptr, s_freeMoveAnchor) ? 1 : 0, s_reacquireMs ? t - s_reacquireMs : 0ULL);
                    VerbLog(kb);
                    VerbLog("[WASDCombat] dc_crippled_can_move=false");
                    VerbLog("[WASDCombat] dc_crippled_move_blocked_reason=UNCONSCIOUS"); }
            }
            else if (!s_cameraLockTurretSuspend && !s_menuSuspendActive)
            {
                if (s_healingJobActive)
                {
                    if (!s_medicalJobSuppressedThisHold)
                    {
                        s_medicalJobSuppressedThisHold = true;
                        VerbLog("[WASDCombat] medical_job_blocks_wasd");
                    }
                }
                else
                {
                    { LONGLONG _wi = qpcNow();
                      applyPlayerMovement(bW, bA, bS, bD);
                      s_prof_wasdInject += qpcNow() - _wi; }
                    { ULONGLONG t = GetTickCount64();
                      if (t - s_movInjLogTick >= 1000) { s_movInjLogTick = t;
                          if (g_log.debugVerbose)
                              DebugLog("[WASDCombat] movement_injection_allowed"); } }
                }
            }
        }
    }

    // 6. Run original game loop — AI + CharMovement::update + CombatClass::go run here.
    // Combat is untouched at the AI level (DC is locomotion-only); WASD priority
    // over combat locomotion is enforced inside charMovUpdate_hook.
    s_retreatLockGoSuppressed = false;

    s_mainLoopOrig(thisptr, time);

    // Post-loop load guard.
    if (!ou || !ou->player || dcRealLoad())
    {
        // Distinguish true LOADGAME teardown from temporary micro-load pointer loss.
        //   Hard shutdown only when:
        //     (a) ou == null          — game world destroyed (absolute teardown)
        //     (b) !ou->player AND isLoadingFromASaveGame() — player freed during confirmed load
        //   !ou->player WITHOUT a load signal = micro-load travel; preserve DC, do not shutdown.
        bool absoluteHard      = (!ou);
        bool confirmedLoadGame = (!absoluteHard) && (!ou->player) && ou->isLoadingFromASaveGame();
        bool hardLoss          = absoluteHard || confirmedLoadGame;

        if (hardLoss || !s_userWantsDC)
        {
            // True LOADGAME (hardLoss): full shutdown with s_dcShutdownInProgress.
            // DC not intended (!s_userWantsDC): normal load guard only, no shutdown flag.
            if (hardLoss && !s_dcShutdownInProgress)
            {
                s_dcShutdownInProgress = true;
                DebugLog("[WASDCombat] dc_loadgame_shutdown_begin");
                DebugLog("[WASDCombat] dc_shutdown_triggered_by_true_load");
                if (s_userWantsDC || s_mode == MODE_FREE_MOVE)
                {
                    DebugLog("[WASDCombat] dc_hard_shutdown_reason=LOADGAME");
                    s_userWantsDC = false;
                }
                s_healingJobActive             = false;
                s_medicalJobSuppressedThisHold = false;
                s_freeMoveAnchor    = nullptr;
                s_selectedCharacter = nullptr;
                DebugLog("[WASDCombat] dc_loadgame_clear_anchor");
                s_anchorMovement    = nullptr;
                s_selectedMovement  = nullptr;
                s_prevAttackTarget  = nullptr;
                DebugLog("[WASDCombat] dc_loadgame_clear_movement");
                s_savedFreeCameraMode    = false;
                s_cameraLockInvSuspend   = false;
                s_cameraLockTurretSuspend = false;
                s_savedCamFollowOffY     = 0.0f;
                DebugLog("[WASDCombat] dc_loadgame_clear_camera");
                DebugLog("[WASDCombat] dc_loadgame_old_state_cleared");
            }
            if (!s_loadGuardActive)
            {
                DebugLog("[WASDCombat] loadgame_signal_hard_shutdown");
                s_loadGuardActive = true;
                clearAllState();
                if (hardLoss)
                    DebugLog("[WASDCombat] dc_loadgame_shutdown_complete");
            }
            return;
        }

        // s_userWantsDC = true and no confirmed LOADGAME signal:
        // Pointer temporarily invalid during chunk travel — preserve DC, skip post-AI steps.
        {
            static ULONGLONG s_skipLogTick = 0;
            ULONGLONG nowSk = GetTickCount64();
            if (nowSk - s_skipLogTick >= 2000) { s_skipLogTick = nowSk;
                VerbLog("[WASDCombat] dc_shutdown_skipped_microload"); }
        }
        return;
    }

    // Skip all DC post-processing if a hard shutdown is in progress.
    if (s_dcShutdownInProgress)
        return;

    // The original world update has now returned, so campaign/awareness state
    // raised by a neutral attack is visible before the victim receives its one
    // retaliation task. This closes the first-victim ordering race without
    // restoring any player-side focused attack task.
    manualVictimResponseTick(thisptr);

    // The game updates movement and animation inside mainLoop. Reconcile the
    // direct native state, then apply no-target clips so the update cannot
    // replace them this frame.
    // Reactions own the body over attacks. Service a missing native reaction
    // transition before reconciling the outgoing attack bookkeeping.
    manualHitReactionLifecycleTick(thisptr, time);
    manualAttackLifecycleTick(thisptr, time);
    manualFreeActionTick();


    // 7b. Auto-off when the controlled character is knocked out (or dead) and
    // there is NOBODY else to switch to (user req 2026-09-21).  With a squad
    // the player simply selects someone else and DC follows the selection; a
    // lone character has no such out, and DC would sit on an unconscious body
    // (camera locked, WASD dead, FP/OTS still engaged) until V is pressed.
    // Takes the same path as V OFF so the camera/FP/OTS teardown is identical.
    // PS_KO/isDead only (the same "truly out" test as isDownedButMovable);
    // crippled / playing-dead / conscious-downed keep DC (they can crawl).
    // Debounced 1 s so the knockdown-to-KO transition does not flap.
    if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && ou && ou->player)
    {
        static ULONGLONG s_koAloneSinceMs = 0;
        bool anchorOut = s_freeMoveAnchor->isDead()
                      || s_freeMoveAnchor->getProneState() == PS_KO;
        bool alone = false;
        if (anchorOut)
        {
            alone = true;
            const lektor<Character*>& pcs = ou->player->playerCharacters;
            for (auto it = pcs.begin(); it != pcs.end(); ++it)
            {
                Character* c = *it;
                if (!c || c == s_freeMoveAnchor || !c->movement) continue;
                if (c->isDead() || c->getProneState() == PS_KO) continue;
                alone = false;   // someone conscious remains -> the player can switch
                break;
            }
        }
        if (anchorOut && alone)
        {
            ULONGLONG nowKo = GetTickCount64();
            if (s_koAloneSinceMs == 0) s_koAloneSinceMs = nowKo;
            else if (nowKo - s_koAloneSinceMs >= 1000)
            {
                s_koAloneSinceMs = 0;
                s_userWantsDC = false;
                s_userWantsFP = false;
                setMode(MODE_VANILLA);
                DebugLog(s_freeMoveAnchor->isDead()
                    ? "[WASDCombat] dc_auto_off_anchor_dead_alone"
                    : "[WASDCombat] dc_auto_off_anchor_ko_alone");
            }
        }
        else s_koAloneSinceMs = 0;
    }

    // 8. Periodic squad-threat scan.
    if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor)
    {
        ULONGLONG nowMs   = GetTickCount64();
        bool inCombat     = s_freeMoveAnchor->isInCombatMode(true, true);

        if (nowMs - s_lastScanTick >= SCAN_INTERVAL_MS)
        {
            s_lastScanTick = nowMs;
            LONGLONG _tsStart = qpcNow();

            bool foundEnemyTargetingAnchor     = false;
            bool foundEnemyTargetingSquad      = false;
            bool foundConsciousAllyUnderAttack = false;
            int  enemiesTargetingAnchor        = 0;
            int  nearbyEnemies                 = 0;
            int  pathfindingEnemies            = 0;

            auto& allChars = thisptr->getCharacterUpdateList();
            for (auto it = allChars.begin(); it != allChars.end(); ++it)
            {
                Character* c = *it;
                if (!c || c == s_freeMoveAnchor || !c->movement) continue;
                if (!c->isEnemy(s_freeMoveAnchor, true)) continue;
                nearbyEnemies++;
                {
                    CombatClass* ccc = c->getCombatClass();
                    if (ccc)
                    {
                        swordStateEnum cs = ccc->getCombatState();
                        if (cs == TARGET_PATHFINDING || cs == TARGET_PATHFINDING_STARTUP)
                            pathfindingEnemies++;
                    }
                }

                Character* attacked = c->getAttackTarget().getCharacter();
                if (!attacked) continue;

                if (attacked == s_freeMoveAnchor)
                {
                    foundEnemyTargetingAnchor = true;
                    enemiesTargetingAnchor++;
                }
                else if (attacked->isPlayerCharacter())
                {
                    foundEnemyTargetingSquad = true;
                    if (!attacked->isUnconcious() && !attacked->isDead())
                        foundConsciousAllyUnderAttack = true;
                }
            }
            s_lastKnownEnemyCount   = enemiesTargetingAnchor;
            s_nearbyEnemyCount      = nearbyEnemies;
            s_pathfindingEnemyCount = pathfindingEnemies;
            s_prof_threatScan      += qpcNow() - _tsStart;

            {
                bool wasdNow = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
                if (foundEnemyTargetingAnchor && !s_enemyTargetingLogged)
                {
                    if (wasdNow && s_retreatLockGoSuppressed)
                    {
                    }
                    else
                    {
                        if (g_log.debugVerbose)
                            DebugLog("[WASDCombat] enemy_targeting_player");
                        s_enemyTargetingLogged = true;
                    }
                }
                if (!inCombat) s_enemyTargetingLogged = false;

            }

            s_squadThreat         = foundEnemyTargetingSquad;
            s_consciousAllyThreat = foundConsciousAllyUnderAttack;

            // Combat engagement tracking — target acquired/lost, range entry/exit.
            Character* curTgt = s_freeMoveAnchor->getAttackTarget().getCharacter();
            if (curTgt != s_prevAttackTarget)
            {
                if (curTgt && !s_prevAttackTarget)
                    VerbLog("[WASDCombat] combat_target_acquired");
                else if (!curTgt && s_prevAttackTarget)
                    VerbLog("[WASDCombat] combat_target_lost");
                s_prevAttackTarget = curTgt;
            }

            bool curInRange = false;
            if (curTgt && curTgt->movement)
            {
                Ogre::Vector3 toTgt = curTgt->movement->pos - s_freeMoveAnchor->movement->pos;
                toTgt.y = 0.0f;
                float tgtDist = toTgt.length();
                curInRange = (tgtDist <= ATTACK_RANGE);

                if (curInRange && !s_prevTargetInRange)
                {
                    char buf[128];
                    sprintf_s(buf, sizeof(buf),
                        "[WASDCombat] enemy_entered_attack_range dist=%.0f", tgtDist);
                    VerbLog(buf);
                }
                else if (!curInRange && s_prevTargetInRange)
                {
                    char buf[128];
                    sprintf_s(buf, sizeof(buf),
                        "[WASDCombat] enemy_left_attack_range dist=%.0f", tgtDist);
                    DebugLog(buf);
                }
            }
            s_prevTargetInRange = curInRange;
        }
    }

    // ----------------------------------------------------------------
    // 9. Post-AI WASD re-application + instant stop.
    // ----------------------------------------------------------------
    if (!(s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && s_freeMoveAnchor->movement) || s_lootUiSuspendActive)
    {
        // A live WASD crawl order must not keep moving the character into
        // a menu/suspend — cancel it at the current position.
        if (s_wasdDownedMovementActive
            && s_freeMoveAnchor && s_freeMoveAnchor->movement)
        {
            s_freeMoveAnchor->playerMoveOrderDefault(
                nullptr, nullptr, s_freeMoveAnchor->movement->pos);
            s_freeMoveAnchor->movement->halt();
            s_wasdDownedMovementActive = false;
            DebugLog("[WASDCombat] dc_downed_crawl_cancelled_on_suspend");
        }
        s_wasdWasActive    = false;
        s_combatWASDLogged = false;
        s_retreatLogged    = false;
        s_rmbPressedEdge   = false;  // discard stale click edges outside V-mode
        return;
    }

    bool bW = s_wHeld, bA = s_aHeld, bS = s_sHeld, bD = s_dHeld;
    bool wasdActive = bW || bA || bS || bD;
    bool inCombat   = s_freeMoveAnchor->isInCombatMode(true, true);

    // DIAG (2026-09-14): slave-state edges with the DC context at that instant —
    // settles what flips IS_SLAVE -> ESCAPING when the user right-clicks inside
    // Rebirth with DC on (user says it's safe with DC off).
    {
        int stS = (int)s_freeMoveAnchor->isSlave();
        if (stS != s_lastSlaveState)
        {
            ULONGLONG nowS = GetTickCount64();
            char sb[224];
            sprintf_s(sb, sizeof(sb),
                "[WASDCombat] dc_slave_state from=%d to=%d walls=%d hold=%d click=%d wasd=%d combat=%d sinceModOrderMs=%llu chained=%d",
                s_lastSlaveState, stS, s_freeMoveAnchor->amInsideTownWalls(),
                s_wasdHoldActive ? 1 : 0, s_playerPointClickActive ? 1 : 0, wasdActive ? 1 : 0,
                inCombat ? 1 : 0,
                s_lastModOrderMs ? (unsigned long long)(nowS - s_lastModOrderMs) : 0ULL,
                s_freeMoveAnchor->isChainedMode() ? 1 : 0);
            VerbLog(sb);
            s_lastSlaveState = stS;
        }
    }

    // Earliest reliable player-click signal: RMB press edge from the poll
    // thread.  Input-level — cannot be blocked by any game-side dispatch
    // (field finding 2026-06-10: ground clicks during the hold never
    // reached PlayerInterface::playerMove, so the dispatcher hook alone
    // could not release the hold).  A real click while the hold is active
    // releases it immediately; the click's own order then proceeds under
    // vanilla control.  The playerMove and addOrder paths remain as
    // backup/diagnostics.
    if (s_rmbPressedEdge)
    {
        s_rmbPressedEdge = false;
        VerbLog("[WASDCombat] dc_click_input_seen");
        // In directional V combat the same physical RMB is the guard key.
        // Treating it as a ground click cleared the post-WASD hold, handing a
        // queued path or the defensive-combat order control of the feet.
        if (s_directionalCombat && s_manualAttackOn && s_settingManualBlock
            && !s_interactionMenuByE)
            VerbLog("[Ironwind] guard_input_keeps_wasd_hold");
        else if (s_wasdHoldActive)
        {
            s_wasdHoldActive         = false;
            s_playerPointClickActive = true;
            s_holdPosValid           = false;
            VerbLog("[WASDCombat] dc_wasd_hold_cleared_by_click_input");
        }
    }

    // Combat mode transition — suppress log and detect flicker during WASD retreat.
    if (inCombat && !s_wasPrevInCombat)
    {
        s_chaseFlapsCount++;
        if (!wasdActive)
            VerbLog("[WASDCombat] combat_entered");
    }
    else if (!inCombat && s_wasPrevInCombat)
    {
        s_chaseFlapsCount++;
        if (!wasdActive)
            VerbLog("[WASDCombat] combat_exited");
    }
    s_wasPrevInCombat = inCombat;

    if (wasdActive)
    {
        if (!s_wasdWasActive)
        {
            s_combatWASDLogged     = false;
            s_retreatLogged        = false;
            s_postWasdGraceActive  = false;
            s_combatReentryAllowed = false;

            // WASD always wins: release the post-WASD hold while keys are
            // held (it re-engages at the next release edge), and clear the
            // active point-click — the disengage order below replaces the
            // clicked order itself.  Capture whether a point-click was actually
            // pending BEFORE clearing it: the disengage move-order is ONLY needed
            // to cancel such a click.  When the player is merely navigating with
            // WASD (no pending click) there is nothing to cancel, so we must NOT
            // issue a move order — that order pathfinds, and inside a locked
            // building (esp. at the locked door/threshold, where isIndoors() reads
            // FALSE) the path-out fails and the vanilla "I can't get out of here"
            // bark fires (field 2026-06-20, log-proven: dc_disengage_order_issued
            // fired with indoors=0 at the threshold).
            bool hadPendingClick     = s_playerPointClickActive;
            s_wasdHoldActive         = false;
            s_playerPointClickActive = false;
            s_holdPosValid           = false;
            s_idleHoldEngaged        = false;

            if (isUsingStationaryTurret(s_freeMoveAnchor))
                DebugLog("[WASDCombat] stationary_crossbow_cancelled_by_wasd");

            // First WASD press: cancel any in-flight point-click/path order with a
            // disengage move order — but ONLY OUTDOORS.  playerMoveOrderDefault
            // ALWAYS issues a MOVE_CUS_ORDERED (task 29) that runs pathfinding, and
            // when the character is locked inside a building the path out runs
            // through a locked door -> the pathfinder fails -> the vanilla "I can't
            // get out of here" bark fires on every WASD press (field 2026-06-20;
            // confirmed it barked even with dest = current pos, because the move
            // order itself is the trigger, not the destination).  So skip the order
            // entirely indoors (same condition the release-snap already uses): there
            // setDirectMovement drives movement and the hold-clamp parks the
            // character on release, so no explicit order-cancel is needed.  Also
            // skipped while the crawl order owns movement (downed outdoors).
            // Only issue the disengage when a point-click was actually pending
            // (hadPendingClick) AND it won't bark (moveOrderMayBark) AND not downed.
            if (s_freeMoveAnchor->movement
                && hadPendingClick
                && !downedOrderDriven(s_freeMoveAnchor)
                && !moveOrderMayBark(s_freeMoveAnchor))
            {
                Ogre::Vector3 cancelDir;
                if (computeWASDDirection(bW, bA, bS, bD, cancelDir))
                {
                    Ogre::Vector3 dest = s_freeMoveAnchor->movement->pos;
                    s_freeMoveAnchor->playerMoveOrderDefault(nullptr, nullptr, dest);
                }
            }
        }

        if (!s_cameraLockTurretSuspend && !s_menuSuspendActive
            && !slaveOrderDriven(s_freeMoveAnchor)      // obeying slave: order-driven (step 5)
            && !downedOrderDriven(s_freeMoveAnchor))    // outdoor downed crawl is
                                                        // order-driven; halt() here
                                                        // was killing it every frame.
                                                        // Indoors-downed = direct
                                                        // injection, must run.
        {
            if (s_healingJobActive)
            {
                if (!s_medicalJobSuppressedThisHold)
                {
                    s_medicalJobSuppressedThisHold = true;
                    VerbLog("[WASDCombat] medical_job_blocks_wasd");
                }
            }
            else
            {
                { LONGLONG _wi = qpcNow();
                  applyPlayerMovement(bW, bA, bS, bD);
                  s_prof_wasdInject += qpcNow() - _wi; }
                { ULONGLONG t = GetTickCount64();
                  if (t - s_movInjLogTick >= 1000) { s_movInjLogTick = t;
                      if (g_log.debugVerbose)
                          DebugLog("[WASDCombat] movement_injection_allowed"); } }
            }
        }
        // Note: downed/crippled movement is handled in step 5 (pre-AI) only —
        // playerMoveOrderDefault persists through the AI loop, no re-issue needed.

        // Retreat detection.
        if (inCombat && !s_retreatLogged && ou->player->camera)
        {
            Character* aiTarget = s_freeMoveAnchor->getAttackTarget().getCharacter();
            if (aiTarget && aiTarget->movement)
            {
                Ogre::Vector3 camFwd = ou->player->camera->getFacingDirection();
                camFwd.y = 0.0f;
                float cflen = camFwd.length();
                if (cflen > 0.001f)
                {
                    camFwd /= cflen;
                    Ogre::Vector3 camRight(-camFwd.z, 0.0f, camFwd.x);
                    Ogre::Vector3 mv = Ogre::Vector3::ZERO;
                    if (bW) mv += camFwd; if (bS) mv -= camFwd;
                    if (bD) mv += camRight; if (bA) mv -= camRight;
                    float mlen = mv.length();
                    if (mlen > 0.001f)
                    {
                        mv /= mlen;
                        Ogre::Vector3 toEnemy = aiTarget->movement->getPosition()
                                              - s_freeMoveAnchor->movement->getPosition();
                        toEnemy.y = 0.0f;
                        float elen = toEnemy.length();
                        if (elen > 0.001f && mv.dotProduct(toEnemy / elen) < -0.3f)
                            s_retreatLogged = true;
                    }
                }
            }
        }

        // Athletics XP bridge — CharMovement::periodicUpdate skips xpRunning when
        // movementMode == MOVE_DIRECTION; award manually during DC WASD movement.
        {
            static const float     WALK_THRESHOLD           = 0.1f;
            static const ULONGLONG ATHLETICS_XP_INTERVAL_MS = 1000;

            float currentSpd = s_freeMoveAnchor->movement->desiredSpeed;

            if (!s_dcShutdownInProgress
                && !s_cameraLockTurretSuspend
                && !s_healingJobActive
                && !isProtectedAnimationState(s_freeMoveAnchor)
                && currentSpd > WALK_THRESHOLD)
            {
                ULONGLONG nowXP   = GetTickCount64();
                ULONGLONG elapsed = nowXP - s_athleticsXpLastTick;

                if (s_athleticsXpLastTick == 0)
                {
                    // First movement frame — arm timer, do not award.
                    s_athleticsXpLastTick = nowXP;
                }
                else if (elapsed >= ATHLETICS_XP_INTERVAL_MS)
                {
                    CharStats* stXP = s_freeMoveAnchor->getStats();
                    if (stXP)
                    {
                        float deltaTime = elapsed / 1000.0f;
                        stXP->xpRunning(deltaTime, currentSpd);
                        if (g_log.debugLogging)
                        {
                            MoveSpeed tier = s_freeMoveAnchor->movement->speedOrders;
                            const char* tierName = (tier == WALK) ? "walk"
                                                 : (tier == JOG)  ? "jog"
                                                 :                  "run";
                            char xpBuf[96];
                            sprintf_s(xpBuf, sizeof(xpBuf),
                                "[WASDCombat] dc_xp_movement_bridge_tick skill=Athletics speed=%s",
                                tierName);
                            DebugLog(xpBuf);
                        }
                    }
                    s_athleticsXpLastTick = nowXP;
                }
            }
            else
            {
                // Guard failed or speed too low — reset so the next movement hold re-arms.
                s_athleticsXpLastTick = 0;
            }
        }
    }
    else
    {
        if (s_wasdWasActive)
        {
            s_combatWASDLogged     = false;
            s_retreatLogged        = false;
            s_wasdReleasedTick     = GetTickCount64();
            s_postWasdGraceActive  = true;
            s_postWasdGraceStart   = GetTickCount64();
            s_combatReentryAllowed = false;

            // Nudge-tap check — consume tap-start timestamp and decide release path.
            ULONGLONG tapMs      = s_wasdTapStartMs;
            ULONGLONG tapElapsed = (tapMs > 0) ? (GetTickCount64() - tapMs) : ~0ULL;
            bool isNudgeTap      = (tapElapsed <= g_loco.wasdNudgeTapWindowMs);
            s_wasdTapStartMs     = 0;

            if (isNudgeTap && !isDownedButMovable(s_freeMoveAnchor))
            {
                // Nudge-safe release: zero velocity, clear stale dir, snap anchor — no facing correction.
                VerbLog("[WASDCombat] wasd_nudge_tap_detected");
                CharMovement* mvN = s_freeMoveAnchor ? s_freeMoveAnchor->movement : nullptr;
                if (mvN)
                {
                    mvN->halt();
                    mvN->desiredMotion    = Ogre::Vector3::ZERO;
                    mvN->moveLimit        = 0.0f;
                    mvN->currentMotion    = Ogre::Vector3::ZERO;
                    s_wasdMovementApplied = false;
                    VerbLog("[WASDCombat] wasd_nudge_stop_no_turnaround");
                    s_prevWasdDir = Ogre::Vector3::ZERO;
                    VerbLog("[WASDCombat] stale_movement_vector_cleared");
                    dcSnapCancelOrder(s_freeMoveAnchor);   // gated: no bark indoors/locked
                    VerbLog("[WASDCombat] anchor_snapped_no_facing_change");
                }
            }
            else
            {
                // Structured release-stop sequence — fires once on full WASD release.
                if (g_release.wasdStopOnRelease && s_freeMoveAnchor && s_freeMoveAnchor->movement)
                {
                    if (g_log.debugLogging) DebugLog("[WASDCombat] wasd_release_detected");
                    CharMovement* mvR = s_freeMoveAnchor->movement;

                    // STARTUP_STATE (attack windup) alone must not block release-stop.
                    // Compute whether a *real* committed action is blocking: re-use
                    // isCommittedAction for the full check, then subtract STARTUP_STATE.
                    // A HEAL JOB is not a committed action for the STOP (build 51; log
                    // 2026-09-22: all 45 skipped release-stops were the frame the yielded
                    // heal job resumed - step 5 promotes it BEFORE this check, so the
                    // momentum was never zeroed and the character walked on for a
                    // second or two after the key came up).  The medic re-paths on its
                    // own; the WASD vector must still be zeroed.
                    bool releaseCommitted = false;
                    bool healSaved = s_healingJobActive;
                    s_healingJobActive = false;                       // evaluate without the heal reason
                    bool committedNoHeal = isCommittedAction(s_freeMoveAnchor);
                    s_healingJobActive = healSaved;
                    if (committedNoHeal)
                    {
                        CombatClass*   ccRel = s_freeMoveAnchor->getCombatClass();
                        swordStateEnum stRel = ccRel ? ccRel->getCombatState() : COMBAT_FINISHED;
                        bool onlyStartup = (stRel == STARTUP_STATE)
                                        && !isProtectedAnimationState(s_freeMoveAnchor);
                        releaseCommitted = !onlyStartup;
                    }

                    if (releaseCommitted)
                    {
                        VerbLog("[WASDCombat] wasd_release_stop_skipped_committed_action");
                    }
                    else if (!downedOrderDriven(s_freeMoveAnchor))
                    {
                        // Standing AND indoors-downed (direct injection) both
                        // stop here; outdoor downed crawl is order-driven and
                        // stops in the downed block below.
                        // 1. Zero DC movement vector.
                        s_prevWasdDir = Ogre::Vector3::ZERO;
                        VerbLog("[WASDCombat] wasd_release_vector_zeroed");

                        // 2. Snap anchor — cancel any pending pathfind destination.
                        if (g_release.wasdAnchorSnapOnRelease)
                        {
                            dcSnapCancelOrder(s_freeMoveAnchor);   // gated: no bark indoors/locked
                            if (g_log.debugLogging) DebugLog("[WASDCombat] wasd_release_anchor_snapped");
                        }

                        // 3. Zero injected velocity — halt + force-zero all motion fields.
                        if (g_release.wasdZeroVelocityOnRelease)
                        {
                            mvR->halt();
                            mvR->desiredMotion = Ogre::Vector3::ZERO;
                            mvR->moveLimit     = 0.0f;
                            if (g_release.wasdReleaseDecelerationMultiplier > 1.0f)
                                mvR->currentMotion = Ogre::Vector3::ZERO;
                            s_wasdMovementApplied = false;
                            if (g_log.debugLogging) DebugLog("[WASDCombat] wasd_release_velocity_zeroed");
                        }
                    }
                    // else: downed — handled by existing downed-stop block below.
                }
            }

            // Downed/crippled instant stop.
            // Gate on s_wasdDownedMovementActive alone — NOT isDownedButMovable.
            // isDownedButMovable may flip false mid-release (e.g. isCurrentlyGettingUp
            // becomes true inside the AI loop), yet the WASD-issued destination is
            // still pending and must be cancelled.
            if (s_wasdDownedMovementActive)
            {
                CharMovement* mvDown = s_freeMoveAnchor ? s_freeMoveAnchor->movement : nullptr;
                if (mvDown)
                {
                    // playerMoveOrderDefault at current pos cancels the MOVE job entirely,
                    // not just the CharMovement destination field.
                    s_freeMoveAnchor->playerMoveOrderDefault(nullptr, nullptr, mvDown->pos);
                    mvDown->halt();
                }
                ProneState proneStop = s_freeMoveAnchor ? s_freeMoveAnchor->getProneState()
                                                        : PS_NORMAL;
                DebugLog("[WASDCombat] wasd_downed_key_released");
                DebugLog("[WASDCombat] wasd_downed_destination_cleared");
                DebugLog("[WASDCombat] wasd_downed_cached_direction_cleared");
                DebugLog("[WASDCombat] wasd_downed_movement_stopped");
                if (proneStop == PS_CRIPPLED ||
                    (s_freeMoveAnchor && s_freeMoveAnchor->isCrippled()))
                    DebugLog("[WASDCombat] wasd_crippled_instant_stop_applied");
                DebugLog("[WASDCombat] wasd_downed_pointclick_destination_not_persisted");
            }
            else if (s_freeMoveAnchor && isDownedButMovable(s_freeMoveAnchor))
            {
                // Destination was not WASD-created — likely a real player point-click.
                DebugLog("[WASDCombat] vanilla_pointclick_downed_destination_preserved");
            }
            s_wasdDownedMovementActive    = false;
            s_retreatLockEverActive       = false;
            s_retreatSessionCacheCount    = 0;  // clear session cache on WASD release
            s_retreatTargetsProcessed     = 0;
            s_retreatTargetsCachedSkipped = 0;
            s_retreatBlockedAttackerCount    = 0;
            s_medicalJobSuppressedThisHold   = false;
            // Standing instant stop after AI loop (fallback — fires only if the structured
            // release-stop sequence above did not already clear s_wasdMovementApplied).
            CharMovement* mvStop = s_freeMoveAnchor->movement;
            if (mvStop && s_wasdMovementApplied &&
                !isCommittedAction(s_freeMoveAnchor) &&
                !isDownedButMovable(s_freeMoveAnchor))
            {
                Ogre::Vector3 velPre = mvStop->currentMotion;
                mvStop->halt();
                mvStop->desiredMotion = Ogre::Vector3::ZERO;
                mvStop->moveLimit     = 0.0f;
                if (g_loco.wasdDecelerationMultiplier > 1.0f)
                    mvStop->currentMotion = Ogre::Vector3::ZERO;
                s_wasdMovementApplied = false;
                s_prevWasdDir         = Ogre::Vector3::ZERO;

                char buf[192];
                sprintf_s(buf, sizeof(buf),
                    "[WASDCombat] wasd_released instant_stop vel_pre=(%.1f,%.1f,%.1f)",
                    velPre.x, velPre.y, velPre.z);
                DebugLog(buf);
            }

            // Engage the post-WASD hold: WASD left the character here —
            // keep them here until a new point-click, the next WASD press,
            // or V OFF.  Any click made during the drive was already
            // cancelled by the release anchor-snap above, so its flag is
            // cleared too.  (Enforcement + engage log live in the
            // charMovUpdate hold branch and the end-of-frame clamp.)
            s_wasdHoldActive         = true;
            s_playerPointClickActive = false;
        }
    }

    s_wasdWasActive = wasdActive;

    // Post-WASD grace period: suppress combat re-entry unless enemy is close and actively targeting.
    if (!wasdActive)
    {
        if (s_postWasdGraceActive)
        {
            ULONGLONG elapsed = GetTickCount64() - s_postWasdGraceStart;
            if (elapsed >= POST_WASD_GRACE_MS && !isCommittedAction(s_freeMoveAnchor))
            {
                s_postWasdGraceActive  = false;
                s_combatReentryAllowed = true;
                VerbLog("[WASDCombat] combat_state_restored_after_wasd_release");
            }
            else
            {
                // Allow re-engagement only if the old target is within close range and attacking us.
                bool enemyCloseAndActive = false;
                Character* tgt = s_freeMoveAnchor->getAttackTarget().getCharacter();
                if (tgt && tgt->movement)
                {
                    float dist = (tgt->movement->pos - s_freeMoveAnchor->movement->pos).length();
                    bool tgtAttackingUs = (tgt->getAttackTarget().getCharacter() == s_freeMoveAnchor);
                    if (dist <= POST_WASD_REENGAGEMENT_RANGE && tgtAttackingUs)
                        enemyCloseAndActive = true;
                }
                s_combatReentryAllowed = enemyCloseAndActive;
            }
        }
    }
    else
    {
        s_combatReentryAllowed = false;
    }

    // Retreat state tracking — WASD held while combat AI has been suppressed.
    // s_retreatLockEverActive is sticky: set on the first frame go() is suppressed
    // during this WASD hold, cleared on release.  This prevents log bursts on frames
    // where Kenshi skips calling go() (no enemy nearby) while WASD is still held.
    {
        bool curRetreat = wasdActive && s_retreatLockEverActive;
        if (curRetreat && !s_wasdRetreatActive)
        {
            s_wasdRetreatActive      = true;
            s_retreatActiveStartTick = GetTickCount64();
            s_retreatCleanLogged     = false;
            s_combatFlickerLogged    = false;
        }
        else if (!curRetreat && s_wasdRetreatActive)
        {
            s_wasdRetreatActive = false;
        }
        if (s_wasdRetreatActive && !s_retreatCleanLogged &&
            GetTickCount64() - s_retreatActiveStartTick >= 1000)
        {
            s_retreatCleanLogged = true;
            DebugLog("[WASDCombat] retreat_clean_locomotion_confirmed");
            if (s_lastKnownEnemyCount > 1)
                DebugLog("[WASDCombat] retreat_locomotion_stable_under_multi_chase");
            DebugLog("[WASDCombat] retreat_perf_optimized");
            char scanBuf[128];
            sprintf_s(scanBuf, sizeof(scanBuf),
                "[WASDCombat] retreat_scan_interval_ms %llu cache_size=%d processed=%d skipped=%d",
                JOB_REMOVAL_INTERVAL_MS, s_retreatSessionCacheCount,
                s_retreatTargetsProcessed, s_retreatTargetsCachedSkipped);
            DebugLog(scanBuf);
        }
    }

    { LONGLONG _ctStart = qpcNow();
    // Protected animation state transition tracking.
    if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor)
    {
        bool nowProtected = isProtectedAnimationState(s_freeMoveAnchor);

        if (nowProtected && !s_wasProtectedState)
        {
            if (s_freeMoveAnchor->isCurrentlyGettingUp)
                VerbLog("[WASDCombat] getup_animation_started");
            else
                VerbLog("[WASDCombat] knockdown_or_stagger_started");
        }
        else if (!nowProtected && s_wasProtectedState)
        {
            VerbLog("[WASDCombat] protected_animation_completed");
            VerbLog("[WASDCombat] vmode_runtime_resumed");
        }

        s_wasProtectedState = nowProtected;
    }
    s_prof_combatTarget += qpcNow() - _ctStart; }  // end combat-target timer

    // ----------------------------------------------------------------
    // Final hold clamp — LAST DC-controlled write point in the frame.
    // Everything (AI, Taskers, indoor routing, pathing) has already run.
    // While the post-WASD hold is enforcing, restore the anchor's X/Z and
    // zero motion so no system that moved the character mid-frame keeps
    // the displacement.  Y is left free for gravity/ramp settling.
    // ----------------------------------------------------------------
    if (!wasdActive)
    {
        const char* gateReason = "";
        bool allowFC = computeHoldDecision(s_freeMoveAnchor, &gateReason);
        if (!allowFC)
        {
            CharMovement* mvFC = s_freeMoveAnchor->movement;
            if (mvFC)
            {
                if (!s_holdPosValid)
                {
                    s_holdPos      = mvFC->pos;
                    s_holdPosValid = true;
                }
                if (manualBlockMovementSlow())
                {
                    // See the movement-hook guard branch: never rewind only
                    // CharMovement::pos after the rendered body has updated.
                    s_holdPos.x = mvFC->pos.x;
                    s_holdPos.z = mvFC->pos.z;
                }
                else
                {
                    mvFC->pos.x = s_holdPos.x;
                    mvFC->pos.z = s_holdPos.z;
                }
                mvFC->desiredMotion = Ogre::Vector3::ZERO;
                mvFC->currentMotion = Ogre::Vector3::ZERO;
                mvFC->moveLimit     = 0.0f;
            }
        }
        else
        {
            s_holdPosValid = false;
        }

        // dc_authority_gate diagnostic — on state/reason change + 1 s heartbeat.
        ULONGLONG nowAG = GetTickCount64();
        bool agChanged = (allowFC != s_authGateLastAllow)
                      || (strcmp(gateReason, s_authGateLastReason) != 0);
        if (agChanged || nowAG - s_authGateLogTick >= 1000)
        {
            s_authGateLogTick   = nowAG;
            s_authGateLastAllow = allowFC;
            strcpy_s(s_authGateLastReason, sizeof(s_authGateLastReason), gateReason);
            if (g_log.debugLogging)
            {
                char gbuf[128];
                sprintf_s(gbuf, sizeof(gbuf),
                    "[WASDCombat] dc_authority_gate allowVanillaMove=%d reason=%s",
                    (int)allowFC, gateReason);
                VerbLog(gbuf);
            }
        }
    }
    else
    {
        s_holdPosValid = false;

        // Gate diagnostic during WASD drive: WASD owns locomotion.
        ULONGLONG nowAG = GetTickCount64();
        bool agChanged = s_authGateLastAllow
                      || (strcmp("wasd", s_authGateLastReason) != 0);
        if (agChanged || nowAG - s_authGateLogTick >= 1000)
        {
            s_authGateLogTick   = nowAG;
            s_authGateLastAllow = false;
            strcpy_s(s_authGateLastReason, sizeof(s_authGateLastReason), "wasd");
            if (g_log.debugLogging)
                VerbLog("[WASDCombat] dc_authority_gate allowVanillaMove=0 reason=wasd");
        }
    }

    // dc_perf: emit aggregate profiling line once per second.
    {
        ULONGLONG nowMs = GetTickCount64();
        if (nowMs - s_prof_windowStart >= 1000)
        {
            // Gated (v1.4.1): the emit ran unconditionally — 2+ log lines/sec
            // in every release session.  Accumulators still reset below so the
            // window stays coherent when diagnostics are re-enabled.
            if (g_log.debugLogging)
            {
            float f = (float)s_profFreq / 1000.0f;  // ticks → ms divisor
            char perfBuf[512];
            sprintf_s(perfBuf, sizeof(perfBuf),
                "[WASDCombat] dc_perf mainLoop_ms=%.2f charMove_ms=%.2f"
                " playerControl_ms=%.2f committedAction_ms=%.2f"
                " threatScan_ms=%.2f cameraLock_ms=%.2f"
                " wasdInject_ms=%.2f combatTarget_ms=%.2f"
                " enemyCount=%d nearbyCombatantCount=%d dcMode=%d wasdHeld=%d",
                s_prof_mainLoop      / f,
                s_prof_charMove      / f,
                s_prof_playerControl / f,
                s_prof_committedAct  / f,
                s_prof_threatScan    / f,
                s_prof_cameraLock    / f,
                s_prof_wasdInject    / f,
                s_prof_combatTarget  / f,
                s_lastKnownEnemyCount,
                s_nearbyEnemyCount,
                (int)(s_mode == MODE_FREE_MOVE),
                (int)(s_wHeld || s_aHeld || s_sHeld || s_dHeld));
            DebugLog(perfBuf);

            // Chase diagnostics — emitted once per second alongside dc_perf.
            {
                bool wasdNowC    = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
                bool enemiesChasing = (s_lastKnownEnemyCount > 0);
                char chaseBuf[256];
                sprintf_s(chaseBuf, sizeof(chaseBuf),
                    "[WASDCombat] dc_chase_perf enemyCount=%d nearbyCombatantCount=%d"
                    " combatStateFlaps=%d pathRecalcSuspected=%d",
                    s_lastKnownEnemyCount, s_nearbyEnemyCount,
                    s_chaseFlapsCount, (int)(s_pathfindingEnemyCount > 0));
                DebugLog(chaseBuf);
                if (enemiesChasing)
                {
                    if (wasdNowC)
                        VerbLog("[WASDCombat] dc_wasd_chase_active");
                    else
                        VerbLog("[WASDCombat] dc_point_click_chase_compare");
                }
                s_chaseFlapsCount = 0;
            }
            }
            else
                s_chaseFlapsCount = 0;   // window bookkeeping even when silent

            // Reset accumulators for the next window.
            s_prof_mainLoop      = 0;
            s_prof_charMove      = 0;
            s_prof_playerControl = 0;
            s_prof_committedAct  = 0;
            s_prof_threatScan    = 0;
            s_prof_cameraLock    = 0;
            s_prof_wasdInject    = 0;
            s_prof_combatTarget  = 0;
            s_prof_windowStart   = nowMs;
        }
    }

    // FP BEARD RE-ASSERT (1 Hz) — appearance rebuilds (clothing/injury/reload)
    // recreate the beard entity VISIBLE while FP still hides the head; the
    // cheap re-walk heals it within a second.  Quiet when nothing changed
    // (already-hidden beards are skipped before any log line).
    // (2026-08-31 note: a per-frame upgrade was tried and reverted the same
    // day with the zero-offset camera test — the engine re-shows the beard
    // per frame, so 1 Hz loses the visibility fight; the visible hide comes
    // from the head-bone shrink, this walk is the rebuild-heal backstop.)
    if (s_firstPersonActive && s_freeMoveAnchor
        && !s_loadGuardActive && !s_dcShutdownInProgress)
    {
        if (s_fpHideHeadgear || s_fpHideHair)
        {
            // Headwear walk: PER-FRAME once anything is on record (engine
            // re-shows facial attachments every frame — the 1 Hz logs proved
            // it); discovery stays 1 Hz so bare-headed anchors skip the walk.
            static ULONGLONG s_fpBeardTick = 0;
            ULONGLONG nowBrd = GetTickCount64();
            if (s_fpHiddenBeardCount > 0 || nowBrd - s_fpBeardTick >= 1000)
            {
                s_fpBeardTick = nowBrd;
                fpSetBeardHidden(true, false);
            }
        }
        if (s_fpHideHead || s_fpHeadPixelHide)
        {
            static ULONGLONG s_fpMaskTick = 0;   // shader constant: 1 Hz heal
            ULONGLONG nowMk = GetTickCount64();
            if (nowMk - s_fpMaskTick >= 1000)
            {
                s_fpMaskTick = nowMk;
                fpSetHeadMask(true);
            }
        }
    }

    // FP INJURY-OVERLAY SUPPRESSION (v1.3.2) — POST-orig so the disables
    // survive to the render (pre-orig placement was undone by the engine's own
    // update re-enabling overlays from injury status — the feature never
    // visibly worked there).  List fixed 2026-08-24: the chest-clutch clip is
    // named `hand2chest` at runtime (the log's enabled-state dump), not the
    // skeleton-file `handtowounds` family — keep both.  Ogre access rules:
    // enabled-iterator + getAnimationName + setEnabled ONLY (uint-hash map;
    // String-keyed lookups don't exist in OgreMain — plugin fails to load).
    if (s_firstPersonActive && s_freeMoveAnchor && s_fpHideInjuryOverlay
        && !s_loadGuardActive && !s_dcShutdownInProgress)
    {
        AppearanceBase* apS = s_freeMoveAnchor->getAppearance();
        Ogre::Entity* bodyS = apS ? apS->getBody() : nullptr;
        Ogre::AnimationStateSet* setS = bodyS ? bodyS->getAllAnimationStates() : nullptr;
        if (setS)
        {
            static const char* const FP_SUPPRESS_ANIMS[] =
                { "hand2head", "hand2head-R", "hand2chest",
                  "handtowounds", "handtowoundsR" };
            Ogre::ConstEnabledAnimationStateIterator itS =
                setS->getEnabledAnimationStateIterator();
            while (itS.hasMoreElements())
            {
                Ogre::AnimationState* stS = itS.getNext();
                const Ogre::String& nmS = stS->getAnimationName();
                for (int i = 0; i < 5; ++i)
                {
                    if (nmS == FP_SUPPRESS_ANIMS[i])
                    {
                        stS->setEnabled(false);
                        static ULONGLONG s_fpSupLogTick = 0;
                        ULONGLONG nowSup = GetTickCount64();
                        if (nowSup - s_fpSupLogTick >= 1000) { s_fpSupLogTick = nowSup;
                            char supBuf[128];
                            sprintf_s(supBuf, sizeof(supBuf),
                                "[WASDCombat] dc_fp_injury_overlay_suppressed anim=%s",
                                nmS.c_str());
                            DebugLog(supBuf); }
                        break;
                    }
                }
            }
        }
    }


    // AI MOTION FEED v2 end-of-frame publish — the frame's LAST word on the
    // anchor's bookkeeping, so every AI reader (whatever its update order or
    // engine substep) sees the published fleeing-character state from here until
    // the anchor's next update restores → re-publishes.  Gated on the inject
    // path having ACTUALLY driven this frame (see s_aiPubDroveThisFrame) — never
    // on s_wasdMovementApplied, which survives a committed-action release and
    // made a released character keep walking.
    if (s_aiPubDroveThisFrame)
    {
        s_aiPubDroveThisFrame = false;
        if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && s_freeMoveAnchor->movement
            && s_settingEnemyPursuit
            && !s_loadGuardActive && !s_dcShutdownInProgress)
        {
            CharMovement* mvPub = s_freeMoveAnchor->movement;
            mvPub->currentMotion     = s_aiPubVel;
            mvPub->currentSpeed      = s_aiPubSpd;
            mvPub->movementMode      = MOVE_NORMAL;
            mvPub->officiallyStopped = false;
            s_aiFeedWrote            = true;
            s_aiFeedModePublished    = true;
        }
    }

    // Native settings tab: detect UI-driven setting changes → unit
    // conversions, FP camera re-apply, immediate INI persistence.  Touches
    // only our own statics (no GUI calls, no character state).
    manualRuntimeInvariantTick();
    dcOptionsWatch();
}


// -----------------------------------------------------------------------
// PlayerInterface::playerControl hook
// -----------------------------------------------------------------------
static void (*s_playerControlOrig)(PlayerInterface*, InputHandler&);

static void playerControl_hook(PlayerInterface* thisptr, InputHandler& k)
{
    if (s_dcShutdownInProgress) {
        if (!s_hookBlockLoggedPCtrl) {
            s_hookBlockLoggedPCtrl = true;
            DebugLog("[WASDCombat] dc_hooks_blocked_during_loadgame hook=playerControl"); }
        s_playerControlOrig(thisptr, k); return; }
    ScopeTimer _tPC(s_prof_playerControl);
    if (!ou || !ou->player || dcRealLoad())
    {
        s_playerControlOrig(thisptr, k);
        return;
    }
    bool interactionOpenedThisFrame = false;
    if (s_mode == MODE_FREE_MOVE && !s_lootUiSuspendActive)
    {
        if (s_interactionOpenPending)
        {
            RootObject* target = s_interactionPendingHandle.isValid()
                ? s_interactionPendingHandle.getRootObject() : nullptr;
            s_interactionOpenPending = false;
            s_interactionPendingHandle.setNull();
            if (manualInteractionRootValid(target, s_freeMoveAnchor))
            {
                // showContextMenu is the original hold-RMB path.  Set its latch
                // and the matching InputHandler state before the original
                // playerControl update, otherwise it observes an immediate
                // release and executes/closes the default option in one frame.
                thisptr->contextMenu.rightMouseActivated = true;
                k.mRight = k.lastMRight = true;
                k.mRDown = k.mRUp = false;
                s_interactionMenuByE = true;
                thisptr->contextMenu.showContextMenu(true, target);
                interactionOpenedThisFrame = thisptr->contextMenu.isVisible();
                if (!interactionOpenedThisFrame)
                {
                    s_interactionMenuByE = false;
                    VerbLog("[Ironwind] interact_menu_open_failed reason=no_native_options");
                }
            }
            else VerbLog("[Ironwind] interact_menu_open_failed reason=stale_target");
        }
        if (s_interactionMenuByE && thisptr->contextMenu.isVisible())
        {
            // Native hold-menu normally expects RMB to remain down. Keep only
            // that latch alive; MyGUI receives the user's real pointer/buttons
            // and executes its own option callbacks.
            k.mRight = k.lastMRight = true;
            k.mRDown = k.mRUp = false;
        }
        else if (s_interactionMenuByE)
        {
            manualInteractionScheduleResume("input_phase_close");
        }
        if (mouseCombatContext())
        {
            // Let the poller own mouse combat without issuing vanilla click
            // orders or opening the right-click context menu during a guard.
            k.mLeft = k.mRight = k.lastMLeft = k.lastMRight = false;
            k.mLDown = k.mRDown = k.mLUp = k.mRUp = false;
        }
        bool wasdHeld = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
        bool atTurret = isUsingStationaryTurret(s_freeMoveAnchor);

        if (atTurret && !wasdHeld)
        {
            // Manning a turret: preserve directional inputs for turret aiming.
            static ULONGLONG s_turretProtTick = 0;
            ULONGLONG t = GetTickCount64();
            if (t - s_turretProtTick >= 2000) { s_turretProtTick = t;
                DebugLog("[WASDCombat] stationary_crossbow_action_detected");
                DebugLog("[WASDCombat] stationary_action_protected");
                DebugLog("[WASDCombat] vmode_suppression_skipped_stationary_crossbow"); }
        }
        else
        {
            // Suppress camera pan inputs while DC is active.
            // Zeroing k.up/down/left/right prevents keyboard scroll from detaching
            // the camera lock.  During WASD hold this also blocks AI facing override.
            k.up    = false;
            k.down  = false;
            k.left  = false;
            k.right = false;
            k.pgup  = false;
            k.pgdn  = false;
            static ULONGLONG s_panSupLogTick = 0;
            ULONGLONG t = GetTickCount64();
            if (t - s_panSupLogTick >= 2000) { s_panSupLogTick = t;
                if (g_log.debugLogging && g_log.verboseMovementLogs)
                    DebugLog("[WASDCombat] free_camera_input_suppressed_dc"); }
        }
    }
    s_playerControlOrig(thisptr, k);

    if (s_mode == MODE_FREE_MOVE && s_interactionMenuByE)
    {
        if (thisptr->contextMenu.isVisible())
        {
            // Pause only after the native player-control update accepted and
            // retained the menu.  Existing pauses are never claimed.
            if (!s_interactionPauseOwned && ou && !ou->isPaused())
            {
                ou->userPause(true);
                s_interactionPauseOwned = true;
            }
            if (interactionOpenedThisFrame)
            {
                char b[224];
                sprintf_s(b, sizeof(b),
                    "[Ironwind] interact_menu_open visible=1 orders=%u pause_owned=%d",
                    (unsigned)thisptr->contextMenu.orders.size(),
                    s_interactionPauseOwned ? 1 : 0);
                VerbLog(b);
            }
        }
        else manualInteractionScheduleResume("post_input_close");
    }

    if (s_mode == MODE_FREE_MOVE
        && !s_fpActive && !s_firstPersonActive   // OTS/FP own a DETACHED camera node — never
        && !s_otsCamActive                       // (detached OTS flag — was missing: 5,169
                                                 //  re-tracks in one 5-min OTS run, 09-12)
                                     // re-track it here (this fired every frame
                                     // during OTS, fighting the detached camera
                                     // and breaking it after a reload).
        && !s_lootUiSuspendActive
        && !s_cameraLockTurretSuspend
        && !s_dcPtrLossActive
        && s_freeMoveAnchor != nullptr
        && !ou->player->isTrackingCharacter())
    {
        ou->player->startTrackCharacter(s_freeMoveAnchor);
        DebugLog("[WASDCombat] camera_restored_after_external_detach");
    }
}

// -----------------------------------------------------------------------
// taskTypeName — used by removeJob_hook
// -----------------------------------------------------------------------
static const char* taskTypeName(TaskType t)
{
    switch (t)
    {
        case MELEE_ATTACK:               return "MELEE_ATTACK";
        case FOCUSED_MELEE_ATTACK:       return "FOCUSED_MELEE_ATTACK";
        case UNPROVOKED_FOCUSED_MELEE_ATTACK: return "UNPROVOKED_FOCUSED_MELEE_ATTACK";
        case CHOOSE_ENEMY_AND_ATTACK:    return "CHOOSE_ENEMY_AND_ATTACK";
        case CHOOSE_ATTACKER_OF_ALLY:    return "CHOOSE_ATTACKER_OF_ALLY";
        case ATTACK_CHARACTERS_ATTACKER: return "ATTACK_CHARACTERS_ATTACKER";
        case ATTACK_ATTACKERS_OF:        return "ATTACK_ATTACKERS_OF";
        case PROTECT_ALLIES:             return "PROTECT_ALLIES";
        case ATTACK_ENEMIES:             return "ATTACK_ENEMIES";
        case JOB_MEDIC:                  return "JOB_MEDIC";
        case FIRST_AID_ORDER:            return "FIRST_AID_ORDER";
        case FIRST_AID_ROBOT:            return "FIRST_AID_ROBOT";
        case JOB_REPAIR_ROBOT:           return "JOB_REPAIR_ROBOT";
        case SPLINT_ORDER:               return "SPLINT_ORDER";
        case SPLINT_JOB:                 return "SPLINT_JOB";
        case HEAL_MY_LEGS:               return "HEAL_MY_LEGS";
        default: { static char buf[32]; sprintf_s(buf, sizeof(buf), "TASK_%d", (int)t); return buf; }
    }
}

// -----------------------------------------------------------------------
// Character::removeJob hook — misclassification guard
// -----------------------------------------------------------------------
static void (*s_removeJobOrig)(Character* thisptr, TaskType t);

static void removeJob_hook(Character* thisptr, TaskType t)
{
    if (s_dcShutdownInProgress) {
        if (!s_hookBlockLoggedRemJob) {
            s_hookBlockLoggedRemJob = true;
            DebugLog("[WASDCombat] dc_hooks_blocked_during_loadgame hook=removeJob"); }
        s_removeJobOrig(thisptr, t); return; }
    if (!s_loadGuardActive && s_mode == MODE_FREE_MOVE && thisptr == s_freeMoveAnchor)
    {
        bool isMedicalJob = (t == JOB_MEDIC       || t == FIRST_AID_ORDER  ||
                              t == FIRST_AID_ROBOT || t == JOB_REPAIR_ROBOT ||
                              t == SPLINT_ORDER    || t == SPLINT_JOB       ||
                              t == HEAL_MY_LEGS);

        // Clear healing job flags — DC movement injection resumes.
        if (isMedicalJob && (s_healingJobActive || s_healingJobPending))
        {
            s_healingJobActive  = false;
            s_healingJobPending = false;
            DebugLog("[WASDCombat] medical_job_ended_restore_dc");
            DebugLog("[WASDCombat] healing_action_completed");
        }

    }
    s_removeJobOrig(thisptr, t);
}

// -----------------------------------------------------------------------
// Character::addJob hook — log attack job creation
// -----------------------------------------------------------------------

// -----------------------------------------------------------------------
// Character::addOrder hook — door suppression on the player-order channel
// (separate from the job queue; proven hookable via GetRealAddress).  Same
// rule as the addJob gate: door-type orders on the anchor are swallowed
// ONLY while the post-WASD hold is active.  Everything else — including
// MOVE_CUS_ORDERED, which DC's own disengage orders use — passes through.
// -----------------------------------------------------------------------
static void (*s_addOrderOrig)(Character* thisptr, Building* dest, TaskType t,
                              RootObject* subject, bool shift, bool clear,
                              const Ogre::Vector3& location);

static void addOrder_hook(Character* thisptr, Building* dest, TaskType t,
                          RootObject* subject, bool shift, bool clear,
                          const Ogre::Vector3& location)
{
    // ANY MOVE while an inventory window is open — swallow it (matches the
    // addJob_hook rule).  subject==nullptr ⇒ a move (to a position OR a
    // building/door); loot/attack/interact/equip carry a subject and pass
    // through.  This is INDEPENDENT of s_fpActive (the face-cam): movement is
    // blocked whenever inventory is open, so a point-click can't walk the
    // character even if the face-cam isn't engaged (field 2026-06-17: moves with
    // a non-null dest leaked through because the old gate required dest==null OR
    // s_fpActive, and s_fpActive is false whenever the face-cam is off/broken).
    // `|| s_fpActive` additionally blocks subject-bearing clicks during the
    // face-cam.  Any PLAYER character, not just the anchor (the face-cam can lock
    // onto the SELECTED character).
    if (!s_dcShutdownInProgress && !s_loadGuardActive
        && s_mode == MODE_FREE_MOVE && thisptr && thisptr->isPlayerCharacter()
        && (subject == nullptr || s_fpActive)
        && gui && gui->isAnyInventoryWindowOpen())
    {
        static ULONGLONG s_invOrdTick = 0;
        ULONGLONG nowIO = GetTickCount64();
        if (nowIO - s_invOrdTick >= 1000) { s_invOrdTick = nowIO;
            char b[120]; sprintf_s(b, sizeof(b),
                "[WASDCombat] dc_order_suppressed_inventory hook=addOrder task=%d", (int)t);
            DebugLog(b); }
        return;
    }

    // DIAG (2026-09-14): every order reaching a SLAVE anchor (any state) —
    // who issues what around the IS_SLAVE -> ESCAPING flip.  1 s throttle.
    if (g_log.debugLogging && !s_dcShutdownInProgress && !s_loadGuardActive
        && thisptr == s_freeMoveAnchor && thisptr->isSlave() != NOT_SLAVE)
    {
        static ULONGLONG s_slvOrdTick = 0;
        ULONGLONG nowSO = GetTickCount64();
        if (nowSO - s_slvOrdTick >= 1000) { s_slvOrdTick = nowSO;
            char ob[160]; sprintf_s(ob, sizeof(ob),
                "[WASDCombat] dc_slave_addorder task=%d clear=%d subject=%d dest=%d state=%d",
                (int)t, (int)clear, subject ? 1 : 0, dest ? 1 : 0, (int)thisptr->isSlave());
            DebugLog(ob); }
    }

    // Diagnostic: every order reaching the anchor while the hold is active.
    // The clear flag is the candidate discriminator between a fresh player
    // click's order and a stale automatic re-issue — field data from this
    // log decides whether addOrder can ever clear the hold safely.
    if (!s_dcShutdownInProgress && !s_loadGuardActive
        && s_mode == MODE_FREE_MOVE && thisptr == s_freeMoveAnchor
        && s_wasdHoldActive)
    {
        ULONGLONG nowAD = GetTickCount64();
        if (nowAD - s_addOrderDiagLogTick >= 1000)
        {
            s_addOrderDiagLogTick = nowAD;
            char dbuf[160];
            sprintf_s(dbuf, sizeof(dbuf),
                "[WASDCombat] dc_addorder_during_hold task=%d clear=%d dest=%p",
                (int)t, (int)clear, (void*)dest);
            VerbLog(dbuf);
        }
    }

    if (!s_dcShutdownInProgress && !s_loadGuardActive
        && s_mode == MODE_FREE_MOVE && thisptr == s_freeMoveAnchor
        && s_wasdHoldActive && !s_playerPointClickActive
        && !s_lootUiSuspendActive && !s_cameraLockTurretSuspend
        && !s_menuSuspendActive
        && !slaveEscapeZone(thisptr)         // escaping slave: the AI's door tasks must run
        && isDoorInteractionTask(t))
    {
        ULONGLONG nowDO = GetTickCount64();
        if (nowDO - s_doorSuppressLogTick >= 1000)
        {
            s_doorSuppressLogTick = nowDO;
            char obuf[128];
            sprintf_s(obuf, sizeof(obuf),
                "[WASDCombat] dc_door_addorder_suppressed task=%d", (int)t);
            DebugLog(obuf);
        }
        return;  // swallowed — the order never enters the queue
    }

    s_addOrderOrig(thisptr, dest, t, subject, shift, clear, location);
}

static void (*s_addJobOrig)(Character* thisptr, TaskType t, RootObject* subject,
                             bool shift, bool addDontClear, const Ogre::Vector3& location);

static void addJob_hook(Character* thisptr, TaskType t, RootObject* subject,
                         bool shift, bool addDontClear, const Ogre::Vector3& location)
{
    if (s_dcShutdownInProgress) {
        if (!s_hookBlockLoggedAddJob) {
            s_hookBlockLoggedAddJob = true;
            DebugLog("[WASDCombat] dc_hooks_blocked_during_loadgame hook=addJob"); }
        s_addJobOrig(thisptr, t, subject, shift, addDontClear, location); return; }

    // Ground-click MOVE while an inventory window is open: swallow it so the
    // freed cursor can't walk the controlled character (the OTS "no point-click
    // while in inventory" feel).  The order reaches the anchor here, NOT through
    // playerMove (field log 2026-06-15: playerMove suppression never fired).
    // subject==nullptr ⇒ a pure position move; loot/attack/interact carry a
    // subject, so equipping/looting/attacking are unaffected.  gui->isAny... is
    // authoritative (s_lootUiSuspendActive can lag a frame).
    if (!s_loadGuardActive && s_mode == MODE_FREE_MOVE
        && thisptr && thisptr->isPlayerCharacter()
        && (subject == nullptr || s_fpActive)   // s_fpActive = own-inventory face-cam: block ALL
        && gui && gui->isAnyInventoryWindowOpen())
    {
        static ULONGLONG s_invJobTick = 0;
        ULONGLONG nowIJ = GetTickCount64();
        if (nowIJ - s_invJobTick >= 1000) { s_invJobTick = nowIJ;
            char b[120]; sprintf_s(b, sizeof(b),
                "[WASDCombat] dc_order_suppressed_inventory hook=addJob task=%d", (int)t);
            DebugLog(b); }
        return;
    }

    if (!s_loadGuardActive && s_mode == MODE_FREE_MOVE && thisptr == s_freeMoveAnchor)
    {
        // Door suppression — ONLY while the post-WASD hold is active: that
        // is the window where no player intent exists and stale indoor
        // door tasks used to auto-fire.  Vanilla door behavior everywhere
        // else (point-clicks, fresh V-mode, suspends, V OFF).
        if (s_wasdHoldActive && !s_playerPointClickActive
            && !s_lootUiSuspendActive && !s_cameraLockTurretSuspend
            && !s_menuSuspendActive
            && !slaveEscapeZone(thisptr)     // escaping slave: the AI's door tasks must run
            && isDoorInteractionTask(t))
        {
            ULONGLONG nowDJ = GetTickCount64();
            if (nowDJ - s_doorSuppressLogTick >= 1000)
            {
                s_doorSuppressLogTick = nowDJ;
                char jbuf[128];
                sprintf_s(jbuf, sizeof(jbuf),
                    "[WASDCombat] dc_door_addjob_suppressed task=%d", (int)t);
                DebugLog(jbuf);
            }
            return;  // swallowed — the job never enters the queue
        }

        bool isAttackJob  = (t == MELEE_ATTACK            || t == FOCUSED_MELEE_ATTACK      ||
                              t == UNPROVOKED_FOCUSED_MELEE_ATTACK ||
                              t == CHOOSE_ENEMY_AND_ATTACK || t == ATTACK_CHARACTERS_ATTACKER ||
                              t == ATTACK_ENEMIES);
        bool isMedicalJob = (t == JOB_MEDIC        || t == FIRST_AID_ORDER  ||
                              t == FIRST_AID_ROBOT  || t == JOB_REPAIR_ROBOT ||
                              t == SPLINT_ORDER     || t == SPLINT_JOB       ||
                              t == HEAL_MY_LEGS);

        // Healing job detection with WASD-aware deferral.
        if (isMedicalJob)
        {
            if (s_healingJobActive)
            {
                // Already committed — DC has already yielded; do not re-interrupt.
                DebugLog("[WASDCombat] dc_heal_committed_action_preserved");
            }
            else if (!s_healingJobPending)
            {
                DebugLog("[WASDCombat] dc_auto_heal_job_detected");
                bool wasdNow = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
                if (wasdNow)
                {
                    // Defer: WASD active — preserve locomotion until keys release.
                    s_healingJobPending = true;
                    DebugLog("[WASDCombat] dc_auto_heal_deferred_due_to_wasd");
                }
                else
                {
                    // WASD not held: activate immediately and zero stale motion.
                    s_healingJobActive = true;
                    if (s_freeMoveAnchor && s_freeMoveAnchor->movement && !isDownedButMovable(s_freeMoveAnchor))
                    {
                        CharMovement* mvH = s_freeMoveAnchor->movement;
                        mvH->halt();
                        mvH->desiredMotion    = Ogre::Vector3::ZERO;
                        mvH->moveLimit        = 0.0f;
                        mvH->currentMotion    = Ogre::Vector3::ZERO;
                        s_wasdMovementApplied = false;
                        s_prevWasdDir         = Ogre::Vector3::ZERO;
                        dcSnapCancelOrder(s_freeMoveAnchor);   // gated: no bark indoors/locked
                        DebugLog("[WASDCombat] medical_job_started_anchor");
                        DebugLog("[WASDCombat] medical_job_movement_zeroed");
                    }
                    DebugLog("[WASDCombat] dc_manual_heal_allowed");
                    DebugLog("[WASDCombat] healing_action_detected");
                }
            }
        }

        if (isAttackJob)
        {
            char buf[128];
            sprintf_s(buf, sizeof(buf), "[WASDCombat] attack_job_created type=%s", taskTypeName(t));
            DebugLog(buf);
        }
        if (g_log.debugLogging && !isAttackJob && !isMedicalJob)
        {
            char axbuf[128];
            sprintf_s(axbuf, sizeof(axbuf),
                "[WASDCombat] dc_xp_vanilla_action_allowed skill=%s", taskTypeName(t));
            DebugLog(axbuf);
        }
    }
    s_addJobOrig(thisptr, t, subject, shift, addDontClear, location);
}

// -----------------------------------------------------------------------
// Native Controls-menu keybind hooks (v1.7, KEP pattern)
// -----------------------------------------------------------------------

// InputHandler::loadConfig — register Direct Control commands BEFORE the
// original runs so the game's keyboard config applies any user-saved
// bindings on top of the defaults, and the game persists rebinds itself.
static bool s_processKeysHookOk = false;  // set at install; native registration
                                          // requires the event reader too

// Belt-and-braces persistence: the game saves bound plugin commands to
// controls.cfg (proven by KEP's toggle_devtools=F12), but our own INI copy
// guards against any case where the command ends up unbound at save time.
// The value is the raw bound int (OIS code | modifier masks), round-
// tripped verbatim through InputHandler::bind.
// SPEEDSYNC-TWEAK: dc_speed_cycle REMOVED (v1.4.0, user req 2026-09-03) — it
// duplicated Kenshi's own cycle_run_speed command (both drive OrdersPanel::
// speedNext).  DC now listens to the vanilla cycle_run_speed key in every
// mode and the DC-tab row edits that same vanilla bind (see processKeys_hook
// + the tab injector).  Only dc_toggle remains a plugin-owned native command.
static const int DC_CMD_COUNT = 1;
static const char* const DC_CMD_NAMES[DC_CMD_COUNT] =
{
    "dc_toggle"
};

// [NativeBinds] format version.  v1 (no "version=" key) WROTE Command::bound
// (0x40) — which is NOT the keycode (field 2026-06-20: bind(name,1) then read
// bound = 2, MISMATCH), so it persisted garbage ("=1") and menu rebinds never
// survived a restart.  v2 persists the real keycode via getBoundKeys().  On a
// version mismatch the old per-command values are IGNORED (defaults stand) and
// the file is re-stamped, so corrupted v1 INIs self-heal instead of binding the
// command to key 1.
static const int DC_NATIVE_BIND_FORMAT_VERSION = 2;

// readBoundKey — the keycode the dc_ command is CURRENTLY bound to (its first
// bound key) via the public InputHandler::getBoundKeys API.  Returns INT_MIN when
// the command is unknown or unbound.  Do NOT read Command::bound (0x40): it is an
// internal value, not the keycode.
static int readBoundKey(const char* name)
{
    if (!key) return INT_MIN;
    lektor<int> keys = key->getBoundKeys(name);
    if (keys.size() == 0) return INT_MIN;   // command unbound
    return keys[0];
}

// readChangeToken — a CHEAP, non-allocating value that merely CHANGES when the
// command's binding changes, used only for change DETECTION (getBoundKeys returns
// a heap lektor, too costly to poll every few seconds).  Command::bound is not
// the keycode but it does differ per binding, so it is a valid change token.
static int readChangeToken(const char* name)
{
    if (!key) return INT_MIN;
    auto it = key->commands.find(name);
    if (it == key->commands.end()) return INT_MIN;
    return it->second.bound;
}

static void saveNativeBindsToIni(const char* reason)
{
    if (!key) return;
    char path[MAX_PATH];
    getConfigPath(path, sizeof(path));
    // Stamp the format version first so a partially-written file is still
    // recognised as v2 (and never re-applies the v1 garbage).
    {
        char vbuf[16];
        sprintf_s(vbuf, sizeof(vbuf), "%d", DC_NATIVE_BIND_FORMAT_VERSION);
        WritePrivateProfileStringA("NativeBinds", "version", vbuf, path);
    }
    for (int i = 0; i < DC_CMD_COUNT; ++i)
    {
        int bound = readBoundKey(DC_CMD_NAMES[i]);
        if (bound == INT_MIN)
        {
            char ebuf[96];
            sprintf_s(ebuf, sizeof(ebuf),
                "[WASDCombat] dc_native_bind_save_failed name=%s reason=unbound_or_not_found",
                DC_CMD_NAMES[i]);
            DebugLog(ebuf);
            continue;
        }
        char val[16];
        sprintf_s(val, sizeof(val), "%d", bound);
        BOOL ok = WritePrivateProfileStringA("NativeBinds", DC_CMD_NAMES[i], val, path);
        char sbuf[160];
        sprintf_s(sbuf, sizeof(sbuf),
            "[WASDCombat] dc_native_bind_saved name=%s bound=%d write_ok=%d",
            DC_CMD_NAMES[i], bound, (int)ok);
        DebugLog(sbuf);
    }
    char rbuf[MAX_PATH + 96];
    sprintf_s(rbuf, sizeof(rbuf),
        "[WASDCombat] dc_native_binds_saved reason=%s path=%s", reason, path);
    DebugLog(rbuf);
}

// Periodic change detector — persistence must not depend on the options
// menu calling saveOptions (and its logs reveal whether a menu rebind even
// updates Command::bound).  Runs from mainLoop every BIND_WATCH_INTERVAL_MS
// once native bindings are registered; first pass only snapshots.
static int       s_bindSnapshot[DC_CMD_COUNT] = { 0 };
static bool      s_bindSnapshotValid    = false;
static ULONGLONG s_bindWatchTick        = 0;
static const ULONGLONG BIND_WATCH_INTERVAL_MS = 3000;

static void watchNativeBindChanges()
{
    ULONGLONG nowBW = GetTickCount64();
    if (nowBW - s_bindWatchTick < BIND_WATCH_INTERVAL_MS) return;
    s_bindWatchTick = nowBW;

    int cur[DC_CMD_COUNT];
    for (int i = 0; i < DC_CMD_COUNT; ++i)
        cur[i] = readChangeToken(DC_CMD_NAMES[i]);   // cheap change-detection token

    if (!s_bindSnapshotValid)
    {
        for (int i = 0; i < DC_CMD_COUNT; ++i) s_bindSnapshot[i] = cur[i];
        s_bindSnapshotValid = true;
        return;
    }

    bool changed = false;
    for (int i = 0; i < DC_CMD_COUNT; ++i)
    {
        if (cur[i] != s_bindSnapshot[i])
        {
            char cbuf[160];
            sprintf_s(cbuf, sizeof(cbuf),
                "[WASDCombat] dc_native_bind_change_detected name=%s old=%d new=%d",
                DC_CMD_NAMES[i], s_bindSnapshot[i], cur[i]);
            DebugLog(cbuf);
            s_bindSnapshot[i] = cur[i];
            changed = true;
        }
    }
    if (changed)
        saveNativeBindsToIni("change_detected");
}

static void applyNativeBindsFromIni(InputHandler* self)
{
    char path[MAX_PATH];
    getConfigPath(path, sizeof(path));

    // Migration guard: only apply stored binds written by the CURRENT format.
    // A v1 file (no "version=" key, or < current) stored Command::bound garbage
    // (e.g. "=1"); applying it would bind the command to key 1.  Ignore those,
    // leave the addCommand default (V) in place, and re-stamp the file so it
    // self-heals to v2 going forward.
    int ver = (int)GetPrivateProfileIntA("NativeBinds", "version", 0, path);
    if (ver < DC_NATIVE_BIND_FORMAT_VERSION)
    {
        char mbuf[MAX_PATH + 96];
        sprintf_s(mbuf, sizeof(mbuf),
            "[WASDCombat] dc_native_binds_migrated old_ver=%d -> v%d (defaults kept) path=%s",
            ver, DC_NATIVE_BIND_FORMAT_VERSION, path);
        DebugLog(mbuf);
        saveNativeBindsToIni("format_migration");   // re-stamp version + correct keycodes
        return;
    }

    int applied = 0;
    for (int i = 0; i < DC_CMD_COUNT; ++i)
    {
        int v = (int)GetPrivateProfileIntA("NativeBinds", DC_CMD_NAMES[i], -1, path);
        if (v > 0)
        {
            // bind() ADDS a key, it does not replace — so the addCommand default
            // (V) would remain ALONGSIDE the saved key and BOTH would fire
            // (field 2026-06-20: default + new bind both activated after restart).
            // Unbind the command first so exactly the saved key remains.  This
            // also cleans up any leftover double-binding from the old format.
            self->unbind(std::string(DC_CMD_NAMES[i]));
            self->bind(DC_CMD_NAMES[i], v);
            int after = readBoundKey(DC_CMD_NAMES[i]);
            char abuf[160];
            sprintf_s(abuf, sizeof(abuf),
                "[WASDCombat] dc_native_bind_applied name=%s ini=%d bound_after=%d%s",
                DC_CMD_NAMES[i], v, after,
                (after == v) ? "" : " MISMATCH");
            DebugLog(abuf);
            ++applied;
        }
    }
    char cbuf[MAX_PATH + 96];
    sprintf_s(cbuf, sizeof(cbuf),
        "[WASDCombat] dc_native_binds_applied count=%d path=%s", applied, path);
    DebugLog(cbuf);
}

// iniSavedNativeBind — the keycode a dc_ command was rebound to in the v2
// [NativeBinds] INI, or -1 if the user never rebound it (no file, pre-v2
// format, or value absent).  Mirrors applyNativeBindsFromIni's version gate
// exactly.  Used at registration to decide whether to claim the V default
// at all: if the user already moved the command (e.g. to Ctrl+V), registering
// plain V would let Kenshi's one-command-per-key rule steal V from any
// vanilla command (camera tilt, zoom-out) the player bound there, wiping it
// every session (field report 2026-06-23).
static int iniSavedNativeBind(const char* name)
{
    char path[MAX_PATH];
    getConfigPath(path, sizeof(path));
    int ver = (int)GetPrivateProfileIntA("NativeBinds", "version", 0, path);
    if (ver < DC_NATIVE_BIND_FORMAT_VERSION) return -1;
    int v = (int)GetPrivateProfileIntA("NativeBinds", name, -1, path);
    return (v > 0) ? v : -1;
}

// registerNativeCommands — register dc_toggle and apply
// INI-persisted bindings.  FIELD FINDING (2026-06-10 log): the game runs
// InputHandler::loadConfig BEFORE RE_Kenshi loads plugins, so a loadConfig
// hook alone never fires.  This is therefore called from the first
// mainLoop pass (key global valid, main thread) — the loadConfig hook
// remains only as a re-registration path if the game ever reloads its
// keyboard config. Only the V toggle is plugin-owned; movement keys must never be
// registered (one command per key; vanilla camera owns W/S/A/D).
static void registerNativeCommands(InputHandler* self)
{
    if (s_nativeCommandsRegistered || !self) return;
    if (!s_processKeysHookOk)
    {
        // Without the event reader, toggle presses would be lost —
        // stay on the INI/poll fallback entirely.
        DebugLog("[WASDCombat] dc_native_keybinds_skipped_no_event_reader");
        return;
    }
    // Claim the V default ONLY when the user has not rebound the command.
    // If a saved rebind exists (e.g. Ctrl+V), register with NO physical key so
    // loadConfig never steals plain V from a vanilla binding; the
    // saved key is restored by applyNativeBindsFromIni immediately below.
    const int savedToggle = iniSavedNativeBind("dc_toggle");
    self->addCommand("dc_toggle",        0,
                     (savedToggle > 0) ? OIS::KC_UNASSIGNED : OIS::KC_V,
                     OIS::KC_UNASSIGNED, InputHandler::NONE_MASK, InputHandler::GLOBAL);
    // SPEEDSYNC-TWEAK: no dc_speed_cycle command — cycle_run_speed (vanilla)
    // is used instead (processKeys_hook syncs the anchor's speed to it).
    char rnbuf[128];
    sprintf_s(rnbuf, sizeof(rnbuf),
        "[WASDCombat] dc_native_register defaults toggle=%s",
        (savedToggle > 0) ? "deferred(rebound)" : "V");
    DebugLog(rnbuf);
    applyNativeBindsFromIni(self);       // our INI is the real persistence
    s_nativeCommandsRegistered = true;   // poll-thread toggle stands down
    DebugLog("[WASDCombat] dc_native_keybinds_registered");
}

static void (*s_inputLoadConfigOrig)(InputHandler*);
static void inputLoadConfig_hook(InputHandler* self)
{
    registerNativeCommands(self);
    s_inputLoadConfigOrig(self);
    if (s_nativeCommandsRegistered)
        applyNativeBindsFromIni(self);   // re-assert ours over any cfg reload
}

// OptionsWindow::saveOptions — the game just saved every binding it knows
// about to controls.cfg, which excludes plugin commands; persist ours.
static void (*s_optionsSaveOrig)(OptionsWindow*);
static void optionsSave_hook(OptionsWindow* self)
{
    s_optionsSaveOrig(self);
    if (s_nativeCommandsRegistered)
        saveNativeBindsToIni("save_options");
}

// OptionsWindow::create — after the original builds the options UI, find
// the Controls tab (category 0x19) and append the Direct Control rows.
// Rebinding then uses the game's own press-a-key flow and conflict
// handling; nothing custom is drawn.
static void (*s_optionsCreateOrig)(OptionsWindow*);
static void optionsCreate_hook(OptionsWindow* self)
{
    s_optionsCreateOrig(self);

    // v1.5: ALL Direct Control rows now live on the mod's own settings page
    // (sliders + checkboxes + keybinds) — see the NATIVE SETTINGS TAB block.
    // The dc_toggle and vanilla cycle_run_speed rows moved there from the
    // Controls tab (user request 2026-09-02); their registration and
    // saveOptions persistence are unchanged.
    dcInjectSettingsTab(self);
}

// GameWorld::processKeys — toggle and observed vanilla speed-cycle events arrive in key->events
// for exactly one processKeys cycle; consume them here on the main thread.
// No pause gate: the V toggle has always worked while paused (poll-thread
// behavior preserved).  Loading screens are skipped.
static void (*s_processKeysOrig)(GameWorld* thisptr);
static void processKeys_hook(GameWorld* thisptr)
{
    if (key && mouseCombatContext())
    {
        key->mLeft = key->mRight = key->lastMLeft = key->lastMRight = false;
        key->mLDown = key->mRDown = key->mLUp = key->mRUp = false;
    }
    // While DC is on, a physical mod key must not also dispatch the vanilla
    // command bound to the same key. Orig never clears key->events, so erasing
    // the matching command BEFORE orig reliably stops the double action.
    if (s_mode == MODE_FREE_MOVE && !s_dcShutdownInProgress && key)
    {
        // Build 53 (user 2026-09-23: "F2 stops working"): these vanilla commands
        // are dropped ONLY while the mod key that collides with them is physically
        // down.  The old rule dropped every speed_1 / speed_3 / toggle_block event
        // by NAME, so F2 (speed_1 in controls.cfg), F4 (speed_3) and NUM0 (block
        // toggle) were eaten for the whole time DC was on, not just for Q / E / G.
        bool viewDown   = (GetAsyncKeyState(s_bindVk[KR_FP]) & 0x8000) != 0;
        bool attackDown = s_settingManualBlock && !s_directionalCombat
                       && (GetAsyncKeyState(s_bindVk[KR_ATTACK]) & 0x8000) != 0;
        bool blockDown  = s_settingManualBlock && !s_directionalCombat
                       && (GetAsyncKeyState(s_bindVk[KR_BLOCK])  & 0x8000) != 0;
        for (auto it = key->events.begin(); it != key->events.end(); )
        {
            const std::string& en = (*it)->name;
            // v117: FirstPerson sits on F = vanilla focus_char. Consume that
            // one collision only in V mode; outside V the original F survives.
            bool drop = (en == "focus_char"   && viewDown)
                     || (en == "toggle_block" && blockDown)
                     || (en == "speed_3"      && attackDown);
            if (drop) it = key->events.erase(it);
            else ++it;
        }
    }
    s_processKeysOrig(thisptr);
    if (s_dcShutdownInProgress || !s_nativeCommandsRegistered)
        return;
    if (gui && gui->isLoadingMessageVisible())
        return;
    for (auto it = key->events.begin(); it != key->events.end(); ++it)
    {
        const std::string& n = (*it)->name;
        if (n == "dc_toggle")             handleTogglePress();
        // SPEEDSYNC-TWEAK: vanilla cycle_run_speed drives the speed in DC too.
        // Orig already ran speedNext (cycled the orders panel) this frame, so
        // do NOT cycle again — just flag the anchor's desired speed to be
        // re-synced to the panel in mainLoop (the DC anchor runs under
        // MOVE_DIRECTION, where the panel change doesn't reach its movement
        // on its own).
        else if (n == "cycle_run_speed" && s_mode == MODE_FREE_MOVE)
            s_syncSpeedToAnchor = true;
    }
}

// -----------------------------------------------------------------------
// verifyPatchSiteBytes — REQUIRED gate for any hook installed by raw RVA.
//
// KenshiLib::GetRealAddress hooks are symbol-based and survive exe changes;
// raw-RVA hooks do not.  RE_Kenshi regenerates its patched Kenshi_x64.exe
// on its own updates (last: 2026-06-21), silently shifting all code — a
// stale RVA then patches the middle of an unrelated instruction and MinHook
// still reports SUCCESS.  That shipped two landmines in v1.3.0 (playerMove
// RVA → navmesh crash, showTradeWindow RVA → corrupted conversion helper;
// see the retired install blocks in startPlugin).
//
// Usage: record the first `len` bytes at the target RVA from the SAME exe
// the RVA was derived on, and only AddHook when they still match.  On
// mismatch the hook is skipped (feature degrades, nothing corrupts) and the
// actual bytes are logged for re-derivation.
// -----------------------------------------------------------------------
static bool verifyPatchSiteBytes(intptr_t addr, const unsigned char* expected,
                                 size_t len, const char* name)
{
    if (len > 16) len = 16;
    if (memcmp((const void*)addr, expected, len) == 0)
        return true;
    char hex[3 * 16 + 1] = { 0 };
    for (size_t i = 0; i < len; ++i)
        sprintf_s(hex + 3 * i, sizeof(hex) - 3 * i, "%02X ",
                  ((const unsigned char*)addr)[i]);
    char buf[224];
    sprintf_s(buf, sizeof(buf),
        "WASDCombatPlugin: %s RVA hook SKIPPED — patch-site bytes changed (exe updated?), got: %s",
        name, hex);
    ErrorLog(buf);
    return false;
}

// -----------------------------------------------------------------------
// DllMain / startPlugin
// -----------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        s_thisModule = hModule;   // for the keybind INI path (DLL directory)
    if (reason == DLL_PROCESS_DETACH)
        DebugLog("WASDCombatPlugin: unloaded");
    return TRUE;
}

__declspec(dllexport) void startPlugin()
{
    // SINGLE-INSTANCE GUARD (build 54; field 2026-09-26): the Workshop release
    // and this test mod were both enabled (a Workshop update re-appeared in the
    // mod list), so TWO copies of the plugin hooked the game - every hook ran
    // twice and two first-person drivers fought over one camera (view upside
    // down, mouse look dead).  The first copy to start owns the process-wide
    // mutex; any later copy logs the conflict and installs NOTHING.
    {
        HANDLE mtx = CreateMutexA(NULL, FALSE, "Local\\WASDCombatPlugin_DirectControl_SingleInstance");
        if (mtx && GetLastError() == ERROR_ALREADY_EXISTS)
        {
            ErrorLog("WASDCombatPlugin: ANOTHER COPY of Direct Control is already loaded (two DC mods enabled?) - this copy is disabled. Enable only one.");
            CloseHandle(mtx);
            return;
        }
        // (handle intentionally kept for the life of the process)
    }
    DebugLog("Ironwind Combat v" IRONWIND_VERSION " (Direct Control base v" DIRECT_CONTROL_BASE_VERSION ") — manual combat, aim focus, realistic first person");

    // Resolve the non-virtual CombatClass base update explicitly. Calling
    // cc->update() would dispatch to CombatClassAI::update and restore the AI
    // ownership v67 removed. The qualified base implementation dispatches a
    // CHOP only to CombatClass::attackState.
    s_combatBaseUpdateNative = (void (*)(CombatClass*, float))
        KenshiLib::GetRealAddress(&CombatClass::_NV_update);
    if (!s_combatBaseUpdateNative)
        ErrorLog("WASDCombatPlugin: CombatClass base update resolve FAILED — direct native attacks disabled");
    else
    {
        char ub[112];
        sprintf_s(ub, sizeof(ub), "[Ironwind] combat_base_update_resolved rva=%llX",
            (unsigned long long)((intptr_t)s_combatBaseUpdateNative
                - (intptr_t)GetModuleHandleA(NULL)));
        DebugLog(ub);
    }

    // INI keybinds remain the FALLBACK: loaded unconditionally so the poll
    // thread works from frame one and keeps working if the native command
    // registration never fires (load-order or hook failure).  Once
    // dc_native_keybinds_registered appears, the INI is inert.
    loadKeybinds();

    // Native Controls-menu keybinds (KEP pattern) — all three hooks are
    // DIAG (2026-09-14): real RVAs of the functions behind two open questions
    // (CameraClass::teleport side effects; what flips a slave to ESCAPING) so the
    // current RE exe can be disassembled offline — header RVAs are stale.
    {
        intptr_t baseR = (intptr_t)GetModuleHandleA(NULL);
        char rb[320];
        sprintf_s(rb, sizeof(rb),
            "[WASDCombat] dc_rva base=%p teleport=%llX stopFollowing=%llX startTrack=%llX isSlave=%llX setSlaveAIJob=%llX pmoDefault=%llX checkOrder=%llX addOrder=%llX addJob=%llX",
            (void*)baseR,
            (unsigned long long)(KenshiLib::GetRealAddress(&CameraClass::teleport) - baseR),
            (unsigned long long)(KenshiLib::GetRealAddress(&CameraClass::stopFollowing) - baseR),
            (unsigned long long)(KenshiLib::GetRealAddress(&PlayerInterface::startTrackCharacter) - baseR),
            (unsigned long long)(KenshiLib::GetRealAddress(&Character::isSlave) - baseR),
            (unsigned long long)(KenshiLib::GetRealAddress(&Character::setSlaveAIJob) - baseR),
            (unsigned long long)(KenshiLib::GetRealAddress(&Character::_NV_playerMoveOrderDefault) - baseR),
            (unsigned long long)(KenshiLib::GetRealAddress(&Character::checkPlayerOrderForProblems) - baseR),
            (unsigned long long)(KenshiLib::GetRealAddress(&Character::addOrder) - baseR),
            (unsigned long long)(KenshiLib::GetRealAddress(&Character::addJob) - baseR));
        DebugLog(rb);
    }
    // header-declared members resolved via GetRealAddress; no raw-RVA hooks.
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&InputHandler::loadConfig),
            &inputLoadConfig_hook, &s_inputLoadConfigOrig))
        ErrorLog("WASDCombatPlugin: InputHandler::loadConfig hook FAILED — using INI keybind fallback");
    else
        DebugLog("WASDCombatPlugin: InputHandler::loadConfig hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&OptionsWindow::create),
            &optionsCreate_hook, &s_optionsCreateOrig))
        ErrorLog("WASDCombatPlugin: OptionsWindow::create hook FAILED — Controls-menu rows unavailable");
    else
        DebugLog("WASDCombatPlugin: OptionsWindow::create hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&OptionsWindow::saveOptions),
            &optionsSave_hook, &s_optionsSaveOrig))
        ErrorLog("WASDCombatPlugin: OptionsWindow::saveOptions hook FAILED — rebinds will not persist across restarts");
    else
        DebugLog("WASDCombatPlugin: saveOptions hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&GameWorld::processKeys),
            &processKeys_hook, &s_processKeysOrig))
        ErrorLog("WASDCombatPlugin: GameWorld::processKeys hook FAILED — using INI keybind fallback");
    else
    {
        s_processKeysHookOk = true;
        DebugLog("WASDCombatPlugin: processKeys hook OK");
    }

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff),
            &mainLoop_hook, &s_mainLoopOrig))
        ErrorLog("WASDCombatPlugin: mainLoop hook FAILED");
    else
        DebugLog("WASDCombatPlugin: mainLoop hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CharMovement::_NV_update),
            &charMovUpdate_hook, &s_charMovUpdateOrig))
        ErrorLog("WASDCombatPlugin: charMovUpdate hook FAILED");
    else
        DebugLog("WASDCombatPlugin: charMovUpdate hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CharacterHuman::_NV_sheatheWeapon),
            &sheatheWeapon_hook, &s_sheatheWeaponOrig))
        ErrorLog("WASDCombatPlugin: sheatheWeapon hook FAILED — V mode weapon hold unavailable");
    else
        DebugLog("WASDCombatPlugin: sheatheWeapon hook OK");

    // OTS action camera — drive (runs after the game's camera update, before
    // render) and the RTS-clamp bypass.  If either fails, OTS is unavailable
    // but the rest of DC is unaffected.
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CameraClass::update),
            &cameraUpdate_hook, &s_cameraUpdateOrig))
        ErrorLog("WASDCombatPlugin: CameraClass::update hook FAILED — OTS camera unavailable");
    else
        DebugLog("WASDCombatPlugin: cameraUpdate hook OK");

    // Keep all V-mode 3D audio centred on the controlled character even when
    // the visual camera is detached or pulled far away.  Failure is isolated:
    // vanilla camera-listener audio remains active and the rest of V mode works.
    {
        intptr_t audioSubmit = dcResolveAudioListenerSubmit();
        if (!audioSubmit)
            ErrorLog("WASDCombatPlugin: audio listener anchor unavailable — updateAudio signature changed");
        else if (KenshiLib::SUCCESS != KenshiLib::AddHook(
                     audioSubmit, &audioListenerSubmit_hook, &s_audioListenerSubmitOrig))
            ErrorLog("WASDCombatPlugin: audio listener anchor hook FAILED");
        else
            DebugLog("WASDCombatPlugin: audio listener anchor hook OK");
    }

    // The native character emitter rejects ranged events against the visual
    // camera before Wwise sees them.  Override that range only for the actively
    // controlled V-mode character; this complements (and does not replace) the
    // listener-position hook above.
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&Character::audioEvent),
            &characterAudioEvent_hook, &s_characterAudioEventOrig))
        ErrorLog("WASDCombatPlugin: character audio range hook FAILED — distant V-mode character sounds may be culled");
    else
        DebugLog("WASDCombatPlugin: character audio range hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CameraClass::getFacingDirection),
            &getFacing_hook, &s_getFacingOrig))
        ErrorLog("WASDCombatPlugin: getFacingDirection hook FAILED — FP map arrow may be inverted");
    else
        DebugLog("WASDCombatPlugin: getFacingDirection hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CameraClass::restrictPosition),
            &restrictPos_hook, &s_restrictPosOrig))
        ErrorLog("WASDCombatPlugin: restrictPosition hook FAILED — OTS camera may clamp to floor");
    else
        DebugLog("WASDCombatPlugin: restrictPosition hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&PlayerInterface::playerControl),
            &playerControl_hook, &s_playerControlOrig))
        ErrorLog("WASDCombatPlugin: playerControl hook FAILED");
    else
        DebugLog("WASDCombatPlugin: playerControl hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&Character::removeJob),
            &removeJob_hook, &s_removeJobOrig))
        ErrorLog("WASDCombatPlugin: removeJob hook FAILED");
    else
        DebugLog("WASDCombatPlugin: removeJob hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&Character::addJob),
            &addJob_hook, &s_addJobOrig))
        ErrorLog("WASDCombatPlugin: addJob hook FAILED");
    else
        DebugLog("WASDCombatPlugin: addJob hook OK");

    // addOrder — door suppression on the player-order channel while the
    // post-WASD hold is active.
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&Character::addOrder),
            &addOrder_hook, &s_addOrderOrig))
        ErrorLog("WASDCombatPlugin: addOrder hook FAILED — door suppression partial (addJob only)");
    else
        DebugLog("WASDCombatPlugin: addOrder hook OK");

    // PlayerInterface::playerMove — NOT INSTALLED (2026-08-05, crash-dump
    // verified).  RVA 0x7F95F0 was derived from the pre-2026-06-21 RE_Kenshi
    // exe; RE_Kenshi regenerated its patched Kenshi_x64.exe on 6/21 and all
    // code shifted.  On the current exe 0x7F95F0 is MID-INSTRUCTION (+0x2ED
    // into a NavMesh-path function, one byte into a 5-byte call at 0x7F95EF):
    // the MinHook E9 byte became that call's displacement low byte, and any
    // pathfind reaching the rare branch at 0x7F95EF jumped into unmapped
    // memory — the "pack bull + right-click inside hive home" crash.  The
    // hook never fired on this exe (0 log lines across full sessions); its
    // job is fully covered by the RMB press-edge poller + addOrder/addJob
    // gates, so nothing replaces it.  To re-enable: re-derive the RVA on the
    // CURRENT exe and install through verifyPatchSiteBytes().

    // combatGo — DC passive-combat model: suppress the controlled character's
    // combat AI unless engaged (manual attack / meleed) and not moving.
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CombatClass::_NV_go),
            &combatGo_hook, &s_combatGoOrig))
        ErrorLog("WASDCombatPlugin: combatGo hook FAILED — passive-combat model inactive");
    else
        DebugLog("WASDCombatPlugin: combatGo hook OK");

    // _iHitYouAreYouHit - perfect-block verdict (Build 4).  Header-declared,
    // resolved via GetRealAddress like every other hook here (no raw RVA).
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CombatClass::_iHitYouAreYouHit),
            &iHitYouAreYouHit_hook, &s_iHitOrig))
        ErrorLog("WASDCombatPlugin: iHitYouAreYouHit hook FAILED - directional guard verdict inactive");
    else
        DebugLog("WASDCombatPlugin: iHitYouAreYouHit hook OK");

    // Native reaction authority.  The original `stumble` argument and the
    // resulting STUMBLE/REACTION_BLOCK states define control ownership.
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CombatClass::_getHit),
            &getHit_hook, &s_getHitOrig))
        ErrorLog("WASDCombatPlugin: _getHit hook FAILED - native reaction timing unavailable");
    else
        DebugLog("WASDCombatPlugin: _getHit hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CombatClassAI::_NV_update),
            &combatAiUpdate_hook, &s_combatAiUpdateOrig))
        ErrorLog("WASDCombatPlugin: CombatClassAI update hook FAILED - direct attacks may lose state ownership");
    else
        DebugLog("WASDCombatPlugin: CombatClassAI update hook OK");

    // AttackState::_NV_initialise - manual-attack mode denies the AI's own swings.
    {
        intptr_t ai = dcResolveKenshiLibExport("?_NV_initialise@AttackState@@QEAA_NXZ");
        if (!ai)
            ErrorLog("WASDCombatPlugin: AttackState::_NV_initialise export not found - manual attack mode cannot silence AI attacks");
        else if (KenshiLib::SUCCESS != KenshiLib::AddHook(ai, &attackInit_hook, &s_attackInitOrig))
            ErrorLog("WASDCombatPlugin: AttackState::_NV_initialise hook FAILED - manual attack mode cannot silence AI attacks");
        else
            DebugLog("WASDCombatPlugin: AttackState::_NV_initialise hook OK");
    }
    // Focus: the AI's target / threat queries answer with the lock.
    {
        intptr_t cat = dcResolveKenshiLibExport("?chooseAttackTarget@CombatClassAI@@QEAAPEAVCharacter@@XZ");
        if (!cat)
            ErrorLog("WASDCombatPlugin: CombatClassAI::chooseAttackTarget export not found - focus cannot pin the AI target");
        else if (KenshiLib::SUCCESS != KenshiLib::AddHook(cat, &chooseAttackTarget_hook, &s_chooseAttackTargetOrig))
            ErrorLog("WASDCombatPlugin: chooseAttackTarget hook FAILED - focus cannot pin the AI target");
        else
            DebugLog("WASDCombatPlugin: chooseAttackTarget hook OK");
    }
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&Character::attackingYou),
            &attackingYou_hook, &s_attackingYouOrig))
        ErrorLog("WASDCombatPlugin: attackingYou hook FAILED - automatic guard incoming-attack tracking unavailable");
    else
        DebugLog("WASDCombatPlugin: attackingYou hook OK");
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CharStats::chooseBlock),
            &chooseBlock_hook, &s_chooseBlockOrig))
        ErrorLog("WASDCombatPlugin: chooseBlock hook FAILED - manual attack mode cannot silence AI blocks");
    else
        DebugLog("WASDCombatPlugin: chooseBlock hook OK");
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CharStats::chooseAttack),
            &chooseAttack_hook, &s_chooseAttackOrig))
        ErrorLog("WASDCombatPlugin: chooseAttack hook FAILED - directional attacks inactive");
    else
        DebugLog("WASDCombatPlugin: chooseAttack hook OK");
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(DcRobResult<DcSetCombatStateTag>::ptr),
            &setCombatState_hook, &s_setCombatStateOrig))
        ErrorLog("WASDCombatPlugin: setCombatState hook FAILED - manual attack mode second choke point inactive");
    else
        DebugLog("WASDCombatPlugin: setCombatState hook OK");

    // initCombatMode and youKnowImAttacking hooks remain removed — DC does not
    // block combat ENTRY or attack notifications; only the per-frame go() decision
    // is gated (passive-combat model).

    // XHAIR-INDICATOR: observe cursor-context changes at the source.  These
    // are MyGUIEngine_x64.dll EXPORTS resolved by GetProcAddress — no RVAs to
    // go stale.  Record-and-forward only; if any resolve/hook fails the
    // indicator stays inert and the crosshair behaves exactly like v1.4.0.
    {
        HMODULE mg = GetModuleHandleA("MyGUIEngine_x64.dll");
        void* pSet  = mg ? (void*)GetProcAddress(mg,
            "?setPointer@PointerManager@MyGUI@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z") : nullptr;
        void* pSetW = mg ? (void*)GetProcAddress(mg,
            "?setPointer@PointerManager@MyGUI@@AEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@PEAVWidget@2@@Z") : nullptr;
        void* pRst  = mg ? (void*)GetProcAddress(mg,
            "?resetToDefaultPointer@PointerManager@MyGUI@@QEAAXXZ") : nullptr;
        if (pSet && KenshiLib::SUCCESS == KenshiLib::AddHook(
                pSet, &xhSetPointer_hook, (void**)&s_xhSetPtrOrig))
            DebugLog("WASDCombatPlugin: PointerManager::setPointer hook OK");
        else
            ErrorLog("WASDCombatPlugin: PointerManager::setPointer hook FAILED — crosshair indicator inactive");
        if (pSetW && KenshiLib::SUCCESS == KenshiLib::AddHook(
                pSetW, &xhSetPointerW_hook, (void**)&s_xhSetPtrWOrig))
            DebugLog("WASDCombatPlugin: PointerManager::setPointer(owner) hook OK");
        if (pRst && KenshiLib::SUCCESS == KenshiLib::AddHook(
                pRst, &xhResetPointer_hook, (void**)&s_xhResetOrig))
            DebugLog("WASDCombatPlugin: PointerManager::resetToDefaultPointer hook OK");
    }

    // showTradeWindow — NOT INSTALLED (2026-08-05, same stale-RVA disease as
    // playerMove above).  On the current exe 0x7905D0 is MID-INSTRUCTION
    // (+0x50 into a double→int64 conversion helper at 0x790580, inside a
    // 10-byte movabs), so the patch was corrupting that helper's COMMON path
    // — wrong return values + dirty MMX state on every call — while the hook
    // itself never fired (the function is not showTradeWindow).  Loot/trade
    // detection has been carried entirely by the mainLoop GUI poll
    // (isAnyInventoryWindowOpen → s_lootUiSuspendActive) since 6/21; every
    // tested-good build ran that way, so nothing replaces this either.  To
    // re-enable: re-derive the RVA and install through verifyPatchSiteBytes().

    HANDLE h = CreateThread(nullptr, 0, PollThread, nullptr, 0, nullptr);
    if (!h)
    {
        char buf[64];
        sprintf_s(buf, sizeof(buf), "WASDCombatPlugin: poll thread FAILED err=%lu", GetLastError());
        ErrorLog(buf);
    }
    else
    {
        CloseHandle(h);
        DebugLog("WASDCombatPlugin: poll thread OK");
    }
}
