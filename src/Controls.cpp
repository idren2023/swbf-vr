// Controls.cpp: controller buttons and sticks become the keyboard and mouse input the game already understands.
//
// Everything goes through Input.cpp's injection (the same path the test scripts use), so the game's own key
// bindings apply. Turning is injected as horizontal mouse movement: it turns the character, and with it the base
// frame of the view and of both hands, so the player, the view and the hands turn together.
//
//   left stick              move (W A S D)
//   right stick left/right  turn: snap (one step per flick) or smooth
//   right trigger           fire (left mouse button)
//   left trigger            under the gun it takes the magazine (reload); in a vehicle / as the droideka secondary fire
//   A (right lower button)  jump (Space)
//   B (right upper button)  crouch (Left Ctrl; a toggle in the game)

#include "Controls.h"

#include "Aim.h"
#include "EarMenu.h"
#include "EngineSurvey.h"
#include "Feedback.h"
#include "Flight.h"
#include "FpGuard.h"
#include "GameBuild.h"
#include "Gesture.h"
#include "GunnerSeat.h"
#include "Holster.h"
#include "Hud.h"
#include "Input.h"
#include "Jet.h"
#include "Levers.h"
#include "Log.h"
#include "Net.h"
#include "Pointer.h"
#include "Settings.h"
#include "Squad.h"
#include "Stereo.h"
#include "Vehicle.h"
#include "WalkerDrive.h"
#include "Xr.h"
#include "Zoom.h"
#include "engine.h"

#include <windows.h>

