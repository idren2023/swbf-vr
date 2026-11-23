#pragma once

/// Handlebars: the pilot seat of the speeder bikes and the STAP (Vehicle::Drive::Handlebars, PLAN 55, idren2023
/// 2026-10-17). The game's first-person models of these are made for a flat screen (the bike's nose 6 m ahead of the
/// eye, the STAP's guns 4 m), so in VR the seat shows the vehicle's own third-person model instead, on the hull's
/// level heading, with the view's resting eye where a rider's head would be. The hands take the handlebars' grips
/// with the grip (within reach of a grip) and are drawn on them; the handlebars stay still. Leaning steers, leaning
/// forward over the hands drives (or, as an option, the right grip twists as a throttle; GetDrive); the sticks drive
/// as before. The animal mounts (tauntaun, Kaadu: walker seats) ride the same way; the game draws the animal itself (a
/// hook on its first-person visibility check), the reins are the grips, the lean turns it through its strafe input.
///
/// All functions run on the game's main thread (OnPresent from the Present hook, DrawHull and GetHandPose from the
/// first-person draw, GetSeatEye from the camera hook during Update): the game updates and renders on that one thread.

namespace SwbfVr
{

namespace Handlebars
{

/// Once per presented frame, after Vehicle::OnPresent: which bike the seat is, and which hand holds which grip.
void OnPresent();

/// This frame's drive from the rider (idren2023 2026-10-17/18): lean steering (the neck's sideways place from the resting
/// head, while a hand holds a grip) and the throttle: by default the head's place ahead of the hands since the grab
/// (head over the hands = forward, hands far out ahead = reverse), or the twist grip (the right hand's pitch since it
/// took its grip, rolled back = forward). Vehicle adds them to the sticks' inputs.
/// @param move set to -1..1, forward positive.
/// @param turn set to -1..1, right positive.
/// @return false outside a handlebar seat (both are then 0).
bool GetDrive(float* move, float* turn);

/// The resting eye of the view in a handlebar seat in first person (Stereo, during Update): the rider's eye point of
/// the vehicle's model on the hull's level heading. Logs where the game's own camera sits in the hull at a seat
/// change.
/// @param gameCameraPosition the game camera's world position (3 floats), to tell first from third person.
/// @param eyeWorld set to the eye's world position (3 floats) when the result is true.
/// @return false outside a handlebar seat, with VR off, or in third person (the game camera far from the eye).
bool GetSeatEye(const float* gameCameraPosition, float* eyeWorld);

/// Where a hand is drawn (the arms, Aim): the controller's pose; while it holds a grip, on the grip itself.
/// @param hand 0 right, 1 left.
/// @param restPose set to the pose in resting-pose space (16 floats) when the result is true.
/// @return false outside a handlebar seat or while the hand is not tracked.
bool GetHandPose(int hand, float* restPose);

/// Whether a hand holds its grip now.
/// @param hand 0 right, 1 left.
bool IsHolding(int hand);

/// Our hands on the bars (PLAN 61 j, BodyArm::DrawOnBar): a held grip's bar frame in the world (rows: x out along the
/// bar, y up, z back; the left grip's frame the right's mirror image), the rider's shoulder on that side in it (from
/// the head) and the twist grip's turn.
/// @param hand 0 right, 1 left.
/// @return false when that hand holds no grip (or not yet in this seat).
bool GetBarFrame(int hand, float* barWorld, float* shoulderInBar, float* twistRadians);

/// Draws the vehicle's third-person model into the current eye image, in place of the game's first-person model
/// (inside the first-person pass; Aim's first-person hook), the handlebars at rest.
/// @param flags the first-person draw's flags (passed on to the model draw).
/// @return false when nothing was drawn (not a handlebar seat, model not loaded): the caller draws the game's model.
bool DrawHull(unsigned flags);

/// On a mount (tauntaun, Kaadu): the rider's eye in the world (the game has no first-person view there). Update
/// thread, FpGuard held by the caller.
/// @param eye set (3 floats) when the result is true.
/// @return false when not riding a mount.
bool GetMountEye(float* eye);

/// The throttle (swbf-vr.ini bike_throttle, PLAN 57 f): lean (the default) or the right grip's twist.
/// @param isTwist true: the twist grip; false: the head ahead of the hands.
void SetTwistThrottle(bool isTwist);

/// Test (script command `bike log <0|1>`): log the lean, twist and drive once per second.
/// @param isLogOn true logs.
void SetDriveLog(bool isLogOn);

/// Test (script command `bike grip <roll> <pitch>`): how the hands sit on the grips, in degrees, instead of the
/// table's; -1 for the table's.
/// @param rollDegrees thumb inward.
/// @param pitchDegrees pointing down.
void SetGripTest(float rollDegrees, float pitchDegrees);

} // namespace Handlebars

} // namespace SwbfVr
