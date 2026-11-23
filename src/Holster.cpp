// Holster.cpp: the soldier's weapons that are not in the hand, drawn on the body, and taken by the right hand.
//
// Drawing (run137, run138): the engine draws a weapon model at any matrix we give it. A model's draw is its vtable
// slot 1, thiscall (matrix, pose, light, flags, 0); Weapon::Render (Weapon vtable slot 23, 0x243000) calls it on
// weapon+0xD0 (its first-person model) or weapon+0xCC (third-person model) with pose 0 and the light the first-person
// draw uses (GameBuild ModelLight). Here it is called once per holstered weapon inside the first-person pass, after
// the game's own draw, while Aim's hand-placed matrices are still in.
//
// Places: a pose per place in Gesture's torso frame (under the head, upright, turned like the body; resting-pose
// space, x right, y up, -z ahead), times the base frame (Aim) = world. A model looks along +z and a pose along -z, so
// the model's matrix is TURN * pose.
//
// Weapons (docs/offsets.md "Soldier weapons"): a flat array of up to 8 at soldier+0xA30, both channels; a weapon's
// channel = (weapon+0x70, its trigger pointer, - (Controllable+0x38)) / 2; the current index per channel at
// soldier+0xA50 / +0xA54. Primary weapons (channel 0) go to the hips (right, then left), secondary ones (channel 1:
// grenades, mines, detpacks) to the chest (left, then right); further ones get no place yet.
//
// The right hand: it holds the current primary weapon (as the game has it) until the grip is let go; then it is
// empty (Aim leaves the weapon out of the first-person draw, Controls blocks fire) and the weapon hangs in its place.
// Grip in a place takes that place's weapon: a primary one by the player's own "next weapon" (one mouse wheel notch
// per step until the soldier's current primary index is the wanted one; Controllable::RequestWeapon did not switch
// the player's weapon in run140); a secondary one is requested through Controllable::RequestWeapon for its channel and
// drawn in the hand, and letting go throws it (a short secondary fire, Controls). An expendable's model is back in its
// place once the weapon is idle with a round in the clip (its reload after a throw is the game's own delay); without
// ammo the place stays empty.

#include "Holster.h"

#include "Aim.h"
#include "BodyArm.h"
#include "Controls.h"
#include "D3dSurvey.h"
#include "Feedback.h"
#include "FpGuard.h"
#include "GameBuild.h"
#include "Gesture.h"
#include "Input.h"
#include "Log.h"
#include "Net.h"
#include "ReloadZone.h"
#include "Xr.h"
#include "Zoom.h"
#include "engine.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace SwbfVr
{

namespace Holster
{

namespace
{

/// A model's draw, vtable slot 1: world matrix (4x4 row vectors, read during the call), pose (0 for weapons).
using ModelDrawFunction =
    void(__thiscall*)(void* model, const float* matrix, void* pose, void* light, unsigned flags, int unknown);
/// void __thiscall Controllable::RequestWeapon(int index), 0xC3540.
using RequestWeaponFunction = void(__thiscall*)(void* controllable, int index);

/// Weapon: its first-person and third-person models (Weapon::Render 0x243000), its Aimer, its channel's trigger
/// (pointer to Controllable+0x38+2*channel, set at spawn 0x10A7xx), its loaded fraction of a clip (float, 1 = full),
/// its state (0 idle / ready, 1-3 firing, 4 charging, 5 reloading, 8 empty; Weapon::Update 0x244C80). Observed.
constexpr uintptr_t OFFSET_weaponFirstPersonModel = 0xD0;
constexpr uintptr_t OFFSET_weaponThirdPersonModel = 0xCC;
constexpr uintptr_t OFFSET_weaponAimer = 0x6C;
constexpr uintptr_t OFFSET_weaponTrigger = 0x70;
constexpr uintptr_t OFFSET_weaponClipFraction = 0x8C;
constexpr uintptr_t OFFSET_weaponState = 0xB4;
/// WeaponClass: rounds per clip (int). Observed (WeaponClass parser).
constexpr uintptr_t OFFSET_weaponClassRoundsPerClip = 0x54;
/// Controllable: the triggers, 2 bytes per channel (0x38 primary, 0x3A secondary). Observed (PlayerController::Update).
constexpr uintptr_t OFFSET_controllableTriggers = 0x38;
/// Soldier: the flat weapon array and the current index per channel (-1 none). Observed (soldier update 0x11ABB8).
constexpr uintptr_t OFFSET_soldierWeapons = 0xA30;
constexpr uintptr_t OFFSET_soldierWeaponIndex = 0xA50;
constexpr int WEAPON_COUNT = 8;
/// Soldier object size (offsets.md: the next pooled soldier starts here).
constexpr uintptr_t SOLDIER_SIZE = 0xB50;
/// Flags the game's first-person draw adds to its own for the model draws (0x130833).
constexpr unsigned MODEL_DRAW_FLAGS = 0x80001;
constexpr int PLACE_COUNT = static_cast<int>(Place::Count);
constexpr float DEGREES_TO_RADIANS = 3.14159265f / 180;
/// A model looks along +z, a pose along -z.
constexpr float TURN[16] = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1};
/// How long the secondary fire is held for a throw (b151: 60 ms, was 150: the grenade left late; 4 frames at 72 Hz),
/// and how long the throw direction is kept for the aim.
constexpr DWORD THROW_MILLISECONDS = 60;
/// Root node hash of the super battle droid's first-person arm blaster model (run156).
constexpr unsigned SUPER_BATTLE_DROID_MODEL = 0x8D95A295;
constexpr unsigned THROW_DIRECTION_MILLISECONDS = 400;
/// After letting go, the game's release is looked for this long (SkipReleaseDelay).
constexpr DWORD THROW_SKIP_WINDOW_MILLISECONDS = 600;
/// WeaponGrenade fields (offsets "Thrown ordnance"): fired flag (byte), release timer, release duration; its class's
/// InitialSalvoDelay.
constexpr uintptr_t OFFSET_grenadeFired = 0x120;
constexpr uintptr_t OFFSET_grenadeTimer = 0xB8;
constexpr uintptr_t OFFSET_grenadeDuration = 0xBC;
constexpr uintptr_t OFFSET_grenadeClassSalvoDelay = 0x1D4;
/// WeaponDispenserClass InitialSalvoDelay (the dispenser's fired flag and timer are at the grenade's offsets).
constexpr uintptr_t OFFSET_dispenserClassSalvoDelay = 0x1DC;
/// The swing is measured over this long before the release; slower than this, the throw keeps the hand's pointing.
/// 0.05 s since b176 (0.10 before): over the longer span an underhand swing's chord pointed well below the hand's path
/// at the release (headset run 13: underhand throws went in strange directions); the shorter one is its tangent.
constexpr double SWING_SECONDS = 0.05;
constexpr float SWING_MINIMUM_METERS_PER_SECOND = 1.0f;
/// Throw strength (PLAN 42): the hand's speed at the release over this gives the game's full throw; slower swings throw
/// in proportion, down to THROW_MINIMUM_STRENGTH (a let-go without a swing). Capped at the game's own (1).
/// b206: 10 m/s (was 4): headset run 14's throws measured 1.3 to 11 m/s and everything over 4 was full (idren2023: too
/// easy to max). Underhand swings (moving upward, UNDERHAND_MINIMUM_RISE) need 12 m/s: they maxed at 3.7 to 6.6 m/s.
constexpr float FULL_THROW_METERS_PER_SECOND = 10.0f;
constexpr float FULL_UNDERHAND_THROW_METERS_PER_SECOND = 12.0f;
constexpr float UNDERHAND_MINIMUM_RISE = 0.3f; // the swing direction's upward part (world y, unit vector)
constexpr float THROW_MINIMUM_STRENGTH = 0.25f;
/// The strength is applied to the thrown weapon's next shot within this long of letting go (the game's release delay
/// and a frame or two).
constexpr DWORD THROW_STRENGTH_MILLISECONDS = 1500;
/// Vtable slot (offsets "Thrown ordnance"): the ordnance class's create (thiscall, descriptor pointer, ret 4).
constexpr int ORDNANCE_CREATE_SLOT = 2;
/// Ordnance create descriptor: its direction (3 floats; its length scales the launch speed, not normalised again) and
/// the Weapon that fired. WeaponClass+0x50: the ordnance class.
constexpr uintptr_t OFFSET_descriptorDirection = 0x0C;
constexpr uintptr_t OFFSET_descriptorWeapon = 0x38;
constexpr uintptr_t OFFSET_weaponClassOrdnance = 0x50;
constexpr int PATCHED_SLOT_CAPACITY = 6;
/// Manual reload (PLAN 58 s): Weapon +0x84 reserve clips (float, FLT_MAX unlimited); WeaponClass +0x54 RoundsPerClip
/// (int; 0 heat weapons, >= 0x7FFFFFFF unlimited); state 5 reloading (docs/research/manual-reload.md). Weapon slot 44
/// (0x241F20, thiscall, no arguments, bool: rounds <= 0) starts the game's own reload in state 0.
constexpr uintptr_t OFFSET_weaponReserve = 0x84;
constexpr int WEAPON_STATE_RELOADING = 5;
constexpr int CLIP_EMPTY_SLOT = 44;
/// Weapon slot 42 (0x241EE0, thiscall, bool: rounds <= 0 and reserve < 1): "nothing left". In state 0 an empty clip
/// (slot 44) goes to state 8 (empty: the trigger plays the class's empty sound, class+0x100) when it is true, else to
/// state 5 (reload); state 8 itself reloads as soon as it turns false (Weapon::Update 0x244C80, its case 8 at 0x2450F5;
/// read 2026-10-24).
constexpr int NO_SPARE_SLOT = 42;
/// WeaponDispenserClass (also WeaponRemote's class): the entity class it puts down (OrdnanceName).
constexpr uintptr_t OFFSET_dispenserClassItem = 0x1D0;
constexpr int SAMPLE_COUNT = 32;

/// A place's pose in the torso frame: centimetres and degrees (see SetPose).
struct PlacePose
{
    float x, y, z, yaw, pitch, roll;
};

/// Near Gesture's hip and chest zones (hips +-22, -60, -2; chest +-15, -30, -12): weapons hang muzzle down, tilted
/// 15 degrees ahead, their top facing ahead. b143 (headset run 12): primaries 6 cm lower, the right hip's muzzle
/// swung 25 degrees toward the left leg (long weapons were in view), chest places 6 cm closer to the chest. To be
/// tuned in the headset.
/// The canister: its pose in the torso frame (Gesture's BeltLeft zone, front left of the belt; cm, degrees), its size,
/// how near the right hand (the gun) the left hand must let go of it to put it in, and how long the game has to begin
/// the reload after the tap before the canister goes back to the belt.
const PlacePose AMMO_POSE = {-12, -52, -20, 0, 0, 0};
constexpr float AMMO_MODEL_SCALE = 0.15f * 0.6f * 3.0f;
/// item_powerup_ammo's middle in its own model space (its bounding box: x -0.112..0.139, y 0..0.478, z -0.12..0.12;
/// the origin is its base): the point put in the palm.
constexpr float AMMO_MODEL_CENTRE[3] = {0.0135f, 0.239f, 0.0f};
constexpr float AMMO_INSERT_METERS = 0.30f;
constexpr DWORD AMMO_ACCEPT_MILLISECONDS = 600;
PlacePose _poses[PLACE_COUNT] = {
    {22, -68, 0, 25, -75, 0},  // right hip
    {-22, -68, 0, 0, -75, 0},  // left hip
    {-15, -32, -6, 0, -75, 0}, // left chest
    {15, -32, -6, 0, -75, 0},  // right chest
};
/// Changes to a place's pose for what it holds (idren2023 2026-11-01): secondaries other than grenades (fusion cutters,
/// dispensers, the recon droid) were in the way on the chest, 10 cm lower; a launcher on the right hip (the vanguard
/// classes) blocked the view, its muzzle swung further toward the left leg (its back end out to the right), hung
/// steeper back and 4 cm further back. Guesses, to be judged in the headset.
constexpr float CHEST_ITEM_DROP_CENTIMETERS = 10;
const PlacePose RIGHT_HIP_LAUNCHER_POSE = {22, -68, 4, 40, -65, 0};
/// Headset run 19 (idren2023): the fusion cutter on the belt in front of the left hip's pistol, away from the reload
/// canister on the chest's left. A guess, to be judged in the headset.
const PlacePose BELT_CUTTER_POSE = {-12, -64, -10, 0, -75, 0};
/// Thrown secondaries are drawn smaller than their models: the models were far too big (headset run 12; run148: the
/// thermal detonator and time bomb about 25 cm across). Sizes per item from headset run 14 (idren2023: the standard
/// soldier's grenades at 0.6 were close to right, some classes' looked smaller; grenades 0.75, time bomb 0.5,
/// mines 0.6; run 15: grenades still a little small, 0.8). Applied in any channel (one class carries its grenade in
/// channel 0, which got no scale before b206).
constexpr float GRENADE_MODEL_SCALE = 0.8f;
constexpr float TIME_BOMB_MODEL_SCALE = 0.5f;
constexpr float MINE_MODEL_SCALE = 0.6f;
/// The dispensed items' own models (PLAN 41): the health / ammo canister is drawn by the game at its ODF's
/// GeometryScale 0.15 (com_item_powerup_dual.odf); the recon droid at its full flying size, shrunk to a holster item.
/// Headset run 16 (idren2023): canister 10 times and recon droid 1.75 times the b206 size (run 15 had 3 and 1.5); headset
/// run 17 (PLAN 58 j): the canister 5 times (10 was in the way); headset run 18: 4 times; idren2023 2026-11-01: as big as
/// the manual reload's canister (its canisters are item_powerup_ammo's size, about 0.48 tall).
constexpr float POWERUP_MODEL_SCALE = AMMO_MODEL_SCALE;
constexpr float RECON_DROID_MODEL_SCALE = 0.4f * 1.75f;

/// What the right hand holds.
enum class Holding
{
    Primary,   // the game's current primary weapon (the default, also without holsters)
    Empty,     // nothing: the grip was let go
    Secondary, // a secondary weapon taken from a place (see Kind)
};

/// How a secondary weapon is used once in the hand (from its C++ type, read from the game's RTTI).
enum class Kind
{
    Throwable, // WeaponGrenade, WeaponDispenser (grenades, mines, time bombs, health / ammo), WeaponRemote (the recon
               // droid: launched by a throw only, idren2023 2026-11-22): thrown when let go
    Trigger,   // everything else (fusion cutter, binoculars, detonator): the right trigger is
               // the secondary fire while it is held; letting go puts it back
};

/// The soldier's weapons by place, worked out once per presented frame (main thread) and read by Draw.
struct Layout
{
    uintptr_t soldier;
    uintptr_t weapons[PLACE_COUNT]; // 0 = no weapon in that place
    int indices[PLACE_COUNT];       // index in the soldier's weapon array
    int channels[PLACE_COUNT];
    Kind kinds[PLACE_COUNT];
    float scales[PLACE_COUNT]; // model size factor (ItemScale)
    const char* grips[PLACE_COUNT]; // BodyArm's grip for the item model drawn in the hand (ItemGrip), or null
    PlacePose poses[PLACE_COUNT]; // the place's pose for what it holds
    uintptr_t primary;         // the current primary weapon (in the hand while holding Primary)
};

volatile int _mode;                    // see SetMode
volatile bool _isSuperBattleDroid = false; // the player's soldier is a super battle droid (MakeLayout, OnPresent)
bool _isDroidFound = false;            // MakeLayout's finding this frame
bool _isCis = false;                   // the player's soldier is on the CIS side (by its weapons' models)
volatile float _spacingMeters = 0.22f; // row spacing, modes 1 and 2
float _placeInBase[PLACE_COUNT][16]; // relative to Aim's base frame; written by OnPresent between frames, read by Draw
bool _hasPlaces;                     // _placeInBase is valid for this frame
Layout _layout;                      // written by OnPresent between frames, read by Draw
volatile LONG _logsLeft;             // layout log lines still to write (Interlocked)
volatile Holding _holding = Holding::Primary;
int _heldPlace = -1;       // the place the weapon in the hand came from (-1: none taken since the spawn)
int _wantedSecondary = -1; // secondary weapon index asked for (-1 = none)
int _secondarySteps;       // "next secondary" key taps given for it so far
DWORD _lastSecondaryStepTick;
DWORD _keyUpTick;        // the "next secondary" key is released at this tick (0 = not down)
bool _wasGripped;        // the right grip in the last presented frame
uintptr_t _lastSoldier;  // a new soldier (respawn) starts with the primary weapon in the hand
DWORD _throwUntilTick;   // secondary fire held until this tick (0 = none)
uintptr_t _thrownWeapon; // the grenade or dispenser just thrown, until the game has fired it (SkipReleaseDelay)
// Throw strength for the weapon just let go (any throwable), for its next shot (PLAN 42).
uintptr_t _strengthWeapon = 0;
float _throwStrength = 1.0f;
DWORD _strengthTick = 0;

// Vtable slots pointed at our hooks, with the game's function each held (shared by every object of that class, AI
// included: the hooks act only for _strengthWeapon).
struct PatchedSlot
{
    uintptr_t table;
    uintptr_t original;
};

PatchedSlot _createSlots[PATCHED_SLOT_CAPACITY];
int _createSlotCount = 0;
PatchedSlot _clipSlots[PATCHED_SLOT_CAPACITY]; // slot 44 of the clip weapons' vtables (manual reload)
PatchedSlot _spareSlots[PATCHED_SLOT_CAPACITY]; // slot 42 of the clip weapons' vtables (manual reload)
int _spareSlotCount = 0;
int _clipSlotCount = 0;
/// Manual reload: on or off; the player's weapon whose own reload is blocked (0: none); the canister's state.
volatile bool _isManualReload = true;
/// Manual reload as set, but never in a network game: there the host reloads an empty gun by its own rules and
/// sends the result, so a gun held empty here would only disagree with the host's.
bool IsManualReloadWanted()
{
    return _isManualReload && !Net::IsNetGame();
}
volatile uintptr_t _manualWeapon = 0;
enum class Ammo
{
    Hidden,    // no clip weapon in the hand, or no spare clip
    Holstered, // on the belt
    InHand,    // in the left hand
    Inserting, // let go at the gun: R tapped, waiting for the game to begin the reload
    Reloading  // the game is reloading (state 5)
};
Ammo _ammo = Ammo::Hidden;
DWORD _ammoTick = 0;
bool _wasLeftGripped = false;
bool _wasCanisterAtGun = false; // the held canister was inside the insert zone last frame (its haptics)
float _ammoPlaceInBase[16];
DWORD _thrownTick;        // when it was let go
bool _isAutomatic = true; // mode 3 follows VR running (false after the test command set a mode)

/// The right hand's recent positions in resting-pose space (walking and turning do not move them), for the swing.
struct HandSample
{
    double seconds;
    float position[3];
};

HandSample _samples[SAMPLE_COUNT];
int _nextSample;
int _wantedPrimary = -1; // primary weapon index being stepped to (-1 = none)
int _primarySteps;       // wheel notches given for it so far
DWORD _lastStepTick;
/// Primary weapon change by wheel notches: time between notches, and how many before giving up.
constexpr DWORD STEP_MILLISECONDS = 250;
constexpr int MAXIMUM_STEPS = 8;
/// Secondary weapon change: Controllable::RequestWeapon first; if the soldier's secondary index is still another one
/// after STEP_MILLISECONDS, the player's "next secondary" key (idren2023's profile: G) is tapped, KEY_TAP_MILLISECONDS
/// long, until it is.
constexpr unsigned KEY_G = 0x22;
constexpr DWORD KEY_TAP_MILLISECONDS = 60;

// product = first * second (4x4, row vectors: first is applied first). product may be one of the inputs.
void Multiply(float* product, const float* first, const float* second)
{
    float result[16];
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            result[row * 4 + column] = first[row * 4] * second[column] + first[row * 4 + 1] * second[4 + column] +
                                       first[row * 4 + 2] * second[8 + column] +
                                       first[row * 4 + 3] * second[12 + column];
        }
    }
    memcpy(product, result, sizeof(result));
}

