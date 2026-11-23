// WalkerDrive.h: the throttle lever in a walker's pilot seat (the vehicle proposal 2026-11-04, idren2023: every walker
// a throttle lever with reverse / neutral / forward and head steering), and in the hailfire droid's (a hover).
#pragma once

namespace SwbfVr
{

namespace WalkerDrive
{

/// Once per frame (render thread, D3dSurvey's present): shown in a walker's pilot seat with VR running; takes, moves
/// and lets go of the lever with the right hand's grip.
void OnPresent();

/// The drive for Vehicle (game thread): move -1 reverse, 0 neutral, 1 forward (the lever); turn -1..1 for the walker's
/// turn input (Controllable +0x58, A / D: a walker turns on it at its own rates, run570) toward the head's heading
/// (the guns aim within their own limits without turning the hull, run571). False when not shown.
bool GetDrive(float* move, float* turn, bool isStickTurning = false);

/// The lever is shown this frame (a walker pilot: Controls makes the seat a station seat, the view keeps its own
/// heading and the walker turns under it toward the head, Vehicle).
bool IsShown();

/// The hand's pose closed round the lever's grip while held (resting-pose space, as Levers::GetHandPose).
bool GetHandPose(int hand, float* restPose);

/// The hand holds the lever.
bool IsHolding(int hand);

/// Draws the lever (Aim's vehicle first-person pass, after the cockpit and its shell).
void Draw(void* device);

/// Script `walker log <0|1>`.
void SetLog(bool isLogOn);

} // namespace WalkerDrive

} // namespace SwbfVr
