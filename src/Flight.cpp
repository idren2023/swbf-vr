// Flight.cpp: the flight stick, throttle and take-off / landing lever in a flyer's pilot seat (see Flight.h).
//
// Everything is worked out in resting-pose space (x right, y up, z back, metres, origin at the resting head), the
// space the controller poses come in (Gesture::Hand::pose). In a flyer's pilot seat the level camera, which carries
// that space into the world, has the hull's whole attitude (Stereo HoldHullHeading), so the handles stand in the
// cockpit and pitch and roll with it. The engine (runs 455-457, Bespin Platforms, the X-wing): the pilot's move input
// (Controllable +0x54) is the throttle, -1 slowing to the ODF's MinSpeed (about 24 m/s seen), 0 settling at its
// MidSpeed (50) and 1 at its MaxSpeed (82); the look inputs (+0x5C yaw, right positive; +0x60 pitch, up positive) turn
// the nose, which keeps its attitude without input; Space takes off from the ground (a 3 m rise, then the ship speeds
// up by itself) and, pressed again, lands.

// Makes d3d9.h declare the Direct3D interfaces as C structs with an explicit vtable (device->lpVtbl->Method(device,
// ...)), the form the rest of the mod uses. Has to be a macro: d3d9.h reads it.
#define CINTERFACE

#include "Flight.h"

#include "Aim.h"
#include "CockpitShell.h"
#include "Feedback.h"
#include "FpGuard.h"
#include "Gesture.h"
#include "LeverModel.h"
#include "LeverPlacement.h"
#include "Log.h"
#include "Stereo.h"
#include "Vehicle.h"
#include "Xr.h"

#include <windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstring>