// A place's pose as a matrix in the torso frame: roll about z, then pitch about x, then yaw about y, then moved.
void PoseMatrix(const PlacePose& pose, float* matrix)
{
    const float roll = pose.roll * DEGREES_TO_RADIANS, pitch = pose.pitch * DEGREES_TO_RADIANS,
                yaw = pose.yaw * DEGREES_TO_RADIANS;
    const float rollMatrix[16] = {cosf(roll), sinf(roll), 0, 0, -sinf(roll), cosf(roll), 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float pitchMatrix[16] =
        {1, 0, 0, 0, 0, cosf(pitch), sinf(pitch), 0, 0, -sinf(pitch), cosf(pitch), 0, 0, 0, 0, 1};
    const float yawMatrix[16] = {cosf(yaw), 0, -sinf(yaw), 0, 0, 1, 0, 0, sinf(yaw), 0, cosf(yaw), 0, 0, 0, 0, 1};
    // Yaw before pitch (b143): with the weapon hanging muzzle down, the yaw swings the muzzle sideways.
    Multiply(matrix, rollMatrix, yawMatrix);
    Multiply(matrix, matrix, pitchMatrix);
    matrix[12] = pose.x / 100;
    matrix[13] = pose.y / 100;
    matrix[14] = pose.z / 100;
}

// True if the pointer is a weapon owned by the soldier (its Aimer lies inside the soldier object).
bool IsSoldierWeapon(uintptr_t weapon, uintptr_t soldier)
{
    if (weapon < 0x10000 || (weapon & 3) || IsBadReadPtr(reinterpret_cast<void*>(weapon), 0xD4))
    {
        return false;
    }
    const uintptr_t aimer = *reinterpret_cast<uintptr_t*>(weapon + OFFSET_weaponAimer);
    return aimer >= soldier && aimer < soldier + SOLDIER_SIZE;
}

// A weapon's channel (0 primary, 1 secondary) from its trigger pointer; -1 if it is not one of the soldier's two.
int WeaponChannel(uintptr_t weapon, uintptr_t soldier)
{
    const uintptr_t trigger = *reinterpret_cast<uintptr_t*>(weapon + OFFSET_weaponTrigger);
    const uintptr_t first = soldier + OFFSET_soldierControllable + OFFSET_controllableTriggers;
    return trigger == first ? 0 : trigger == first + 2 ? 1 : -1;
}

// True if a throwable could be thrown now: idle, with a round in the clip (trigger items such as the fusion cutter
// have no clip and are always there).
bool IsReady(uintptr_t weapon)
{
    const uintptr_t weaponClass = *reinterpret_cast<uintptr_t*>(weapon + OFFSET_weaponClass);
    if (!weaponClass || IsBadReadPtr(reinterpret_cast<void*>(weaponClass), OFFSET_weaponClassRoundsPerClip + 4))
    {
        return false;
    }
    const int rounds = static_cast<int>(
        static_cast<float>(*reinterpret_cast<int*>(weaponClass + OFFSET_weaponClassRoundsPerClip)) *
        *reinterpret_cast<float*>(weapon + OFFSET_weaponClipFraction));
    return *reinterpret_cast<int*>(weapon + OFFSET_weaponState) == 0 && rounds >= 1;
}

// The decorated C++ class name of a game object from its RTTI (".?AVWeaponGrenade@@"), or null. MSVC keeps a pointer
// to the CompleteObjectLocator just before each vtable; its +0xC is the TypeDescriptor, whose name starts at +8.
const char* TypeName(uintptr_t object)
{
    if (object < 0x10000 || IsBadReadPtr(reinterpret_cast<void*>(object), 4))
    {
        return nullptr; // small values are offsets from a null pointer (IsBadReadPtr would fault on them first)
    }
    const uintptr_t vtable = *reinterpret_cast<uintptr_t*>(object);
    if (vtable < 0x10000 || IsBadReadPtr(reinterpret_cast<void*>(vtable - 4), 4))
    {
        return nullptr;
    }
    const uintptr_t locator = *reinterpret_cast<uintptr_t*>(vtable - 4);
    if (locator < 0x10000 || IsBadReadPtr(reinterpret_cast<void*>(locator), 0x14))
    {
        return nullptr;
    }
    const uintptr_t descriptor = *reinterpret_cast<uintptr_t*>(locator + 0xC);
    if (descriptor < 0x10000 || IsBadReadPtr(reinterpret_cast<void*>(descriptor + 8), 64))
    {
        return nullptr;
    }
    const char* name = reinterpret_cast<const char*>(descriptor + 8);
    return strncmp(name, ".?AV", 4) == 0 ? name : nullptr;
}

// True if the object's C++ class is exactly this one (name without decoration).
bool IsType(uintptr_t object, const char* className)
{
    const char* name = TypeName(object);
    const size_t length = strlen(className);
    return name && strncmp(name + 4, className, length) == 0 && strcmp(name + 4 + length, "@@") == 0;
}

// The player's soldier (from the first-person renderable of view 0), or 0.
uintptr_t PlayerSoldier()
{
    if (!GameBuild::IsAvailable(GameBuild::Feature::FirstPersonModel))
    {
        return 0;
    }
    const uintptr_t renderable =
        *reinterpret_cast<uintptr_t*>(GameBuild::Absolute(GameBuild::Address::FirstPersonRenderables));
    if (!renderable || IsBadReadPtr(reinterpret_cast<void*>(renderable), OFFSET_firstPersonControllable + 4))
    {
        return 0;
    }
    const uintptr_t controllable = *reinterpret_cast<uintptr_t*>(renderable + OFFSET_firstPersonControllable);
    if (!controllable || IsBadReadPtr(reinterpret_cast<void*>(controllable - OFFSET_soldierControllable), SOLDIER_SIZE))
    {
        return 0;
    }
    // Holsters are for soldiers only: the droideka (EntityDroideka) has one weapon and its shield, nothing to swap
    // (idren2023, 2026-10-12), and other controllables have other layouts.
    static uintptr_t lastRefused = 0;
    if (!IsType(controllable, "EntitySoldier"))
    {
        if (controllable != lastRefused)
        {
            lastRefused = controllable;
            const char* name = TypeName(controllable);
            Log::Info("holster", "the player is a %s: no holsters", name ? name : "(unknown type)");
        }
        return 0;
    }
    return controllable - OFFSET_soldierControllable;
}

float ItemScale(uintptr_t weapon); // defined with the item models below
const char* ItemGrip(uintptr_t weapon);
bool IsFusionCutter(uintptr_t weapon);

// Works out which weapon goes to which place.
void MakeLayout(uintptr_t soldier, Layout* layout)
{
    *layout = Layout{};
    if (!soldier)
    {
        return;
    }
    layout->soldier = soldier;
    const Place order[2][2] = {{Place::RightHip, Place::LeftHip}, {Place::LeftChest, Place::RightChest}};
    int used[2] = {0, 0};
    for (int index = 0; index < WEAPON_COUNT; ++index)
    {
        const uintptr_t weapon = *reinterpret_cast<uintptr_t*>(soldier + OFFSET_soldierWeapons + 4 * index);
        if (!IsSoldierWeapon(weapon, soldier))
        {
            continue;
        }
        const int channel = WeaponChannel(weapon, soldier);
        if (channel < 0 || used[channel] >= 2)
        {
            continue;
        }
        const int place = static_cast<int>(order[channel][used[channel]++]);
        layout->weapons[place] = weapon;
        layout->indices[place] = index;
        layout->channels[place] = channel;
        layout->kinds[place] =
            IsType(weapon, "WeaponGrenade") || IsType(weapon, "WeaponDispenser") || IsType(weapon, "WeaponRemote")
                ? Kind::Throwable
                : Kind::Trigger;
        layout->scales[place] = ItemScale(weapon);
        layout->grips[place] = ItemGrip(weapon);
    }
    for (int place = 0; place < PLACE_COUNT; ++place)
    {
        const uintptr_t weapon = layout->weapons[place];
        layout->poses[place] = _poses[place];
        if (weapon && layout->channels[place] == 1 && !IsType(weapon, "WeaponGrenade"))
        {
            layout->poses[place].y -= CHEST_ITEM_DROP_CENTIMETERS;
        }
        if (weapon && layout->channels[place] == 1 && IsFusionCutter(weapon))
        {
            layout->poses[place] = BELT_CUTTER_POSE;
        }
        if (weapon && place == static_cast<int>(Place::RightHip) && IsType(weapon, "WeaponLauncher"))
        {
            layout->poses[place] = RIGHT_HIP_LAUNCHER_POSE;
        }
    }
    // The super battle droid's weapons are its arms: nothing to holster; the stick and the right grip change its
    // weapons (idren2023, 2026-10-12). Recognised by its first primary's first-person model, whose root node hash is
    // 0x8D95A295 (run156; nodes: that and hp_fire; the name is none of the mod tools' msh names, so the hash is used).
    for (int place = 0; place < PLACE_COUNT; ++place)
    {
        const uintptr_t weapon = layout->weapons[place];
        if (weapon && layout->channels[place] == 0 &&
            Zoom::HasFirstPersonNode(
                *reinterpret_cast<uintptr_t*>(weapon + OFFSET_weaponClass),
                SUPER_BATTLE_DROID_MODEL))
        {
            static uintptr_t saidFor = 0;
            if (saidFor != soldier)
            {
                saidFor = soldier;
                Log::Info("holster", "the player is a super battle droid: no holsters");
            }
            *layout = Layout{};
            _isDroidFound = true;
            return;
        }
    }
    const int primaryIndex = *reinterpret_cast<int*>(soldier + OFFSET_soldierWeaponIndex);
    if (primaryIndex >= 0 && primaryIndex < WEAPON_COUNT)
    {
        layout->primary = *reinterpret_cast<uintptr_t*>(soldier + OFFSET_soldierWeapons + 4 * primaryIndex);
    }
}

// The model to draw for a weapon, or null. Without a first-person model (the dispenser in hand showed nothing,
// run144), or with one that has no nodes (the CIS mine: invisible in its holster, headset run 13; its class logs no
// first-person nodes) the third-person one is used.
// The model of what a dispenser weapon puts down (PLAN 41, idren2023 2026-10-14: mines, health / ammo, the recon droid
// show as the item itself): the dispensed entity class (WeaponDispenserClass+0x1D0, also WeaponRemote's) is told apart
// by its C++ class, and the item's model is looked up by its ODF GeometryName in the game's model registry (0x64570,
// the lookup that fills WeaponClass+0x3C; offsets "Thrown ordnance"). Null when not a dispenser or not loaded.
// A loaded model by its ODF GeometryName, or null. The registry behind ModelByHash (0x64570, the lookup that fills
// WeaponClass+0x3C) is keyed by the game's name hash 0x3008F0, FNV-1a over the name with each byte | 0x20 (the one
// textures use, tools/texnames.py), not by HashName 0x3007F0's CRC: b180 used HashName and every lookup failed ("not
// loaded" in headset run 14's log). Found by disassembling the caller at 0x243A83 (0x300730 -> 0x3008F0).
void* ModelByName(const char* name)
{
    if (!GameBuild::IsAvailable(GameBuild::Feature::Holsters))
    {
        return nullptr;
    }
    unsigned hash = 0x811C9DC5u;
    for (const char* character = name; *character != 0; ++character)
    {
        hash = (hash ^ (static_cast<unsigned char>(*character) | 0x20u)) * 0x01000193u;
    }
    using ModelByHashFunction = void*(__cdecl*)(unsigned);
    void* model = reinterpret_cast<ModelByHashFunction>(GameBuild::Absolute(GameBuild::Address::ModelByHash))(hash);
    return model && !IsBadReadPtr(model, 8) && !IsBadReadPtr(*reinterpret_cast<void**>(model), 8) ? model : nullptr;
}

/// What a dispenser weapon puts down, from the dispensed entity class's C++ class (WeaponDispenserClass+0x1D0).
enum class Dispensed
{
    None,
    Mine, // EntityMineClass: mines and time bombs (the remote charge dispenser's ordnance is a "mine" too)
    Powerup,
    ReconDroid,
};

Dispensed DispensedKind(uintptr_t weapon)
{
    const uintptr_t weaponClass = *reinterpret_cast<uintptr_t*>(weapon + OFFSET_weaponClass);
    if (!weaponClass || !(IsType(weapon, "WeaponDispenser") || IsType(weapon, "WeaponRemote")))
    {
        return Dispensed::None;
    }
    const uintptr_t dispensed = *reinterpret_cast<uintptr_t*>(weaponClass + OFFSET_dispenserClassItem);
    if (!dispensed || IsBadReadPtr(reinterpret_cast<void*>(dispensed), 4))
    {
        return Dispensed::None;
    }
    return IsType(dispensed, "EntityMineClass")    ? Dispensed::Mine
           : IsType(dispensed, "PowerupItemClass") ? Dispensed::Powerup
           : IsType(dispensed, "EntityDroidClass") ? Dispensed::ReconDroid
                                                   : Dispensed::None;
}

// The time bomb: a mine dispenser whose own model is the seismic charge (every side's remote_charge_dispenser.odf has
// GeometryName com_weap_inf_seismic_mine; the mine dispensers have a landmine or none).
bool IsTimeBomb(uintptr_t weapon)
{
    void* const model = *reinterpret_cast<void**>(weapon + OFFSET_weaponThirdPersonModel);
    return DispensedKind(weapon) == Dispensed::Mine && model && model == ModelByName("com_weap_inf_seismic_mine");
}

// The model of what a dispenser weapon puts down (PLAN 41, idren2023 2026-10-14: mines, health / ammo, the recon droid
// show as the item itself), or null to draw the weapon's own. Time bombs keep their own (the seismic charge, right in
// headset run 14); mines show the placed landmine (the CIS and Imperial mine dispensers have no GeometryName).
void* DispensedModel(uintptr_t weapon)
{
    const Dispensed kind = DispensedKind(weapon);
    const char* name = kind == Dispensed::Powerup                       ? "com_item_powerup_dual"
                       : kind == Dispensed::ReconDroid                  ? "com_weap_probedroid"
                       : kind == Dispensed::Mine && !IsTimeBomb(weapon) ? "com_weap_inf_landmine_placed"
                                                                        : nullptr;
    if (!name)
    {
        return nullptr;
    }
    void* const model = ModelByName(name);
    static void* loggedModel = reinterpret_cast<void*>(1);
    if (model != loggedModel)
    {
        loggedModel = model;
        Log::Info("holster", "dispensed item model %s: %s", name, model ? "found" : "not loaded");
    }
    return model;
}

// A fusion cutter (any side's), by its third-person model.
bool IsFusionCutter(uintptr_t weapon)
{
    void* const model = *reinterpret_cast<void**>(weapon + OFFSET_weaponThirdPersonModel);
    if (!model)
    {
        return false;
    }
    for (const char* name : {"all_weap_inf_fusioncutter", "imp_weap_inf_fusioncutter", "rep_weap_inf_fusioncutter",
                             "cis_weap_inf_fusioncutter"})
    {
        if (model == ModelByName(name))
        {
            return true;
        }
    }
    return false;
}

// The size an item is drawn at in its holster and in the hand.
float ItemScale(uintptr_t weapon)
{
    if (IsType(weapon, "WeaponGrenade"))
    {
        return GRENADE_MODEL_SCALE;
    }
    switch (DispensedKind(weapon))
    {
    case Dispensed::Mine:
        return IsTimeBomb(weapon) ? TIME_BOMB_MODEL_SCALE : MINE_MODEL_SCALE;
    case Dispensed::Powerup:
        return ModelByName("com_item_powerup_dual") ? POWERUP_MODEL_SCALE : MINE_MODEL_SCALE;
    case Dispensed::ReconDroid:
        return ModelByName("com_weap_probedroid") ? RECON_DROID_MODEL_SCALE : 1.0f;
    default:
        return 1.0f;
    }
}

void* WeaponModel(uintptr_t weapon, bool isThirdPerson);

// BodyArm's grip for an item Holster draws in the hand as a model of its own at its item size (ItemScale; the grips
// divide that size out: tools/body_weapons.py item_ objects), or null when the drawn model is not that item's.
const char* ItemGrip(uintptr_t weapon)
{
    void* const model = WeaponModel(weapon, false);
    if (!model)
    {
        return nullptr;
    }
    if (IsType(weapon, "WeaponGrenade"))
    {
        return "com_grenadethermal"; // every side's (BodyArm checks the first-person model's node name)
    }
    switch (DispensedKind(weapon))
    {
    case Dispensed::Mine:
        return IsTimeBomb(weapon) ? (model == ModelByName("com_weap_inf_seismic_mine") ? "item_timebomb" : nullptr)
                                  : (model == ModelByName("com_weap_inf_landmine_placed") ? "item_landmine" : nullptr);
    case Dispensed::Powerup:
        return model == ModelByName("com_item_powerup_dual") ? "item_powerup" : nullptr;
    case Dispensed::ReconDroid:
        return model == ModelByName("com_weap_probedroid") ? "item_probedroid" : nullptr;
    default:
        return nullptr;
    }
}

void* WeaponModel(uintptr_t weapon, bool isThirdPerson)
{
    const uintptr_t weaponClass = *reinterpret_cast<uintptr_t*>(weapon + OFFSET_weaponClass);
    if (void* model = DispensedModel(weapon))
    {
        return model;
    }
    const bool hasFirstPerson = !isThirdPerson && weaponClass && Zoom::HasFirstPersonNodes(weaponClass);
    for (const uintptr_t offset :
         {hasFirstPerson ? OFFSET_weaponFirstPersonModel : OFFSET_weaponThirdPersonModel,
          OFFSET_weaponThirdPersonModel})
    {
        void* model = *reinterpret_cast<void**>(weapon + offset);
        if (model && !IsBadReadPtr(model, 8) && !IsBadReadPtr(*reinterpret_cast<void**>(model), 8))
        {
            return model;
        }
    }
    return nullptr;
}

void DrawModel(void* model, const float* matrix, unsigned flags, float scale = 1.0f)
{
    float scaled[16];
    if (scale != 1.0f)
    {
        memcpy(scaled, matrix, sizeof(scaled));
        for (int element = 0; element < 12; ++element)
        {
            scaled[element] *= scale;
        }
        matrix = scaled;
    }
    void* light = reinterpret_cast<void*>(GameBuild::Absolute(GameBuild::Address::ModelLight));
    const auto draw = (*reinterpret_cast<ModelDrawFunction**>(model))[1];
    draw(model, matrix, nullptr, light, flags | MODEL_DRAW_FLAGS, 0);
}

// The place a gesture zone stands for, or Place::Count.
Place PlaceOfZone(Gesture::Zone zone)
{
    switch (zone)
    {
    case Gesture::Zone::HipRight:
        return Place::RightHip;
    case Gesture::Zone::HipLeft:
        return Place::LeftHip;
    case Gesture::Zone::ChestLeft:
        return Place::LeftChest;
    case Gesture::Zone::ChestRight:
        return Place::RightChest;
    default:
        return Place::Count;
    }
}

// The empty hand has entered a place (b143, headset run 12): the weapon change starts now, so that the grab is quick.
// The hand stays empty; Take follows when the grip closes.
void Prepare(int place)
{
    const uintptr_t weapon = _layout.weapons[place];
    if (!weapon || !GameBuild::IsAvailable(GameBuild::Feature::Holsters))
    {
        return;
    }
    if (_layout.channels[place] == 0)
    {
        if (_wantedPrimary != _layout.indices[place])
        {
            _wantedPrimary = _layout.indices[place];
            _primarySteps = 0;
            Log::Info("holster", "empty hand at place %d: primary weapon %d prepared", place, _wantedPrimary);
        }
    }
    else if (_layout.kinds[place] != Kind::Throwable || IsReady(weapon))
    {
        if (!Net::IsNetGame()) // a direct request never reaches the host: Take steps by the game's own key there
        {
            const auto requestWeapon =
                reinterpret_cast<RequestWeaponFunction>(GameBuild::Absolute(GameBuild::Address::RequestWeapon));
            requestWeapon(
                reinterpret_cast<void*>(_layout.soldier + OFFSET_soldierControllable), _layout.indices[place]);
            Log::Info(
                "holster", "empty hand at place %d: secondary weapon %d requested", place, _layout.indices[place]);
        }
    }
}

// Takes the weapon of a place into the right hand, if there is one to take.
void Take(int place)
{
    const uintptr_t weapon = _layout.weapons[place];
    if (!weapon || !GameBuild::IsAvailable(GameBuild::Feature::Holsters))
    {
        return;
    }
    const bool isPrimary = _layout.channels[place] == 0;
    if (!isPrimary && _layout.kinds[place] == Kind::Throwable && !IsReady(weapon))
    {
        Log::Info("holster", "place %d: nothing to take yet (weapon state %d)", place,
                  *reinterpret_cast<int*>(weapon + OFFSET_weaponState));
        return; // nothing there to take yet
    }
    if (isPrimary)
    {
        _wantedPrimary = _layout.indices[place];
        _primarySteps = 0;
    }
    else
    {
        if (!Net::IsNetGame()) // in a network game StepSecondary's key taps do it: they reach the host
        {
            const auto requestWeapon =
                reinterpret_cast<RequestWeaponFunction>(GameBuild::Absolute(GameBuild::Address::RequestWeapon));
            requestWeapon(
                reinterpret_cast<void*>(_layout.soldier + OFFSET_soldierControllable), _layout.indices[place]);
        }
        _wantedSecondary = _layout.indices[place];
        _secondarySteps = 0;
        _lastSecondaryStepTick = GetTickCount();
    }
    _holding = isPrimary ? Holding::Primary : Holding::Secondary;
    _heldPlace = place;
    Feedback::Pulse(0, 0.04f, 0.5f);
    Log::Info(
        "holster",
        "right hand takes %s weapon %d from place %d",
        isPrimary ? "primary" : "secondary",
        _layout.indices[place],
        place);
}

double NowSeconds()
{
    static LARGE_INTEGER frequency = {};
    if (!frequency.QuadPart)
    {
        QueryPerformanceFrequency(&frequency);
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart);
}

float _testThrowSpeed = 0; // test: the swing's speed for a throw, 0 = as measured (SetTestThrowSpeed)

// The swing of the right hand over the last SWING_SECONDS, as a world direction; false if too slow or unknown.
bool SwingDirection(float* direction, float* metersPerSecond)
{
    *metersPerSecond = 0;
    const HandSample& newest = _samples[(_nextSample + SAMPLE_COUNT - 1) % SAMPLE_COUNT];
    const HandSample* oldest = nullptr;
    for (int step = 2; step <= SAMPLE_COUNT; ++step)
    {
        const HandSample& sample = _samples[(_nextSample + SAMPLE_COUNT - step) % SAMPLE_COUNT];
        if (sample.seconds <= 0)
        {
            break;
        }
        oldest = &sample;
        if (newest.seconds - sample.seconds >= SWING_SECONDS)
        {
            break;
        }
    }
    float base[16];
    if (!oldest || newest.seconds - oldest->seconds < 0.02 || !Aim::GetBaseFrame(base))
    {
        return false;
    }
    float motion[3];
    for (int axis = 0; axis < 3; ++axis)
    {
        motion[axis] = newest.position[axis] - oldest->position[axis];
    }
    const float length = sqrtf(motion[0] * motion[0] + motion[1] * motion[1] + motion[2] * motion[2]);
    *metersPerSecond = length / static_cast<float>(newest.seconds - oldest->seconds);
    if (_testThrowSpeed > 0 && length > 1e-4f)
    {
        *metersPerSecond = _testThrowSpeed;
    }
    if (*metersPerSecond < SWING_MINIMUM_METERS_PER_SECOND)
    {
        return false;
    }
    for (int axis = 0; axis < 3; ++axis) // rest space to world: rotation rows of the base frame
    {
        direction[axis] = (motion[0] * base[axis] + motion[1] * base[4 + axis] + motion[2] * base[8 + axis]) / length;
    }
    return true;
}

// Steps the primary weapon toward the wanted one: one wheel notch at a time.
void StepPrimary()
{
    if (_wantedPrimary < 0 || !_layout.soldier)
    {
        return;
    }
    const int current = *reinterpret_cast<int*>(_layout.soldier + OFFSET_soldierWeaponIndex);
    if (current == _wantedPrimary || _primarySteps >= MAXIMUM_STEPS)
    {
        if (current != _wantedPrimary)
        {
            Log::Info("holster", "primary weapon %d not reached (still %d)", _wantedPrimary, current);
        }
        _wantedPrimary = -1;
        return;
    }
    const DWORD now = GetTickCount();
    if (_primarySteps && now - _lastStepTick < STEP_MILLISECONDS)
    {
        return;
    }
    Input::MouseWheel(WHEEL_DELTA); // wheel up = next primary weapon
    _lastStepTick = now;
    ++_primarySteps;
}

// True if the soldier's current secondary weapon is the one taken.
bool IsSecondarySelected()
{
    return _wantedSecondary >= 0 &&
           *reinterpret_cast<int*>(_layout.soldier + OFFSET_soldierWeaponIndex + 4) == _wantedSecondary;
}

// Guards the secondary weapon change: if RequestWeapon has not made the taken weapon current, the "next secondary"
// key is tapped until it is.
void StepSecondary()
{
    const DWORD now = GetTickCount();
    if (_keyUpTick && static_cast<LONG>(now - _keyUpTick) >= 0)
    {
        Input::Key(KEY_G, false);
        _keyUpTick = 0;
    }
    if (_wantedSecondary < 0 || !_layout.soldier || _holding != Holding::Secondary || IsSecondarySelected() ||
        _keyUpTick || now - _lastSecondaryStepTick < STEP_MILLISECONDS)
    {
        return;
    }
    if (_secondarySteps >= MAXIMUM_STEPS)
    {
        Log::Info(
            "holster",
            "secondary weapon %d not reached (still %d)",
            _wantedSecondary,
            *reinterpret_cast<int*>(_layout.soldier + OFFSET_soldierWeaponIndex + 4));
        _wantedSecondary = -1;
        return;
    }
    Input::Key(KEY_G, true);
    _keyUpTick = (now + KEY_TAP_MILLISECONDS) | 1;
    _lastSecondaryStepTick = now;
    ++_secondarySteps;
    Log::Info("holster", "secondary weapon %d: next secondary (step %d)", _wantedSecondary, _secondarySteps);
}

// The game's function a patched slot held for this object's vtable, or 0.
uintptr_t OriginalOf(const void* object, const PatchedSlot* slots, int count)
{
    const uintptr_t table = *static_cast<const uintptr_t*>(object);
    for (int index = 0; index < count; ++index)
    {
        if (slots[index].table == table)
        {
            return slots[index].original;
        }
    }
    return 0;
}

bool IsStrengthFor(uintptr_t weapon)
{
    return weapon && weapon == _strengthWeapon && GetTickCount() - _strengthTick < THROW_STRENGTH_MILLISECONDS;
}

using OrdnanceCreateFunction = void*(__fastcall*)(void* ordnanceClass, void* edx, uint8_t* descriptor);

// Network game, test: the grenades created shortly after the player's throw are followed and logged (`NG` lines:
// milliseconds since creation, position, velocity), to compare the host's throw with the hand's.
struct TracedOrdnance
{
    uintptr_t object, vtable;
    DWORD createdTick, loggedTick;
};
constexpr int TRACED_ORDNANCE_CAPACITY = 4;
TracedOrdnance _tracedOrdnance[TRACED_ORDNANCE_CAPACITY];
constexpr uintptr_t OFFSET_ordnancePosition = 0x40, OFFSET_ordnanceVelocity = 0xB8, OFFSET_ordnanceGravity = 0xE8;
constexpr DWORD ORDNANCE_TRACE_MILLISECONDS = 5000, ORDNANCE_TRACE_WINDOW_MILLISECONDS = 2000;

void TraceOrdnance()
{
    for (TracedOrdnance& traced : _tracedOrdnance)
    {
        if (!traced.object)
        {
            continue;
        }
        const DWORD now = GetTickCount();
        if (now - traced.createdTick > ORDNANCE_TRACE_MILLISECONDS ||
            IsBadReadPtr(reinterpret_cast<void*>(traced.object), 0xF8) ||
            *reinterpret_cast<uintptr_t*>(traced.object) != traced.vtable)
        {
            Log::Info("holster", "NG %p ends after %u ms", reinterpret_cast<void*>(traced.object),
                      static_cast<unsigned>(now - traced.createdTick));
            traced.object = 0;
            continue;
        }
        if (now - traced.loggedTick < 40)
        {
            continue;
        }
        traced.loggedTick = now;
        const float* position = reinterpret_cast<const float*>(traced.object + OFFSET_ordnancePosition);
        const float* velocity = reinterpret_cast<const float*>(traced.object + OFFSET_ordnanceVelocity);
        Log::Info(
            "holster", "NG %p %u ms at %.2f %.2f %.2f velocity %.2f %.2f %.2f", reinterpret_cast<void*>(traced.object),
            static_cast<unsigned>(now - traced.createdTick), static_cast<double>(position[0]),
            static_cast<double>(position[1]), static_cast<double>(position[2]), static_cast<double>(velocity[0]),
            static_cast<double>(velocity[1]), static_cast<double>(velocity[2]));
    }
}

// Replaces slot 2 (create) of the thrown grenade's ordnance class (OrdnanceGrenadeClass, OrdnanceStickyClass...):
// for the player's throw, the descriptor's direction is scaled by the throw strength, which scales the launch speed
// (ordnance+0xB8 = direction x Velocity at 0x1D4684; offsets "Thrown ordnance"). The game's strength (D+0x18) only
// picks a charge level, and stock grenades have none. thiscall: ecx = the ordnance class, one stack argument (ret 4),
// returns the ordnance; game thread.
void* __fastcall OrdnanceCreateHook(void* ordnanceClass, void* edx, uint8_t* descriptor)
{
    const auto original =
        reinterpret_cast<OrdnanceCreateFunction>(OriginalOf(ordnanceClass, _createSlots, _createSlotCount));
    if (descriptor && IsStrengthFor(*reinterpret_cast<uintptr_t*>(descriptor + OFFSET_descriptorWeapon)))
    {
        {
            FpGuard guard;
            float* direction = reinterpret_cast<float*>(descriptor + OFFSET_descriptorDirection);
            for (int axis = 0; axis < 3; ++axis)
            {
                direction[axis] *= _throwStrength;
            }
        }
        Log::Info("holster", "grenade thrown at strength %.2f", static_cast<double>(_throwStrength));
        _strengthWeapon = 0;
    }
    void* const ordnance = original ? original(ordnanceClass, edx, descriptor) : nullptr;
    if (ordnance && descriptor && Net::IsNetGame() && GetTickCount() - _thrownTick < ORDNANCE_TRACE_WINDOW_MILLISECONDS)
    {
        for (TracedOrdnance& traced : _tracedOrdnance)
        {
            if (!traced.object)
            {
                const uintptr_t object = reinterpret_cast<uintptr_t>(ordnance);
                traced = TracedOrdnance{object, *reinterpret_cast<uintptr_t*>(object), GetTickCount(), 0};
                const float* from = reinterpret_cast<const float*>(descriptor);
                const float* direction = reinterpret_cast<const float*>(descriptor + OFFSET_descriptorDirection);
                const float* velocity = reinterpret_cast<const float*>(object + OFFSET_ordnanceVelocity);
                Log::Info(
                    "holster",
                    "NG %p created in the %s world %u ms after the throw: from %.2f %.2f %.2f direction %.3f %.3f %.3f "
                    "velocity %.2f %.2f %.2f gravity %.3f (class: velocity %.2f gravity %.3f)",
                    ordnance, Net::IsClientWorldActive() ? "client" : "host",
                    static_cast<unsigned>(GetTickCount() - _thrownTick), static_cast<double>(from[0]),
                    static_cast<double>(from[1]), static_cast<double>(from[2]), static_cast<double>(direction[0]),
                    static_cast<double>(direction[1]), static_cast<double>(direction[2]),
                    static_cast<double>(velocity[0]), static_cast<double>(velocity[1]),
                    static_cast<double>(velocity[2]),
                    static_cast<double>(*reinterpret_cast<const float*>(object + OFFSET_ordnanceGravity)),
                    static_cast<double>(*reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(ordnanceClass) + 0xAC)),
                    static_cast<double>(*reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(ordnanceClass) + 0xDC)));
                break;
            }
        }
    }
    return ordnance;
}

