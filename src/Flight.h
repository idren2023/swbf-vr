#pragma once

/// Flight: the controls in a flyer's pilot seat (Vehicle::Drive::FlightStick; idren2023 2026-10-27: standard fighter jet
/// controls), three handles standing in the cockpit, each taken with the grip:
/// - the flight stick before the seat (right hand): tilted back the nose comes up, forward it goes down, to the side it
///   turns, like the mouse; a pistol grip shaped like a VR controller, its trigger the primary fire and the button on
///   its face (A) the secondary fire. Let go, it springs back upright.
/// - the throttle at the left side (left hand): slides into one of three notches, slow, cruise and fast (the game's
///   move input -1, 0, 1: min, mid and max speed); it stays where it was left.
/// - the take-off and landing lever beside the seat, low on the left (an e-brake): pushed down the ship takes off,
///   pulled up it lands (the game's Land / Takeoff key, Space). Let go, it shows whether the ship is flying (down) or
///   landed (up).
/// There is no ejection handle: the stock game does not let the pilot leave a flyer in flight (run456: E at 46 m did
/// nothing; on the ground it left, run457).
///
/// All functions run on the game's main thread (OnPresent from the Present hook, Draw from the first-person draw,
/// GetDrive from Vehicle's input hook, the queries from Controls and Aim).

namespace SwbfVr
{

namespace Flight
{

/// Once per presented frame, after Gesture::OnPresent and Vehicle::OnPresent, before Controls::OnPresent: which hand
/// holds which handle, the stick's tilt, the throttle's notch and the lever.
void OnPresent();

/// The handles' inputs of this frame, added to what the sticks and keys gave (Vehicle).
/// @param move set to the throttle's notch: -1 slow, 0 cruise, 1 fast.
/// @param yaw, pitch set to 0: the stick goes to the game as the mouse (GetStickLook; headset run 18).
/// @return false while the handles are not shown (not in a flyer's pilot seat, VR not running); all are then 0.
/// @param strafe a hover craft's stick aside (helicopter flight); 0 otherwise.
bool GetDrive(float* move, float* yaw, float* pitch, float* strafe);

/// In a hover craft's pilot seat (gunship, MAF): helicopter controls (the stick flies, twist turns, the left handle
/// is the elevation lever; Controls leaves the right thumbstick's X as the turn, not A / D).
bool IsHelicopter();

/// The stick's tilt as shares of full deflection after its dead zone and curve (-1..1), for Controls, which sends
/// them as mouse movement (idren2023, headset run 18: the stick as the mouse; the right stick's X as A / D).
/// @param yaw right positive. @param pitch nose up positive.
/// @return false while the handles are not shown; both are then 0.
bool GetStickLook(float* yaw, float* pitch);

/// Whether a hand holds a handle now (Controls then leaves that hand's grip alone; Aim draws the hand on it).
/// @param hand 0 right, 1 left.
bool IsHolding(int hand);

/// Whether the stick's face button is pressed with the stick in hand: the secondary fire (Controls; A then does not
/// jump, which in a flyer would take off or land).
bool IsSecondaryFire();

/// Whether the stick is in hand.
bool IsStickHeld();

/// In the droid starfighter's pilot seat (idren2023 2026-10-27): remote handles, taken by a grip wherever the hand is,
/// not drawn; X takes off and lands.
bool IsRemote();

/// Test (script command `flight takeoff`): in the droid's seat, asks for a take-off as X would, only if the ship is
/// not already flying (a ship entered in the air: X would land it, run507).
void RequestTakeOffTest();

/// The vertical speed (m/s, up positive) a hover craft's elevation control asks for this frame, written into the
/// flyer's velocity (Vehicle, game thread; entity +0x4DC, run576); 0: none. Script `flight climb <m/s>` (a test) sets it.
float GetClimb();
void SetClimbTest(float metersPerSecond);

/// With remote handles, X held a second on the ground: the use key (E) to leave the ship.
bool IsExitKeyDown();

/// Whether the Land / Takeoff key (Space) is to be held now: for a while after the lever asked for a take-off or a
/// landing (Controls holds it; a short tap was missed twice in run460).
bool IsLandingKeyDown();

/// Where a hand is drawn: the controller's pose; while it holds a handle, closed round its grip.
/// @param hand 0 right, 1 left.
/// @param restPose set to the pose in resting-pose space (16 floats) when the result is true.
/// @return false while the handles are not shown or the hand is not tracked.
bool GetHandPose(int hand, float* restPose);

/// Draws the three handles into the current eye image (inside the first-person pass, after the cockpit; Aim's
/// vehicle draw), with LeverModel.
/// @param device the game's IDirect3DDevice9.
void Draw(void* device);

/// Test switch (script command `flight log 0|1`): log the handles' state once per second.
void SetLog(bool isLogOn);

} // namespace Flight

} // namespace SwbfVr
