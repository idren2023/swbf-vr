#pragma once

#include <cstdint>

/// BodyArm: our own first-person right arm (PLAN 51 B, idren2023 2026-10-22): the rebel trooper's glove rebuilt as a
/// 5 536-triangle quad mesh with finger bones, its sleeve and cuff, posed from our pose library and drawn holding the
/// weapon the game draws, in place of the stock right arm (Aim collapses the stock right forearm and glove in the
/// game's own draw). Its data is generated from the mod tools' models by tools/body_export.py into build/gen
/// (BodyArm.inc, never in git); a build without it has no arm (IsAvailable false) and the stock arms stay.
///
/// All functions run on the game's main thread, inside the first-person pass (Aim's FirstPersonRenderHook).

namespace SwbfVr
{

namespace BodyArm
{

/// Whether the arm can be drawn for this weapon: the build has the arm's data and the weapon class's first-person
/// model is one we fitted a grip for (the rebel's blaster pistol, blaster rifle and thermal detonator), and the arm is
/// switched on (script command `bodyarm 1`).
/// @param weaponClass the held weapon's class (Aim::GetWeaponClass), or 0.
bool IsWanted(uintptr_t weaponClass);

/// Where the weapon goes so that our hand holds it on the controller (idren2023 2026-10-23: the gun in the real hand):
/// the weapon's model matrix in the controller's grip pose frame (Xr::GetGripPose), rows, the game's convention: our
/// hand's grip axis (through its curled fingers) on the handle's axis, the hand's knuckles along the pose's y, then
/// the tilt of Configure "tilt". Aim places the weapon with it instead of on the aim pose.
/// @param weaponClass the held weapon's class.
/// @param weaponInGrip set to the matrix (16 floats) when the result is true.
/// @return false when the arm is off or the weapon is not one we fitted.
bool GetWeaponInGrip(uintptr_t weaponClass, float* weaponInGrip);

/// Whether the empty right hand is ours (the arm is on and the build has its data).
bool IsEmptyHandWanted();

/// Draws the arm with nothing held: the hand round the controller's handle (as round the rifle's grip), posed from the
/// pose library's controller group (Configure "pose", or "nothing held" / "trigger only" by the trigger).
/// @param device the game's IDirect3DDevice9.
/// @param gripWorld the right controller's grip pose in the world (16 floats, rows).
/// @param triggerPull the right controller's trigger, 0 to 1.
void DrawEmptyHand(void* device, const float* gripWorld, float triggerPull);

/// Whether our left arm is drawn in place of the stock one (the arm is on, the build has its data, Configure "left").
bool IsLeftWanted();

/// Draws our left arm: the right arm mirrored, the hand round the left controller's handle, posed round the reload
/// canister while it is held (the pose library's fitted canister pose), else by the trigger as the empty right hand.
/// @param device the game's IDirect3DDevice9.
/// @param gripWorld the left controller's grip pose in the world (16 floats, rows).
/// @param triggerPull the left controller's trigger, 0 to 1.
/// @param isHoldingCanister the manual reload's canister is in the left hand (Holster::IsHoldingCanister).
void DrawLeft(void* device, const float* gripWorld, float triggerPull, bool isHoldingCanister);

/// Draws a hand of ours round a speeder bike's or STAP's handlebar grip (PLAN 61 j): the skeleton's bike grip pose
/// (tools/body_bikegrip.py), the bar through its fist, the hand rolled about the bar so that the forearm points at the
/// elbow of a two-bone arm from the rider's shoulder, the wrist bending the rest (at most 30 degrees).
/// @param device the game's IDirect3DDevice9.
/// @param barWorld the bar frame -> world (16 floats, rows): x out along the bar (toward the little finger), y up,
/// z back, origin on the bar's axis where the fist's middle goes, game metres; mirrored (z the other way round) for
/// the left hand, whose arm is the right one's mirror image.
/// @param shoulderInBar the rider's shoulder on that side in the bar frame (3 floats).
/// @param twistRadians the hand turned further about the bar (the twist-grip throttle; rolled back positive).
/// @param isLeft the left hand (the super battle droid's left parts; PlayerBody's hand slot).
void DrawOnBar(void* device, const float* barWorld, const float* shoulderInBar, float twistRadians, bool isLeft);

/// Where the reload canister goes in our left hand (the hand turned on it as fitted in the render tool, idren2023
/// 2026-10-24): item_powerup_ammo's model matrix in the world (rows, its scale included).
/// @param leftGripWorld the left controller's grip pose in the world (16 floats, rows).
/// @param canisterWorld set (16 floats) when the result is true.
/// @return false when our left arm is not drawn or the build has no canister grip.
/// An item Holster draws in the right hand (a grenade, mine, time bomb, canister, recon droid): its world matrix
/// (game model space -> world, unit rotation) with the hand left on the controller's grip pose `gripWorld` (rows) and
/// the item fitted into that hand by its grip; false for anything without an item grip.
bool GetItemWorld(uintptr_t weaponClass, const float* gripWorld, float* itemWorld);
bool GetCanisterWorld(const float* leftGripWorld, float* canisterWorld);

/// The point of the weapon our hand closes round (the grip's axis at the middle finger's curl), in the weapon's model
/// space: Aim puts it on the controller's grip pose (the middle of its handle, in the real palm) while the weapon
/// points where the controller points (idren2023 2026-10-23: the barrel with the controller, the hand on the gun).
/// @param weaponClass the held weapon's class.
/// @param point set to the point (3 floats, model space) when the result is true.
/// @return false when the arm is off or the weapon is not one we fitted.
bool GetGripPointInModel(uintptr_t weaponClass, float* point);

/// The hand's and weapon's tilt about the controller's sideways axis (Configure "tilt", degrees; fine tuning).
float GripTiltDegrees();

/// Draws the arm into the current eye image holding the weapon where the game just drew it.
/// @param device the game's IDirect3DDevice9.
/// @param weaponWorld the first-person weapon's world matrix as drawn (16 floats, rows, the game's convention).
/// @param weaponClass the held weapon's class (picks the grip and the pose).
/// @param triggerPull the right controller's trigger, 0 to 1 (the index finger follows it).
void Draw(void* device, const float* weaponWorld, uintptr_t weaponClass, float triggerPull);
/// The left arm of a unit whose weapon is its forearm (the super battle droid, PLAN 60 step 4e): drawn as Draw does
/// the right one, from a weapon matrix that is the right's placement mirrored onto the left controller (Aim), with
/// the skeleton's *_l parts and the left controller's hand pose. False when the weapon is no such grip.
bool DrawLeftForearmWeapon(void* device, const float* mirroredWeaponWorld, uintptr_t weaponClass, float triggerPull);

/// Script command `bodyarm <arguments>`: "1" / "0" switches the arm on or off; "axes <n>" picks how the weapon's msh
/// coordinates map to the game's model space (0: as they are, the default; 1: x negated; 2: z negated; 3: x and z negated), for
/// finding the right one by eye; "tilt <degrees>" turns the hand about the controller's handle (fine tuning); "wrist
/// 0|1" the forearm in line with the barrel or as the rest pose; "left 0|1" our left arm or the stock one; "pose <name>" puts the empty hand in that pose of the
/// library (part of its label, e.g. "point"; "auto" by the trigger); "log" logs the next draw's matrices. Script thread; takes effect at the next draw.
/// @param arguments the text after `bodyarm`.
void Configure(const char* arguments);

/// Before the device is reset: releases the state block (default pool).
void OnReset();

/// The last pose of a hand, for PlayerBody's one body (which then draws the hands and BodyArm only poses them):
/// set by the latest Draw / DrawEmptyHand (right) or DrawLeft (left) within the last 250 ms.
/// @param hand 0 right, 1 left.
/// @param armToWorld set (16 floats, rows): arm space (the right hand's rig, the wrist at the origin) -> world; the
/// left hand's includes the mirror.
/// @param skin set to the bones' skin matrices (arm space at rest -> posed; BoneCount of them, row form).
/// @return false when there is none.
bool GetHandPose(int hand, float* armToWorld, const float (**skin)[16]);

/// The arm's bones (forearm, hand, index_1 ...), in the order of GetHandPose's skin matrices.
int BoneCount();
const char* BoneName(int bone);

} // namespace BodyArm

} // namespace SwbfVr