// Points one vtable slot of an object's class at a hook, once per vtable (main thread, between the game's updates).
void PatchSlot(uintptr_t object, int slot, void* hook, PatchedSlot* slots, int& count, const char* what)
{
    if (!object || IsBadReadPtr(reinterpret_cast<void*>(object), 4) ||
        OriginalOf(reinterpret_cast<void*>(object), slots, count))
    {
        return;
    }
    const uintptr_t table = *reinterpret_cast<uintptr_t*>(object);
    uintptr_t* const entry = reinterpret_cast<uintptr_t*>(table) + slot;
    DWORD oldProtection = 0;
    if (count >= PATCHED_SLOT_CAPACITY || IsBadReadPtr(entry, 4) ||
        !VirtualProtect(entry, 4, PAGE_READWRITE, &oldProtection))
    {
        return;
    }
    slots[count++] = PatchedSlot{table, *entry};
    InterlockedExchange(reinterpret_cast<volatile LONG*>(entry), static_cast<LONG>(reinterpret_cast<uintptr_t>(hook)));
    VirtualProtect(entry, 4, oldProtection, &oldProtection);
    Log::Info("holster", "throw strength: %s slot %d of vtable %08x hooked", what, slot, static_cast<unsigned>(table));
}

// A thrown object's flight (no drag: run 704) from a point at a velocity under gravity, followed with the game's ray
// cast until it meets something. @return true when it did; point and seconds are then the place and time of the
// impact, else where it was after FLIGHT_SECONDS.
constexpr float FLIGHT_SECONDS = 5.0f, FLIGHT_STEP_SECONDS = 0.04f;
bool FlyArc(
    const float* from, const float* velocity, float gravity, float* point, float* seconds, float* arrival = nullptr)
{
    float position[3] = {from[0], from[1], from[2]};
    float speed[3] = {velocity[0], velocity[1], velocity[2]};
    for (float time = 0; time < FLIGHT_SECONDS; time += FLIGHT_STEP_SECONDS)
    {
        // one step: the straight piece between the arc's points at its start and its end
        const float step[3] = {speed[0] * FLIGHT_STEP_SECONDS,
                               (speed[1] - 0.5f * gravity * FLIGHT_STEP_SECONDS) * FLIGHT_STEP_SECONDS,
                               speed[2] * FLIGHT_STEP_SECONDS};
        const float length = sqrtf(step[0] * step[0] + step[1] * step[1] + step[2] * step[2]);
        if (length < 1e-4f)
        {
            break;
        }
        const float direction[3] = {step[0] / length, step[1] / length, step[2] / length};
        float freeMeters = length;
        if (Net::CastAimRay(position, direction, length, &freeMeters))
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                point[axis] = position[axis] + direction[axis] * freeMeters;
            }
            *seconds = time + FLIGHT_STEP_SECONDS * freeMeters / length;
            if (arrival)
            {
                memcpy(arrival, speed, sizeof(speed));
            }
            return true;
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            position[axis] += step[axis];
        }
        speed[1] -= gravity * FLIGHT_STEP_SECONDS;
    }
    memcpy(point, position, sizeof(position));
    *seconds = FLIGHT_SECONDS;
    if (arrival)
    {
        arrival[0] = arrival[1] = arrival[2] = 0;
    }
    return false;
}

