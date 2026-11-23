#pragma once

#include <cstdint>

/// Holster: the soldier's weapons that are not in the hand, drawn on the body (hips, chest) where the hand can take
/// them (Holster.cpp). The engine's own model draw is called again with matrices of ours (docs/offsets.md "Model
/// draw"), inside the first-person pass.
///
/// OnPresent and the setters run on the game's main thread; Draw runs inside the first-person draw (Aim.cpp's hook)
/// and only reads what OnPresent left between frames.

namespace SwbfVr
{

namespace Holster
{

/// The places on the body, in idren2023's plan: right hip primary 1, left hip primary 2, left chest secondary 1, right
/// chest secondary 2.
enum class Place
{
    RightHip,
    LeftHip,
    LeftChest,
    RightChest,
    Count
};

/// Once per presented frame, after Gesture::OnPresent: works out each place's world matrix for the next frame from
/// the torso frame (Gesture) and the base frame (Aim).
void OnPresent();

/// True if a game object's C++ class is exactly this one (from its RTTI; a base part of the object will do).
/// @param className undecorated, e.g. "EntitySoldier".
bool IsObjectType(uintptr_t object, const char* className);

/// A game object's decorated C++ class name from its RTTI (".?AVEntityHover@@"), or null; for logs.
const char* ObjectTypeName(uintptr_t object);

/// A model the game has loaded, by its ODF GeometryName (the game's model registry), or null if not loaded.
/// @param geometryName e.g. "imp_hover_speederbike".
void* LoadedModel(const char* geometryName);

/// True while the holsters are on and the right hand holds nothing (grip released): the first-person weapon is not
/// drawn (Aim) and fire is blocked (Controls). Read from the main thread and inside the first-person draw; changes
/// only in OnPresent.
bool IsHandEmpty();

/// True while the right hand holds a primary weapon taken from a place but the game's first-person model still has
/// another weapon object (the lowering half of its swap animation, about 0.3 s; idren2023 2026-10-20: the old weapon came
/// up in the hand, then snapped to the new one). Aim leaves the game's weapon out of the draw and Draw puts the taken
/// one in the hand, so the grab shows the right weapon at once; when it can fire stays the game's (the same in
/// multiplayer). Render thread, inside the first-person draw.
/// @param firstPersonRenderable the FirstPersonRenderable being drawn.
bool IsSwapPending(void* firstPersonRenderable);

/// True while the holsters are on (they replace the right grip's reload).
bool IsActive();

/// True while the manual reload's canister is in the left hand (BodyArm poses our left hand round it).
bool IsHoldingCanister();

/// Manual reload (idren2023 2026-10-20, PLAN 58 s; swbf-vr.ini manual_reload, default on): with a clip weapon in the
/// right hand and spare clips, an ammo canister on the front left of the belt; the game's own reload when the clip is
/// empty is blocked (the weapon's slot 44 for the player's weapon), the left hand takes the canister (grip in the belt
/// zone) and lets go of it at the gun, which taps the game's reload; refused (a full clip), it goes back to the belt.
void SetManualReload(bool isOn);
bool IsManualReloadOn();

/// True if a weapon is idle with a round in its clip (not firing, reloading or empty).
/// @param weapon a Weapon (Aim::GetWeapon), or 0.
bool IsWeaponReady(uintptr_t weapon);

/// True while the right hand is empty (let go of its weapon, nothing taken): no weapon HUD, no aiming line.
bool HoldsNothing();

/// True while the right hand holds a secondary item taken from a chest place (grenade, mine, time bomb, dispenser, fusion
/// cutter, recon droid): no weapon HUD and no aiming line for it (idren2023 2026-11-22: an empty item is not on the chest,
/// so its count needs no display, and a thrown or placed item needs no line).
bool HoldsSecondary();

/// True while the player's soldier has holsters (VR, a soldier with a layout): weapons are taken from them, not cycled.
bool HasPlaces();

/// True while the player's soldier is a super battle droid (its left arm is drawn mirrored on the left controller).
bool IsSuperBattleDroid();

/// True while the player's soldier is on the CIS side (holograms red instead of blue).
bool IsCis();

/// True while the right hand holds a secondary weapon that is used by its trigger (fusion cutter, binoculars, ...):
/// Controls sends the right trigger to the secondary fire instead. Main thread.
bool IsTriggerItemHeld();

/// The class of the secondary weapon the right hand holds (taken from a place, drawn by Holster in the hand), or 0:
/// Aim draws our arm holding it, not the empty hand. The game's current weapon stays the primary until the secondary
/// fires (run535). Changes only in OnPresent.
uintptr_t HeldSecondaryClass();

/// For HeldSecondaryClass's weapon (its class weaponClass) when Holster draws it as an item model of its own at the item's size (the grenade
/// every side draws, the placed landmine, the time bomb's seismic charge, health / ammo, the recon droid): BodyArm's
/// grip for it (com_grenadethermal, item_landmine, item_timebomb, item_powerup, item_probedroid; tools/body_hold.py);
/// else null. Changes only in OnPresent.
const char* HeldItemGrip(uintptr_t weaponClass);

/// True for a short time after an expendable (grenade, mine) was let go: Controls holds the secondary fire, which
/// throws it. Main thread.
bool IsThrowing();

/// Test (script command `throwspeed <m/s>`): the hand's speed taken for the next throws in place of the measured
/// swing's (the simulated hand never swings faster than about 2 m/s); 0 = measured again.
void SetTestThrowSpeed(float metersPerSecond);

/// Inside FirstPersonRenderable::Render, after the game's draw (Aim.cpp's hook): draws the holstered weapons.
/// @param firstPersonRenderable the FirstPersonRenderable being drawn (gives the soldier and the held weapon).
/// @param flags the render flags the game's first-person draw received.
void Draw(void* firstPersonRenderable, unsigned flags);

/// Holsters are always on for a soldier while VR runs (mode 3, switched in OnPresent); weapon change by the stick
/// stays in Controls for what holsters do not cover (vehicle seats, the super battle droid's arms). After this test
/// command the mode stays as set (no automatic switching) until the game restarts.
/// Test "holster <mode> [spacing cm]": 0 off; 1 / 2 the soldier's other weapons in a row beside the held one
/// (first- / third-person models); 3 on the body (first-person models; needs VR and a tracked head).
/// @param mode the mode.
/// @param spacingCentimeters the row's spacing (modes 1 and 2), 0 keeps the current one.
void SetMode(int mode, float spacingCentimeters);

/// Test "holster pose <place> <x> <y> <z> <yaw> <pitch> <roll>": where a place sits in the torso frame (x right,
/// y up, -z ahead, centimetres, from the head) and how the weapon is turned there (degrees; the weapon's muzzle is
/// the pose's -z, its top +y; yaw left positive, pitch up positive, roll left positive).
void SetPose(int place, float x, float y, float z, float yawDegrees, float pitchDegrees, float rollDegrees);

} // namespace Holster

} // namespace SwbfVr
