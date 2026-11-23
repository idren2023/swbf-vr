#pragma once

/// Levers: the drive stick in the pilot seat of a vehicle driven by levers (Vehicle::Drive::Levers: stock tanks;
/// PLAN 54 a2, PLAN 57 d; one stick since headset run 18, PLAN 59 a, idren2023: a single joystick for forward / reverse
/// and strafing, the head turning left and right and aiming up and down). It stands where the right slide handle
/// stood, is taken with the right grip and tilts with the hand: ahead drives, back reverses, sideways strafes, in
/// proportion past a dead zone. Let go, it springs upright. The head steers: the view keeps its own heading and the
/// hull turns under it toward where the head looks; the head's pitch aims the gun (Vehicle, Controls). The thumbsticks
/// keep working; the stick adds to them (Vehicle writes the sum into the seat's inputs).
///
/// All functions run on the game's main thread (OnPresent from the Present hook, Draw from the first-person draw,
/// GetDrive from Vehicle's input hook): the game updates and renders on that one thread.

namespace SwbfVr
{

namespace Levers
{

/// Once per presented frame, after Gesture::OnPresent and Vehicle::OnPresent, before Controls::OnPresent: whether the
/// right hand holds the stick, and its tilt.
void OnPresent();

/// The stick's drive inputs of this frame.
/// @param move set to -1..1 (forward positive): the stick's tilt ahead.
/// @param turn set to 0 (the head steers, Vehicle).
/// @param strafe set to -1..1 (right positive): the stick's tilt to the side.
/// @return false while the stick is not shown (not in a tank's pilot seat, VR not running); all are then 0.
bool GetDrive(float* move, float* turn, float* strafe);

/// Whether the stick is shown: a tank's pilot seat with VR running (the head steers there; Controls, Vehicle).
bool IsShown();

/// Whether a hand holds the stick now (Controls then leaves that hand's grip alone).
/// @param hand 0 right, 1 left (never).
bool IsHolding(int hand);

/// Where a hand is drawn (the arms on the stick, Aim): the controller's pose; the right hand while it holds the stick
/// closed round its grip (the arms' fist on the grip's axis, as GunnerSeat holds a hand).
/// @param hand 0 right, 1 left.
/// @param restPose set to the pose in resting-pose space (16 floats) when the result is true.
/// @return false while the stick is not shown or the hand is not tracked.
bool GetHandPose(int hand, float* restPose);

/// Draws the stick into the current eye image (inside the first-person pass, after the cockpit model and its shell;
/// Aim's first-person hook), with LeverModel.
/// @param device the game's IDirect3DDevice9.
void Draw(void* device);

/// Before the device is reset: LeverModel's state block goes.
void OnReset();

/// Test switch (script command `levers`): log the stick's state once per second.
/// @param isLogOn true logs.
void SetLog(bool isLogOn);

} // namespace Levers

} // namespace SwbfVr
