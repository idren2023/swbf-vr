// Levers.cpp: the drive stick in a tank's pilot seat (PLAN 54 a2, PLAN 57 d, PLAN 59 a; see Levers.h).
//
// Everything is worked out in resting-pose space (x right, y up, z back, metres, origin at the resting head), the
// space the controller poses come in (Gesture::Hand::pose). The stick stands where the right slide handle stood
// (LeverPlacement; headset run 18: that place was right). It is taken with the right grip when the fist is within
// reach of its grip; then it tilts with the hand's motion about its pivot, from where it was taken: tilted ahead the
// tank drives forward, back it reverses, to a side it strafes, each in proportion past a dead zone. Let go, it springs
// upright. Turning and the gun's elevation follow the head (Vehicle: the view keeps its own heading and the hull
// turns under it toward where the head looks, as in the fighter tank's gunner station; Controls).
//
// Drawn by LeverModel inside the first-person pass, after the cockpit model and its shell, so nothing of the cockpit
// covers it.

// Makes d3d9.h declare the Direct3D interfaces as C structs with an explicit vtable (device->lpVtbl->Method(device,
// ...)), the form the rest of the mod uses. Has to be a macro: d3d9.h reads it.
#define CINTERFACE

#include "Levers.h"

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

namespace Levers
{

namespace
{

/// Where the stick stands: LeverPlacement.h's right handle (shared with the cockpit shells' front console).
using LeverPlacement::GRIP_HEIGHT_METERS;
using LeverPlacement::HANDLE_SIDE_METERS;
using LeverPlacement::PIVOT_AHEAD_METERS;
using LeverPlacement::PIVOT_DOWN_METERS;
constexpr float RADIANS_PER_DEGREE = 3.14159265f / 180.0f;
/// The stick's pivot (resting-pose space).
constexpr float PIVOT[3] = {HANDLE_SIDE_METERS, -PIVOT_DOWN_METERS, -PIVOT_AHEAD_METERS};
/// Full input at this tilt, nothing within the dead zone, a curve between them.
constexpr float MOST_RADIANS = 25.0f * RADIANS_PER_DEGREE, DEAD_RADIANS = 3.0f * RADIANS_PER_DEGREE;
constexpr float CURVE = 1.3f;
/// Let go, the stick springs upright with this time constant.
constexpr float SETTLE_SECONDS = 0.08f;
/// A grip takes the stick when the hand's fist is this close to where it closes on the grip.
constexpr float REACH_METERS = 0.20f;
/// Longer steps between two frames are taken as a pause.
constexpr float LONGEST_STEP_SECONDS = 0.1f;
/// The log: at most this often.
constexpr DWORD LOG_MILLISECONDS = 1000;

bool _isShown = false; // in a tank's pilot seat with VR running, this frame
bool _isHeld = false;
bool _wasGripped = false;
float _tiltForward = 0, _tiltSide = 0; // the stick's lean ahead and to the right (radians)
float _grab[2] = {};                   // the tilt when taken minus the hand's
bool _isLogOn = false;
DWORD _lastStepTick = 0, _lastLogTick = 0;

// The grip's axes (rows right, up along the grip, back; resting-pose space) leaning ahead by `forward` and to the
// right by `side` (radians).
void GripAxes(float forward, float side, float* right, float* up, float* back)
{
    up[0] = sinf(side) * cosf(forward);
    up[1] = cosf(side) * cosf(forward);
    up[2] = -sinf(forward);
    back[0] = 0; // x cross up, normalised; right = up cross back
    back[1] = -up[2];
    back[2] = up[1];
    const float length = sqrtf(back[1] * back[1] + back[2] * back[2]);
    back[1] /= length;
    back[2] /= length;
    right[0] = up[1] * back[2] - up[2] * back[1];
    right[1] = up[2] * back[0] - up[0] * back[2];
    right[2] = up[0] * back[1] - up[1] * back[0];
}

// Where the hand closes on the grip, resting-pose space.
void GripPoint(float* point)
{
    float right[3], up[3], back[3];
    GripAxes(_tiltForward, _tiltSide, right, up, back);
    for (int axis = 0; axis < 3; ++axis)
    {
        point[axis] = PIVOT[axis] + up[axis] * GRIP_HEIGHT_METERS;
    }
}

// Where the right hand's fist is (the arms' palm in the hand pose, Aim::GetPalmInHand), resting-pose space.
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

// The tilt that puts the grip toward a point (radians ahead, to the right).
void TiltToward(const float* point, float* forward, float* side)
{
    const float dx = point[0] - PIVOT[0], dy = point[1] - PIVOT[1], dz = point[2] - PIVOT[2];
    *forward = atan2f(-dz, dy);
    *side = atan2f(dx, dy);
}

float Clamp(float value, float limit)
{
    return value > limit ? limit : value < -limit ? -limit : value;
}

// A tilt as an input: dead zone, curve, full at the largest tilt.
float Input(float tilt)
{
    const float size = fabsf(tilt);
    if (size <= DEAD_RADIANS)
    {
        return 0.0f;
    }
    const float share = (size - DEAD_RADIANS) / (MOST_RADIANS - DEAD_RADIANS);
    const float input = powf(share > 1.0f ? 1.0f : share, CURVE);
    return tilt > 0 ? input : -input;
}

// Takes, holds and follows, or lets go and settles upright.
void StepStick(float stepSeconds)
{
    Gesture::Hand state = Gesture::CurrentHand(0);
    if (Vehicle::IsSticksMode())
    {
        state.isGripped = false; // vehicle_controls sticks: the handles stay where they are, the sticks drive
    }
    float fist[3];
    Fist(state.pose, fist);
    const bool hasGripStarted = state.isGripped && !_wasGripped;
    _wasGripped = state.isGripped;
    float forward = 0, side = 0;
    TiltToward(fist, &forward, &side);
    if (!_isHeld && state.isTracked && hasGripStarted)
    {
        float grip[3];
        GripPoint(grip);
        const float dx = fist[0] - grip[0], dy = fist[1] - grip[1], dz = fist[2] - grip[2];
        if (sqrtf(dx * dx + dy * dy + dz * dz) < REACH_METERS)
        {
            _isHeld = true;
            _grab[0] = _tiltForward - forward;
            _grab[1] = _tiltSide - side;
            Feedback::Pulse(0, 0.02f, 0.4f);
            Log::Info("levers", "stick taken");
        }
    }
    else if (_isHeld && (!state.isTracked || !state.isGripped))
    {
        _isHeld = false;
        Log::Info("levers", "stick let go");
    }
    if (_isHeld)
    {
        _tiltForward = Clamp(forward + _grab[0], MOST_RADIANS);
        _tiltSide = Clamp(side + _grab[1], MOST_RADIANS);
        return;
    }
    const float settle = expf(-stepSeconds / SETTLE_SECONDS);
    _tiltForward *= settle;
    _tiltSide *= settle;
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
    const Vehicle::Seat seat = Vehicle::CurrentSeat();
    const bool isShown =
        Xr::IsRunning() && seat == Vehicle::Seat::Hover && Vehicle::CurrentDrive() == Vehicle::Drive::Levers;
    const DWORD now = GetTickCount();
    float stepSeconds = _lastStepTick ? static_cast<float>(now - _lastStepTick) * 0.001f : 0.0f;
    _lastStepTick = now;
    if (stepSeconds > LONGEST_STEP_SECONDS)
    {
        stepSeconds = LONGEST_STEP_SECONDS;
    }
    if (isShown != _isShown)
    {
        // Entering or leaving the seat: the stick upright.
        _isShown = isShown;
        _isHeld = false;
        _tiltForward = _tiltSide = 0;
        Log::Info("levers", "%s", isShown ? "shown (tank pilot seat: drive stick, the head steers)" : "hidden");
    }
    if (!isShown)
    {
        return;
    }
    FpGuard fp;
    StepStick(stepSeconds);
    if (_isLogOn && now - _lastLogTick >= LOG_MILLISECONDS)
    {
        _lastLogTick = now;
        float move = 0, turn = 0, strafe = 0;
        GetDrive(&move, &turn, &strafe);
        Log::Info(
            "levers",
            "stick%s tilt %.1f ahead %.1f right deg: move %.2f strafe %.2f",
            _isHeld ? " held" : "",
            static_cast<double>(_tiltForward / RADIANS_PER_DEGREE),
            static_cast<double>(_tiltSide / RADIANS_PER_DEGREE),
            static_cast<double>(move),
            static_cast<double>(strafe));
    }
}

bool GetDrive(float* move, float* turn, float* strafe)
{
    *move = *turn = *strafe = 0.0f;
    if (!_isShown)
    {
        return false;
    }
    *move = Input(_tiltForward);
    *strafe = Input(_tiltSide);
    return true;
}

bool IsShown()
{
    return _isShown;
}

bool GetHandPose(int hand, float* restPose)
{
    const Gesture::Hand& state = Gesture::CurrentHand(hand & 1);
    if (!_isShown || !state.isTracked)
    {
        return false;
    }
    memcpy(restPose, state.pose, 64);
    if ((hand & 1) != 0 || !_isHeld)
    {
        return true;
    }
    // Round the grip as on a pistol grip: pointing ahead across it, the grip up through the fist (the arms' fist on
    // the grip's axis, as GunnerSeat holds a hand).
    FpGuard fp;
    float right[3], up[3], back[3], grip[3];
    GripAxes(_tiltForward, _tiltSide, right, up, back);
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
    return _isShown && (hand & 1) == 0 && _isHeld;
}

void Draw(void* device)
{
    float base[16];
    if (!_isShown || !device || Stereo::IsScopePass() || !Stereo::GetBaseCamera(base))
    {
        return;
    }
    // In the cockpit's own frame where there is one, so the stick stays on the console while the hull pitches and
    // rolls (idren2023 2026-10-19); else the level camera.
    CockpitShell::GetPilotFrame(base);
    float carriages[1][16], grips[1][16];
    {
        FpGuard fp;
        float right[3], up[3], back[3], carriage[16], grip[16];
        GripAxes(_tiltForward, _tiltSide, right, up, back);
        const float hullRight[3] = {1, 0, 0}, hullUp[3] = {0, 1, 0}, hullBack[3] = {0, 0, 1};
        MakeFrame(hullRight, hullUp, hullBack, PIVOT, carriage);
        MakeFrame(right, up, back, PIVOT, grip);
        Multiply(carriages[0], carriage, base);
        Multiply(grips[0], grip, base);
    }
    // The trigger follows the controller's while the hand holds the stick (primary fire, Controls).
    Xr::ControllerInput pad{};
    Xr::GetControllerInput(&pad);
    const float pulls[1] = {pad.isValid && _isHeld ? pad.trigger[0] : 0.0f};
    LeverModel::Draw(device, carriages, grips, pulls, 1, Vehicle::CurrentStockName());
}

void OnReset()
{
    LeverModel::OnReset();
}

void SetLog(bool isLogOn)
{
    _isLogOn = isLogOn;
}

} // namespace Levers

} // namespace SwbfVr