// Where a grenade comes to rest after its first impact, on ground taken as level. Each bounce keeps a third of the
// speed off the ground, and friction takes FRICTION x (the speed off the ground, before plus after) from the speed
// along it (runs 705-706: 23.4 along / 10.9 down became 7.5 / 3.6 up, then 7.5 / 4.2 became 1.5 / 1.4; a steep
// throw, 18 along / 19.5 down, stayed where it hit). A full-speed flat throw rolled out 5.0 m; this gives 5.0.
constexpr float BOUNCE_KEPT = 0.33f, BOUNCE_FRICTION = 1.08f;
void RestPoint(const float* impact, const float* arrival, float gravity, float* rest)
{
    memcpy(rest, impact, 3 * sizeof(float));
    float along = sqrtf(arrival[0] * arrival[0] + arrival[2] * arrival[2]);
    if (along < 0.01f)
    {
        return;
    }
    const float heading[2] = {arrival[0] / along, arrival[2] / along};
    float down = fabsf(arrival[1]);
    for (int bounce = 0; bounce < 8 && along > 0.05f && down > 0.2f; ++bounce)
    {
        const float up = BOUNCE_KEPT * down;
        along -= BOUNCE_FRICTION * (down + up);
        if (along <= 0)
        {
            break;
        }
        const float hop = along * 2 * up / gravity;
        rest[0] += heading[0] * hop;
        rest[2] += heading[1] * hop;
        down = up;
    }
}

