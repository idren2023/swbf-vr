#pragma once

#include <cstdint>

/// Aim: makes the player's weapon aim independent of the view, and places the first-person weapon, arms and hands
/// from the controller poses (Aim.cpp). Two engine hooks: Aimer::Set (where the game's shots start and go) and
/// FirstPersonRenderable::Render (the draw of the first-person arms and weapon).
///
/// Poses are the engine's 4x4 row-vector matrices passed as pointers to 16 floats (the convention of the whole mod
/// until it has a matrix type): rows 0..2 are the frame's right, up and back axes, row 3 its position; x right, y up,
/// -z ahead, metres. Controller (hand) numbers: 0 right, 1 left.

namespace SwbfVr
{

namespace Aim
{

// ---- Calls from the frame (Direct3D 9 hooks and Stereo) ----

/// Main thread, once per presented frame: reads the controller poses, installs the hooks once a controller is
/// tracked, applies the two-handed grip and the weapon's weight, finds the player's Aimer and the camera, and takes
/// the base frame (the level camera in VR, else the game's camera).
void OnPresent();

/// Just before the two eye passes are drawn: takes the controller poses and the base frame of this very frame, so
/// hands and view cannot drift apart.
void BeforeRender();

// ---- State for the other subsystems ----

/// Heading of a tracked controller relative to the character's facing.
/// @param hand 0 right, 1 left.
/// @param yawRadians set to the heading, positive to the right, when the result is true.
/// @return false if the controller is not tracked or points almost straight up or down.
bool GetHandYaw(int hand, float* yawRadians);

/// True while the player is a droideka (EntityDroideka, VR with controllers): the game aims it and draws its arms,
/// the hands do neither (Controls steers the game's aim toward the head).
bool IsDroideka();

/// A tracked controller's aim pose in the world, as of this frame's render (after grip and weight).
/// @param hand 0 right, 1 left.
/// @param matrix set to the pose (16 floats) when the result is true.
/// @return false if the controller is not tracked or there is no base frame.
bool GetHandWorld(int hand, float* matrix);

/// Where a first-person forearm begins (its bone's origin: the elbow end), as last drawn. Render thread.
/// @param hand 0 right, 1 left.
/// @param position set to x, y, z (world) when the result is true.
/// @return false if that forearm was not drawn within the last 100 ms.
bool GetFirstPersonElbowWorld(int hand, float* position);

/// The player's weapon in hand (through the first-person object), or 0.
uintptr_t GetWeapon();
/// The class of the player's weapon in hand (through the first-person object), or 0.
uintptr_t GetWeaponClass();

/// The first-person weapon model's matrix as last placed on the hand (model to world, rows x, y, z = model forward,
/// position; row vectors). Render thread.
/// @param matrix set to the 16 floats when the result is true.
/// @return false if the weapon was not placed on a hand within the last 100 ms.
bool GetWeaponWorld(float* matrix);
/// GetWeaponWorld with a lifetime of its own (the super battle droid's barrels: 500 ms); `isSteady`: without the
/// recoil kick (the aiming line holds still while the drawn gun kicks; headset run 19).
bool GetWeaponWorldWithin(float* matrix, unsigned long lifetimeMilliseconds, bool isSteady = false);

/// The line the hand-held weapon shoots along (world), for the aiming line and the crosshair.
/// @param from set to where the shots start (x, y, z) when the result is true.
/// @param direction set to the unit direction of the shots when the result is true.
/// @return false if no hand aimed within the last 100 ms.
bool GetFireLine(float* from, float* direction);

/// Where the game itself would start the player's shot (the soldier's own fire point, before the hand moved it): in
/// a network game the host's shots start there.
/// @param origin set to x, y, z (world) when the result is true.
/// @return false if no hand aimed within the last 100 ms.
bool GetStockFireOrigin(float* origin);

/// The fire line on the weapon as drawn in the current frame (GetWeaponWorld): from its muzzle, along its barrel
/// while the weapon sits on the grip pose. Render thread; for the aiming line.
/// @return false when the muzzle's place in the weapon or the drawn weapon is not known (GetFireLine then).
bool GetDrawnFireLine(float* from, float* direction);

/// The direction the game's target search (and with it the lock-on) uses for the player's soldier while the hand aims
/// (TargetSearchDirectionHook: the hand's), a unit vector.
/// @return false when it is not fresh (the search then uses the soldier's own aim, Controllable+0xEC).
bool GetTargetSearchDirection(float* direction);

/// @return true while the left hand is steadying the gun (two-handed grip).
bool IsTwoHanded();

/// @return true while the left hand is on or near the gun (around the barrel, grip button held or two-handed) and
/// for 0.7 s after.
bool IsLeftHandAtGun();

/// The reload hold: the left hand's model is locked onto the magazine under the gun while this is on (Controls).
/// @param isOn true takes hold with the next pose update; false gives the hand back to its controller.
void SetLeftHandOnMagazine(bool isOn);

/// @return how far the left controller is from the magazine while the reload hold lasts (metres), else 0.
float MagazinePullMeters();

/// Test: whether the game's first-person draw (with a hand placed) is running right now.
/// @return true inside that draw.
bool IsInFirstPersonRender();

/// True while a mirrored first-person model is drawn this frame (the super battle droid's left arm): its triangles
/// are turned inside out, so the first-person pass draws without back-face culling (D3dSurvey).
bool IsMirrorDrawn();

/// The super battle droid's holoscope emitter (between its barrels) and the barrels' direction, in the weapon model's
/// frame (as GetWeaponWorld places it).
/// @return false unless the player is the droid and its barrels were found this half second.
bool GetBarrelEmitter(float* emitter, float* axis);

/// The super battle droid: the point between its barrels halfway along them, in the world (its ammunition sign goes
/// there). False unless the player is the droid.
bool GetBarrelMiddleWorld(float* position);

/// The super battle droid: its barrels' opening and their direction in the world, from the weapon matrix the arms
/// were last drawn with (the aiming line starts there, so it stays on the drawn barrels).
/// @param position set to the opening (3 floats).
/// @param direction set to the barrels' direction (3 floats, unit length).
/// @return false unless the player is the droid.
bool GetBarrelOpeningWorld(float* position, float* direction);

/// True while the fire line starts exactly at a barrel opening (the droid): the aiming line is drawn from there.
bool IsFireLineFromOpening();

/// The gun model kicks back once (a shot leaves the player's weapon; called by Feedback.cpp). The aim is not moved.
void Kick();

/// Forgets the weapons' resting poses in the hand (one was taken during an animation); they are taken again within
/// a second or so. Any thread.
void ForgetRestingPoses();

// ---- Settings (swbf-vr.ini) ----

/// Zoomed with a weapon that has no lens, the head aims, not the hand ("scope_head_aim", default off). Weapons with
/// a lens always aim with the weapon.
/// @param isOn true: head aims while zoomed with a lensless weapon.
void SetScopeHeadAim(bool isOn);

/// Whether the left hand may take hold of the gun ("two_handed").
/// @param isAllowed true: two-handed grip allowed.
void SetTwoHandedAllowed(bool isAllowed);

/// Weapon handling ("weapon_weight", "virtual_stock").
/// @param weight weapon weight, 0 off .. 3 heavy (clamped).
/// @param hasVirtualStock true: virtual stock while two-handed.
void SetWeaponHandling(int weight, bool hasVirtualStock);

// ---- Script commands (tests). Any thread. ----

/// The game's aim turned by an offset ("aim <yaw> <pitch>").
/// @param isOn false gives the aim back to the game.
/// @param yawDegrees left positive.
/// @param pitchDegrees up positive.
void SetOffset(bool isOn, float yawDegrees, float pitchDegrees);

/// The first-person arms and weapon moved and turned about the eye ("fp").
/// @param isOn false: drawn as the game draws them.
/// @param xCentimeters right.
/// @param yCentimeters up.
/// @param zCentimeters back.
/// @param yawDegrees left positive.
/// @param pitchDegrees up positive.
void SetFirstPerson(
    bool isOn,
    float xCentimeters,
    float yCentimeters,
    float zCentimeters,
    float yawDegrees,
    float pitchDegrees);

/// One pose places the weapon and arms and gives the aim direction ("hand"), relative to the base frame's origin
/// (the eye; in VR the resting head). The game's own look is about 23 -30 58 0 0. Tracked controllers take over.
/// @param isOn false: off.
/// @param xCentimeters right.
/// @param yCentimeters up.
/// @param forwardCentimeters forward.
/// @param yawDegrees left positive.
/// @param pitchDegrees up positive.
void SetHand(
    bool isOn,
    float xCentimeters,
    float yCentimeters,
    float forwardCentimeters,
    float yawDegrees,
    float pitchDegrees);

/// The left forearm and hand follow their own pose ("lhand"), same units and base frame as SetHand; the pose's
/// position is the wrist.
/// @param isOn false: off.
/// @param xCentimeters right.
/// @param yCentimeters up.
/// @param forwardCentimeters forward.
/// @param yawDegrees left positive.
/// @param pitchDegrees up positive.
void SetLeftHand(
    bool isOn,
    float xCentimeters,
    float yCentimeters,
    float forwardCentimeters,
    float yawDegrees,
    float pitchDegrees);

/// Installs the hook on the first-person draw if it is not in yet (Holster draws from it).
/// @return true if the hook is in.
bool InstallFirstPersonModelHook();

/// For a throw (Holster): the controller's aim direction is replaced by this one for a while, so the thrown object
/// leaves along the hand's swing. Main thread.
/// @param direction unit vector in world space.
/// @param milliseconds how long.
/// Test ("droidtest <aim write 0|1> <mirror 0|1>", run183-run184): write the hand's direction to soldier+0x33C again
/// for every soldier (off since b175; b147-b174 wrote it) and draw the super battle droid's mirrored left arm (on by
/// default).
void SetDroidTest(bool shouldWriteAim, bool shouldMirror);

void SetThrowDirection(const float* direction, unsigned milliseconds);

/// Network game, for a grenade throw (Holster): the direction the local soldier is to face for a while, in place of
/// the gun hand's aim (Controls steers it there), so the host's own throw lands where the hand's would. Main thread.
/// @param direction unit vector in world space.
/// @param milliseconds how long.
void SetNetThrowAim(const float* direction, unsigned milliseconds);
/// @param direction set to the direction of SetNetThrowAim while it is in force.
/// @return false when there is none.
bool GetNetThrowAim(float* direction);

/// The base frame of this presented frame (resting-pose space to world: the level camera in VR, else the game's
/// camera), for other subsystems' poses.
/// @param matrix receives the 4x4 row-vector matrix.
/// @return false before the first base frame.
bool GetBaseFrame(float* matrix);

/// Where the right hand's fist closes in the right hand's pose (the first-person arms' hand round its grip, as the arms
/// were last copied on foot), for seats that close the hand round a grip; the left hand's is its mirror in x.
/// @param offset set to the palm's place in the hand pose's frame (3 floats, metres: right, up, back).
/// @return false before the arms were copied (no spawn in VR yet) or without the node.
bool GetPalmInHand(float* offset);

/// Logs which way the base frame faces (degrees, growing to the right), to measure turns ("heading").
void LogHeading();

/// Test (script command `gunner frame <0|1>`): draw the game's first-person model of a gunner seat (the stock tanks'
/// targeting frames) or leave it out.
/// @param isOn true draws it (default).
void SetGunnerFrameTest(bool isOn);

} // namespace Aim

} // namespace SwbfVr
