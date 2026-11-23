// WalkerDrive.cpp: the throttle lever in a walker's pilot seat (see WalkerDrive.h).
//
// The vehicle proposal (docs/research/vehicle-controls-proposal.md, idren2023 2026-11-04): every walker (AT-ST, AT-AT,
// AT-TE, spider walker) is driven by one lever in the right hand with three notches, reverse / neutral / forward, that
// stays in its notch when let go (the fighters' throttle, Flight.cpp); the walker turns under the view toward where
// the head looks (Vehicle's yaw loop: a walker's look yaw turns its hull, run570), as the tanks' drive stick does.
// Walkers take the move input on Controllable +0x54 (run570: 1 walks the AT-ST at 5-7 m/s, -1 reverses); their
// strafe input turns them instead (A / D), so the lever drives only the move input.
//
// Resting-pose space throughout (x right, y up, z back, metres, origin at the resting head), as Levers and Flight.

// Makes d3d9.h declare the Direct3D interfaces as C structs with an explicit vtable (device->lpVtbl->Method(device,
// ...)), the form the rest of the mod uses. Has to be a macro: d3d9.h reads it.
#define CINTERFACE

#include "WalkerDrive.h"

#include "Aim.h"
#include "CockpitShell.h"
#include "Feedback.h"
#include "FpGuard.h"
#include "Gesture.h"
#include "LeverModel.h"
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

namespace WalkerDrive
{

namespace
{

/// The lever's hinge: right of the seat, under and ahead of the eyes (the fighters' throttle mirrored to the right
/// hand; the walkers' cockpits put their right console here, ShellDesigns WALKER_THROTTLE_PIVOT).
constexpr float PIVOT[3] = {0.25f, -0.62f, -0.30f};
/// From the hinge up the grip to where the hand closes (the fighters' boat-style throttle, LeverModel::Throttle).
constexpr float GRIP_METERS = LeverModel::THROTTLE_GRIP_METERS;
/// Notches 8 cm apart along the grip's arc (reverse, neutral, forward), past half way + 1.5 cm to change, 1.2 cm of
/// give either side of a notch while held (Flight's throttle).
constexpr float NOTCH_SPACING_METERS = 0.08f, NOTCH_HYSTERESIS_METERS = 0.015f, GIVE_METERS = 0.012f;
/// Let go, the lever settles into its notch at this speed (metres along the arc per second).
constexpr float SETTLE_METERS_PER_SECOND = 0.5f;
/// A grip takes the lever when the fist is this close to its grip.
constexpr float REACH_METERS = 0.20f;
constexpr float LONGEST_STEP_SECONDS = 0.1f;
/// Head steering: no turn within 3 degrees of the head's heading, full turn input 20 degrees off.
constexpr float TURN_DEAD_RADIANS = 3.0f * 3.14159265f / 180.0f, TURN_FULL_RADIANS = 20.0f * 3.14159265f / 180.0f;
constexpr DWORD LOG_MILLISECONDS = 1000;
constexpr int HAND = 0; // the right hand

bool _isShown = false;
bool _isWalker = false; // a walker (turns on its strafe input); else a hover with the lever (the hailfire droid)
bool _isHeld = false;
bool _wasGripped = false;
int _notch = 0;           // -1 reverse, 0 neutral, 1 forward
float _slideMeters = 0;   // the grip's place along its arc from the neutral notch, ahead positive
float _grab = 0;          // the hand's place along the arc minus the grip's when taken
float _headOffset = 0;    // head steering's heading target offset after the right stick turned the hull (radians)
bool _isLogOn = false;
DWORD _lastStepTick = 0, _lastLogTick = 0;

// The grip's axes (rows right, up along the grip, back) leaning ahead by `forward` radians.
void GripAxes(float forward, float* right, float* up, float* back)
{
    up[0] = 0;
    up[1] = cosf(forward);
    up[2] = -sinf(forward);
    back[0] = 0;
    back[1] = sinf(forward);
    back[2] = cosf(forward);
    right[0] = 1;
    right[1] = 0;
    right[2] = 0;
}

float Lean()
{
    return _slideMeters / GRIP_METERS;
}

void GripPoint(float* point)
{
    float right[3], up[3], back[3];
    GripAxes(Lean(), right, up, back);
    for (int axis = 0; axis < 3; ++axis)
    {
        point[axis] = PIVOT[axis] + up[axis] * GRIP_METERS;
    }
}

// Where the right hand's fist is (the arms' palm in the hand pose), resting-pose space.
void Fist(const float* pose, float* fist)
{
    fist[0] = pose[12];
    fist[1] = pose[13];
    fist[2] = pose[14];
    float palm[3];
    if (Aim::GetPalmInHand(palm))
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            fist[axis] += palm[0] * pose[axis] + palm[1] * pose[4 + axis] + palm[2] * pose[8 + axis];
        }
    }
}