namespace SwbfVr
{

namespace Flight
{

namespace
{

constexpr float RADIANS_PER_DEGREE = 3.14159265f / 180.0f;
/// Where each hand closes on a grip above its pivot (LeverModel's grip).
using LeverPlacement::GRIP_HEIGHT_METERS;
/// The handles (index into _handles).
enum Handle
{
    STICK,
    THROTTLE,
    LEVER,
    HANDLE_COUNT
};
/// The hand that takes each handle (0 right, 1 left).
constexpr int HAND_OF[HANDLE_COUNT] = {0, 1, 1};
/// The stick's pivot, resting-pose space (idren2023 2026-10-27: a fighter jet's centre stick between the legs): on the
/// floor plate in the middle, 86 cm under and 34 cm ahead of the eyes, so the grip (LeverModel::JetStick, the hand
/// JET_STICK_GRIP_METERS up the shaft) stands 53 cm under the eyes before the seat.
constexpr float STICK_PIVOT[3] = {0.0f, -0.86f, -0.34f};
constexpr float STICK_GRIP_METERS = LeverModel::JET_STICK_GRIP_METERS;
/// The stick's tilt is taken from the hand's move about its pivot (the shaft drawn is that long); full input at the
/// largest tilt, nothing within the dead zone, and a curve between them for fine aim.
constexpr float STICK_ARM_METERS = STICK_GRIP_METERS;
/// Headset run 18 (idren2023: stiffer): full input at 28 degrees (was 20: about 16 cm of hand travel at the grip).
constexpr float STICK_MOST_RADIANS = 28.0f * RADIANS_PER_DEGREE, STICK_DEAD_RADIANS = 2.0f * RADIANS_PER_DEGREE;
constexpr float STICK_CURVE = 1.4f;
/// The stick's rumble while held (headset run 18: haptics by the manoeuvre's strength): a pulse this often, its
/// strength from the rest value to the full one with the tilt's share.
constexpr DWORD STICK_RUMBLE_MILLISECONDS = 60;
constexpr float STICK_RUMBLE_REST = 0.04f, STICK_RUMBLE_FULL = 0.45f;
/// The throttle (idren2023 2026-10-27: a boat-style lever, LeverModel::Throttle): its hinge in the left console's slot,
/// the arm swinging ahead and back over it; the notches' spacing is measured along the knob's arc (8 cm: 33 degrees);
/// the hub half out of the console's top.
constexpr float THROTTLE_PIVOT[3] = {-0.25f, -0.64f, -0.27f};
constexpr float THROTTLE_GRIP_METERS = LeverModel::THROTTLE_GRIP_METERS;
constexpr float NOTCH_SPACING_METERS = 0.08f, NOTCH_HYSTERESIS_METERS = 0.015f, GIVE_METERS = 0.012f;
/// The arm upright at cruise.
constexpr float THROTTLE_LEAN_RADIANS = 0.0f;
/// Helicopter flight (gunship, MAF; the vehicle proposal 2026-11-04, idren2023: as Forefront's): the stick ahead / back
/// flies forward / back across the move axis (MinSpeed -15 .. MaxSpeed 45, centre hovering), aside strafes, twisted
/// about its shaft turns (dead zone, full); the left handle is the elevation lever (the collective): the hand raised
/// from where it took it climbs, lowered sinks, full at COLLECTIVE_FULL_METERS; let go it springs to the middle (hold
/// height). The lever leans ahead from its hinge beside the seat, tipping up and down with the climb.
constexpr float TWIST_DEAD_RADIANS = 6.0f * RADIANS_PER_DEGREE, TWIST_FULL_RADIANS = 40.0f * RADIANS_PER_DEGREE;
constexpr float COLLECTIVE_PIVOT[3] = {-0.27f, -0.70f, -0.05f};
constexpr float COLLECTIVE_LEAN_RADIANS = 65.0f * RADIANS_PER_DEGREE, COLLECTIVE_SWING_RADIANS = 25.0f * RADIANS_PER_DEGREE;
constexpr float COLLECTIVE_DEAD_METERS = 0.015f, COLLECTIVE_FULL_METERS = 0.10f;
/// The fastest climb or sink the lever (or the right thumbstick) asks for (m/s; the take-off rises at 5).
constexpr float MOST_CLIMB_METERS_PER_SECOND = 8.0f;
/// The take-off lever (an e-brake beside the seat, low on the left): its hinge, and its angle up from lying ahead:
/// down (flying) and up (landed); pushed below the first commits a take-off, pulled above the second a landing.
constexpr float LEVER_PIVOT[3] = {-0.36f, -0.62f, -0.10f}; // in the X-wing's left console's gate (ShellDesigns)
constexpr float LEVER_DOWN_RADIANS = 5.0f * RADIANS_PER_DEGREE, LEVER_UP_RADIANS = 45.0f * RADIANS_PER_DEGREE;
constexpr float LEVER_TAKE_OFF_RADIANS = 15.0f * RADIANS_PER_DEGREE, LEVER_LAND_RADIANS = 35.0f * RADIANS_PER_DEGREE;
/// Let go, the lever swings to the ship's state at this speed.
constexpr float LEVER_SETTLE_RADIANS_PER_SECOND = 3.0f;
/// The ship counts as flying above this speed and as landed below the second for the time after it; a take-off or
/// landing asked for is trusted this long before the speed must show it.
constexpr float FLYING_METERS_PER_SECOND = 8.0f, LANDED_METERS_PER_SECOND = 0.5f; // a landing sinks at 5 m/s (run467)
constexpr DWORD LANDED_MILLISECONDS = 1000, REQUEST_MILLISECONDS = 4000;
/// A hover craft has landed when it stopped within this long after sinking faster than this (a hover: no sink).
constexpr float SINKING_METERS_PER_SECOND = 1.5f;
constexpr DWORD SINK_TO_STOP_MILLISECONDS = 1500;
/// The Land / Takeoff key is held this long for a lever's request.
constexpr DWORD KEY_HOLD_MILLISECONDS = 300;
/// A grip takes a handle when the hand's fist is this close to where it closes on the grip.
constexpr float REACH_METERS = 0.20f;
/// Longer steps between two frames are taken as a pause.
constexpr float LONGEST_STEP_SECONDS = 0.1f;
constexpr DWORD LOG_MILLISECONDS = 1000;

struct HandleState
{
    bool isHeld;
    float grab[3]; // stick: its tilt (forward, side) when taken minus the hand's; throttle: the hand's place along
                   // the track minus the carriage's
};

HandleState _handles[HANDLE_COUNT] = {};
bool _wasGripped[2] = {};
/// Remote handles (idren2023 2026-10-27, the droid starfighter: no cockpit, the controls "not in real space"): a grip
/// takes its hand's handle wherever the hand is (right the stick, left the throttle), as though it had closed on it
/// there; the stick then tilts from that place (the hand's offset from the stick's grip when taken); X takes off and
/// lands (on release, under a second), held a second on the ground it leaves the ship (IsExitKeyDown); nothing is
/// drawn (ControllerHolo shows the controllers).
bool _isRemote = false;
float _remoteOffset[3] = {};
DWORD _xDownSince = 0;
volatile bool _isTakeOffTestPending = false; // RequestTakeOffTest
constexpr DWORD EXIT_HOLD_MILLISECONDS = 1000;
bool _isShown = false;
bool _isHelicopter = false;      // a hover craft's seat: helicopter controls
float _collective = 0;           // the elevation lever: -1 full sink .. 1 full climb
float _twist = 0;                // the stick's twist since taken (radians, right positive)
float _grabYaw = 0;              // the hand's heading when it took the stick
float _tiltForward = 0, _tiltSide = 0; // the stick's tilt (radians; ahead and right positive)
int _notch = 0;                         // throttle: -1 slow, 0 cruise, 1 fast
float _slideMeters = 0;                 // throttle carriage from the cruise notch, forward positive
float _leverRadians = LEVER_UP_RADIANS; // the lever's angle up from lying ahead
bool _isFlying = false;
DWORD _requestTick = 0;  // when the lever last asked for a take-off or landing (0: none pending)
bool _isRequestTakeOff = false;
DWORD _keyUntilTick = 0;      // Controls holds the Land / Takeoff key until then (0: not)
float _lastHull[3] = {};
bool _hasLastHull = false;
float _speed = 0;
DWORD _slowSinceTick = 0;
bool _isSecondaryFire = false;
DWORD _sinkTick = 0; // when the ship last sank faster than SINKING_METERS_PER_SECOND
bool _isLogOn = false;
DWORD _lastStepTick = 0, _lastLogTick = 0;

// Where a hand's fist is (the arms' palm in the hand pose; the left hand's is the right's mirror in x), resting-pose
// space.
void Fist(int hand, const float* pose, float* fist)
{
    fist[0] = pose[12];
    fist[1] = pose[13];
    fist[2] = pose[14];
    float palm[3];
    if (Aim::GetPalmInHand(palm))
    {
        palm[0] = hand == 0 ? palm[0] : -palm[0];
        for (int axis = 0; axis < 3; ++axis)
        {
            fist[axis] += palm[0] * pose[axis] + palm[1] * pose[4 + axis] + palm[2] * pose[8 + axis];
        }
    }
}

// A grip's axes (rows right, up along the grip, back; resting-pose space) for a grip leaning ahead by `forward` and to
// the right by `side` (radians).
void GripAxes(float forward, float side, float* right, float* up, float* back)
{
    up[0] = sinf(side) * cosf(forward);
    up[1] = cosf(side) * cosf(forward);
    up[2] = -sinf(forward);
    // back = x cross up, normalised; right = up cross back
    back[0] = 0;
    back[1] = -up[2];
    back[2] = up[1];
    const float length = sqrtf(back[1] * back[1] + back[2] * back[2]);
    back[1] /= length;
    back[2] /= length;
    right[0] = up[1] * back[2] - up[2] * back[1];
    right[1] = up[2] * back[0] - up[0] * back[2];
    right[2] = up[0] * back[1] - up[1] * back[0];
}

// A handle's pivot and its grip's lean (radians ahead, to the right), resting-pose space.
void Placement(int handle, float* pivot, float* forward, float* side)
{
    if (handle == STICK)
    {
        memcpy(pivot, STICK_PIVOT, 12);
        *forward = _tiltForward;
        *side = _tiltSide;
    }
    else if (handle == THROTTLE && _isHelicopter)
    {
        memcpy(pivot, COLLECTIVE_PIVOT, 12);
        *forward = COLLECTIVE_LEAN_RADIANS - _collective * COLLECTIVE_SWING_RADIANS; // raised: leans less
        *side = 0;
    }
    else if (handle == THROTTLE)
    {
        memcpy(pivot, THROTTLE_PIVOT, 12);
        *forward = THROTTLE_LEAN_RADIANS + _slideMeters / THROTTLE_GRIP_METERS;
        *side = 0;
    }
    else
    {
        // the lever lies ahead at angle 0 and rises toward the pilot's side: its grip leans ahead by 90 deg - angle
        memcpy(pivot, LEVER_PIVOT, 12);
        *forward = 90.0f * RADIANS_PER_DEGREE - _leverRadians;
        *side = 0;
    }
}

// Where a hand closes on a handle's grip, resting-pose space.
void GripPoint(int handle, float* point)
{
    float forward, side, right[3], up[3], back[3];
    Placement(handle, point, &forward, &side);
    GripAxes(forward, side, right, up, back);
    const float height = handle == STICK ? STICK_GRIP_METERS : handle == THROTTLE ? THROTTLE_GRIP_METERS : GRIP_HEIGHT_METERS;
    for (int axis = 0; axis < 3; ++axis)
    {
        point[axis] += up[axis] * height;
    }
}

// Where a hand is along the throttle's arc (metres from upright at the knob, ahead positive).
float ThrottleAlong(const float* fist)
{
    return THROTTLE_GRIP_METERS * atan2f(THROTTLE_PIVOT[2] - fist[2], fist[1] - THROTTLE_PIVOT[1]);
}

// The throttle's notch for a place along the track (metres from the cruise notch, forward positive).
int NotchFor(float along, int notch)
{
    for (int step = 0; step < 2; ++step)
    {
        const float up = (static_cast<float>(notch) + 0.5f) * NOTCH_SPACING_METERS + NOTCH_HYSTERESIS_METERS;
        const float down = (static_cast<float>(notch) - 0.5f) * NOTCH_SPACING_METERS - NOTCH_HYSTERESIS_METERS;
        if (notch < 1 && along > up)
        {
            ++notch;
        }
        else if (notch > -1 && along < down)
        {
            --notch;
        }
    }
    return notch;
}

// The stick's tilt (ahead, right; radians) that puts its long arm's top at a point (resting-pose space).
void TiltToward(const float* point, float* forward, float* side)
{
    const float dx = point[0] - STICK_PIVOT[0], dy = point[1] - STICK_PIVOT[1] + STICK_ARM_METERS - STICK_GRIP_METERS,
                dz = point[2] - STICK_PIVOT[2];
    *forward = atan2f(-dz, dy);
    *side = atan2f(dx, dy);
}

float Clamp(float value, float limit)
{
    return value > limit ? limit : value < -limit ? -limit : value;
}

// A hand pose's heading (its pointing line's, about the vertical; right positive), radians.
float HandHeading(const float* pose)
{
    return atan2f(-pose[8], pose[10]);
}

float Wrapped(float radians)
{
    while (radians > 3.14159265f)
    {
        radians -= 6.2831853f;
    }
    while (radians < -3.14159265f)
    {
        radians += 6.2831853f;
    }
    return radians;
}

// Takes and lets go: a handle is taken by a new grip press with the hand's fist within reach of its grip (the nearest
// of the hand's handles); held while the grip is.
void StepGrabs()
{
    for (int hand = 0; hand < 2; ++hand)
    {
        Gesture::Hand state = Gesture::CurrentHand(hand);
        if (Vehicle::IsSticksMode())
        {
            state.isGripped = false; // vehicle_controls sticks: the sticks drive, A takes off and lands
        }
        const bool hasGripStarted = state.isGripped && !_wasGripped[hand];
        _wasGripped[hand] = state.isGripped;
        float fist[3];
        Fist(hand, state.pose, fist);
        for (int handle = 0; handle < HANDLE_COUNT; ++handle)
        {
            if (HAND_OF[handle] == hand && _handles[handle].isHeld && (!state.isTracked || !state.isGripped))
            {
                _handles[handle].isHeld = false;
                Log::Info("flight", "handle %d let go", handle);
            }
        }
        if (!hasGripStarted || !state.isTracked)
        {
            continue;
        }
        if (_isRemote)
        {
            const int handle = hand == 0 ? STICK : THROTTLE;
            HandleState& taken = _handles[handle];
            taken.isHeld = true;
            if (handle == STICK)
            {
                float grip[3], forward, side;
                GripPoint(STICK, grip);
                for (int axis = 0; axis < 3; ++axis)
                {
                    _remoteOffset[axis] = fist[axis] - grip[axis];
                }
                TiltToward(grip, &forward, &side);
                taken.grab[0] = _tiltForward - forward;
                taken.grab[1] = _tiltSide - side;
            }
            else
            {
                taken.grab[0] = ThrottleAlong(fist) - _slideMeters;
            }
            Feedback::Pulse(hand, 0.02f, 0.4f);
            Log::Info("flight", "%s taken (remote)", handle == STICK ? "stick" : "throttle");
            continue;
        }
        int nearest = -1;
        float nearestDistance = REACH_METERS;
        for (int handle = 0; handle < HANDLE_COUNT; ++handle)
        {
            if (HAND_OF[handle] != hand)
            {
                continue;
            }
            float grip[3];
            GripPoint(handle, grip);
            const float dx = fist[0] - grip[0], dy = fist[1] - grip[1], dz = fist[2] - grip[2];
            const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
            if (distance < nearestDistance)
            {
                nearest = handle;
                nearestDistance = distance;
            }
        }
        if (nearest < 0)
        {
            continue;
        }
        HandleState& taken = _handles[nearest];
        taken.isHeld = true;
        if (nearest == STICK)
        {
            float forward, side;
            TiltToward(fist, &forward, &side);
            taken.grab[0] = _tiltForward - forward;
            taken.grab[1] = _tiltSide - side;
            _grabYaw = HandHeading(state.pose);
        }
        else if (nearest == THROTTLE && _isHelicopter)
        {
            taken.grab[0] = fist[1] - _collective * COLLECTIVE_FULL_METERS; // the hand's height at the lever's middle
        }
        else if (nearest == THROTTLE)
        {
            taken.grab[0] = ThrottleAlong(fist) - _slideMeters;
        }
        Feedback::Pulse(hand, 0.02f, 0.4f);
        Log::Info("flight", "%s taken", nearest == STICK ? "stick" : nearest == THROTTLE ? "throttle" : "lever");
    }
}

// The ship's state from the hull's speed: flying, or landed after a while slow; a lever request is trusted for a time.
// Whether the ship can stop in the air (gunship, MAF: MinSpeed -15, MidSpeed 0; run574: a hovering gunship stands
// still at height), so standing still does not mean landed.
bool IsHoverCraft()
{
    const char* name = Vehicle::CurrentStockName();
    return name && (!strcmp(name, "rep_fly_gunship") || !strcmp(name, "cis_fly_maf"));
}

void StepShipState(DWORD now, float stepSeconds)
{
    float hull[16];
    if (Vehicle::GetHullMatrix(hull) && stepSeconds > 0.0f)
    {
        if (_hasLastHull)
        {
            const float dx = hull[12] - _lastHull[0], dy = hull[13] - _lastHull[1], dz = hull[14] - _lastHull[2];
            const float speed = sqrtf(dx * dx + dy * dy + dz * dz) / stepSeconds;
            _speed += (speed - _speed) * (stepSeconds / (stepSeconds + 0.2f));
            if (dy / stepSeconds < -SINKING_METERS_PER_SECOND && GetClimb() >= 0.0f)
            {
                _sinkTick = now | 1; // a hover craft lands by sinking (the Land key: 5-6 m/s, run574), unless the
                                     // elevation lever sinks it (run581: read as a landing)
            }
        }
        memcpy(_lastHull, hull + 12, 12);
        _hasLastHull = true;
    }
    if (_speed > LANDED_METERS_PER_SECOND)
    {
        _slowSinceTick = 0;
    }
    else if (!_slowSinceTick)
    {
        _slowSinceTick = now | 1;
    }
    // (stamps are `now | 1`, so one may lie a tick ahead of now: compared signed, run461 asked three times at once)
    const bool isPending = _requestTick && static_cast<LONG>(now - _requestTick) < static_cast<LONG>(REQUEST_MILLISECONDS);
    if (!isPending)
    {
        _requestTick = 0;
        const bool wasFlying = _isFlying;
        if (_speed > FLYING_METERS_PER_SECOND)
        {
            _isFlying = true;
        }
        else if (_slowSinceTick && static_cast<LONG>(now - _slowSinceTick) > static_cast<LONG>(LANDED_MILLISECONDS) &&
                 (!IsHoverCraft() ||
                  (_sinkTick && static_cast<LONG>(_slowSinceTick - _sinkTick) < static_cast<LONG>(SINK_TO_STOP_MILLISECONDS))))
        {
            _isFlying = false;
        }
        if (wasFlying != _isFlying)
        {
            Log::Info("flight", "ship %s (%.1f m/s)", _isFlying ? "flying" : "landed", static_cast<double>(_speed));
        }
    }
}

// The stick: follows the held hand (its long arm's tilt from where it was taken), springs upright when let go.
void StepStick(float stepSeconds)
{
    if (_handles[STICK].isHeld)
    {
        float fist[3], forward, side;
        Fist(0, Gesture::CurrentHand(0).pose, fist);
        if (_isRemote)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                fist[axis] -= _remoteOffset[axis]; // the stick's grip where the hand took it
            }
        }
        TiltToward(fist, &forward, &side);
        _tiltForward = Clamp(forward + _handles[STICK].grab[0], STICK_MOST_RADIANS);
        _tiltSide = Clamp(side + _handles[STICK].grab[1], STICK_MOST_RADIANS);
        _twist = _isHelicopter ? Wrapped(HandHeading(Gesture::CurrentHand(0).pose) - _grabYaw) : 0.0f;
        static DWORD lastRumbleTick = 0;
        const DWORD now = GetTickCount();
        if (now - lastRumbleTick >= STICK_RUMBLE_MILLISECONDS)
        {
            lastRumbleTick = now;
            const float tilt = sqrtf(_tiltForward * _tiltForward + _tiltSide * _tiltSide);
            const float share = tilt <= STICK_DEAD_RADIANS ? 0.0f
                                : tilt >= STICK_MOST_RADIANS
                                    ? 1.0f
                                    : (tilt - STICK_DEAD_RADIANS) / (STICK_MOST_RADIANS - STICK_DEAD_RADIANS);
            if (share > 0.0f)
            {
                Feedback::Pulse(0, STICK_RUMBLE_MILLISECONDS * 0.001f,
                                STICK_RUMBLE_REST + (STICK_RUMBLE_FULL - STICK_RUMBLE_REST) * share * share);
            }
        }
        return;
    }
    const float settle = expf(-stepSeconds / 0.08f);
    _tiltForward *= settle;
    _tiltSide *= settle;
}

// The throttle: follows the held hand along its track between three notches; stays in its notch when let go.
void StepThrottle(float stepSeconds)
{
    if (_isHelicopter)
    {
        // The elevation lever: the hand's height from where it took the lever's middle; let go, back to the middle.
        if (_handles[THROTTLE].isHeld)
        {
            float fist[3];
            Fist(1, Gesture::CurrentHand(1).pose, fist);
            const float rise = fist[1] - _handles[THROTTLE].grab[0];
            const float size = fabsf(rise) <= COLLECTIVE_DEAD_METERS ? 0.0f
                               : (fabsf(rise) - COLLECTIVE_DEAD_METERS) / (COLLECTIVE_FULL_METERS - COLLECTIVE_DEAD_METERS);
            _collective = (size > 1.0f ? 1.0f : size) * (rise > 0 ? 1.0f : -1.0f);
            return;
        }
        const float step = 4.0f * stepSeconds;
        _collective = fabsf(_collective) <= step ? 0.0f : _collective - (_collective > 0 ? step : -step);
        return;
    }
    const float notchPlace = static_cast<float>(_notch) * NOTCH_SPACING_METERS;
    if (_handles[THROTTLE].isHeld)
    {
        float fist[3];
        Fist(1, Gesture::CurrentHand(1).pose, fist);
        const float along = ThrottleAlong(fist) - _handles[THROTTLE].grab[0];
        const int notch = NotchFor(along, _notch);
        if (notch != _notch)
        {
            _notch = notch;
            Feedback::Pulse(1, 0.015f, notch == 0 ? 0.45f : 0.3f);
            Log::Info("flight", "throttle %s", notch < 0 ? "slow" : notch > 0 ? "fast" : "cruise");
        }
        const float place = static_cast<float>(_notch) * NOTCH_SPACING_METERS;
        _slideMeters = place + Clamp(along - place, GIVE_METERS);
        return;
    }
    const float step = 0.5f * stepSeconds, offset = _slideMeters - notchPlace;
    _slideMeters = fabsf(offset) <= step ? notchPlace : _slideMeters - (offset > 0 ? step : -step);
}

// The take-off lever: follows the held hand's height about its hinge; past the take-off or landing angle it asks for
// that once; let go, it swings to the ship's state.
void StepLever(DWORD now, float stepSeconds)
{
    const bool isPending = _requestTick != 0;
    if (_isTakeOffTestPending && !isPending && !_isRemote)
    {
        // script `flight takeoff` in any flyer (the vehicle proposal's gunship study): as the lever pushed down
        _isTakeOffTestPending = false;
        if (!_isFlying)
        {
            _requestTick = now | 1;
            _isRequestTakeOff = true;
            _isFlying = true;
            _keyUntilTick = (now + KEY_HOLD_MILLISECONDS) | 1;
            Log::Info("flight", "take-off test: take off");
        }
        else
        {
            Log::Info("flight", "take-off test: already flying");
        }
    }
    if (_isRemote)
    {
        // X instead of the lever: released within a second, a take-off or a landing (as the lever's push / pull)
        Xr::ControllerInput pad{};
        Xr::GetControllerInput(&pad);
        // X; in vehicle_controls sticks A (idren2023 2026-11-04: the jump button takes off and lands)
        const bool isDown = pad.isValid && pad.isLowerPressed[Vehicle::IsSticksMode() ? 0 : 1];
        if (isDown && !_xDownSince)
        {
            _xDownSince = now | 1;
        }
        else if (!isDown && _xDownSince)
        {
            const bool isShort = now - _xDownSince < EXIT_HOLD_MILLISECONDS;
            _xDownSince = 0;
            if (isShort && !isPending)
            {
                const bool wantsTakeOff = !_isFlying;
                _requestTick = now | 1;
                _isRequestTakeOff = wantsTakeOff;
                _isFlying = wantsTakeOff;
                _keyUntilTick = (now + KEY_HOLD_MILLISECONDS) | 1;
                Feedback::Pulse(1, 0.04f, 0.6f);
                Log::Info("flight", "X: %s", wantsTakeOff ? "take off" : "land");
            }
        }
        if (_isTakeOffTestPending && !isPending)
        {
            _isTakeOffTestPending = false;
            if (_isFlying)
            {
                Log::Info("flight", "take-off test: already flying");
            }
            else
            {
                _requestTick = now | 1;
                _isRequestTakeOff = true;
                _isFlying = true;
                _keyUntilTick = (now + KEY_HOLD_MILLISECONDS) | 1;
                Log::Info("flight", "take-off test: take off");
            }
        }
        _leverRadians = _isFlying ? LEVER_DOWN_RADIANS : LEVER_UP_RADIANS;
        return;
    }
    if (_handles[LEVER].isHeld)
    {
        float fist[3];
        Fist(1, Gesture::CurrentHand(1).pose, fist);
        float angle = atan2f(fist[1] - LEVER_PIVOT[1], -(fist[2] - LEVER_PIVOT[2]));
        angle = angle < LEVER_DOWN_RADIANS ? LEVER_DOWN_RADIANS : angle > LEVER_UP_RADIANS ? LEVER_UP_RADIANS : angle;
        _leverRadians = angle;
        const bool wantsTakeOff = angle < LEVER_TAKE_OFF_RADIANS && !_isFlying;
        const bool wantsLanding = angle > LEVER_LAND_RADIANS && _isFlying;
        if (!isPending && (wantsTakeOff || wantsLanding))
        {
            _requestTick = now | 1;
            _isRequestTakeOff = wantsTakeOff;
            _isFlying = wantsTakeOff; // trusted until the speed shows it (StepShipState)
            _keyUntilTick = (now + KEY_HOLD_MILLISECONDS) | 1;
            Feedback::Pulse(1, 0.04f, 0.6f);
            Log::Info("flight", "lever: %s", wantsTakeOff ? "take off" : "land");
        }
        return;
    }
    const float target = _isFlying ? LEVER_DOWN_RADIANS : LEVER_UP_RADIANS;
    const float step = LEVER_SETTLE_RADIANS_PER_SECOND * stepSeconds, offset = _leverRadians - target;
    _leverRadians = fabsf(offset) <= step ? target : _leverRadians - (offset > 0 ? step : -step);
}

// The stick's tilt as a look input: dead zone, curve, full input at the largest tilt.
float StickInput(float tilt)
{
    const float size = fabsf(tilt);
    if (size <= STICK_DEAD_RADIANS)
    {
        return 0.0f;
    }
    const float share = (size - STICK_DEAD_RADIANS) / (STICK_MOST_RADIANS - STICK_DEAD_RADIANS);
    const float input = powf(share > 1.0f ? 1.0f : share, STICK_CURVE);
    return tilt > 0 ? input : -input;
}

// A 4x4 row-vector frame from three axes and a position.
void MakeFrame(const float* right, const float* up, const float* back, const float* position, float* frame)
{
    const float values[16] = {
        right[0],
        right[1],
        right[2],
        0,
        up[0],
        up[1],
        up[2],
        0,
        back[0],
        back[1],
        back[2],
        0,
        position[0],
        position[1],
        position[2],
        1};
    memcpy(frame, values, sizeof(values));
}

// out = first x second (4x4, row vectors).
void Multiply(float* out, const float* first, const float* second)
{
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            float sum = 0;
            for (int inner = 0; inner < 4; ++inner)
            {
                sum += first[row * 4 + inner] * second[inner * 4 + column];
            }
            out[row * 4 + column] = sum;
        }
    }
}

} // namespace