// Network game, a grenade let go: the host throws from its own soldier, along that soldier's aim (plus the weapon's
// ShotElevate), at the grenade's full speed, about 0.3 s after the release; nothing of the hand's swing reaches it.
// So the place where the hand's throw would land is predicted (the launch the client world shows: from the hand,
// along the swing, at the swing's strength) together with where it comes to rest, and the soldier is turned to the
// aim whose stock throw comes to rest there (Controls steers it, Aim::SetNetThrowAim). Of the two arcs that reach a
// point, the one whose flight time is nearer the hand's is taken, the other when the first meets something on its way
// (idren2023's decision, netcode.md).
constexpr uintptr_t OFFSET_grenadeClassShotElevate = 0x1D8, OFFSET_ordnanceClassVelocity = 0xAC,
                    OFFSET_ordnanceClassGravity = 0xDC;
constexpr float NET_THROW_PITCH_MINIMUM = -0.8f, NET_THROW_PITCH_MAXIMUM = 1.1f; // a soldier's pitch limits
constexpr unsigned NET_THROW_AIM_MILLISECONDS = 700; // the host fires 0.3 s after the release (run 704: 280-300 ms)
void SolveNetThrow(uintptr_t weapon, const float* swing, float strength)
{
    if (!IsType(weapon, "WeaponGrenade"))
    {
        return; // mines and dispensers: a short fixed toss along the aim, left as it is
    }
    const uintptr_t weaponClass = *reinterpret_cast<uintptr_t*>(weapon + OFFSET_weaponClass);
    const uintptr_t ordnanceClass =
        weaponClass ? *reinterpret_cast<uintptr_t*>(weaponClass + OFFSET_weaponClassOrdnance) : 0;
    float hand[3], handDirection[3], origin[3];
    if (!ordnanceClass || IsBadReadPtr(reinterpret_cast<void*>(ordnanceClass), 0xE0) ||
        !Aim::GetFireLine(hand, handDirection) || !Aim::GetStockFireOrigin(origin))
    {
        return;
    }
    FpGuard guard;
    const float elevate = *reinterpret_cast<const float*>(weaponClass + OFFSET_grenadeClassShotElevate);
    const float fullSpeed = *reinterpret_cast<const float*>(ordnanceClass + OFFSET_ordnanceClassVelocity);
    const float gravity = -*reinterpret_cast<const float*>(ordnanceClass + OFFSET_ordnanceClassGravity);
    if (!(fullSpeed > 1 && fullSpeed < 200 && gravity > 0.5f && gravity < 100 && elevate > -1 && elevate < 1))
    {
        Log::Info("holster", "net throw: unexpected class values (speed %.2f gravity %.2f elevate %.2f), not solved",
                  static_cast<double>(fullSpeed), static_cast<double>(gravity), static_cast<double>(elevate));
        return;
    }
    // The hand's throw as the client world launches it: normalize(direction + (0, ShotElevate, 0)) x speed x strength.
    const float* along = swing ? swing : handDirection;
    float launch[3] = {along[0], along[1] + elevate, along[2]};
    const float launchLength = sqrtf(launch[0] * launch[0] + launch[1] * launch[1] + launch[2] * launch[2]);
    if (launchLength < 1e-3f)
    {
        return;
    }
    for (float& part : launch)
    {
        part *= fullSpeed * strength / launchLength;
    }
    float landing[3], handArrival[3], handRest[3], handSeconds = 0;
    const bool isLanding = FlyArc(hand, launch, gravity, landing, &handSeconds, handArrival);
    RestPoint(landing, handArrival, gravity, handRest);
    // The host's throw is aimed along the line from its fire point to that resting place, at a point on it (part 1 =
    // the resting place itself) that is moved until the host's grenade, which arrives faster and rolls on farther,
    // is predicted to rest where the hand's does. Nearer than about 8 m a full-speed throw cannot come to rest.
    const float aside[2] = {handRest[0] - origin[0], handRest[2] - origin[2]};
    const float reach = sqrtf(aside[0] * aside[0] + aside[1] * aside[1]);
    if (reach < 0.3f)
    {
        return; // dropped at the feet: no direction to solve
    }
    const float heading[2] = {aside[0] / reach, aside[1] / reach};
    const float speedSquared = fullSpeed * fullSpeed;
    float bestAim[3] = {0, 0, 0}, bestOff = 1e9f, bestPitch = 0, bestLanding[3] = {0, 0, 0}, bestRest[3] = {0, 0, 0};
    // (found by halving: the resting place moves out with the point, first tried at part 1)
    float part = 1.0f, low = 0, high = 0, targetHeight = handRest[1];
    for (int round = 0; round < 9; ++round)
    {
        const float distance = part * reach < 1.0f ? 1.0f : part * reach;
        // the ground under the target point (a ray straight down from above it)
        const float above[3] = {origin[0] + heading[0] * distance, (handRest[1] > origin[1] ? handRest[1] : origin[1]) + 2.0f,
                                origin[2] + heading[1] * distance};
        const float down[3] = {0, -1, 0};
        float fall = 0;
        if (Net::CastAimRay(above, down, 15.0f, &fall))
        {
            targetHeight = above[1] - fall;
        }
        const float target[3] = {above[0], targetHeight, above[2]};
        const float height = targetHeight - origin[1];
        const float root =
            speedSquared * speedSquared - gravity * (gravity * distance * distance + 2 * height * speedSquared);
        float angles[2];
        int angleCount = 1;
        if (root < 0)
        {
            angles[0] = atanf(speedSquared / (gravity * distance)); // out of reach: the farthest throw that way
        }
        else
        {
            // two arcs reach the point: the one whose flight time is nearer the hand's first
            angles[0] = atanf((speedSquared - sqrtf(root)) / (gravity * distance));
            angles[1] = atanf((speedSquared + sqrtf(root)) / (gravity * distance));
            angleCount = 2;
            const float seconds[2] = {distance / (fullSpeed * cosf(angles[0])), distance / (fullSpeed * cosf(angles[1]))};
            if (fabsf(seconds[1] - handSeconds) < fabsf(seconds[0] - handSeconds))
            {
                const float first = angles[0];
                angles[0] = angles[1];
                angles[1] = first;
            }
        }
        float roundRest[3] = {0, 0, 0};
        for (int index = 0; index < angleCount; ++index)
        {
            // The game launches along normalize(aim + (0, ShotElevate, 0)): the aim's pitch that gives the angle.
            float sine = elevate * cosf(angles[index]);
            sine = sine > 1 ? 1 : sine < -1 ? -1 : sine;
            float pitch = angles[index] - asinf(sine);
            pitch = pitch > NET_THROW_PITCH_MAXIMUM   ? NET_THROW_PITCH_MAXIMUM
                    : pitch < NET_THROW_PITCH_MINIMUM ? NET_THROW_PITCH_MINIMUM
                                                      : pitch;
            const float aim[3] = {cosf(pitch) * heading[0], sinf(pitch), cosf(pitch) * heading[1]};
            float hostLaunch[3] = {aim[0], aim[1] + elevate, aim[2]};
            const float hostLength =
                sqrtf(hostLaunch[0] * hostLaunch[0] + hostLaunch[1] * hostLaunch[1] + hostLaunch[2] * hostLaunch[2]);
            for (float& axis : hostLaunch)
            {
                axis *= fullSpeed / hostLength;
            }
            float hostLanding[3], hostArrival[3], hostRest[3], hostSeconds = 0;
            FlyArc(origin, hostLaunch, gravity, hostLanding, &hostSeconds, hostArrival);
            RestPoint(hostLanding, hostArrival, gravity, hostRest);
            const float miss = sqrtf(
                (hostLanding[0] - target[0]) * (hostLanding[0] - target[0]) +
                (hostLanding[1] - target[1]) * (hostLanding[1] - target[1]) +
                (hostLanding[2] - target[2]) * (hostLanding[2] - target[2]));
            const float off = sqrtf(
                (hostRest[0] - handRest[0]) * (hostRest[0] - handRest[0]) +
                (hostRest[1] - handRest[1]) * (hostRest[1] - handRest[1]) +
                (hostRest[2] - handRest[2]) * (hostRest[2] - handRest[2]));
            memcpy(roundRest, hostRest, sizeof(hostRest));
            if (off < bestOff)
            {
                bestOff = off;
                bestPitch = pitch;
                memcpy(bestAim, aim, sizeof(aim));
                memcpy(bestLanding, hostLanding, sizeof(hostLanding));
                memcpy(bestRest, hostRest, sizeof(hostRest));
            }
            if (miss < 1.0f + 0.1f * distance)
            {
                break; // this arc gets to the point; the other one only when this one met something on its way
            }
        }
        // how far the host's grenade rests beyond (+) or short of (-) the hand's, along the heading
        const float beyond = (roundRest[0] - handRest[0]) * heading[0] + (roundRest[2] - handRest[2]) * heading[1];
        if (fabsf(beyond) < 0.15f)
        {
            break;
        }
        if (round == 0)
        {
            low = beyond > 0 ? (reach > 1.0f ? 1.0f / reach : 1.0f) : 1.0f;
            high = beyond > 0 ? 1.0f : 2.0f;
        }
        else if (beyond > 0)
        {
            high = part;
        }
        else
        {
            low = part;
        }
        part = 0.5f * (low + high);
    }
    Aim::SetNetThrowAim(bestAim, NET_THROW_AIM_MILLISECONDS);
    Log::Info(
        "holster",
        "net throw: hand's throw (strength %.2f, %s) %s at %.2f %.2f %.2f after %.2f s, rest at %.2f %.2f %.2f, %.1f m "
        "from the soldier's fire point; soldier aimed at pitch %.1f degrees, heading %.1f: host's throw predicted to "
        "land at %.2f %.2f %.2f and to rest at %.2f %.2f %.2f, %.2f m from the hand's",
        static_cast<double>(strength), swing ? "along the swing" : "along the hand", isLanding ? "lands" : "still flies",
        static_cast<double>(landing[0]), static_cast<double>(landing[1]), static_cast<double>(landing[2]),
        static_cast<double>(handSeconds), static_cast<double>(handRest[0]), static_cast<double>(handRest[1]),
        static_cast<double>(handRest[2]), static_cast<double>(reach), static_cast<double>(bestPitch * 57.29578f),
        static_cast<double>(atan2f(heading[0], heading[1]) * 57.29578f), static_cast<double>(bestLanding[0]),
        static_cast<double>(bestLanding[1]), static_cast<double>(bestLanding[2]), static_cast<double>(bestRest[0]),
        static_cast<double>(bestRest[1]), static_cast<double>(bestRest[2]), static_cast<double>(bestOff));
}