// Where a fist is along the grip's arc (metres from upright at the grip, ahead positive).
float Along(const float* fist)
{
    return GRIP_METERS * atan2f(PIVOT[2] - fist[2], fist[1] - PIVOT[1]);
}

// The notch for a place along the arc (from the neutral notch, ahead positive).
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

float Clamp(float value, float limit)
{
    return value > limit ? limit : value < -limit ? -limit : value;
}

void StepLever(float stepSeconds)
{
    Gesture::Hand state = Gesture::CurrentHand(HAND);
    if (Vehicle::IsSticksMode())
    {
        state.isGripped = false; // vehicle_controls sticks: the handles stay where they are, the sticks drive
    }
    float fist[3];
    Fist(state.pose, fist);
    const bool hasGripStarted = state.isGripped && !_wasGripped;
    _wasGripped = state.isGripped;
    if (!_isHeld && state.isTracked && hasGripStarted)
    {
        float grip[3];
        GripPoint(grip);
        const float dx = fist[0] - grip[0], dy = fist[1] - grip[1], dz = fist[2] - grip[2];
        if (sqrtf(dx * dx + dy * dy + dz * dz) < REACH_METERS)
        {
            _isHeld = true;
            _grab = Along(fist) - _slideMeters;
            Feedback::Pulse(HAND, 0.02f, 0.4f);
            Log::Info("walker", "throttle taken");
        }
    }
    else if (_isHeld && (!state.isTracked || !state.isGripped))
    {
        _isHeld = false;
        Log::Info("walker", "throttle let go in %s", _notch < 0 ? "reverse" : _notch > 0 ? "forward" : "neutral");
    }
    const float notchPlace = static_cast<float>(_notch) * NOTCH_SPACING_METERS;
    if (_isHeld)
    {
        const float along = Along(fist) - _grab;
        const int notch = NotchFor(along, _notch);
        if (notch != _notch)
        {
            _notch = notch;
            Feedback::Pulse(HAND, 0.015f, notch == 0 ? 0.45f : 0.3f);
            Log::Info("walker", "throttle %s", notch < 0 ? "reverse" : notch > 0 ? "forward" : "neutral");
        }
        const float place = static_cast<float>(_notch) * NOTCH_SPACING_METERS;
        _slideMeters = place + Clamp(along - place, GIVE_METERS);
        return;
    }
    const float step = SETTLE_METERS_PER_SECOND * stepSeconds, offset = _slideMeters - notchPlace;
    _slideMeters = fabsf(offset) <= step ? notchPlace : _slideMeters - (offset > 0 ? step : -step);
}

void MakeFrame(const float* right, const float* up, const float* back, const float* position, float* frame)
{
    const float values[16] = {
        right[0], right[1], right[2], 0, up[0], up[1], up[2], 0, back[0], back[1], back[2], 0,
        position[0], position[1], position[2], 1};
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
    const Vehicle::Seat seat = Vehicle::CurrentSeat();
    const bool isShown = Xr::IsRunning() && (seat == Vehicle::Seat::Walker || seat == Vehicle::Seat::Hover) &&
                         Vehicle::CurrentDrive() == Vehicle::Drive::Throttle;
    _isWalker = seat == Vehicle::Seat::Walker;
    const DWORD now = GetTickCount();
    float stepSeconds = _lastStepTick ? static_cast<float>(now - _lastStepTick) * 0.001f : 0.0f;
    _lastStepTick = now;
    if (stepSeconds > LONGEST_STEP_SECONDS)
    {
        stepSeconds = LONGEST_STEP_SECONDS;
    }
    if (isShown != _isShown)
    {
        // Entering or leaving the seat: the lever in neutral (a walker entered stands).
        _isShown = isShown;
        _isHeld = false;
        _notch = 0;
        _slideMeters = 0;
        Log::Info("walker", "%s", isShown ? "throttle shown (walker pilot: throttle lever, the head steers)" : "hidden");
    }
    if (!isShown)
    {
        return;
    }
    FpGuard fp;
    StepLever(stepSeconds);
    if (_isLogOn && now - _lastLogTick >= LOG_MILLISECONDS)
    {
        _lastLogTick = now;
        Log::Info(
            "walker",
            "throttle%s notch %d at %.1f cm",
            _isHeld ? " held" : "",
            _notch,
            static_cast<double>(_slideMeters * 100.0f));
    }
}