#include <MinHook.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace SwbfVr
{

namespace Controls
{

namespace
{
bool _isStatisticsScreen = false; // the statistics after a match are up (IsStatisticsScreen)

// DirectInput key codes (DIK_... in dinput.h).
constexpr unsigned KEY_W = 0x11, KEY_A = 0x1E, KEY_S = 0x1F, KEY_D = 0x20, KEY_SPACE = 0x39;
constexpr unsigned KEY_R = 0x13, KEY_E = 0x12, KEY_Q = 0x10, KEY_Z = 0x2C, KEY_ESCAPE = 0x01, KEY_RETURN = 0x1C;
constexpr unsigned KEY_UP = 0xC8, KEY_DOWN = 0xD0, KEY_LEFT = 0xCB, KEY_RIGHT = 0xCD, KEY_ENTER = 0x1C;
constexpr unsigned KEY_M = 0x32, KEY_TAB = 0x0F;
/// Hysteresis for stick directions used as keys: pressed past the first, released under the second.
constexpr float STICK_KEY_PRESS = 0.45f, STICK_KEY_RELEASE = 0.30f;
/// Hysteresis for snap turns (and for re-arming the weapon change on the right stick).
constexpr float SNAP_PRESS = 0.70f, SNAP_RELEASE = 0.35f;
/// Hysteresis for the triggers as mouse buttons.
constexpr float TRIGGER_PRESS = 0.5f, TRIGGER_RELEASE = 0.3f;
/// Reload hold: a buzz in the left hand this often while the magazine is held, at full strength when the controller
/// is this far from the magazine (the zone lets go at 14 cm from its centre).
constexpr DWORD RESISTANCE_PULSE_MILLISECONDS = 80;
constexpr float RESISTANCE_FULL_PULL_METERS = 0.12f;
/// A flick (one step per push, in menus, the spawn screen and the ear menu): the stick must come back inside the
/// first before a push past the second counts again.
constexpr float FLICK_CENTRED = 0.3f, FLICK_PUSHED = 0.6f;
/// Ear menu (headset run 15): a choice is the stick pushed past this (of its full throw)...
constexpr float EAR_STICK_EDGE = 0.85f;
/// ...and back inside FLICK_CENTRED within this long after it left the edge (a stick let go springs back in a few
/// frames; a thumb bringing it back takes longer).
constexpr DWORD EAR_RELEASE_MILLISECONDS = 150;
/// Dead zone of the sticks for analog movement and smooth turning; the rest of the range is stretched to 0..1.
constexpr float STICK_DEAD_ZONE = 0.15f;
constexpr float STICK_LIVE_RANGE = 0.85f;
/// How long a tapped key stays down.
constexpr DWORD TAP_MILLISECONDS = 100;
/// Most taps in flight at once.
constexpr unsigned MAXIMUM_TAPS = 6;
/// The analog movement values count only this long after the last controller frame.
constexpr DWORD MOVE_FRESH_MILLISECONDS = 200;
/// Held keys are sent again this often (a test script ending clears the injected state).
constexpr DWORD REFRESH_MILLISECONDS = 500;
/// Crouch by head height: the head this far under its resting height crouches, back above the second stands up.
constexpr float CROUCH_HEAD_DROP_METERS = 0.30f, STAND_HEAD_DROP_METERS = 0.20f;
/// Stance by head height (PLAN 58 h): the head this far under its resting height goes prone, back above the second
/// crouches.
constexpr float PRONE_HEAD_DROP_METERS = 0.85f, PRONE_LEAVE_HEAD_DROP_METERS = 0.75f;
/// Stance by head height: presses toward one stance before giving up (stand -> prone takes two).
constexpr int STANCE_MAXIMUM_PRESSES = 5;
/// Crouch by head height: at most one crouch key press this often.
constexpr DWORD CROUCH_REPEAT_MILLISECONDS = 700;
/// The game lowers the camera this much when crouched (1.72 m -> 1.22 m at the eye).
constexpr float CROUCH_CAMERA_DROP_METERS = 0.50f, PRONE_CAMERA_DROP_METERS = 1.09f; // prone: 0.63 m at the eye
/// Soldier stance (soldier + OFFSET_soldierStance): 0 stand, 1 crouch, 2 prone.
constexpr int STANCE_STAND = 0, STANCE_CROUCH = 1, STANCE_PRONE = 2;
/// Scope gesture: at most one zoom key press this often.
constexpr DWORD ZOOM_REPEAT_MILLISECONDS = 600;
/// Scope zone (headset run 15): the lens counts as at the eye within this distance of the point between the eyes,
/// and leaves beyond the second (so it does not flicker at the edge).
constexpr float SCOPE_ENTER_METERS = 0.22f;
constexpr float SCOPE_LEAVE_METERS = 0.28f;
/// ... and the weapon's direction within 60 degrees of the head's (cosine).
constexpr float SCOPE_MINIMUM_ALIGNMENT = 0.5f;
/// A zoom the game ended by itself (a reload) is given back within this long once the weapon is ready again.
constexpr DWORD ZOOM_RESTORE_MILLISECONDS = 6000;
/// Y held this long opens the pause menu; the left stick pressed this long recentres the view.
constexpr DWORD HOLD_MILLISECONDS = 1000;
/// How long the keys pressed by Y (Q, Esc) and the secondary weapon key stay down.
constexpr DWORD KEY_PULSE_MILLISECONDS = 120;
/// Weapon change: no change this long after the right stick was pressed in (zoom).
constexpr DWORD WEAPON_CLICK_LOCKOUT_MILLISECONDS = 400;
/// Weapon change: the push needed (b93: 0.6, was 0.85), and the sideways part allowed relative to it (0.75 = within
/// about 37 degrees of straight up or down).
constexpr float WEAPON_PUSH = 0.60f, WEAPON_SIDEWAYS_RATIO = 0.75f;
/// Smooth turning: longer frame times are taken as a pause and not turned.
constexpr float LONGEST_TURN_STEP_SECONDS = 0.25f;
constexpr float SECONDS_PER_MILLISECOND = 0.001f;

bool _isSnapTurn = false;    // idren2023 prefers smooth turning (2026-10-07); swbf-vr.ini can change it
float _turnDegrees = 180.0f; // snap: per step; smooth: per second at full deflection
// Mouse counts per degree of turn. Measured with idren2023's settings: 400 counts/s for 8 s is about one full turn.
// Depends on the game's mouse sensitivity setting.
float _countsPerDegree = 3200.0f / 360.0f;

/// What we are holding down in the game's input.
struct HeldInputs
{
    bool isKeyHeld[256];      // by DirectInput key code
    bool isFireHeld;          // left mouse button
    bool isSecondaryFireHeld; // right mouse button
};

HeldInputs _held;

/// The key pressed for crouch (B, crouch by head height): Right Ctrl, bound to crouch by default and still in idren2023's
/// profile after he moved crouch from Ctrl to C on 2026-10-16 (Left Ctrl did nothing since: headset run 16, run287's
/// binding dump). swbf-vr.ini: crouch_key.
unsigned _crouchKey = 0x9D;
unsigned _secondaryKey = 0x22; // DirectInput code of "next secondary weapon": G in idren2023's profile (read with
                               // ScriptCB_GetKeyBoardCmds, 2026-10-07)
DWORD _secondaryKeyDownTick = 0;
bool _isAnalogMove = true;        // stick movement through the game's analog inputs (swbf-vr.ini: move = analog | keys)
bool _isMoveByHead = false;       // the character and the jet hover follow the head, not the left controller
bool _isWeaponChangeArmed = true; // right stick up/down: one weapon step per flick
DWORD _recentreSince = 0;         // left stick held down since (0 = not held, 1 = done for this press)
bool _isSnapArmed = true;
float _turnRemainderCounts = 0;
float _followRemainderCounts = 0; // character follow's fraction of a mouse count not yet sent
constexpr float DEGREES_PER_RADIAN = 57.29578f;
// The game clamps one frame's mouse movement to about 33 degrees (measured, run35: 1600 and 800 counts both
// turned 33 degrees, 265 counts turned 29.7). Larger turns are fed over several frames.
constexpr long MAXIMUM_COUNTS_PER_FRAME = 200;
/// The flight stick as the mouse (headset run 18): mouse movement at full tilt, in the degrees of turn the counts per
/// degree were measured with on foot (a flyer turns at its own rate per count; to be tuned in the headset).
constexpr float FLIGHT_STICK_DEGREES_PER_SECOND = 120.0f;
/// Its pitch faster (headset run 19: the pitch felt too unresponsive).
constexpr float FLIGHT_STICK_PITCH_DEGREES_PER_SECOND = 200.0f;
/// The stick tilted this far aside (of its full input) also gives the game's sharp turn, A / D (headset run 19).
constexpr float FLIGHT_STICK_SHARP_TURN = 0.95f;
float _flightPitchRemainderCounts = 0; // the flight stick's fraction of a vertical mouse count not yet sent
/// Character follow: the character turns toward the left controller's heading with this time constant, at most this
/// fast. The view does not turn with it (Stereo's held view heading), so this may be quick.
constexpr float CHARACTER_FOLLOW_SECONDS = 0.08f;
constexpr float CHARACTER_FOLLOW_MAXIMUM_DEGREES_PER_SECOND = 720.0f;
/// The droideka rolled up: the left stick's sideways push turns the ball this fast at full deflection.
constexpr float DROIDEKA_BALL_TURN_DEGREES_PER_SECOND = 150.0f;
/// Droideka modes (offsets "Droideka", run216): 11 rolling up, 12 ball, 13 standing up.
constexpr int DROIDEKA_MODE_ROLLING_UP = 11, DROIDEKA_MODE_STANDING_UP = 13;
/// The ball's turn from the left stick sideways (PLAN 58 e, headset run 17: impossible to roll straight): nothing
/// within this much, then the square of the rest, so a stick pushed ahead and a little off does not turn.
constexpr float DROIDEKA_BALL_TURN_DEAD_ZONE = 0.25f;
/// The ball turns this many times faster per mouse count than the soldier the counts per degree were measured on
/// (run368: 60 % aside asked for 33 deg/s and got about 110), so its turns are divided by it.
constexpr float DROIDEKA_BALL_COUNT_GAIN = 3.4f;
volatile bool _isDroidekaStanding = false;
long _turnPendingCounts = 0;
float _pitchRemainderCounts = 0; // droideka head aim: fraction of a vertical mouse count not yet sent
DWORD _lastTick = 0, _lastRefreshTick = 0;
bool _isActive = false;

// Short key presses that end by themselves.
struct Tap
{
    unsigned keyCode;
    DWORD endTick; // 0 = free slot
};

Tap _taps[MAXIMUM_TAPS];

void TapKey(unsigned keyCode, DWORD now)
{
    for (Tap& tap : _taps)
    {
        if (!tap.endTick)
        {
            tap.keyCode = keyCode;
            tap.endTick = (now + TAP_MILLISECONDS) | 1; // | 1: never 0, which marks a free slot
            Input::Key(keyCode, true);
            return;
        }
    }
}

void ServiceTaps(DWORD now)
{
    for (Tap& tap : _taps)
    {
        // The signed difference stays right when GetTickCount wraps around (every 49.7 days).
        if (tap.endTick && static_cast<LONG>(now - tap.endTick) >= 0)
        {
            Input::Key(tap.keyCode, false);
            tap.endTick = 0;
        }
    }
}

bool _isEarMenuOn = true, _isReloadGestureOn = true,
     _isScopeGestureOn = false;    // swbf-vr.ini: ear_menu, reload_gesture, scope_gesture
bool _isCrouchByHead = false;      // swbf-vr.ini: crouch = head | button
DWORD _zoomKeyTick = 0;            // the last zoom key press, ours or the right stick's
float _motion = 0;                 // 0..1: being turned or moved by the sticks (for the comfort vignette)
bool _isInMenu = false;            // the pause menu is open: sticks and buttons steer the menu
bool _isWaitingForRelease = false;
bool _isReloadRequested = false; // RequestReload (Holster's manual reload): R is tapped at the next OnPresent // after the menu: A, B and the trigger count only once they have been let go

/// What a pull of the left trigger does. Decided when the pull starts and kept until the trigger is let go, so a
/// reload never fires the secondary weapon (it did, headset run 10).
enum class LeftTrigger
{
    Free,          // not pulled
    SecondaryFire, // pulled elsewhere: right mouse button
    Magazine,      // pulled with the left hand under the gun: the hand holds the magazine
    Spent          // the pull reloaded or was cut short: nothing until it is let go
};
LeftTrigger _leftTrigger = LeftTrigger::Free;

/// Presses or releases a key in the game's input, only when that changes something.
/// @param shouldRefresh true = send a held key again even when it is already down.
void SetKey(unsigned keyCode, bool isDown, bool shouldRefresh)
{
    if (_held.isKeyHeld[keyCode] == isDown && !(shouldRefresh && isDown))
    {
        return;
    }
    _held.isKeyHeld[keyCode] = isDown;
    Input::Key(keyCode, isDown);
}

/// A stick direction used as a key, with hysteresis.
/// @param isHeld whether the key is down now.
/// @param value the stick's deflection in that direction.
bool IsStickPast(bool isHeld, float value)
{
    return isHeld ? value > STICK_KEY_RELEASE : value > STICK_KEY_PRESS;
}

void ReleaseAll()
{
    if (_leftTrigger == LeftTrigger::Magazine)
    {
        Aim::SetLeftHandOnMagazine(false);
    }
    if (_leftTrigger != LeftTrigger::Free)
    {
        _leftTrigger = LeftTrigger::Spent;
    }
    for (unsigned keyCode :
         {KEY_W, KEY_A, KEY_S, KEY_D, KEY_SPACE, _crouchKey, KEY_R, KEY_E, KEY_Q, KEY_Z, KEY_ESCAPE, KEY_RETURN})
    {
        if (_held.isKeyHeld[keyCode])
        {
            _held.isKeyHeld[keyCode] = false;
            Input::Key(keyCode, false);
        }
    }
    if (_held.isFireHeld)
    {
        _held.isFireHeld = false;
        Input::MouseButton(0, false);
    }
    if (_held.isSecondaryFireHeld)
    {
        _held.isSecondaryFireHeld = false;
        Input::MouseButton(1, false);
    }
}

// ---- analog movement ----
// PlayerController's update hands the keyboard's movement to the player's Controllable with two small setters
// (found with the watch tool, run61): Controllable::SetStrafe(float) at 0xDA0A0 writes +0x58 (soldier +0x2A8),
// called from +0x1F7507; Controllable::SetMove(float) at 0xDA0C0 writes +0x54 (soldier +0x2A4), called from +0x1F751A.
// Values are -1..1 (W = +1 forward, D = +1 strafe). While a controller is active those two calls get the stick's
// values.
/// The two setters are __thiscall (the Controllable in ECX); our hooks are __fastcall with an unused second
/// parameter (EDX), which receives ECX the same way.
using SetAxisFunction = void(__fastcall*)(void* self, void* edx, float value);
// The game's setters behind our hooks, called through MinHook's trampolines.
SetAxisFunction _originalSetStrafe = nullptr, _originalSetMove = nullptr;
// The setters and the return addresses of PlayerController's two calls (the instructions after them; only these
// calls are replaced) come from GameBuild: Address::SetStrafe, SetMove, SetStrafeReturn, SetMoveReturn,
// Feature::AnalogMove (offsets.md, "Player movement input"). The return addresses are kept as absolute addresses,
// set before the hooks are enabled.
uintptr_t _setStrafeReturn = 0, _setMoveReturn = 0;
bool _areMoveHooksInstalled = false, _wereMoveHooksTried = false;
float _moveStrafe = 0, _moveForward = 0;
DWORD _moveTick = 0;
// The player's Controllable as PlayerController last handed it its movement (SetMoveHook), and when; game thread.
uintptr_t _playerControllable = 0;
DWORD _playerControllableTick = 0;

bool IsMoveFresh()
{
    return _isActive && GetTickCount() - _moveTick < MOVE_FRESH_MILLISECONDS;
}

// __builtin_return_address(0) is where the call came from (absolute address).
void __fastcall SetStrafeHook(void* self, void* edx, float value)
{
    const uintptr_t from = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    if (from == _setStrafeReturn && _isAnalogMove && IsMoveFresh() && !Net::IsNetGame())
    {
        value = _moveStrafe;
    }
    _originalSetStrafe(self, edx, value);
}

void __fastcall SetMoveHook(void* self, void* edx, float value)
{
    const uintptr_t from = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    // In a network game PlayerController also runs in the host world, there for every player; the client world
    // has controllers only for the local players.
    const bool isNetGame = Net::IsNetGame();
    if (from == _setMoveReturn && (!isNetGame || Net::IsClientWorldActive()))
    {
        _playerControllable = reinterpret_cast<uintptr_t>(self);
        _playerControllableTick = GetTickCount();
        if (_isAnalogMove && IsMoveFresh() && !isNetGame)
        {
            value = _moveForward;
        }
    }
    _originalSetMove(self, edx, value);
}

// Network game, set by Update each frame: the angle from the character's facing to the direction the stick is
// meant in (right positive); whether the soldier is steered to the gun hand, and a counter of Update's frames.
float _netMoveTurnRadians = 0;
bool _isNetAimOn = false;
unsigned _netAimFrame = 0, _netAimMeasuredFrame = 0;
// What is left of this frame's measured aim error, to be turned by the frame's remaining turns.
float _netYawLeftRadians = 0, _netPitchLeftRadians = 0;
// Part of the measured error taken out per frame (below 1: the loop cannot overshoot when a frame has no turn).
constexpr float NET_AIM_GAIN = 0.8f;
// Where the host's line of fire is made to cross the hand's (see WriteNetAim): at what the hand's line hits, found
// by a ray of this length; nearer than the nearest value the soldier would have to turn far off the hand's aim.
constexpr float NET_AIM_RAY_METERS = 300.0f, NET_AIM_NEAREST_METERS = 1.0f;
// The distance is followed as its reciprocal (the angle between the two lines goes with 1 / distance), by this part
// of the way per frame: a sweep over an edge turns the body over a few frames instead of in one.
constexpr float NET_AIM_MEET_SMOOTHING = 0.3f;
float _netAimMeetPerMeter = 1.0f / 40.0f;
// A soldier's strafing speed over its running speed (measured: 8.9 m against 13.1 m in 2 s, run 682).
constexpr float SOLDIER_STRAFE_SPEED_RATIO = 0.67f;
// Errors below this are left alone: the rates are sent in steps (0.03 degrees a turn), and correcting less than a
// few steps only makes the soldier tremble (run 683: +-0.1 degrees back and forth).
constexpr float NET_AIM_DEAD_RADIANS = 0.0015f;
// The game's limit for the turn and pitch rates of a soldier on foot.
constexpr float NET_RATE_LIMIT = 16.0f;
// Radians a soldier turns per unit of rate and second (class +0x1640, default 2.0; client-input.md section 3).
constexpr float SOLDIER_TURN_PER_RATE = 2.0f;
// A soldier's pitch limits (class +0x1648 / +0x164C defaults).
constexpr float SOLDIER_PITCH_MINIMUM = -0.8f, SOLDIER_PITCH_MAXIMUM = 1.1f;

/// Network game: the stick's movement written into the local player's control state of this turn (Net.h). The
/// two setter hooks above do not act there: they sit below the state the host is sent, and in the host world they
/// would move every player.
void WriteNetControls(Net::ControlState& state)
{
    if (!_isAnalogMove || !IsMoveFresh())
    {
        return;
    }
    static bool wasLogged = false;
    if (!wasLogged && (_moveStrafe != 0 || _moveForward != 0))
    {
        wasLogged = true;
        Log::Info(
            "controls", "net game: stick movement goes into the control state (first: strafe %.2f move %.2f over %.2f %.2f)",
            _moveStrafe, _moveForward, state.strafe, state.move);
    }
    // The character faces along the gun hand here (WriteNetAim), so the stick's vector, which is meant relative to
    // the left controller (or the head), is turned by the angle between the two.
    // A soldier strafes slower than it runs (about 0.67 of the speed: runs 682, 685), so the stick's vector is taken
    // as the velocity it gives in single player, that velocity is turned, and the inputs that produce it are sent;
    // turned inputs alone walked 9 degrees off (run 685). Backward speed is taken as the forward speed.
    const float cosine = cosf(_netMoveTurnRadians), sine = sinf(_netMoveTurnRadians);
    const float aside = _moveStrafe * SOLDIER_STRAFE_SPEED_RATIO;
    float strafe = (aside * cosine + _moveForward * sine) / SOLDIER_STRAFE_SPEED_RATIO;
    float move = _moveForward * cosine - aside * sine;
    const float largest = fabsf(strafe) > fabsf(move) ? fabsf(strafe) : fabsf(move);
    if (largest > 1)
    {
        strafe /= largest;
        move /= largest;
    }
    state.strafe = strafe;
    state.move = move;
}

/// Network game, on foot: the host fires from its own copy of the soldier, along that soldier's heading and pitch,
/// and the only things it is told are the turn and pitch rates of each turn. So the soldier is steered until it
/// faces along the weapon in the hand (idren2023's decision, docs/research/netcode.md): a closed loop on the predicted
/// soldier of the client world, one measurement per frame, spread over the turns of that frame. The game turns a
/// soldier by rate * SOLDIER_TURN_PER_RATE * turn length per turn (less while zoomed: the loop then takes longer).
void WriteNetAim(Net::ControlState& state)
{
    if (!_isNetAimOn)
    {
        return;
    }
    if (_netAimFrame != _netAimMeasuredFrame)
    {
        _netAimMeasuredFrame = _netAimFrame;
        _netYawLeftRadians = _netPitchLeftRadians = 0;
        float from[3], direction[3], soldier[16], pitchRadians = 0;
        if (!Aim::GetFireLine(from, direction) || !Net::GetLocalSoldier(true, soldier, &pitchRadians))
        {
            return;
        }
        // The host's shot starts at the soldier's own fire point, not at the hand (0.2-0.75 m apart): the soldier is
        // aimed from there at the point the hand's line hits (the game's ray cast along the hand's line), so the
        // host's shot lands where the hand aims, at 2 m as at 150 m. Nothing hit: the end of the ray.
        float stockOrigin[3], hitMeters = NET_AIM_RAY_METERS, throwAim[3];
        // A grenade just let go: the soldier faces where its own throw lands at the hand's landing point (Holster).
        const bool isThrowAim = Aim::GetNetThrowAim(throwAim);
        const bool isHit = !isThrowAim && Net::CastAimRay(from, direction, NET_AIM_RAY_METERS, &hitMeters);
        _netAimMeetPerMeter +=
            (1.0f / (hitMeters < NET_AIM_NEAREST_METERS ? NET_AIM_NEAREST_METERS : hitMeters) - _netAimMeetPerMeter) *
            NET_AIM_MEET_SMOOTHING;
        const float meetMeters = 1.0f / _netAimMeetPerMeter;
        const float meet[3] = {from[0] + direction[0] * meetMeters, from[1] + direction[1] * meetMeters,
                               from[2] + direction[2] * meetMeters};
        if (isThrowAim)
        {
            memcpy(direction, throwAim, sizeof(throwAim));
        }
        else if (Aim::GetStockFireOrigin(stockOrigin))
        {
            float toward[3], lengthSquared = 0;
            for (int axis = 0; axis < 3; ++axis)
            {
                toward[axis] = meet[axis] - stockOrigin[axis];
                lengthSquared += toward[axis] * toward[axis];
            }
            const float length = sqrtf(lengthSquared);
            if (length > 0.5f)
            {
                for (int axis = 0; axis < 3; ++axis)
                {
                    direction[axis] = toward[axis] / length;
                }
            }
        }
        // A soldier's matrix has row 0 = its left and row 2 = its forward (run 683: with the rows taken as right
        // and back the loop settled facing away from the aim). The error to the right is the angle from the
        // soldier's forward to the aim.
        const float alongRight = -(direction[0] * soldier[0] + direction[2] * soldier[2]);
        const float alongForward = direction[0] * soldier[8] + direction[2] * soldier[10];
        if (alongRight * alongRight + alongForward * alongForward < 0.04f)
        {
            return; // aiming almost straight up or down: no heading to follow
        }
        const float yawError = atan2f(alongRight, alongForward);
        _netYawLeftRadians = fabsf(yawError) < NET_AIM_DEAD_RADIANS ? 0 : yawError * NET_AIM_GAIN;
        float targetPitch = asinf(direction[1] > 1 ? 1 : direction[1] < -1 ? -1 : direction[1]);
        targetPitch = targetPitch > SOLDIER_PITCH_MAXIMUM   ? SOLDIER_PITCH_MAXIMUM
                      : targetPitch < SOLDIER_PITCH_MINIMUM ? SOLDIER_PITCH_MINIMUM
                                                            : targetPitch;
        const float pitchError = targetPitch - pitchRadians;
        _netPitchLeftRadians = fabsf(pitchError) < NET_AIM_DEAD_RADIANS ? 0 : pitchError * NET_AIM_GAIN;
        static DWORD lastLogTick = 0;
        if (GetTickCount() - lastLogTick >= 500)
        {
            lastLogTick = GetTickCount();
            Log::Info(
                "controls", "net aim: soldier off the hand's aim by %.1f degrees right, %.1f up (pitch %.1f), lines meet at %.1f m (ray %s at %.1f m, %s world) point %.2f %.2f %.2f",
                static_cast<double>(yawError * DEGREES_PER_RADIAN),
                static_cast<double>(pitchError * DEGREES_PER_RADIAN),
                static_cast<double>(pitchRadians * DEGREES_PER_RADIAN), static_cast<double>(meetMeters),
                isThrowAim ? "throw" : isHit ? "hit" : "free", static_cast<double>(hitMeters), Net::IsClientWorldActive() ? "client" : "host",
                static_cast<double>(meet[0]), static_cast<double>(meet[1]), static_cast<double>(meet[2]));
        }
    }
    const float perRate = SOLDIER_TURN_PER_RATE * Net::GetTurnSeconds();
    // C++: a lambda, a small local function; [perRate] copies that variable into it.
    const auto steer = [perRate](float& leftRadians, float& rate) {
        float wanted = leftRadians / perRate;
        wanted = wanted > NET_RATE_LIMIT ? NET_RATE_LIMIT : wanted < -NET_RATE_LIMIT ? -NET_RATE_LIMIT : wanted;
        leftRadians -= wanted * perRate;
        rate += wanted;
        rate = rate > NET_RATE_LIMIT ? NET_RATE_LIMIT : rate < -NET_RATE_LIMIT ? -NET_RATE_LIMIT : rate;
    };
    steer(_netYawLeftRadians, state.turnRate);
    steer(_netPitchLeftRadians, state.pitchRate);
}

/// Network game, in a vehicle, turret or gunner seat: the head aim's look rates and the levers' drive, which the
/// seat's hooks set on the Controllable in single player (Vehicle), go into the control state: the game then sets
/// them in both worlds. A seat's inputs are all limited to -1..1 by the game.
void WriteNetSeat(Net::ControlState& state)
{
    Vehicle::NetSeatInputs seat;
    if (!Vehicle::GetNetSeatInputs(&seat) || !seat.isInSeat)
    {
        return;
    }
    const auto limit = [](float value) { return value > 1.0f ? 1.0f : value < -1.0f ? -1.0f : value; };
    state.move = limit(state.move + seat.move);
    state.strafe = limit(state.strafe + seat.strafe);
    state.turnRate = limit((seat.hasYaw ? seat.yaw : state.turnRate) + seat.turn);
    state.pitchRate = limit((seat.hasPitch ? seat.lookPitch : state.pitchRate) + seat.pitch);
}

void WriteNetControlState(Net::ControlState& state)
{
    WriteNetControls(state);
    WriteNetAim(state);
    WriteNetSeat(state);
}

void InstallMoveHooks()
{
    if (_wereMoveHooksTried)
    {
        return;
    }
    _wereMoveHooksTried = true;
    // Both setters start with: push ebp; mov ebp, esp; movss xmm0, [ebp+8]; movss [ecx+0x58 or 0x54], xmm0
    // (their signatures in GameBuild).
    if (!GameBuild::IsAvailable(GameBuild::Feature::AnalogMove))
    {
        Log::Warning(
            "controls",
            "movement setters not found (%s is off for this game build): stick movement stays on W/A/S/D",
            GameBuild::FeatureName(GameBuild::Feature::AnalogMove));
        return;
    }
    _setStrafeReturn = GameBuild::Absolute(GameBuild::Address::SetStrafeReturn);
    _setMoveReturn = GameBuild::Absolute(GameBuild::Address::SetMoveReturn);
    void* const setStrafe = reinterpret_cast<void*>(GameBuild::Absolute(GameBuild::Address::SetStrafe));
    void* const setMove = reinterpret_cast<void*>(GameBuild::Absolute(GameBuild::Address::SetMove));
    MH_Initialize();
    MH_STATUS strafeStatus = MH_CreateHook(
        setStrafe,
        reinterpret_cast<void*>(&SetStrafeHook),
        reinterpret_cast<void**>(&_originalSetStrafe));
    if (strafeStatus == MH_OK)
    {
        strafeStatus = MH_EnableHook(setStrafe);
    }
    MH_STATUS moveStatus =
        MH_CreateHook(setMove, reinterpret_cast<void*>(&SetMoveHook), reinterpret_cast<void**>(&_originalSetMove));
    if (moveStatus == MH_OK)
    {
        moveStatus = MH_EnableHook(setMove);
    }
    _areMoveHooksInstalled = strafeStatus == MH_OK && moveStatus == MH_OK;
    if (_areMoveHooksInstalled)
    {
        Net::SetLocalControlWriter(&WriteNetControlState);
    }
    Log::Info(
        "controls",
        "analog movement hooks: %s, %s",
        MH_StatusToString(strafeStatus),
        MH_StatusToString(moveStatus));
}

// True while the player controls a soldier or a droideka on foot (not a vehicle or turret): the character then
// faces where the left controller points. Needs the movement hooks (they see the player's Controllable).
bool IsOnFoot()
{
    static uintptr_t checked = 0;
    static bool isOnFoot = false;
    const uintptr_t controllable = _playerControllable;
    if (!controllable || GetTickCount() - _playerControllableTick > MOVE_FRESH_MILLISECONDS)
    {
        return false;
    }
    if (controllable != checked)
    {
        checked = controllable;
        isOnFoot = Holster::IsObjectType(controllable, "EntitySoldier") ||
                   Holster::IsObjectType(controllable, "EntityDroideka");
        Log::Info("controls", "the player controls %s", isOnFoot ? "a soldier on foot" : "something else (no follow)");
    }
    return isOnFoot;
}

} // namespace

namespace
{
// Pause menu (and the screens under it): left stick = arrow keys, one step per flick; A = Enter; B = Esc (back,
// resume); the trigger stays the left mouse button for the pointer; left hand at an ear and its trigger or grip
// pressed again = Esc (resume, b123: the ear menu opens the pause menu, idren2023 after headset run 10). Nothing reaches
// the game's own controls.
void SteerMenu(const Xr::ControllerInput& pad, DWORD now, bool shouldRefresh)
{
    // Static locals keep their value from one call to the next (like a private static field used by one method).
    static int isFlickArmed = 1;
    static bool isAHeld = true, isBHeld = true; // "held" until first seen released, so the press that opened the
                                                // menu does nothing
    static bool isEarPressHeld = true;          // left trigger or grip, the same way
    if (!_isInMenu)
    {
        isAHeld = isBHeld = isEarPressHeld = true;
        isFlickArmed = 0;
    } // first frame in the menu
    {
        const Gesture::Hand& left = Gesture::CurrentHand(1);
        const bool isAtEar = _isEarMenuOn && left.isTracked &&
                             (left.zone == Gesture::Zone::EarLeft || left.zone == Gesture::Zone::EarRight);
        const bool isPressed = pad.trigger[1] > (isEarPressHeld ? TRIGGER_RELEASE : TRIGGER_PRESS) ||
                               pad.grip[1] > (isEarPressHeld ? 0.4f : 0.7f);
        if (isAtEar && isPressed && !isEarPressHeld)
        {
            TapKey(KEY_ESCAPE, now);
            Feedback::Pulse(1, 0.02f, 0.3f);
            Log::Info("controls", "pause menu: left hand at the ear, resume (Esc)");
        }
        isEarPressHeld = isPressed;
    }
    const float x = pad.stickX[1], y = pad.stickY[1];
    if (fabsf(x) < FLICK_CENTRED && fabsf(y) < FLICK_CENTRED)
    {
        isFlickArmed = 1;
    }
    else if (isFlickArmed && (fabsf(x) > FLICK_PUSHED || fabsf(y) > FLICK_PUSHED))
    {
        isFlickArmed = 0;
        TapKey(fabsf(y) >= fabsf(x) ? (y > 0 ? KEY_UP : KEY_DOWN) : (x > 0 ? KEY_RIGHT : KEY_LEFT), now);
    }
    if (pad.isLowerPressed[0] && !isAHeld)
    {
        TapKey(KEY_ENTER, now);
    }
    if (pad.isUpperPressed[0] && !isBHeld)
    {
        TapKey(KEY_ESCAPE, now);
    }
    isAHeld = pad.isLowerPressed[0];
    isBHeld = pad.isUpperPressed[0];
    const bool isFire = _held.isFireHeld ? pad.trigger[0] > TRIGGER_RELEASE : pad.trigger[0] > TRIGGER_PRESS;
    if (isFire != _held.isFireHeld || (shouldRefresh && isFire))
    {
        _held.isFireHeld = isFire;
        Input::MouseButton(0, isFire);
    }
}

/// The droideka's mode (0 standing, 11 rolling up, 12 ball, 13 standing up), or -1 when the player is not a droideka
/// or the mode cannot be read.
int DroidekaMode()
{
    if (!Aim::IsDroideka())
    {
        return -1;
    }
    const uintptr_t droideka = EngineSurvey::Resolve("soldier");
    if (!droideka || IsBadReadPtr(reinterpret_cast<void*>(droideka + OFFSET_droidekaMode), 4))
    {
        return -1;
    }
    return *reinterpret_cast<int*>(droideka + OFFSET_droidekaMode);
}

} // namespace

bool IsDroidekaStanding()
{
    return _isDroidekaStanding;
}

void SetTurn(bool isSnap, float degrees)
{
    _isSnapTurn = isSnap;
    _turnDegrees = degrees;
    Log::Info(
        "controls",
        "%s turn, %.0f degrees%s",
        isSnap ? "snap" : "smooth",
        static_cast<double>(degrees),
        isSnap ? " per step" : " per second");
}

void SetMove(bool isAnalog)
{
    _isAnalogMove = isAnalog;
    Log::Info("controls", "stick movement %s", isAnalog ? "analog" : "by W/A/S/D");
}

void SetMoveByHead(bool isByHead)
{
    _isMoveByHead = isByHead;
    Log::Info("controls", "movement direction: %s", isByHead ? "the head" : "the left controller");
}

void RequestReload()
{
    _isReloadRequested = true;
}

void SetGestures(bool isEarMenuOn, bool isReloadGestureOn, bool isScopeGestureOn)
{
    _isEarMenuOn = isEarMenuOn;
    _isReloadGestureOn = isReloadGestureOn;
    _isScopeGestureOn = isScopeGestureOn;
    Log::Info(
        "controls",
        "gestures: ear menu %s, reload under the gun %s, scope to the eye %s",
        isEarMenuOn ? "on" : "off",
        isReloadGestureOn ? "on" : "off",
        isScopeGestureOn ? "on" : "off");
}

void SetCrouchByHead(bool isOn)
{
    _isCrouchByHead = isOn;
    Log::Info("controls", "crouch %s", isOn ? "by lowering the head (and B)" : "by B only");
}

float CurrentMotion()
{
    return _isActive && !_isInMenu ? _motion : 0.0f;
}

void SetSecondaryKey(unsigned keyCode)
{
    _secondaryKey = keyCode & 0xFF;
}

void SetCrouchKey(unsigned keyCode)
{
    _crouchKey = keyCode & 0xFF;
}

void SetCountsPerDegree(float counts)
{
    if (counts > 0.1f && counts < 1000.0f)
    {
        _countsPerDegree = counts;
    }
    Log::Info("controls", "%.2f mouse counts per degree of turn", static_cast<double>(_countsPerDegree));
}

bool IsStatisticsScreen()
{
    return _isStatisticsScreen;
}

void OnPresent()
{
    Xr::ControllerInput pad;
    Xr::GetControllerInput(&pad);
    const DWORD now = GetTickCount();
    if (!pad.isValid)
    {
        Stereo::SetViewFree(false);
        if (_isActive)
        {
            ReleaseAll();
            _isActive = false;
            Log::Info("controls", "controller input ended");
        }
        _lastTick = now;
        return;
    }
    FpGuard guard;
    if (!_isActive)
    {
        _isActive = true;
        _isSnapArmed = true;
        _turnRemainderCounts = 0;
        _turnPendingCounts = 0;
        _lastTick = now;
        Log::Info("controls", "controller input active");
    }
    // Held keys are sent again twice a second: a test script ending clears the injected state.
    const bool shouldRefresh = now - _lastRefreshTick > REFRESH_MILLISECONDS;
    if (shouldRefresh)
    {
        _lastRefreshTick = now;
    }

    ServiceTaps(now);
    if (Settings::IsMenuOpen())
    {
        if (!_isInMenu)
        {
            ReleaseAll();
            _turnPendingCounts = 0;
            _moveStrafe = _moveForward = 0;
            Log::Info("controls", "pause menu: sticks and buttons steer the menu");
        }
        SteerMenu(pad, now, shouldRefresh);
        _isInMenu = true;
        _isWaitingForRelease = true;
        _lastTick = now;
        return;
    }
    if (_isInMenu)
    {
        _isInMenu = false;
        if (_held.isFireHeld)
        {
            _held.isFireHeld = false;
            Input::MouseButton(0, false);
        }
        Log::Info("controls", "pause menu closed, back to the game's controls");
    }
    if (_isWaitingForRelease)
    {
        if (!pad.isLowerPressed[0] && !pad.isUpperPressed[0] && pad.trigger[0] < TRIGGER_RELEASE)
        {
            _isWaitingForRelease = false;
        }
        else
        {
            pad.isLowerPressed[0] = pad.isUpperPressed[0] = false;
            pad.trigger[0] = 0;
        }
    }
    float leftX = pad.stickX[1], leftY = pad.stickY[1];
    // Spawn screen: the game itself takes Up / Down for the class and Space to spawn (run86; Right / Left change
    // sides, so they are left alone). The left stick flicked up or down steps through the classes; A is Space
    // already. Command posts are picked with the pointer.
    if (Stereo::IsSpawnScreen())
    {
        static bool isClassFlickArmed = true;
        if (fabsf(leftY) < FLICK_CENTRED)
        {
            isClassFlickArmed = true;
        }
        else if (isClassFlickArmed && fabsf(leftY) > FLICK_PUSHED && fabsf(leftX) < 0.5f)
        {
            isClassFlickArmed = false;
            TapKey(leftY > 0 ? KEY_UP : KEY_DOWN, now);
            Feedback::Pulse(1, 0.015f, 0.3f);
        }
        leftX = leftY = 0;
    }
    // ---- gestures (Gesture.cpp) ----
    {
        const Gesture::Hand& right = Gesture::CurrentHand(0);
        const Gesture::Hand& left = Gesture::CurrentHand(1);
        // A hand at an ear with its grip held: that hand's stick picks instead of moving or turning, and EarMenu shows
        // the choices as a radial menu projected from that ear (PLAN 32; the right hand since headset run 15, which
        // found it missing: only the left hand was ever read). The left hand wins when both are there.
        // Left = pause menu (Esc). Down = map, right = player list: each stays until the same choice is made
        // again, so the player can let go and walk about with the map up (idren2023, headset run 8). Up = recentre the
        // view (idren2023 2026-10-13), as the left stick held down or Home.
        // A choice is taken when the stick, pushed to the edge, springs back to the centre quickly (released, headset
        // run 15: any flick took one before); brought back slowly by the thumb, nothing happens.
        static bool isEarMenuOpen = false, isTabHeld = false;
        static int earHand = -1;       // the hand at the ear while open
        static int edgeDirection = -1; // 0 up, 1 right, 2 down, 3 left while the stick is at the edge
        static DWORD leftEdgeTick = 0; // when the stick last came inside the edge
        const auto isHandAtEar = [](const Gesture::Hand& hand, int handIndex)
        {
            return hand.isTracked && hand.isGripped && !Squad::IsArmed(handIndex) &&
                   (hand.zone == Gesture::Zone::EarLeft || hand.zone == Gesture::Zone::EarRight);
        };
        const int handAtEar = !_isEarMenuOn ? -1 : isHandAtEar(left, 1) ? 1 : isHandAtEar(right, 0) ? 0 : -1;
        const bool isAtEar = handAtEar >= 0;
        if (isAtEar != isEarMenuOpen || (isAtEar && handAtEar != earHand))
        {
            isEarMenuOpen = isAtEar;
            earHand = handAtEar;
            edgeDirection = -1;
            Log::Info(
                "controls",
                "ear menu %s",
                isAtEar ? (handAtEar == 1 ? "open (left hand): stick up = recentre, left = pause, down = map, right = "
                                            "player list"
                                          : "open (right hand)")
                        : "closed");
            Feedback::Pulse(isAtEar ? handAtEar : 1, 0.02f, 0.3f);
        }
        EarMenu::Choice taken = EarMenu::Choice::None;
        float earStickX = 0, earStickY = 0;
        if (isAtEar)
        {
            earStickX = handAtEar == 1 ? leftX : pad.stickX[0];
            earStickY = handAtEar == 1 ? leftY : pad.stickY[0];
            const float length = sqrtf(earStickX * earStickX + earStickY * earStickY);
            if (length > EAR_STICK_EDGE)
            {
                edgeDirection =
                    fabsf(earStickX) >= fabsf(earStickY) ? (earStickX > 0 ? 1 : 3) : (earStickY > 0 ? 0 : 2);
                leftEdgeTick = 0;
            }
            else if (edgeDirection >= 0 && !leftEdgeTick)
            {
                leftEdgeTick = now | 1;
            }
            if (edgeDirection >= 0 && leftEdgeTick && length < FLICK_CENTRED)
            {
                const int direction = edgeDirection;
                // Signed: leftEdgeTick is `now | 1`, one more than now on an even tick (run214: every release was
                // slow).
                const bool isQuick =
                    static_cast<LONG>(now - leftEdgeTick) <= static_cast<LONG>(EAR_RELEASE_MILLISECONDS);
                edgeDirection = -1;
                leftEdgeTick = 0;
                if (isQuick && direction == 3)
                {
                    TapKey(KEY_ESCAPE, now);
                    taken = EarMenu::Choice::Pause;
                    Log::Info("controls", "ear menu: pause");
                }
                else if (isQuick && direction == 1)
                {
                    isTabHeld = !isTabHeld;
                    Input::Key(KEY_TAB, isTabHeld);
                    taken = EarMenu::Choice::PlayerList;
                    Log::Info("controls", "ear menu: player list %s", isTabHeld ? "on" : "off");
                }
                else if (isQuick && direction == 2)
                {
                    TapKey(KEY_M, now);
                    taken = EarMenu::Choice::Map;
                    Log::Info("controls", "ear menu: map toggled");
                }
                else if (isQuick)
                {
                    Xr::Recenter();
                    Aim::ForgetRestingPoses();
                    taken = EarMenu::Choice::Recenter;
                    Log::Info("controls", "ear menu: view recentred");
                }
                if (isQuick)
                {
                    Feedback::Pulse(handAtEar, 0.03f, 0.5f);
                }
            }
            else if (
                edgeDirection >= 0 && leftEdgeTick &&
                static_cast<LONG>(now - leftEdgeTick) > static_cast<LONG>(EAR_RELEASE_MILLISECONDS))
            {
                edgeDirection = -1; // brought back by the thumb: no choice
                leftEdgeTick = 0;
            }
        }
        EarMenu::Update(
            isAtEar,
            (handAtEar == 1 ? left.zone : right.zone) == Gesture::Zone::EarRight,
            earStickX,
            earStickY,
            taken);
        if (isAtEar)
        {
            if (handAtEar == 1)
            {
                leftX = leftY = 0; // no walking while picking
            }
            else
            {
                pad.stickX[0] = pad.stickY[0] = 0; // no turning or weapon change while picking
            }
        }
        // Reload (b118, idren2023 after headset run 9): the left trigger pulled with the left hand under the gun takes
        // the magazine: the drawn hand snaps onto it and stays there while the trigger is held, with a buzz that
        // grows as the controller pulls away (resistance). The controller leaving the zone with the trigger still
        // held reloads and gives the hand back; letting go inside the zone cancels. Not while two-handed.
        {
            static bool isHandInPlace = false;
            static DWORD resistanceTick = 0;
            // Off with the manual reload (Holster: the ammo canister takes its place, idren2023 2026-10-20).
            const bool isThere = _isReloadGestureOn && !Holster::IsManualReloadOn() && !Holster::IsSuperBattleDroid() &&
                                 Vehicle::CurrentSeat() == Vehicle::Seat::OnFoot &&
                                 left.isTracked && left.zone == Gesture::Zone::UnderGun && !Aim::IsTwoHanded();
            if (isThere != isHandInPlace)
            {
                isHandInPlace = isThere;
                if (isThere && _leftTrigger == LeftTrigger::Free)
                {
                    Feedback::Pulse(1, 0.02f, 0.35f);
                }
            }
            const bool isPulled =
                pad.trigger[1] > (_leftTrigger == LeftTrigger::Free ? TRIGGER_PRESS : TRIGGER_RELEASE);
            if (!isPulled)
            {
                if (_leftTrigger == LeftTrigger::Magazine)
                {
                    Aim::SetLeftHandOnMagazine(false);
                    Log::Info("controls", "reload cancelled: trigger let go under the gun");
                }
                _leftTrigger = LeftTrigger::Free;
            }
            else if (_leftTrigger == LeftTrigger::Free)
            {
                // On foot the secondaries are taken from the holsters and thrown by letting go (Holster): a pull
                // elsewhere does nothing (idren2023 2026-10-20: it threw grenades / fired the secondary by mistake).
                // In a vehicle and as the droideka it is the secondary fire.
                // (and the super battle droid's wrist rocket, its weapon readied by the right grip, idren2023 2026-11-04)
                const bool isSecondaryByTrigger =
                    Vehicle::CurrentSeat() != Vehicle::Seat::OnFoot || Aim::IsDroideka() ||
                    (Holster::IsSuperBattleDroid() && !Holster::IsHandEmpty());
                _leftTrigger = isThere                ? LeftTrigger::Magazine
                               : isSecondaryByTrigger ? LeftTrigger::SecondaryFire
                                                      : LeftTrigger::Spent;
                if (Holster::IsSuperBattleDroid())
                {
                    Log::Info("controls", "left trigger pulled: %s", isThere ? "reload" : isSecondaryByTrigger ? "secondary fire" : "nothing (weapon lowered)");
                }
                if (isThere)
                {
                    Aim::SetLeftHandOnMagazine(true);
                    Feedback::Pulse(1, 0.04f, 0.6f);
                    resistanceTick = now;
                    Log::Info("controls", "reload: magazine taken (left trigger under the gun)");
                }
            }
            else if (_leftTrigger == LeftTrigger::Magazine)
            {
                if (!left.isTracked || Aim::IsTwoHanded())
                {
                    Aim::SetLeftHandOnMagazine(false);
                    _leftTrigger = LeftTrigger::Spent;
                    Log::Info("controls", "reload cancelled: left hand lost or two-handed");
                }
                else if (left.zone != Gesture::Zone::UnderGun)
                {
                    Aim::SetLeftHandOnMagazine(false);
                    _leftTrigger = LeftTrigger::Spent;
                    TapKey(KEY_R, now);
                    Feedback::Pulse(1, 0.07f, 0.9f);
                    Feedback::Pulse(0, 0.05f, 0.6f);
                    Log::Info("controls", "reload: magazine pulled out of the zone");
                }
                else if (now - resistanceTick >= RESISTANCE_PULSE_MILLISECONDS)
                {
                    resistanceTick = now;
                    float strength = Aim::MagazinePullMeters() / RESISTANCE_FULL_PULL_METERS;
                    strength = strength > 1 ? 1 : strength;
                    Feedback::Pulse(1, RESISTANCE_PULSE_MILLISECONDS * 0.0012f, 0.1f + 0.5f * strength);
                }
            }
        }
        // Scope to the eye (headset run 15, idren2023): for a weapon with a lens, zoom is on while the lens is near the
        // head and off once it leaves; weapons without a lens zoom only by the right stick. The game's zoom key is a
        // toggle, so the zoom the game reports decides whether a press is needed; at most one press per 0.6 s. The
        // lens is placed with the weapon model as last drawn (Aim) and the head of the last stereo frame (Stereo).
        static bool isZoomOurs = false, isLensNear = false;
        static int zoomOffPresses = 0;
        static float lensDistanceMeters = 0;
        if (_isScopeGestureOn && right.isTracked)
        {
            float lensInModel[3], weaponWorld[16], headWorld[16];
            // Only on foot: in a vehicle seat the soldier's weapon chain is garbage (headset run 16 crash).
            if (Vehicle::CurrentSeat() == Vehicle::Seat::OnFoot &&
                Zoom::GetLensCentre(Aim::GetWeaponClass(), lensInModel) && Aim::GetWeaponWorld(weaponWorld) &&
                Stereo::GetLastHeadWorld(headWorld) && !Holster::HoldsNothing())
            {
                FpGuard guard;
                // Near, in front of the eyes, and the gun pointing about where the head looks (run214: the launcher
                // held at the ear zoomed, its lens 20 cm behind the hand).
                float distanceSquared = 0, ahead = 0, alignment = 0;
                for (int axis = 0; axis < 3; ++axis)
                {
                    const float lens = lensInModel[0] * weaponWorld[axis] + lensInModel[1] * weaponWorld[4 + axis] +
                                       lensInModel[2] * weaponWorld[8 + axis] + weaponWorld[12 + axis];
                    distanceSquared += (lens - headWorld[12 + axis]) * (lens - headWorld[12 + axis]);
                    ahead -= (lens - headWorld[12 + axis]) * headWorld[8 + axis];
                    alignment -= weaponWorld[8 + axis] * headWorld[8 + axis]; // model +z against the head's -z
                }
                const float limit = isLensNear ? SCOPE_LEAVE_METERS : SCOPE_ENTER_METERS;
                isLensNear = distanceSquared < limit * limit && ahead > 0 && alignment > SCOPE_MINIMUM_ALIGNMENT;
                lensDistanceMeters = sqrtf(distanceSquared);
            }
            else
            {
                isLensNear = false;
            }
            const bool isZoomed = Zoom::IsZoomed();
            if (now - _zoomKeyTick > ZOOM_REPEAT_MILLISECONDS)
            {
                if (isLensNear && !isZoomed && Holster::IsWeaponReady(Aim::GetWeapon()))
                {
                    TapKey(KEY_Z, now);
                    _zoomKeyTick = now;
                    if (!isZoomOurs)
                    {
                        Log::Info(
                            "controls",
                            "zoom on: lens %.0f cm from the eyes",
                            static_cast<double>(lensDistanceMeters * 100));
                    }
                    isZoomOurs = true;
                }
                else if (!isLensNear && isZoomed && isZoomOurs)
                {
                    // Ours until the game reports the zoom off (PLAN 58 i, headset run 17: zoom stayed on after the
                    // weapon left the eye): a tap the game missed is pressed again 0.6 s later.
                    TapKey(KEY_Z, now);
                    _zoomKeyTick = now;
                    Log::Info(
                        "controls",
                        "zoom off: lens %.0f cm from the eyes%s",
                        static_cast<double>(lensDistanceMeters * 100),
                        zoomOffPresses++ ? " (pressed again)" : "");
                }
                else if (!isLensNear && !isZoomed)
                {
                    isZoomOurs = false;
                    zoomOffPresses = 0;
                }
            }
        }
    }
    // The droideka rolled up (headset run 16, idren2023): the left stick alone steers the ball, sideways turning it
    // (below, with the right stick's turn); the view takes the ball's heading and the head only looks.
    const int droidekaMode = DroidekaMode();
    const bool isDroidekaBall = droidekaMode >= DROIDEKA_MODE_ROLLING_UP && droidekaMode <= DROIDEKA_MODE_STANDING_UP;
    _isDroidekaStanding = droidekaMode >= 0 && !isDroidekaBall;
    float ballTurn = 0;
    if (isDroidekaBall && fabsf(leftX) > DROIDEKA_BALL_TURN_DEAD_ZONE)
    {
        const float rest = (fabsf(leftX) - DROIDEKA_BALL_TURN_DEAD_ZONE) / (1.0f - DROIDEKA_BALL_TURN_DEAD_ZONE);
        ballTurn = (leftX > 0 ? rest : -rest) * rest;
    }
    if (isDroidekaBall)
    {
        leftX = 0;
    }
    // In a flyer's pilot seat with the flight stick the right stick's X is A / D (the game's sharp turn; headset run 18),
    // the stick in the hand the mouse (below).
    float flightYaw = 0, flightPitch = 0;
    const bool isFlightStick = Flight::GetStickLook(&flightYaw, &flightPitch);
    const bool isRightStickAd = isFlightStick && !Flight::IsHelicopter(); // a gunship's right stick X turns (Flight)
    const float sideX = isRightStickAd && fabsf(pad.stickX[0]) > fabsf(leftX) ? pad.stickX[0] : leftX;
    // the flight stick at its full tilt aside: the sharp turn as well (headset run 19)
    const bool isStickSharpRight = isRightStickAd && flightYaw >= FLIGHT_STICK_SHARP_TURN;
    const bool isStickSharpLeft = isRightStickAd && flightYaw <= -FLIGHT_STICK_SHARP_TURN;
    InstallMoveHooks();
    if (_areMoveHooksInstalled && _isAnalogMove)
    {
        // Analog movement relative to the character's facing (which follows the left controller): the stick's vector
        // is handed to the game's own movement inputs as it is (see SetMoveHook). No W/A/S/D keys are pressed but the
        // flight's A / D.
        for (unsigned keyCode : {KEY_W, KEY_S})
        {
            SetKey(keyCode, false, false);
        }
        SetKey(KEY_D, isStickSharpRight || (isRightStickAd && IsStickPast(_held.isKeyHeld[KEY_D], pad.stickX[0])), false);
        SetKey(KEY_A, isStickSharpLeft || (isRightStickAd && IsStickPast(_held.isKeyHeld[KEY_A], -pad.stickX[0])), false);
        float x = leftX, y = leftY;
        const float size = sqrtf(x * x + y * y);
        if (size < STICK_DEAD_ZONE)
        {
            x = y = 0;
        } // dead zone
        else
        {
            const float scale = (size > 1 ? 1 : (size - STICK_DEAD_ZONE) / STICK_LIVE_RANGE) / size;
            x *= scale;
            y *= scale;
        }
        _moveStrafe = x;
        _moveForward = y;
        _moveTick = now;
    }
    else
    {
        SetKey(KEY_W, IsStickPast(_held.isKeyHeld[KEY_W], leftY), shouldRefresh);
        SetKey(KEY_S, IsStickPast(_held.isKeyHeld[KEY_S], -leftY), shouldRefresh);
        SetKey(KEY_D, isStickSharpRight || IsStickPast(_held.isKeyHeld[KEY_D], sideX), shouldRefresh);
        SetKey(KEY_A, isStickSharpLeft || IsStickPast(_held.isKeyHeld[KEY_A], -sideX), shouldRefresh);
    }
    // Statistics after a match (a large overlay while the player controls nothing; headset run 12: no way out): A is
    // Enter (Accept: details, next page), B is Escape (Back: done), as the game's ifs_teamstats screen takes them.
    const bool isListScreen = Hud::HasOverlay() && !IsOnFoot() && !Stereo::IsSpawnScreen();
    const Vehicle::Seat seat = Vehicle::CurrentSeat();
    const bool isInVehicle = seat != Vehicle::Seat::OnFoot && seat != Vehicle::Seat::None;
    _isStatisticsScreen = isListScreen;
    {
        static bool wasA = false, wasB = false, wasListScreen = false;
        if (isListScreen != wasListScreen)
        {
            wasListScreen = isListScreen;
            Log::Info("controls", "statistics screen %s: A = Enter, B = Escape", isListScreen ? "up" : "gone");
        }
        if (isListScreen && pad.isLowerPressed[0] && !wasA)
        {
            TapKey(KEY_RETURN, now);
            Log::Info("controls", "statistics: A (Enter)");
        }
        if (isListScreen && pad.isUpperPressed[0] && !wasB)
        {
            TapKey(KEY_ESCAPE, now);
            Log::Info("controls", "statistics: B (Escape)");
        }
        wasA = pad.isLowerPressed[0];
        wasB = pad.isUpperPressed[0];
    }
    // The droideka (idren2023, 2026-10-17): A rolls up / stands up instead. Its toggle is the game's enter / exit control
    // (E, below; offsets "Droideka"); jump does nothing for it.
    const bool isRollButton = Aim::IsDroideka();
    {
        static int lastMode = -1;
        if (isRollButton && droidekaMode >= 0 && droidekaMode != lastMode)
        {
            lastMode = droidekaMode;
            Log::Info(
                "controls",
                "droideka mode %d (0 standing, 11 rolling up, 12 ball, 13 standing up)",
                droidekaMode);
        }
    }
    // A with a flyer's stick in hand is the stick's face button, the secondary fire (Flight), not Land / Takeoff.
    // The take-off lever (Flight) holds the game's Land / Takeoff key for a moment.
    SetKey(
        KEY_SPACE,
        (pad.isLowerPressed[0] && !isListScreen && !isRollButton && !Flight::IsStickHeld()) || Flight::IsLandingKeyDown(),
        shouldRefresh);
    SetKey(_crouchKey, pad.isUpperPressed[0] && !isListScreen, false);
    // Stance by head height (standing play; PLAN 58 h, headset run 17): the head more than 30 cm under its resting
    // height = crouch (back above 20 cm = stand), more than 85 cm under = prone (back above 75 cm = crouch), each
    // reached from any other. The game's crouch key is a toggle, stand -> crouch -> prone -> stand (run289), and a jump
    // stands a crouched soldier up; so each step toward the head's stance is the crouch key, or a jump from a crouch
    // to stand, at most one per 0.7 s, until the soldier's own stance (soldier + 0xA60) is the head's. Only when the
    // head's stance changes: the game's own changes (B, a jump) win until then. Not for the droideka.
    if (_isCrouchByHead && !Aim::IsDroideka())
    {
        static int headStance = STANCE_STAND, wantedStance = -1; // wanted: the stance being driven to, or none
        static DWORD pressTick = 0;
        static int pressCount = 0; // presses toward the wanted stance
        float head[16];
        const uintptr_t soldier = EngineSurvey::Resolve("soldier");
        const bool hasStance = soldier && !IsBadReadPtr(reinterpret_cast<void*>(soldier + OFFSET_soldierStance), 4);
        const int stance = hasStance ? *reinterpret_cast<int*>(soldier + OFFSET_soldierStance) : STANCE_STAND;
        if (Stereo::GetHeadPose(head) && hasStance)
        {
            // head[13] is the matrix's row 3 y: the head's height relative to its resting pose, metres.
            const float drop = -head[13];
            const int before = headStance;
            if (headStance == STANCE_PRONE)
            {
                headStance = drop < STAND_HEAD_DROP_METERS ? STANCE_STAND
                             : drop < PRONE_LEAVE_HEAD_DROP_METERS ? STANCE_CROUCH
                                                                   : STANCE_PRONE;
            }
            else if (headStance == STANCE_CROUCH)
            {
                headStance = drop > PRONE_HEAD_DROP_METERS ? STANCE_PRONE
                             : drop < STAND_HEAD_DROP_METERS ? STANCE_STAND
                                                             : STANCE_CROUCH;
            }
            else
            {
                headStance = drop > PRONE_HEAD_DROP_METERS   ? STANCE_PRONE
                             : drop > CROUCH_HEAD_DROP_METERS ? STANCE_CROUCH
                                                              : STANCE_STAND;
            }
            if (headStance != before)
            {
                wantedStance = headStance;
                pressCount = 0;
                Log::Info(
                    "controls",
                    "stance by head: %s (head %.0f cm under its resting height, soldier's stance %d)",
                    headStance == STANCE_STAND ? "stand" : headStance == STANCE_CROUCH ? "crouch" : "prone",
                    static_cast<double>(drop * 100),
                    stance);
            }
            if (wantedStance >= 0 && stance == wantedStance)
            {
                wantedStance = -1; // reached
            }
            if (wantedStance >= 0 && now - pressTick > CROUCH_REPEAT_MILLISECONDS)
            {
                if (pressCount >= STANCE_MAXIMUM_PRESSES)
                {
                    Log::Info("controls", "stance by head: gave up at stance %d (wanted %d)", stance, wantedStance);
                    wantedStance = -1;
                }
                else
                {
                    const bool isJump = stance == STANCE_CROUCH && wantedStance == STANCE_STAND;
                    TapKey(isJump ? KEY_SPACE : _crouchKey, now);
                    pressTick = now;
                    ++pressCount;
                    Log::Info(
                        "controls",
                        "stance by head: %s (stance %d, wanted %d)",
                        isJump ? "jump" : "crouch key",
                        stance,
                        wantedStance);
                }
            }
        }
        // While crouched or prone as the head is, the game's own lowering of the camera (1.72 m at the eye standing,
        // 1.22 crouched, 0.63 prone) is taken out: the player's head has gone down already.
        Stereo::SetLift(
            stance == STANCE_CROUCH && headStance != STANCE_STAND ? CROUCH_CAMERA_DROP_METERS
            : stance == STANCE_PRONE && headStance != STANCE_STAND ? PRONE_CAMERA_DROP_METERS
                                                                    : 0.0f);
    }
    else
    {
        Stereo::SetLift(0.0f);
    }

    // Right trigger: primary fire; nothing with an empty hand; the secondary fire while the hand holds a secondary
    // weapon used by its trigger (Holster). While the pointer is on a screen (the statistics after a match) it clicks,
    // also with an empty hand (headset run 14: the statistics' options could not be pressed any more).
    const bool isTriggerItem = Holster::IsTriggerItemHeld();
    // In a vehicle the right trigger fires its primary weapon whatever the holster's hand state (the hand is on a
    // lever or free; idren2023 2026-10-17: right trigger primary, left trigger secondary).
    const bool isFire = (!Holster::IsHandEmpty() || Pointer::IsActive() || isInVehicle) &&
                        (_held.isFireHeld ? pad.trigger[0] > TRIGGER_RELEASE : pad.trigger[0] > TRIGGER_PRESS);
    if (isFire != _held.isFireHeld || (shouldRefresh && isFire))
    {
        _held.isFireHeld = isFire;
        Input::MouseButton(0, isFire);
    }
    // Left trigger: secondary fire (right mouse button) unless the pull began as a reload (see the gestures above).
    const bool isSecondaryFire = _leftTrigger == LeftTrigger::SecondaryFire || Holster::IsThrowing() ||
                                 Flight::IsSecondaryFire() ||
                                 (isTriggerItem && (_held.isSecondaryFireHeld ? pad.trigger[0] > TRIGGER_RELEASE
                                                                              : pad.trigger[0] > TRIGGER_PRESS));
    if (isSecondaryFire != _held.isSecondaryFireHeld || (shouldRefresh && isSecondaryFire))
    {
        if (isSecondaryFire != _held.isSecondaryFireHeld)
        {
            Log::Info("controls", "secondary fire %s", isSecondaryFire ? "on" : "off");
        }
        _held.isSecondaryFireHeld = isSecondaryFire;
        Input::MouseButton(1, isSecondaryFire);
    }
    if (_isReloadRequested) // the manual reload's canister put into the gun (Holster)
    {
        _isReloadRequested = false;
        TapKey(KEY_R, now);
        Log::Info("controls", "reload: R tapped for the ammo canister");
    }
    // Right grip: reload, unless the holsters use it to hold the weapon; where there are no holsters in VR (super
    // battle droid, droideka, vehicles) a grip press is the next primary weapon (idren2023, 2026-10-12).
    SetKey(KEY_R, !Holster::IsActive() && (_held.isKeyHeld[KEY_R] ? pad.grip[0] > 0.4f : pad.grip[0] > 0.7f), false);
    {
        static bool wasGripped = false;
        const bool isGripped = wasGripped ? pad.grip[0] > 0.4f : pad.grip[0] > 0.7f;
        // (the super battle droid: on the release, idren2023 2026-11-04: the grip held readies the weapon, Holster)
        const bool isChange = Holster::IsSuperBattleDroid() ? !isGripped && wasGripped : isGripped && !wasGripped;
        if (isChange && Holster::IsActive() && (!Holster::HasPlaces() || Holster::IsSuperBattleDroid()) &&
            !isListScreen && !isInVehicle) // in a vehicle the grip is for the levers; seats change by the stick click
        {
            Input::MouseWheel(WHEEL_DELTA); // wheel up = next primary weapon
            Log::Info("controls", "right grip: next primary weapon (%s)",
                      Holster::IsSuperBattleDroid() ? "super battle droid, grip released" : "no holsters");
        }
        wasGripped = isGripped;
    }
    // X: use / enter vehicle; A for the droideka (roll toggle, see KEY_SPACE above).
    // (The droid starfighter: X takes off and lands, held a second on the ground it leaves; Flight.)
    SetKey(KEY_E,
           Flight::IsRemote() ? Flight::IsExitKeyDown()
                              : pad.isLowerPressed[1] || (isRollButton && pad.isLowerPressed[0] && !isListScreen),
           false);
    // Y: a short press changes first / third person (on release), held for a second it opens the pause menu
    // (SteamVR keeps the left menu button for itself: "not given to the game by the runtime", headset run 7).
    {
        static DWORD yDownSince = 0, firstPersonKeyUpTick = 0, escapeKeyUpTick = 0;
        if (pad.isUpperPressed[1])
        {
            if (!yDownSince)
            {
                yDownSince = now | 1; // never 0 (= not pressed) nor 1 (= hold already handled)
            }
            else if (yDownSince != 1 && now - yDownSince >= HOLD_MILLISECONDS)
            {
                yDownSince = 1;
                Input::Key(KEY_ESCAPE, true);
                escapeKeyUpTick = now + KEY_PULSE_MILLISECONDS;
                Log::Info("controls", "pause from holding Y");
            }
        }
        else
        {
            if (yDownSince && yDownSince != 1)
            {
                Input::Key(KEY_Q, true);
                firstPersonKeyUpTick = now + KEY_PULSE_MILLISECONDS;
            }
            yDownSince = 0;
        }
        if (firstPersonKeyUpTick && now >= firstPersonKeyUpTick)
        {
            Input::Key(KEY_Q, false);
            firstPersonKeyUpTick = 0;
        }
        if (escapeKeyUpTick && now >= escapeKeyUpTick)
        {
            Input::Key(KEY_ESCAPE, false);
            escapeKeyUpTick = 0;
        }
    }
    // Right stick pressed: zoom on foot; in a vehicle the next seat (the weapon-change action, mouse wheel up; idren2023
    // 2026-10-17), one per press.
    {
        static bool wasStickPressed = false;
        if (isInVehicle && pad.isStickPressed[0] && !wasStickPressed && !isListScreen)
        {
            Input::MouseWheel(WHEEL_DELTA);
            Log::Info("controls", "right stick click: next seat");
        }
        wasStickPressed = pad.isStickPressed[0];
    }
    SetKey(KEY_Z, pad.isStickPressed[0] && !isInVehicle, false);
    { // Zoomed when the right hand lets go of its weapon: zoom off, or the scope picture stays in mid-air where the gun
      // was (headset run 12, PLAN 27). The game's zoom key is a toggle; at most one press per 0.6 s.
        static DWORD unzoomTick = 0;
        if (Holster::HoldsNothing() && Zoom::IsZoomed() && !pad.isStickPressed[0] &&
            now - unzoomTick > ZOOM_REPEAT_MILLISECONDS)
        {
            TapKey(KEY_Z, now);
            unzoomTick = now;
            _zoomKeyTick = now;
            Log::Info("controls", "zoom off: the hand let go of the weapon");
        }
    }
    if (pad.isStickPressed[0])
    {
        _zoomKeyTick = now;
    }
    { // A zoom the game ends by itself (a reload: a sniper's last shot, headset run 15) is given back once the same
      // weapon is ready again, while the hand still holds it.
        static bool wasZoomed = false;
        static uintptr_t zoomedWeapon = 0;
        static DWORD restoreUntilTick = 0;
        const bool isZoomed = Zoom::IsZoomed();
        const uintptr_t weapon = Aim::GetWeapon();
        if (isZoomed)
        {
            zoomedWeapon = weapon;
            restoreUntilTick = 0;
        }
        else if (wasZoomed && now - _zoomKeyTick > ZOOM_REPEAT_MILLISECONDS && weapon && weapon == zoomedWeapon)
        {
            restoreUntilTick = now + ZOOM_RESTORE_MILLISECONDS;
            Log::Info("controls", "zoom ended by the game: given back once the weapon is ready");
        }
        if (restoreUntilTick && !isZoomed)
        {
            if (static_cast<LONG>(now - restoreUntilTick) >= 0 || weapon != zoomedWeapon || Holster::HoldsNothing())
            {
                restoreUntilTick = 0;
            }
            else if (now - _zoomKeyTick > ZOOM_REPEAT_MILLISECONDS && Holster::IsWeaponReady(weapon))
            {
                TapKey(KEY_Z, now);
                _zoomKeyTick = now; // pressed again every 0.6 s until the game zooms or the time is up
                Log::Info("controls", "zoom given back after the reload");
            }
        }
        wasZoomed = isZoomed;
    }
    SetKey(KEY_ESCAPE, pad.isMenuPressed, false); // left menu button: pause menu
    // Right stick up / down: next / previous weapon, one step per flick (mouse wheel).
    const float rightY = pad.stickY[0];
    // Jet hover (PLAN 36): flown by the left stick along the left controller (or the head); right stick up / down
    // is the climb.
    Jet::OnPresent(rightY, pad.stickX[1], pad.stickY[1], _isMoveByHead);
    // Up: next secondary weapon, down: next primary. Needs a firm, mostly vertical push, and nothing happens while
    // the stick is pressed in (zoom) or shortly after, so a click cannot change the weapon.
    static DWORD lastClickTick = 0;
    if (pad.isStickPressed[0])
    {
        lastClickTick = now;
    }
    // Loosened in b93 (idren2023: too hard to change weapon in a fight): 0.6 of the way, within about 37 degrees of
    // straight up or down (it was 0.85 and 0.35 sideways).
    const bool isVertical = fabsf(rightY) > WEAPON_PUSH &&
                            fabsf(pad.stickX[0]) < fabsf(rightY) * WEAPON_SIDEWAYS_RATIO && !pad.isStickPressed[0] &&
                            now - lastClickTick > WEAPON_CLICK_LOCKOUT_MILLISECONDS;
    // With holsters the weapons are taken from the body; the stick changes weapons only where there are none
    // (vehicles, droideka) (headset run 12).
    if (_isWeaponChangeArmed && isVertical && !Holster::HasPlaces() && !Jet::IsHovering())
    {
        if (rightY > 0 && _secondaryKey)
        {
            Input::Key(_secondaryKey, true);
            _secondaryKeyDownTick = now | 1;
        }
        else if (!isInVehicle) // in a vehicle the wheel changes seats: that is the stick click
        {
            Input::MouseWheel(WHEEL_DELTA); // wheel up (one notch) = next primary weapon
        }
        _isWeaponChangeArmed = false;
    }
    else if (fabsf(rightY) < SNAP_RELEASE)
    {
        _isWeaponChangeArmed = true;
    }
    if (_secondaryKeyDownTick && now - _secondaryKeyDownTick > KEY_PULSE_MILLISECONDS)
    {
        Input::Key(_secondaryKey, false);
        _secondaryKeyDownTick = 0;
    }
    // Left stick held down for a second: recentre the view (same as the Home key).
    if (!pad.isStickPressed[1])
    {
        _recentreSince = 0;
    }
    else if (_recentreSince == 0)
    {
        _recentreSince = now | 2;
    }
    else if (_recentreSince != 1 && now - _recentreSince >= HOLD_MILLISECONDS)
    {
        _recentreSince = 1;
        Xr::Recenter();
        Aim::ForgetRestingPoses();
        Log::Info("controls", "view recentred from the left stick");
    }

    // Hovering with a jet pack the right stick's up / down is the climb: it turns only while the stick is mostly
    // sideways (more than twice as far across as up or down), so a climb pushed a little off straight does not turn
    // (headset run 13).
    const bool isClimbing = Jet::IsHovering() && fabsf(pad.stickX[0]) < 2.0f * fabsf(pad.stickY[0]);
    const float rightX = isClimbing || isFlightStick ? 0.0f : pad.stickX[0]; // the flight's right X is A / D
    const float elapsedSeconds = static_cast<float>(now - _lastTick) * SECONDS_PER_MILLISECOND;
    _lastTick = now;
    float degrees = 0; // positive = to the right
    if (_isSnapTurn)
    {
        if (_isSnapArmed && fabsf(rightX) > SNAP_PRESS)
        {
            degrees = rightX > 0 ? _turnDegrees : -_turnDegrees;
            _isSnapArmed = false;
        }
        else if (fabsf(rightX) < SNAP_RELEASE)
        {
            _isSnapArmed = true;
        }
    }
    else if (fabsf(rightX) > STICK_DEAD_ZONE && elapsedSeconds > 0 && elapsedSeconds < LONGEST_TURN_STEP_SECONDS)
    {
        degrees = rightX * _turnDegrees * elapsedSeconds;
    }
    long flightPitchCounts = 0;
    if (isFlightStick && elapsedSeconds > 0 && elapsedSeconds < LONGEST_TURN_STEP_SECONDS && !Pointer::IsActive())
    {
        degrees += flightYaw * FLIGHT_STICK_DEGREES_PER_SECOND * elapsedSeconds;
        // a flyer takes the mouse's vertical the other way round from a soldier's look (run518: the soldier's sign
        // put the nose down when the stick was pulled back): counts positive = nose up
        const float counts =
            flightPitch * FLIGHT_STICK_PITCH_DEGREES_PER_SECOND * elapsedSeconds * _countsPerDegree +
            _flightPitchRemainderCounts;
        flightPitchCounts = static_cast<long>(counts);
        _flightPitchRemainderCounts = counts - static_cast<float>(flightPitchCounts);
    }
    if (ballTurn != 0 && elapsedSeconds > 0 && elapsedSeconds < LONGEST_TURN_STEP_SECONDS)
    {
        degrees += ballTurn * DROIDEKA_BALL_TURN_DEGREES_PER_SECOND * elapsedSeconds / DROIDEKA_BALL_COUNT_GAIN;
    }
    if (Pointer::IsActive())
    {
        degrees = 0;
        _turnPendingCounts = 0;
    } // in menus the mouse belongs to the pointer
    { // For the comfort vignette: smooth turning by its speed (full at 200 degrees per second), walking by stick half.
        const float turning = (_isSnapTurn                       ? 0.0f
                               : fabsf(rightX) > STICK_DEAD_ZONE ? fabsf(rightX) * _turnDegrees / 200.0f
                                                                 : 0.0f) +
                              fabsf(ballTurn) * DROIDEKA_BALL_TURN_DEGREES_PER_SECOND / 200.0f;
        const float walking = sqrtf(pad.stickX[1] * pad.stickX[1] + pad.stickY[1] * pad.stickY[1]);
        const float motion = turning > walking * 0.5f ? turning : walking * 0.5f;
        _motion = motion > 1 ? 1 : motion;
    }
    // On foot with controllers the view keeps its own heading (Stereo::SetViewFree): snap and smooth turns turn the
    // view, and the character follows the left controller below. Not for the droideka's ball: the view follows it as
    // in a vehicle, the turns go to the game as mouse counts.
    // In a fighter tank's gunner station too (PLAN 58 c): the hull turns under the view, the turret follows the head.
    // A tank's pilot with the drive stick too (PLAN 59 a): the hull turns under the view toward the head (Vehicle).
    const bool isStationSeat = GunnerSeat::IsStationSeat() || GunnerSeat::IsHeldViewCabinSeat() || Levers::IsShown() ||
                               WalkerDrive::IsShown();
    // The recon droid too (idren2023 2026-11-22): it turns under the held view toward the head (Vehicle).
    const bool isViewFree =
        Xr::IsRunning() && ((IsOnFoot() && !isDroidekaBall) || isStationSeat || Vehicle::IsRemoteDroid());
    Stereo::SetViewFree(isViewFree);
    float characterYawRadians = 0;
    const bool isViewHeld = Stereo::GetCharacterYaw(&characterYawRadians);
    if (degrees != 0 && isViewHeld)
    {
        Stereo::TurnView(degrees / DEGREES_PER_RADIAN);
        if (_isSnapTurn)
        {
            Log::Info("controls", "snap turn %.0f degrees (view)", static_cast<double>(degrees));
        }
        degrees = 0;
    }
    if (degrees != 0)
    {
        const float counts = degrees * _countsPerDegree + _turnRemainderCounts;
        const long wholeCounts = static_cast<long>(counts);
        _turnRemainderCounts = counts - static_cast<float>(wholeCounts);
        _turnPendingCounts += wholeCounts;
        if (_isSnapTurn)
        {
            Log::Info("controls", "snap turn %.0f degrees (%ld counts)", static_cast<double>(degrees), wholeCounts);
        }
    }
    // Character follow (b141, PLAN 21; replaces b129's body follow, whose head-based turns were nauseating): the left
    // controller's heading is the character's forward, as mouse X is on the flat screen. The character is turned by
    // mouse counts toward it; the view's heading is held meanwhile, so the world seen does not move. Closed loop on the
    // heading measured at the last Update, less the counts still queued.
    // The droideka (idren2023, 2026-10-17) follows the head instead, in heading and pitch: the game aims its arms along
    // its own camera, so the guns point where the head looks (within its ODF limits, pitch -25..25 degrees). The view
    // ignores the game camera's pitch (Stereo LevelCamera), so the pitch is fed as vertical mouse counts, closed loop
    // on the game camera's pitch at the last Update.
    const bool isDroideka = Aim::IsDroideka();
    float handYawRadians = 0;
    const bool isFollowing =
        isViewHeld && !isStationSeat && !Pointer::IsActive() && elapsedSeconds > 0 &&
        elapsedSeconds < LONGEST_TURN_STEP_SECONDS;
    long pitchCounts = 0;
    float headPitchRadians = 0, gamePitchRadians = 0;
    if (isFollowing && isDroideka && Stereo::GetHeadPitch(&headPitchRadians) && Stereo::GetGamePitch(&gamePitchRadians))
    {
        const float errorDegrees = (headPitchRadians - gamePitchRadians) * DEGREES_PER_RADIAN;
        float stepDegrees = errorDegrees * (1.0f - expf(-elapsedSeconds / CHARACTER_FOLLOW_SECONDS));
        const float largestStep = CHARACTER_FOLLOW_MAXIMUM_DEGREES_PER_SECOND * elapsedSeconds;
        stepDegrees = stepDegrees > largestStep ? largestStep : stepDegrees < -largestStep ? -largestStep : stepDegrees;
        const float counts = -stepDegrees * _countsPerDegree + _pitchRemainderCounts; // mouse down = look down
        pitchCounts = static_cast<long>(counts);
        _pitchRemainderCounts = counts - static_cast<float>(pitchCounts);
        pitchCounts = pitchCounts > MAXIMUM_COUNTS_PER_FRAME    ? MAXIMUM_COUNTS_PER_FRAME
                      : pitchCounts < -MAXIMUM_COUNTS_PER_FRAME ? -MAXIMUM_COUNTS_PER_FRAME
                                                                : pitchCounts;
        static DWORD lastLogTick = 0;
        if (now - lastLogTick >= 2000)
        {
            lastLogTick = now;
            Log::Info(
                "controls",
                "droideka pitch: head %.1f, game %.1f degrees (%ld counts)",
                static_cast<double>(headPitchRadians * DEGREES_PER_RADIAN),
                static_cast<double>(gamePitchRadians * DEGREES_PER_RADIAN),
                pitchCounts);
        }
    }
    // With head-based movement (swbf-vr.ini: move_direction = head) the character faces where the head looks.
    const bool isHeadFollow = isDroideka || _isMoveByHead;
    // In a network game a soldier faces along the gun hand instead, steered by the rates of the control state
    // (WriteNetAim); the left controller or the head still say where the stick's forward is (WriteNetControls).
    const bool isNetSoldier = Net::IsNetGame() && isFollowing && !isDroideka && IsOnFoot();
    _isNetAimOn = isNetSoldier;
    ++_netAimFrame;
    _netMoveTurnRadians = 0;
    if (isNetSoldier)
    {
        if (_isMoveByHead ? Stereo::GetHeadYaw(&handYawRadians) : Aim::GetHandYaw(1, &handYawRadians))
        {
            _netMoveTurnRadians = handYawRadians - characterYawRadians;
        }
    }
    else if (isFollowing && (isHeadFollow ? Stereo::GetHeadYaw(&handYawRadians) : Aim::GetHandYaw(1, &handYawRadians)))
    {
        float errorDegrees = (handYawRadians - characterYawRadians) * DEGREES_PER_RADIAN;
        errorDegrees = errorDegrees > 180.0f    ? errorDegrees - 360.0f
                       : errorDegrees < -180.0f ? errorDegrees + 360.0f
                                                : errorDegrees;
        errorDegrees -= static_cast<float>(_turnPendingCounts) / _countsPerDegree;
        float stepDegrees = errorDegrees * (1.0f - expf(-elapsedSeconds / CHARACTER_FOLLOW_SECONDS));
        const float largestStep = CHARACTER_FOLLOW_MAXIMUM_DEGREES_PER_SECOND * elapsedSeconds;
        stepDegrees = stepDegrees > largestStep ? largestStep : stepDegrees < -largestStep ? -largestStep : stepDegrees;
        const float counts = stepDegrees * _countsPerDegree + _followRemainderCounts;
        const long wholeCounts = static_cast<long>(counts);
        _followRemainderCounts = counts - static_cast<float>(wholeCounts);
        _turnPendingCounts += wholeCounts;
    }
    pitchCounts += flightPitchCounts;
    pitchCounts = pitchCounts > MAXIMUM_COUNTS_PER_FRAME    ? MAXIMUM_COUNTS_PER_FRAME
                  : pitchCounts < -MAXIMUM_COUNTS_PER_FRAME ? -MAXIMUM_COUNTS_PER_FRAME
                                                            : pitchCounts;
    if (_turnPendingCounts || pitchCounts)
    {
        const long step = _turnPendingCounts > MAXIMUM_COUNTS_PER_FRAME    ? MAXIMUM_COUNTS_PER_FRAME
                          : _turnPendingCounts < -MAXIMUM_COUNTS_PER_FRAME ? -MAXIMUM_COUNTS_PER_FRAME
                                                                           : _turnPendingCounts;
        _turnPendingCounts -= step;
        Input::MouseMove(step, pitchCounts);
    }
}

} // namespace Controls

} // namespace SwbfVr