void OnPresent()
{
    const bool isShown = Xr::IsRunning() && Vehicle::CurrentSeat() == Vehicle::Seat::Flyer &&
                         Vehicle::CurrentDrive() == Vehicle::Drive::FlightStick;
    const DWORD now = GetTickCount();
    float stepSeconds = _lastStepTick ? static_cast<float>(now - _lastStepTick) * 0.001f : 0.0f;
    _lastStepTick = now;
    if (stepSeconds > LONGEST_STEP_SECONDS)
    {
        stepSeconds = LONGEST_STEP_SECONDS;
    }
    const char* stock = Vehicle::CurrentStockName();
    _isRemote = isShown && ((stock && !strcmp(stock, "cis_fly_droidfighter")) || Vehicle::IsSticksMode());
    _isHelicopter = isShown && IsHoverCraft();
    if (isShown != _isShown)
    {
        // Entering or leaving the seat: the stick upright, the throttle at cruise, the lever as the ship is (a ship is
        // entered landed; the speed corrects it within a second).
        _isShown = isShown;
        for (HandleState& handle : _handles)
        {
            handle = {};
        }
        _tiltForward = _tiltSide = 0;
        _collective = _twist = 0;
        _notch = 0;
        _slideMeters = 0;
        _isFlying = false;
        _leverRadians = LEVER_UP_RADIANS;
        _requestTick = 0;
        _keyUntilTick = 0;
        _hasLastHull = false;
        _speed = 0;
        _slowSinceTick = 0;
        Log::Info("flight", "%s", isShown ? "shown (flyer pilot seat)" : "hidden");
    }
    if (!isShown)
    {
        _isSecondaryFire = false;
        return;
    }
    FpGuard fp;
    StepGrabs();
    StepShipState(now, stepSeconds);
    StepStick(stepSeconds);
    StepThrottle(stepSeconds);
    StepLever(now, stepSeconds);
    Xr::ControllerInput pad{};
    Xr::GetControllerInput(&pad);
    _isSecondaryFire = pad.isValid && _handles[STICK].isHeld && pad.isLowerPressed[0];
    if (_isLogOn && now - _lastLogTick >= LOG_MILLISECONDS)
    {
        _lastLogTick = now;
        float yaw, pitch;
        GetStickLook(&yaw, &pitch);
        Log::Info(
            "flight",
            "stick%s tilt %.1f ahead %.1f right deg -> yaw %.2f pitch %.2f; throttle%s notch %d at %.0f cm; lever%s "
            "%.0f deg; ship %s at %.1f m/s%s",
            _handles[STICK].isHeld ? " held" : "",
            static_cast<double>(_tiltForward / RADIANS_PER_DEGREE),
            static_cast<double>(_tiltSide / RADIANS_PER_DEGREE),
            static_cast<double>(yaw),
            static_cast<double>(pitch),
            _handles[THROTTLE].isHeld ? " held" : "",
            _notch,
            static_cast<double>(_slideMeters * 100),
            _handles[LEVER].isHeld ? " held" : "",
            static_cast<double>(_leverRadians / RADIANS_PER_DEGREE),
            _isFlying ? "flying" : "landed",
            static_cast<double>(_speed),
            _requestTick ? (_isRequestTakeOff ? ", taking off" : ", landing") : "");
    }
}