bool GetDrive(float* move, float* turn, bool isStickTurning)
{
    *move = *turn = 0.0f;
    if (!_isShown)
    {
        _headOffset = 0;
        return false;
    }
    *move = static_cast<float>(_notch);
    // Head steering: the hull turned toward the head's heading in proportion past a dead zone (the guns follow the
    // head within their limits by themselves, Vehicle's aim loop).
    // (A hover's look yaw turns its hull toward the head already, Vehicle's yaw loop; its strafe input strafes.)
    float head[16], hullX = 0, hullZ = 0;
    if (_isWalker && Stereo::GetLastHeadWorld(head) && Vehicle::GetHullForward(&hullX, &hullZ))
    {
        FpGuard fp;
        const float headHeading = atan2f(-head[8], head[10]); // forward = -z row: heading atan2(x, -z)
        const float hullHeading = atan2f(hullX, -hullZ);
        float delta = headHeading - hullHeading;
        while (delta > 3.14159265f)
        {
            delta -= 6.2831853f;
        }
        while (delta < -3.14159265f)
        {
            delta += 6.2831853f;
        }
        // The right stick overrides (vehicle_controls sticks, idren2023 2026-11-04): while it turns the hull the head's
        // heading target follows the hull, so letting go does not swing the hull back; the head steers again from
        // there.
        if (isStickTurning)
        {
            _headOffset = delta;
            return true;
        }
        delta -= _headOffset;
        const float size = fabsf(delta);
        if (size > TURN_DEAD_RADIANS)
        {
            const float share = (size - TURN_DEAD_RADIANS) / (TURN_FULL_RADIANS - TURN_DEAD_RADIANS);
            *turn = (share > 1.0f ? 1.0f : share) * (delta > 0 ? 1.0f : -1.0f);
        }
    }
    return true;
}

bool IsShown()
{
    return _isShown;
}

bool GetHandPose(int hand, float* restPose)
{
    const Gesture::Hand& state = Gesture::CurrentHand(hand & 1);
    if (!_isShown || !state.isTracked || (hand & 1) != HAND || !_isHeld)
    {
        return false;
    }
    FpGuard fp;
    float right[3], up[3], back[3], grip[3];
    GripAxes(Lean(), right, up, back);
    GripPoint(grip);
    MakeFrame(right, up, back, grip, restPose);
    float palm[3];
    if (Aim::GetPalmInHand(palm))
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            restPose[12 + axis] -= palm[0] * right[axis] + palm[1] * up[axis] + palm[2] * back[axis];
        }
    }
    return true;
}

bool IsHolding(int hand)
{
    return _isShown && (hand & 1) == HAND && _isHeld;
}

void Draw(void* device)
{
    float base[16];
    if (!_isShown || !device || Stereo::IsScopePass() || !Stereo::GetBaseCamera(base))
    {
        return;
    }
    CockpitShell::GetPilotFrame(base); // the cockpit's own frame where a shell was drawn, else the hull's
    float carriages[1][16], grips[1][16];
    {
        FpGuard fp;
        float right[3], up[3], back[3], carriage[16], grip[16];
        GripAxes(Lean(), right, up, back);
        const float hullRight[3] = {1, 0, 0}, hullUp[3] = {0, 1, 0}, hullBack[3] = {0, 0, 1};
        MakeFrame(hullRight, hullUp, hullBack, PIVOT, carriage);
        MakeFrame(right, up, back, PIVOT, grip);
        Multiply(carriages[0], carriage, base);
        Multiply(grips[0], grip, base);
    }
    const float pulls[1] = {0.0f};
    const LeverModel::Model models[1] = {LeverModel::Model::Throttle};
    LeverModel::Draw(device, carriages, grips, pulls, 1, Vehicle::CurrentStockName(), models);
}

void SetLog(bool isLogOn)
{
    _isLogOn = isLogOn;
}

} // namespace WalkerDrive

} // namespace SwbfVr