// At a throw: the strength from the swing speed, and the hook for this weapon's kind of shot.
void SetThrowStrength(uintptr_t weapon, float metersPerSecond, bool isUnderhand)
{
    float strength =
        metersPerSecond / (isUnderhand ? FULL_UNDERHAND_THROW_METERS_PER_SECOND : FULL_THROW_METERS_PER_SECOND);
    strength = strength < THROW_MINIMUM_STRENGTH ? THROW_MINIMUM_STRENGTH : (strength > 1 ? 1 : strength);
    if (IsType(weapon, "WeaponGrenade"))
    {
        const uintptr_t weaponClass = *reinterpret_cast<uintptr_t*>(weapon + OFFSET_weaponClass);
        const uintptr_t ordnanceClass =
            weaponClass ? *reinterpret_cast<uintptr_t*>(weaponClass + OFFSET_weaponClassOrdnance) : 0;
        PatchSlot(
            ordnanceClass,
            ORDNANCE_CREATE_SLOT,
            reinterpret_cast<void*>(&OrdnanceCreateHook),
            _createSlots,
            _createSlotCount,
            "ordnance create");
    }
    // Dispensers (mines, time bombs, health / ammo) throw as stock since b353: their stock throw is a fixed short toss
    // (MinStrength 0.3 to MaxStrength 0.4 of the item's Velocity, no hold), and scaling it by the swing wrote the
    // game's strength 0 for a slow one, which left the mine floating where it was let go, sinking at about 1 cm/s
    // (idren2023, run390).
    _throwStrength = strength;
    if (Net::IsNetGame())
    {
        // The host throws at its own full strength. The client world's grenade (the one drawn here; its weapon is
        // the one this strength is for) still flies as the hand threw it, and SolveNetThrow aims the host's throw at
        // the place where that one lands.
        Log::Info("holster", "net game: throw strength %.2f for the client world's grenade only (hand at %.1f m/s)",
                  static_cast<double>(strength), static_cast<double>(metersPerSecond));
    }
    _strengthTick = GetTickCount();
    _strengthWeapon = weapon;
    Log::Info(
        "holster",
        "throw strength %.2f (hand at %.1f m/s%s)",
        static_cast<double>(strength),
        static_cast<double>(metersPerSecond),
        isUnderhand ? ", underhand" : "");
}

// A grenade leaves the hand when the game's release timer (weapon+0xB8) reaches InitialSalvoDelay (class+0x1D4,
// 0.3 s for the thermal detonator): it appeared late, after the hand had moved on (headset run 12, PLAN 25). Once the
// game has taken the release (fired flag weapon+0x120 cleared, duration +0xBC set), the timer is moved to the delay,
// so it fires at its next update with its own ammunition and heat handling (offsets "Thrown ordnance"). Only the
// player's weapon is touched; the class value (shared with the AI) is not.
// Dispensers the same way (headset run 15: mines appeared late; the health / ammo dispenser's ODF sets
// InitialSalvoDelay 0, the mine dispensers leave the class default): release (slot 27 0x249ED0) clears the fired flag
// +0x120 and sets the timer +0xB8 to -1; the update (slot 28 0x24A850) restarts it at 0, adds the frame time and fires
// once it reaches class+0x1DC (disassembly 2026-10-17).
void SkipReleaseDelay()
{
    const uintptr_t weapon = _thrownWeapon;
    if (!weapon || Net::IsNetGame()) // (the host keeps its own release delay)
    {
        return;
    }
    if (GetTickCount() - _thrownTick > THROW_SKIP_WINDOW_MILLISECONDS || !IsSoldierWeapon(weapon, _layout.soldier))
    {
        _thrownWeapon = 0;
        return;
    }
    if (_throwUntilTick)
    {
        return; // secondary fire still held: not released yet
    }
    const bool isDispenser = IsType(weapon, "WeaponDispenser");
    const uintptr_t delayOffset = isDispenser ? OFFSET_dispenserClassSalvoDelay : OFFSET_grenadeClassSalvoDelay;
    const uintptr_t weaponClass = *reinterpret_cast<uintptr_t*>(weapon + OFFSET_weaponClass);
    if (!weaponClass || IsBadReadPtr(reinterpret_cast<void*>(weaponClass + delayOffset), 4))
    {
        _thrownWeapon = 0;
        return;
    }
    const bool isFired = *reinterpret_cast<uint8_t*>(weapon + OFFSET_grenadeFired) != 0;
    float& timer = *reinterpret_cast<float*>(weapon + OFFSET_grenadeTimer);
    const float delay = *reinterpret_cast<float*>(weaponClass + delayOffset);
    const float duration = *reinterpret_cast<float*>(weapon + OFFSET_grenadeDuration);
    // A grenade's release sets the duration; a dispenser's clears the fired flag alone (its duration is set after a
    // shot). Released, the fired flag stays clear until the shot.
    const bool isReleased = isDispenser || duration > 0;
    if (!isFired && isReleased && timer < delay && delay > 0 && delay < 2.0f)
    {
        Log::Info(
            "holster",
            "%s release delay skipped (%.2f of %.2f s, %u ms after letting go)",
            isDispenser ? "dispenser" : "grenade",
            static_cast<double>(timer),
            static_cast<double>(delay),
            static_cast<unsigned>(GetTickCount() - _thrownTick));
        timer = delay;
        _thrownWeapon = 0;
    }
}

// ---- Manual reload (idren2023 2026-10-20, PLAN 58 s) ----

// A weapon with clips the player reloads (finite RoundsPerClip; heat weapons have 0): its reserve, or -1 if not one.
float ManualReloadReserve(uintptr_t weapon)
{
    if (!weapon || IsBadReadPtr(reinterpret_cast<void*>(weapon), OFFSET_weaponState + 4))
    {
        return -1;
    }
    const uintptr_t weaponClass = *reinterpret_cast<uintptr_t*>(weapon + OFFSET_weaponClass);
    if (!weaponClass || IsBadReadPtr(reinterpret_cast<void*>(weaponClass), OFFSET_weaponClassRoundsPerClip + 4))
    {
        return -1;
    }
    const int rounds = *reinterpret_cast<int*>(weaponClass + OFFSET_weaponClassRoundsPerClip);
    if (rounds < 1 || rounds >= 0x7FFFFFFF)
    {
        return -1;
    }
    return *reinterpret_cast<float*>(weapon + OFFSET_weaponReserve);
}

using ClipEmptyFunction = bool(__fastcall*)(void* weapon, void* edx);

// True for a clip weapon of the player's under manual reload whose own reload is held back: the one in the hand until
// the canister is put in, and every other clip weapon of the player's (headset run 18: a holstered or swapped-out
// empty weapon was reloaded by the game). Game thread; _layout is written between frames (whole words).
bool IsHeldBack(uintptr_t weapon)
{
    if (!IsManualReloadWanted() || _mode != 3 || !weapon || _isSuperBattleDroid)
    {
        return false;
    }
    if (weapon == _manualWeapon)
    {
        return _ammo != Ammo::Inserting;
    }
    for (int place = 0; place < PLACE_COUNT; ++place)
    {
        if (_layout.weapons[place] == weapon)
        {
            // Only primaries (channel 0): grenades, mines, time bombs and dispensers have a clip of one too, and
            // holding theirs back left them empty after the first throw (headset run 19).
            return _layout.channels[place] == 0;
        }
    }
    return weapon == _layout.primary;
}

// Slot 44 of the clip weapons' vtables (clip empty): as the game has it. Until 2026-10-24 it returned false for the
// player's weapon under manual reload (no reload by itself), which left the empty gun in state 0, where the trigger
// fires: the firing sound with nothing fired (idren2023). Now the empty clip goes on to slot 42 (NoSpareHook).
// thiscall, no arguments; game thread.
bool __fastcall ClipEmptyHook(void* weapon, void* edx)
{
    const auto original = reinterpret_cast<ClipEmptyFunction>(OriginalOf(weapon, _clipSlots, _clipSlotCount));
    return original ? original(weapon, edx) : false;
}

// Replaces slot 42 of the clip weapons' vtables ("nothing left"): for the player's weapon under manual reload, with
// its clip empty and the canister not yet put in, true: the game takes the gun to state 8 (empty), where the trigger
// plays its empty sound and no reload starts; once the canister is put in (Ammo::Inserting) the game's own answer,
// false with spare clips, so state 8 starts the reload itself. Every other weapon (AI soldiers share the vtables) as
// the game has it. thiscall, no arguments; game thread.
bool __fastcall NoSpareHook(void* weapon, void* edx)
{
    const auto original = reinterpret_cast<ClipEmptyFunction>(OriginalOf(weapon, _spareSlots, _spareSlotCount));
    const uintptr_t self = reinterpret_cast<uintptr_t>(weapon);
    if (IsHeldBack(self))
    {
        const uintptr_t weaponClass = *reinterpret_cast<uintptr_t*>(self + OFFSET_weaponClass);
        const float rounds = static_cast<float>(*reinterpret_cast<int*>(weaponClass + OFFSET_weaponClassRoundsPerClip)) *
                             *reinterpret_cast<float*>(self + OFFSET_weaponClipFraction);
        if (rounds < 1.0f)
        {
            return true;
        }
    }
    return original ? original(weapon, edx) : false;
}

const char* AmmoName(Ammo state)
{
    return state == Ammo::Hidden      ? "hidden"
           : state == Ammo::Holstered ? "on the belt"
           : state == Ammo::InHand    ? "in the left hand"
           : state == Ammo::Inserting ? "put into the gun"
                                      : "reloading";
}

void SetAmmo(Ammo state, const char* why)
{
    if (state != _ammo)
    {
        Log::Info("holster", "ammo canister %s (%s)", AmmoName(state), why);
        _ammo = state;
        _ammoTick = GetTickCount();
    }
}

// The left palm in the world: the left controller's grip pose (the middle of its handle) in the base frame, as Aim
// places the hands.
bool LeftPalmWorld(float* world)
{
    float grip[16], base[16];
    if (!Xr::GetGripPose(1, grip) || !Aim::GetBaseFrame(base))
    {
        return false;
    }
    FpGuard fp;
    Multiply(world, grip, base);
    return true;
}

// The game's ammo pickup effect on the player's soldier (Powerup_red, as an ammo droid's recharge; EntitySoldier
// 0x1189C0, docs/offsets.md "Powerup effects"): played when the manual reload is done (idren2023 2026-10-24).
void PlayAmmoPowerupEffect()
{
    if (!_layout.soldier || !GameBuild::IsAvailable(GameBuild::Feature::Holsters))
    {
        return;
    }
    using EffectFunction = void(__fastcall*)(uintptr_t soldier, void* edx);
    const auto play = reinterpret_cast<EffectFunction>(GameBuild::Absolute(GameBuild::Address::AmmoPowerupEffect));
    play(_layout.soldier, nullptr);
    Log::Info("holster", "ammo powerup effect played");
}