bool GetDrive(float* move, float* yaw, float* pitch, float* strafe)
{
    *move = *yaw = *pitch = *strafe = 0.0f;
    if (!_isShown)
    {
        return false;
    }
    if (_isHelicopter)
    {
        // the stick flies the ship: ahead / back along the move axis (centre = hovering), aside strafes (run574: the
        // gunship's move -1..1 is 15 m/s back .. 45 ahead, strafe 15 m/s); its twist goes as the mouse (GetStickLook)
        *move = StickInput(_tiltForward);
        *strafe = StickInput(_tiltSide);
        return true;
    }
    *move = static_cast<float>(_notch);
    return true; // the stick goes to the game as mouse counts (GetStickLook, Controls; headset run 18)
}

bool IsHelicopter()
{
    return _isHelicopter;
}

bool GetStickLook(float* yaw, float* pitch)
{
    *yaw = *pitch = 0.0f;
    if (!_isShown)
    {
        return false;
    }
    if (_isHelicopter)
    {
        // the stick's twist turns the ship; the nose stays level (no pitch: the stick's fore / aft flies, GetDrive);
        // without the stick in hand the right thumbstick's X turns (the sticks scheme)
        const float size = fabsf(_twist);
        float turn = size <= TWIST_DEAD_RADIANS ? 0.0f : (size - TWIST_DEAD_RADIANS) / (TWIST_FULL_RADIANS - TWIST_DEAD_RADIANS);
        turn = (turn > 1.0f ? 1.0f : turn) * (_twist > 0 ? 1.0f : -1.0f);
        if (!_handles[STICK].isHeld)
        {
            Xr::ControllerInput pad{};
            Xr::GetControllerInput(&pad);
            turn = pad.isValid && fabsf(pad.stickX[0]) > 0.15f ? pad.stickX[0] : 0.0f;
        }
        *yaw = turn;
        // headset run 19 (idren2023: no pitch, enemies could not be aimed at): with the stick in hand the right
        // thumbstick's Y pitches the nose (pushed ahead = nose down, as a stick); without it it climbs (GetClimb)
        if (_handles[STICK].isHeld)
        {
            Xr::ControllerInput pad{};
            Xr::GetControllerInput(&pad);
            *pitch = pad.isValid && fabsf(pad.stickY[0]) > 0.15f ? -pad.stickY[0] : 0.0f;
        }
        return true;
    }
    *yaw = StickInput(_tiltSide);
    *pitch = -StickInput(_tiltForward); // pulled back (tilted toward the pilot) = nose up
    // headset run 19: the right thumbstick's Y pitches too (pitch and yaw on the right thumbstick, its X the A / D
    // sharp turns); pushed ahead = nose down
    Xr::ControllerInput pad{};
    Xr::GetControllerInput(&pad);
    if (pad.isValid && fabsf(pad.stickY[0]) > 0.15f && fabsf(*pitch) < fabsf(pad.stickY[0]))
    {
        *pitch = -pad.stickY[0];
    }
    return true;
}