// The canister's step (main thread, once per presented frame, after UpdateHand).
void StepAmmo()
{
    const Gesture::Hand& left = Gesture::CurrentHand(1);
    const bool hasGripStarted = left.isGripped && !_wasLeftGripped;
    const bool hasGripEnded = !left.isGripped && _wasLeftGripped;
    _wasLeftGripped = left.isGripped;
    const uintptr_t weapon = _layout.primary;
    const float reserve = _mode == 3 && _holding == Holding::Primary ? ManualReloadReserve(weapon) : -1.0f;
    _manualWeapon = IsManualReloadWanted() && reserve >= 0 ? weapon : 0;
    // Every clip weapon of the player's gets the hooks, held or not (IsHeldBack).
    for (int place = 0; IsManualReloadWanted() && _mode == 3 && place <= PLACE_COUNT; ++place)
    {
        const uintptr_t other = place < PLACE_COUNT ? _layout.weapons[place] : _layout.primary;
        const bool isPrimaryPlace = place == PLACE_COUNT || _layout.channels[place] == 0;
        if (other && other != _manualWeapon && isPrimaryPlace && ManualReloadReserve(other) >= 0)
        {
            PatchSlot(other, CLIP_EMPTY_SLOT, reinterpret_cast<void*>(&ClipEmptyHook), _clipSlots, _clipSlotCount,
                      "clip empty (manual reload)");
            PatchSlot(other, NO_SPARE_SLOT, reinterpret_cast<void*>(&NoSpareHook), _spareSlots, _spareSlotCount,
                      "nothing left (manual reload)");
        }
    }
    if (_manualWeapon)
    {
        PatchSlot(
            weapon,
            CLIP_EMPTY_SLOT,
            reinterpret_cast<void*>(&ClipEmptyHook),
            _clipSlots,
            _clipSlotCount,
            "clip empty (manual reload)");
        PatchSlot(
            weapon,
            NO_SPARE_SLOT,
            reinterpret_cast<void*>(&NoSpareHook),
            _spareSlots,
            _spareSlotCount,
            "nothing left (manual reload)");
    }
    const int state = weapon && reserve >= 0 ? *reinterpret_cast<int*>(weapon + OFFSET_weaponState) : 0;
    static DWORD lastLogTick = 0;
    if (_manualWeapon && GetTickCount() - lastLogTick >= 1000)
    {
        lastLogTick = GetTickCount();
        const uintptr_t weaponClass = *reinterpret_cast<uintptr_t*>(weapon + OFFSET_weaponClass);
        Log::Info(
            "holster",
            "manual reload: %d of %d rounds, %.2f spare clips, state %d, canister %s",
            static_cast<int>(static_cast<float>(*reinterpret_cast<int*>(weaponClass + OFFSET_weaponClassRoundsPerClip)) *
                             *reinterpret_cast<float*>(weapon + OFFSET_weaponClipFraction)),
            *reinterpret_cast<int*>(weaponClass + OFFSET_weaponClassRoundsPerClip),
            static_cast<double>(reserve),
            state,
            AmmoName(_ammo));
    }
    if (!_manualWeapon)
    {
        SetAmmo(Ammo::Hidden, _isManualReload ? "no clip weapon in the hand" : "manual reload off");
        return;
    }
    const DWORD now = GetTickCount();
    switch (_ammo)
    {
    case Ammo::Hidden:
        if (reserve >= 1.0f)
        {
            SetAmmo(Ammo::Holstered, "a clip weapon in the hand with spare clips");
        }
        break;
    case Ammo::Holstered:
        if (reserve < 1.0f)
        {
            SetAmmo(Ammo::Hidden, "no spare clip");
        }
        else if (hasGripStarted && left.isTracked && left.zone == Gesture::Zone::BeltLeft)
        {
            Feedback::Pulse(1, 0.03f, 0.5f);
            SetAmmo(Ammo::InHand, "taken");
        }
        break;
    case Ammo::InHand:
    {
        float gun[16], hand[16];
        bool isAtGun = false;
        if (left.isTracked && Aim::GetHandWorld(0, gun) && Aim::GetHandWorld(1, hand))
        {
            FpGuard fp;
            const float dx = hand[12] - gun[12], dy = hand[13] - gun[13], dz = hand[14] - gun[14];
            isAtGun = dx * dx + dy * dy + dz * dz < AMMO_INSERT_METERS * AMMO_INSERT_METERS;
        }
        if (!left.isTracked || hasGripEnded)
        {
            if (isAtGun)
            {
                Controls::RequestReload();
                Feedback::Pulse(1, 0.08f, 1.0f); // strong: let go into the gun (idren2023 2026-10-24)
                SetAmmo(Ammo::Inserting, "let go at the gun: reload asked");
            }
            else
            {
                SetAmmo(Ammo::Holstered, "let go away from the gun: back to the belt");
            }
            _wasCanisterAtGun = false;
        }
        else if (isAtGun != _wasCanisterAtGun)
        {
            // idren2023 2026-10-24: a strong pulse into the zone with the canister, a small one out of it
            Feedback::Pulse(1, isAtGun ? 0.06f : 0.02f, isAtGun ? 1.0f : 0.3f);
            _wasCanisterAtGun = isAtGun;
        }
        break;
    }
    case Ammo::Inserting:
        if (state == WEAPON_STATE_RELOADING)
        {
            Feedback::Pulse(0, 0.04f, 0.6f);
            SetAmmo(Ammo::Reloading, "the game reloads");
        }
        else if (now - _ammoTick > AMMO_ACCEPT_MILLISECONDS)
        {
            SetAmmo(Ammo::Holstered, "the game refused the reload (a full clip?): back to the belt");
        }
        break;
    case Ammo::Reloading:
        if (state != WEAPON_STATE_RELOADING)
        {
            SetAmmo(reserve >= 1.0f ? Ammo::Holstered : Ammo::Hidden, "reload done");
            PlayAmmoPowerupEffect();
        }
        break;
    }
}

// The right hand: a tick on entering a place with an open hand; grip pressed in a place takes that place's weapon;
// grip let go empties the hand (and throws an expendable).
void UpdateHand()
{
    const Gesture::Hand& right = Gesture::CurrentHand(0);
    if (_isSuperBattleDroid && _mode == 3)
    {
        // The super battle droid (idren2023 2026-11-04): its weapons are its forearms; holding the right grip readies
        // them (the trigger fires, the weapon HUD and the aiming line show), letting go lowers them (Controls changes
        // to the next primary on the release). (Before the soldier checks: its layout is empty, run561.)
        if (right.isGripped != _wasGripped)
        {
            Log::Info("holster", "super battle droid: weapon %s", right.isGripped ? "ready (grip held)" : "lowered");
        }
        _holding = right.isGripped ? Holding::Primary : Holding::Empty;
        _wasGripped = right.isGripped;
        _lastSoldier = 0;
        return;
    }
    if (_mode != 3 || !right.isTracked || !_layout.soldier || _layout.soldier != _lastSoldier)
    {
        // A new soldier (each spawn) starts empty-handed unless the grip is held (idren2023 2026-10-20: he spawned with
        // the primary in the hand without holding the grip); without the holsters or tracking, as the game has it.
        const bool isNewSoldier = _layout.soldier && _layout.soldier != _lastSoldier;
        const bool startsEmpty = isNewSoldier && _mode == 3 && right.isTracked && !right.isGripped;
        _holding = startsEmpty ? Holding::Empty : Holding::Primary;
        if (isNewSoldier)
        {
            _heldPlace = -1;
            Log::Info("holster", "new soldier: right hand %s", startsEmpty ? "empty (grip not held)" : "holds the primary");
        }
        _wasGripped = right.isGripped;
        _lastSoldier = _layout.soldier;
        return;
    }
    HandSample& sample = _samples[_nextSample++ % SAMPLE_COUNT];
    _nextSample %= SAMPLE_COUNT;
    sample.seconds = NowSeconds();
    memcpy(sample.position, &right.pose[12], sizeof(sample.position));
    const Place place = PlaceOfZone(right.zone);
    if (right.hasEntered && place != Place::Count && !right.isGripped)
    {
        Feedback::Pulse(0, 0.02f, 0.3f);
        if (_holding == Holding::Empty)
        {
            Prepare(static_cast<int>(place));
        }
    }
    if (right.isGripped && !_wasGripped && place != Place::Count)
    {
        Take(static_cast<int>(place));
    }
    else if (!right.isGripped && _wasGripped && _holding != Holding::Empty)
    {
        const bool isThrowable = _holding == Holding::Secondary && _heldPlace >= 0 &&
                                 _layout.kinds[_heldPlace] == Kind::Throwable && IsSecondarySelected();
        if (_holding == Holding::Secondary && !isThrowable)
        {
            Log::Info(
                "holster",
                "right hand puts the weapon of place %d back (%s)",
                _heldPlace,
                _layout.kinds[_heldPlace] == Kind::Throwable ? "not selected yet" : "used by the trigger");
        }
        else if (_holding == Holding::Secondary && static_cast<int>(place) == _heldPlace)
        {
            Log::Info("holster", "right hand puts the weapon of place %d back", _heldPlace);
        }
        else if (_holding == Holding::Secondary)
        {
            float direction[3], metersPerSecond = 0;
            const bool hasSwing = SwingDirection(direction, &metersPerSecond);
            SetThrowStrength(
                _layout.weapons[_heldPlace],
                metersPerSecond,
                hasSwing && direction[1] > UNDERHAND_MINIMUM_RISE);
            if (hasSwing)
            {
                Aim::SetThrowDirection(direction, THROW_DIRECTION_MILLISECONDS);
            }
            if (Net::IsNetGame())
            {
                SolveNetThrow(_layout.weapons[_heldPlace], hasSwing ? direction : nullptr, _throwStrength);
            }
            _throwUntilTick = GetTickCount() + THROW_MILLISECONDS;
            _thrownWeapon = IsType(_layout.weapons[_heldPlace], "WeaponGrenade") ||
                                    IsType(_layout.weapons[_heldPlace], "WeaponDispenser")
                                ? _layout.weapons[_heldPlace]
                                : 0;
            _thrownTick = GetTickCount();
            Log::Info(
                "holster",
                "throw along %s %.2f %.2f %.2f",
                hasSwing ? "the swing" : "the hand (swing too slow)",
                static_cast<double>(hasSwing ? direction[0] : 0),
                static_cast<double>(hasSwing ? direction[1] : 0),
                static_cast<double>(hasSwing ? direction[2] : 0));
            Log::Info(
                "holster",
                "right hand throws the weapon of place %d (secondary index now %d)",
                _heldPlace,
                *reinterpret_cast<int*>(_layout.soldier + OFFSET_soldierWeaponIndex + 4));
        }
        else
        {
            Log::Info("holster", "right hand lets go");
        }
        _holding = Holding::Empty;
    }
    _wasGripped = right.isGripped;
    StepPrimary();
    StepSecondary();
    SkipReleaseDelay();
    TraceOrdnance();
}

} // namespace

bool IsObjectType(uintptr_t object, const char* className)
{
    return IsType(object, className);
}

const char* ObjectTypeName(uintptr_t object)
{
    return TypeName(object);
}

void* LoadedModel(const char* geometryName)
{
    return ModelByName(geometryName);
}

bool IsHandEmpty()
{
    return _holding != Holding::Primary;
}

bool IsSwapPending(void* firstPersonRenderable)
{
    const uintptr_t renderable = reinterpret_cast<uintptr_t>(firstPersonRenderable);
    if (_mode != 3 || _holding != Holding::Primary || _heldPlace < 0 || _heldPlace >= PLACE_COUNT || !renderable ||
        _layout.channels[_heldPlace] != 0 || IsBadReadPtr(reinterpret_cast<void*>(renderable + OFFSET_firstPersonWeaponObject), 4))
    {
        return false;
    }
    const uintptr_t taken = _layout.weapons[_heldPlace];
    return taken && *reinterpret_cast<uintptr_t*>(renderable + OFFSET_firstPersonWeaponObject) != taken;
}

void SetManualReload(bool isOn)
{
    _isManualReload = isOn;
    Log::Info("holster", "manual reload %s", isOn ? "on (ammo canister on the belt)" : "off");
}

bool IsManualReloadOn()
{
    return IsManualReloadWanted() && _mode == 3 && !_isSuperBattleDroid; // (the droid reloads by itself, idren2023 2026-11-04)
}

bool IsWeaponReady(uintptr_t weapon)
{
    return weapon && !IsBadReadPtr(reinterpret_cast<void*>(weapon), OFFSET_weaponState + 4) && IsReady(weapon);
}

bool HoldsNothing()
{
    return _mode == 3 && (_layout.soldier || _isSuperBattleDroid) && _holding == Holding::Empty; // (the droid's layout is empty)
}

bool HoldsSecondary()
{
    return _mode == 3 && _holding == Holding::Secondary;
}

bool IsSuperBattleDroid()
{
    return _isSuperBattleDroid;
}

bool IsCis()
{
    return _isCis;
}

bool HasPlaces()
{
    return _mode == 3 && _layout.soldier;
}

bool IsHoldingCanister()
{
    return _ammo == Ammo::InHand;
}

bool IsActive()
{
    return _mode == 3;
}

bool IsTriggerItemHeld()
{
    return _holding == Holding::Secondary && _heldPlace >= 0 && _layout.kinds[_heldPlace] == Kind::Trigger &&
           IsSecondarySelected();
}

uintptr_t HeldSecondaryClass()
{
    if (_mode != 3 || _holding != Holding::Secondary || _heldPlace < 0 || _heldPlace >= PLACE_COUNT)
    {
        return 0;
    }
    const uintptr_t weapon = _layout.weapons[_heldPlace];
    return weapon ? *reinterpret_cast<uintptr_t*>(weapon + OFFSET_weaponClass) : 0;
}

const char* HeldItemGrip(uintptr_t weaponClass)
{
    return weaponClass && HeldSecondaryClass() == weaponClass ? _layout.grips[_heldPlace] : nullptr;
}

void SetTestThrowSpeed(float metersPerSecond)
{
    _testThrowSpeed = metersPerSecond;
    Log::Info("holster", "test: throws take the hand's speed as %.1f m/s", static_cast<double>(metersPerSecond));
}

bool IsThrowing()
{
    if (_throwUntilTick && static_cast<LONG>(GetTickCount() - _throwUntilTick) >= 0)
    {
        _throwUntilTick = 0;
    }
    return _throwUntilTick != 0;
}

void OnPresent()
{
    FpGuard guard;
    if (_isAutomatic && (Xr::IsRunning() ? 3 : 0) != _mode)
    {
        const int wanted = Xr::IsRunning() ? 3 : 0;
        SetMode(wanted, 0);
        // SetMode is also the test command's entry, which ends the automatic switching; keep it unless the
        // first-person hook could not go in (then stop trying).
        _isAutomatic = _mode == wanted;
    }
    Layout layout;
    const uintptr_t soldier = _mode ? PlayerSoldier() : 0;
    // (MakeLayout sets it again for a droid; cleared only when no longer one: the render thread reads it, run562: the
    // droid's barrels were refused whenever it read the moment between)
    _isDroidFound = false;
    MakeLayout(soldier, &layout);
    _isSuperBattleDroid = _isDroidFound;
    static uintptr_t factionSoldier = 0;
    if (soldier != factionSoldier)
    {
        // The side, once per soldier: the CIS side's weapons have cis_1st_* first-person models.
        factionSoldier = soldier;
        _isCis = false;
        for (int index = 0; soldier && index < WEAPON_COUNT && !_isCis; ++index)
        {
            const uintptr_t weapon = *reinterpret_cast<uintptr_t*>(soldier + OFFSET_soldierWeapons + 4 * index);
            _isCis = IsSoldierWeapon(weapon, soldier) &&
                     Zoom::IsCisWeapon(*reinterpret_cast<uintptr_t*>(weapon + OFFSET_weaponClass));
        }
        if (soldier)
        {
            Log::Info("holster", "the player's side: %s (hologram colour)", _isCis ? "CIS (red)" : "not CIS (blue)");
        }
    }
    if (layout.soldier != _layout.soldier || InterlockedExchange(&_logsLeft, 0) > 0)
    {
        for (int place = 0; place < PLACE_COUNT; ++place)
        {
            if (layout.weapons[place])
            {
                const char* type = TypeName(layout.weapons[place]);
                Log::Info(
                    "holster",
                    "place %d: weapon %d (channel %d, %s, %s) %08X class %08X",
                    place,
                    layout.indices[place],
                    layout.channels[place],
                    type ? type : "?",
                    layout.kinds[place] == Kind::Throwable ? "thrown" : "trigger",
                    static_cast<unsigned>(layout.weapons[place]),
                    *reinterpret_cast<unsigned*>(layout.weapons[place] + OFFSET_weaponClass));
                Zoom::LogFirstPersonModel(*reinterpret_cast<uintptr_t*>(layout.weapons[place] + OFFSET_weaponClass));
            }
        }
    }
    _layout = layout;
    UpdateHand();
    StepAmmo();
    float torso[16], base[16];
    _hasPlaces = _mode == 3 && Gesture::GetTorso(torso) && Aim::GetBaseFrame(base);
    if (!_hasPlaces)
    {
        return;
    }
    // Kept relative to the base frame; Draw adds the base frame of the frame being drawn, which Aim refreshes before
    // the render (headset run 15: placed in the world here, with the frame before's base, the holstered items jittered
    // while the player moved; the weapon in hand, placed at the draw, did not).
    for (int place = 0; place < PLACE_COUNT; ++place)
    {
        float pose[16];
        PoseMatrix(_layout.poses[place], pose);
        Multiply(pose, pose, torso);
        Multiply(_placeInBase[place], TURN, pose);
    }
    float ammoPose[16];
    PoseMatrix(AMMO_POSE, ammoPose);
    Multiply(ammoPose, ammoPose, torso);
    Multiply(_ammoPlaceInBase, TURN, ammoPose);
}

// The manual reload's insert zone as a hologram round the gun (ReloadZone.h, idren2023 2026-11-10): faint while the
// clip is empty and a canister waits on the belt, clear while the canister is in the left hand, bright while it is
// inside the zone. Drawn after the gun, inside the first-person render.
bool _shouldLogCanisterHologram = true;

void DrawReloadZone(unsigned flags)
{
    ReloadZone::Level level = ReloadZone::GetForced();
    const uintptr_t weapon = _manualWeapon;
    if (level == ReloadZone::Level::None && weapon && !IsBadReadPtr(reinterpret_cast<void*>(weapon), 0x100))
    {
        if (_ammo == Ammo::InHand)
        {
            level = _wasCanisterAtGun ? ReloadZone::Level::CanisterIn : ReloadZone::Level::CanisterHeld;
        }
        else if (_ammo == Ammo::Inserting || _ammo == Ammo::Reloading)
        {
            level = ReloadZone::Level::Reloading; // green from the canister let go in the zone until the reload is done
        }
        else if (_ammo == Ammo::Holstered)
        {
            const uintptr_t weaponClass = *reinterpret_cast<uintptr_t*>(weapon + OFFSET_weaponClass);
            const float rounds =
                weaponClass && !IsBadReadPtr(reinterpret_cast<void*>(weaponClass), OFFSET_weaponClassRoundsPerClip + 4)
                    ? static_cast<float>(*reinterpret_cast<int*>(weaponClass + OFFSET_weaponClassRoundsPerClip)) *
                          *reinterpret_cast<float*>(weapon + OFFSET_weaponClipFraction)
                    : 1.0f;
            if (rounds < 0.5f && *reinterpret_cast<int*>(weapon + OFFSET_weaponState) != WEAPON_STATE_RELOADING)
            {
                level = ReloadZone::Level::ReloadNeeded;
            }
        }
    }
    float gun[16];
    if (level != ReloadZone::Level::None && Aim::GetHandWorld(0, gun))
    {
        // The canister's own model, half transparent, upright in the gun's frame, under the hologram layer.
        float can[16], size = 1, strength = 0;
        void* ammoModel = ModelByName("item_powerup_ammo");
        ammoModel = ammoModel ? ammoModel : ModelByName("com_item_powerup_dual");
        if (ammoModel && ReloadZone::GetCanister(gun, level, can, &size, &strength))
        {
            FpGuard fp;
            const float scale = AMMO_MODEL_SCALE * size;
            for (int axis = 0; axis < 3; ++axis)
            {
                can[12 + axis] -= (AMMO_MODEL_CENTRE[0] * can[axis] + AMMO_MODEL_CENTRE[1] * can[4 + axis] +
                                   AMMO_MODEL_CENTRE[2] * can[8 + axis]) * scale;
            }
            for (int element = 0; element < 12; ++element)
            {
                can[element] *= scale;
            }
            // The draw's colour: white, its top byte how solid (the stock light is 0xFFFFFFFF).
            static DWORD colour = 0xFFFFFFFF;
            colour = 0x00FFFFFF | (static_cast<DWORD>(40.0f + 150.0f * strength) << 24);
            const auto draw = (*reinterpret_cast<ModelDrawFunction**>(ammoModel))[1];
            draw(ammoModel, can, nullptr, &colour, flags | MODEL_DRAW_FLAGS, 0);
            if (_shouldLogCanisterHologram)
            {
                _shouldLogCanisterHologram = false;
                Log::Info("reloadzone", "canister model drawn in the zone: colour %08lX size %.2f", colour,
                          static_cast<double>(size));
            }
        }
        ReloadZone::Draw(D3dSurvey::PresentingDevice(), gun, level);
    }
}

void Draw(void* firstPersonRenderable, unsigned flags)
{
    const int mode = _mode;
    if (!mode || !GameBuild::IsAvailable(GameBuild::Feature::Holsters) || !_layout.soldier)
    {
        return;
    }
    const uintptr_t renderable = reinterpret_cast<uintptr_t>(firstPersonRenderable);
    if (*reinterpret_cast<uintptr_t*>(renderable + OFFSET_firstPersonControllable) !=
        _layout.soldier + OFFSET_soldierControllable)
    {
        return; // another view's renderable, or the soldier changed since OnPresent
    }
    DrawReloadZone(flags);
    FpGuard guard;
    const float* handMatrix = reinterpret_cast<const float*>(renderable + OFFSET_firstPersonWeaponMatrix);
    const Holding holding = _holding;
    const bool isSwapPending = IsSwapPending(firstPersonRenderable);
    float base[16];
    const bool hasBase = Aim::GetBaseFrame(base);
    int drawn = 0;
    for (int place = 0; place < PLACE_COUNT; ++place)
    {
        const uintptr_t weapon = _layout.weapons[place];
        if (!weapon || !IsSoldierWeapon(weapon, _layout.soldier))
        {
            continue;
        }
        void* model = WeaponModel(weapon, mode == 2);
        if (!model)
        {
            continue;
        }
        if (mode != 3) // test row beside the held weapon
        {
            if (weapon == _layout.primary)
            {
                continue;
            }
            ++drawn;
            float matrix[16];
            memcpy(matrix, handMatrix, sizeof(matrix));
            const float meters = -_spacingMeters * static_cast<float>(drawn); // row 0 points left: this goes right
            for (int axis = 0; axis < 3; ++axis)
            {
                matrix[12 + axis] += handMatrix[axis] * meters;
            }
            DrawModel(model, matrix, flags);
            continue;
        }
        if (holding == Holding::Secondary && place == _heldPlace)
        {
            DrawModel(model, handMatrix, flags, _layout.scales[place]); // in the hand, where the game would hold it
            continue;
        }
        if (isSwapPending && place == _heldPlace)
        {
            DrawModel(model, handMatrix, flags); // taken: in the hand at once, while the game still swaps
            continue;
        }
        if (holding == Holding::Primary && weapon == _layout.primary && !isSwapPending)
        {
            continue; // in the hand, drawn by the game
        }
        if (_layout.channels[place] == 1 && _layout.kinds[place] == Kind::Throwable && !IsReady(weapon))
        {
            continue; // thrown, not back yet (or no ammo)
        }
        if (_hasPlaces && hasBase)
        {
            float placeWorld[16];
            Multiply(placeWorld, _placeInBase[place], base);
            DrawModel(model, placeWorld, flags, _layout.scales[place]);
        }
    }
    // The manual reload's ammo canister: on the belt, or in the left hand (its palm, as the arms close round it).
    if (mode == 3 && (_ammo == Ammo::Holstered || _ammo == Ammo::InHand))
    {
        void* ammoModel = ModelByName("item_powerup_ammo");
        ammoModel = ammoModel ? ammoModel : ModelByName("com_item_powerup_dual");
        float matrix[16], hand[16];
        if (ammoModel && _ammo == Ammo::Holstered && _hasPlaces && hasBase)
        {
            Multiply(matrix, _ammoPlaceInBase, base);
            DrawModel(ammoModel, matrix, flags, AMMO_MODEL_SCALE);
        }
        else if (ammoModel && _ammo == Ammo::InHand && LeftPalmWorld(hand) && BodyArm::GetCanisterWorld(hand, matrix))
        {
            DrawModel(ammoModel, matrix, flags); // in our left hand as fitted (its scale in the matrix)
        }
        else if (ammoModel && _ammo == Ammo::InHand && LeftPalmWorld(hand))
        {
            // In the palm (idren2023 2026-10-24: it sat far off the hand): the canister's middle on the left controller's
            // grip pose, its length along the handle (toward the thumb), as a can held in the fist.
            FpGuard fp;
            for (int axis = 0; axis < 3; ++axis)
            {
                matrix[axis] = hand[axis];               // model x: the palm's sideways
                matrix[4 + axis] = -hand[8 + axis];      // model y (the canister's length): up the handle
                matrix[8 + axis] = hand[4 + axis];       // model z
            }
            matrix[3] = matrix[7] = matrix[11] = 0;
            matrix[15] = 1;
            for (int axis = 0; axis < 3; ++axis)
            {
                matrix[12 + axis] = hand[12 + axis] - (AMMO_MODEL_CENTRE[0] * matrix[axis] +
                                                       AMMO_MODEL_CENTRE[1] * matrix[4 + axis] +
                                                       AMMO_MODEL_CENTRE[2] * matrix[8 + axis]) * AMMO_MODEL_SCALE;
            }
            DrawModel(ammoModel, matrix, flags, AMMO_MODEL_SCALE);
        }
    }
}

void SetMode(int mode, float spacingCentimeters)
{
    if (mode && !Aim::InstallFirstPersonModelHook())
    {
        return;
    }
    if (spacingCentimeters > 0)
    {
        _spacingMeters = spacingCentimeters / 100;
    }
    _mode = mode;
    _isAutomatic = false;
    InterlockedExchange(&_logsLeft, 1);
    Log::Info("holster", "mode %d, row spacing %.2f m", mode, static_cast<double>(_spacingMeters));
}

void SetPose(int place, float x, float y, float z, float yawDegrees, float pitchDegrees, float rollDegrees)
{
    if (place < 0 || place >= PLACE_COUNT)
    {
        return;
    }
    _poses[place] = {x, y, z, yawDegrees, pitchDegrees, rollDegrees};
    Log::Info(
        "holster",
        "place %d at %.0f %.0f %.0f cm, yaw %.0f pitch %.0f roll %.0f",
        place,
        static_cast<double>(x),
        static_cast<double>(y),
        static_cast<double>(z),
        static_cast<double>(yawDegrees),
        static_cast<double>(pitchDegrees),
        static_cast<double>(rollDegrees));
}

} // namespace Holster

} // namespace SwbfVr