bool IsHolding(int hand)
{
    if (!_isShown)
    {
        return false;
    }
    for (int handle = 0; handle < HANDLE_COUNT; ++handle)
    {
        if (HAND_OF[handle] == (hand & 1) && _handles[handle].isHeld)
        {
            return true;
        }
    }
    return false;
}

bool IsSecondaryFire()
{
    return _isShown && _isSecondaryFire;
}

bool IsStickHeld()
{
    return _isShown && _handles[STICK].isHeld;
}

volatile float _climbTest = 0;

float GetClimb()
{
    if (!_isShown)
    {
        return 0.0f;
    }
    if (_climbTest != 0.0f || !_isHelicopter)
    {
        return _climbTest;
    }
    // the elevation lever; without it in hand the right thumbstick's Y (the sticks scheme)
    float climb = _collective;
    if (!_handles[THROTTLE].isHeld && !_handles[STICK].isHeld) // (with the stick in hand the thumbstick pitches)
    {
        Xr::ControllerInput pad{};
        Xr::GetControllerInput(&pad);
        climb = pad.isValid && fabsf(pad.stickY[0]) > 0.15f ? pad.stickY[0] : 0.0f;
    }
    return _isFlying ? climb * MOST_CLIMB_METERS_PER_SECOND : 0.0f; // (on the ground the take-off lever lifts it)
}

void SetClimbTest(float metersPerSecond)
{
    _climbTest = metersPerSecond;
    Log::Info("flight", "climb test %.1f m/s", static_cast<double>(metersPerSecond));
}

void RequestTakeOffTest()
{
    _isTakeOffTestPending = true;
}

bool IsRemote()
{
    return _isShown && _isRemote;
}

bool IsExitKeyDown()
{
    return _isShown && _isRemote && !_isFlying && _xDownSince &&
           GetTickCount() - _xDownSince >= EXIT_HOLD_MILLISECONDS;
}

bool IsLandingKeyDown()
{
    return _isShown && _keyUntilTick && static_cast<LONG>(_keyUntilTick - GetTickCount()) > 0;
}

bool GetHandPose(int hand, float* restPose)
{
    const Gesture::Hand& state = Gesture::CurrentHand(hand & 1);
    if (!_isShown || _isRemote || !state.isTracked) // remote: no hands drawn (the holograms show the controllers)
    {
        return false;
    }
    memcpy(restPose, state.pose, 64);
    int held = -1;
    for (int handle = 0; handle < HANDLE_COUNT; ++handle)
    {
        if (HAND_OF[handle] == (hand & 1) && _handles[handle].isHeld)
        {
            held = handle;
        }
    }
    if (held < 0)
    {
        return true;
    }
    // Round the grip as on a pistol grip (as Levers::GetHandPose): the arms' fist on the grip's axis.
    FpGuard fp;
    float pivot[3], forward, side, right[3], up[3], back[3], grip[3];
    Placement(held, pivot, &forward, &side);
    GripAxes(forward, side, right, up, back);
    GripPoint(held, grip);
    MakeFrame(right, up, back, grip, restPose);
    float palm[3];
    if (Aim::GetPalmInHand(palm))
    {
        palm[0] = (hand & 1) ? -palm[0] : palm[0];
        for (int axis = 0; axis < 3; ++axis)
        {
            restPose[12 + axis] -= palm[0] * right[axis] + palm[1] * up[axis] + palm[2] * back[axis];
        }
    }
    return true;
}

void Draw(void* device)
{
    float base[16];
    if (!_isShown || _isRemote || !device || Stereo::IsScopePass() || !Stereo::GetBaseCamera(base))
    {
        return;
    }
    CockpitShell::GetPilotFrame(base); // the cockpit's own frame where a shell was drawn, else the hull's (Stereo)
    float carriages[HANDLE_COUNT][16], grips[HANDLE_COUNT][16];
    {
        FpGuard fp;
        const float hullRight[3] = {1, 0, 0}, hullUp[3] = {0, 1, 0}, hullBack[3] = {0, 0, 1};
        for (int handle = 0; handle < HANDLE_COUNT; ++handle)
        {
            float pivot[3], forward, side, right[3], up[3], back[3], carriage[16], grip[16];
            Placement(handle, pivot, &forward, &side);
            GripAxes(forward, side, right, up, back);
            MakeFrame(hullRight, hullUp, hullBack, pivot, carriage);
            MakeFrame(right, up, back, pivot, grip);
            Multiply(carriages[handle], carriage, base);
            Multiply(grips[handle], grip, base);
        }
    }
    Xr::ControllerInput pad{};
    Xr::GetControllerInput(&pad);
    const float pulls[HANDLE_COUNT] = {pad.isValid && _handles[STICK].isHeld ? pad.trigger[0] : 0.0f, 0.0f, 0.0f};
    const LeverModel::Model models[HANDLE_COUNT] = {
        LeverModel::Model::JetStick, LeverModel::Model::Throttle, LeverModel::Model::Tank};
    LeverModel::Draw(device, carriages, grips, pulls, HANDLE_COUNT, Vehicle::CurrentStockName(), models);
}

void SetLog(bool isLogOn)
{
    _isLogOn = isLogOn;
}

} // namespace Flight

} // namespace SwbfVr
