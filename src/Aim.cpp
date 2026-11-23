// Aim.cpp: makes the player's weapon aim independent of the view, and draws the first-person weapon, arms and hands
// where the controllers are.
//
// Aimer::Set (0x8CE90) is the only writer of an Aimer's fire origin and aim direction; the soldier update calls it
// with the values derived from the camera. The hook (AimerSetHook) replaces origin and direction for the player's
// Aimer: from the right controller (the gun's barrel line, shots starting at the weapon model's muzzle), from the
// head while zoomed, or (test, script command "aim <yaw> <pitch>") the game's own direction turned by an offset.
//
// FirstPersonRenderable::Render (0x130320) draws the arms and the weapon. Its hook (FirstPersonRenderHook) puts the
// weapon and the arms on the right controller and the left forearm and hand on the left controller for the duration
// of the call, then puts the game's matrices back.
//
// Poses: the controllers' poses (_trackedPoses) are relative to the base frame (_baseFrame: the level camera in VR,
// else the game's camera), refreshed once per presented frame (OnPresent) and again just before the eyes are drawn
// (BeforeRender). The two-handed grip and the weapon's weight (Handling.cpp) are applied right after they are read,
// so everything downstream (aim, model, muzzle, aiming line, HUD) follows the weapon, not the controller.
// Matrices are float[16], row vectors (v' = v * M), rows right, up, back, position, as everywhere in the mod.
//
// The hooks are MinHook detours (see Stereo.cpp). The game's methods are __thiscall (the object in ECX); the hooks
// are declared __fastcall with an unused second parameter (EDX) to receive it.
#include "Aim.h"

#include "BodyArm.h"

#include "CockpitShell.h"
#include "D3dSurvey.h"
#include "EngineSurvey.h"
#include "ControllerHolo.h"
#include "Flight.h"
#include "FpGuard.h"
#include "GameBuild.h"
#include "GunnerSeat.h"
#include "Handlebars.h"
#include "Handling.h"
#include "Holster.h"
#include "LeverPlacement.h"
#include "Levers.h"
#include "Log.h"
#include "MinHook.h"
#include "ShellDesigns.h"
#include "Stereo.h"
#include "Vehicle.h"
#include "WalkerDrive.h"
#include "Xr.h"
#include "Zoom.h"
#include "engine.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace SwbfVr
{

namespace Aim
{

namespace
{

// ---- Engine addresses (GameBuild; docs/offsets.md, "Functions"; shared layouts in engine.h) ----
// Aimer::Set(const float origin[3], const float direction[3]), thiscall, game thread: the only writer of an Aimer's
// fire origin and aim direction. Observed with memory watches on the Aimer (offsets.md "Functions").
// GameBuild::Address::AimerSet, Feature::Aim.

// ---- Constants ----

/// A walker's cockpit: its place in the camera's frame averaged over this (the stride's sway taken out).
constexpr float WALKER_COCKPIT_SECONDS = 2.0f;
/// Degrees to radians (pi / 180) and centimetres to metres, for the script commands' units.
constexpr float DEGREES_TO_RADIANS = 0.0174532925f;
constexpr float METERS_PER_CENTIMETER = 0.01f;
/// Radians to degrees (180 / pi), for the heading log.
constexpr double RADIANS_TO_DEGREES = 57.29577951;
/// While zoomed the shot starts this far ahead of the point between the eyes, on the head's line.
constexpr float ZOOMED_FIRE_START_AHEAD_METERS = 0.3f;
/// The game's fire origin is carried along with the weapon only if it lies within 1.5 m of the game's weapon matrix
/// (compared squared: 2.25); else the game's own point is kept.
constexpr float MAXIMUM_ORIGIN_IN_WEAPON_SQUARED_METERS = 2.25f;
/// How long the fire line of the last aimed frame stays valid for the aiming line and the crosshair.
constexpr DWORD FIRE_LINE_LIFETIME_MILLISECONDS = 100;
/// The super battle droid's barrels (sign, line, emitter) stay with the weapon as last placed this long: with 100 ms
/// the ammunition sign fell back to the hand's place between draws and jumped (idren2023, b484-b486).
constexpr DWORD BARREL_LIFETIME_MILLISECONDS = 500;
/// With a tracked controller the hand positions and the draw count are logged this often.
constexpr DWORD HAND_LOG_INTERVAL_MILLISECONDS = 2000;

// ---- State ----

/// void __thiscall Aimer::Set(const float origin[3], const float direction[3]), received as __fastcall (see top).
using AimerSetFunction = void(__fastcall*)(void* aimer, void* edx, const float* origin, const float* direction);
/// The game's Aimer::Set, called through MinHook's trampoline.
AimerSetFunction _originalAimerSet;
bool _isAimerHookInstalled;
// Test "aim <yaw> <pitch>": the game's direction turned by an offset. volatile: written by the script, read on the
// game thread, so the compiler must not keep it in a register.
volatile bool _isOffsetOn;
volatile float _offsetYawRadians, _offsetPitchRadians; // yaw left positive, pitch up positive
/// The super battle droid's wrist cannon (PLAN 31): its two barrels are part of the arms model, on the right forearm
/// bone, not of the weapon model, and the animation bends the forearm against the wrist where the weapon (hp_fire)
/// hangs, so bolts and the aiming line started 10-15 cm off the barrels (run159). From the mod tools' msh
/// (cis_1st_sbdroid: cylinder2 and cylinder5), in bone_r_forearm's frame: the muzzles' midpoint, the barrels' axis,
/// and the point between the barrels halfway along them (b160, idren2023: the holoscope's emitter and the ammunition sign
/// go there).
/// The muzzle: the far ends of the barrels in the msh were -0.389; seen in the game the openings are 9 cm further back
/// along the barrels (run170: the line, on the barrels' axis, started 9 cm ahead of them): -0.389 + 0.09.
constexpr float BARREL_MUZZLE_IN_FOREARM[3] = {0.0505f, 0.0374f, -0.299f};
constexpr float BARREL_AXIS_IN_FOREARM[3] = {0.0f, 0.046f, -0.999f};
constexpr float BARREL_MIDDLE_IN_FOREARM[3] = {0.0505f, 0.0415f, -0.270f};
float _barrelMuzzleInWeapon[3], _barrelAxisInWeapon[3], _barrelEmitterInWeapon[3]; // in our weapon matrix's frame
/// hp_fire goes this far ahead of the opening (run170: 2 cm ahead of the corrected opening the flash starts at the
/// barrels' ends; b164: 1.5 cm further out, idren2023).
constexpr float FLASH_AHEAD_OF_OPENING_METERS = 0.035f;
DWORD _barrelTick = 0; // when they were last worked out (first-person draw), 0 = never
float _ourLeftWeapon[16]; // the super battle droid's left arm of ours: the weapon placement mirrored (PLAN 60 step 4e)
bool _hasOurLeftWeapon = false;
/// EntitySoldier: its embedded Aimer and its own aim direction (the reticle's target search ray).
constexpr uintptr_t OFFSET_soldierAimer = 0x8F0;
constexpr uintptr_t OFFSET_soldierAimDirection = 0x33C;
/// Controllable+0xEC (= soldier+0x33C): the direction the getter 0x8FF00 returns (offsets "Reticle target relation").
constexpr uintptr_t OFFSET_controllableAimDirection = 0xEC;
/// The getter's code: lea eax, [ecx+0xEC]; ret. Checked before its call in the target search is redirected.
constexpr uint8_t DIRECTION_GETTER_BYTES[] = {0x8D, 0x81, 0xEC, 0x00, 0x00, 0x00, 0xC3};
constexpr uint8_t CALL_OPCODE = 0xE8; // call rel32
constexpr uint8_t JUMP_OPCODE = 0xE9; // jmp rel32 (the game's jump thunks)
constexpr size_t CALL_LENGTH = 5;
/// The hand's direction is used by the target search while the Aimer hook set it this recently.
constexpr DWORD TARGET_SEARCH_FRESH_MILLISECONDS = 250;
volatile uintptr_t _playerAimer; // the player's Aimer, refreshed once per presented frame
// The player is a droideka (EntityDroideka): the game aims it and draws its arms; Controls steers the game's own aim
// toward the head (idren2023, 2026-10-17). Refreshed once per presented frame.
volatile bool _isDroideka = false;
volatile LONG _aimLogsLeft; // how many more aim lines to log (Interlocked counter)

// Test "hand": one pose for weapon, arms and aim: position and direction relative to the base frame.
volatile bool _isTestHandOn;
volatile float _testHandXMeters, _testHandYMeters, _testHandZMeters, _testHandYawRadians,
    _testHandPitchRadians; // right / up / back; yaw left positive, pitch up positive
// Controller poses relative to the base frame (0 right, 1 left), refreshed per presented frame and before the eyes
// are drawn; grip and weight already applied.
float _trackedPoses[2][16];
volatile bool _isTracked[2]; // the controller is tracked and its hand follows it
// Where the hand-held weapon's shots start and go (world), for the aiming line.
float _fireLineFrom[3], _fireLineDirection[3];
/// Where the shots start in the weapon model's frame (its muzzle, else the game's point carried along) and whether
/// it is known: the aiming line is drawn from it on the weapon as drawn (GetDrawnFireLine; headset run 18: the world
/// point from the game's update sat off the drawn muzzle while walking).
float _fireStartInWeapon[3];
bool _hasFireStartInWeapon = false;
float _elbowWorld[2][3]; // the first-person forearms' elbow ends as last drawn (world), per hand (0 right)
DWORD _elbowTick[2] = {0, 0};
float _weaponWorld[16];     // the first-person weapon model as last placed on the hand (model to world)
float _steadyWeaponWorld[16]; // the same without the recoil kick (the aiming line: headset run 19, no jump per shot)
bool _isWeaponOnGrip = false; // the weapon sits in our arm's hand on the grip pose (BodyArm): shots along its barrel
DWORD _weaponWorldTick;     // when, 0 = never
float _weaponWorldBase[16]; // the base frame the hand was placed in then (GetWeaponWorld moves it to the current one)
float _stockFireOrigin[3] = {}; // where the game itself would start the shot (the soldier's own fire point)
DWORD _fireLineTick = 0;    // GetTickCount of the last aimed frame; 0 = never
bool _shouldWriteAim = false, _shouldMirrorDroid = true; // test switches (SetDroidTest)
// The hand's aim direction for the game's target search (TargetSearchDirectionHook), the player's Controllable and
// when it was last set.
float _targetSearchDirection[3];
uintptr_t _targetSearchControllable = 0;
DWORD _targetSearchTick = 0;
bool _shouldAimWithHeadWhenZoomed = false; // zoomed with a lensless weapon the head aims (swbf-vr.ini: scope_head_aim)
float _baseFrame[16]; // base frame, refreshed once per presented frame: level camera in VR, else the game's camera
volatile bool _hasBaseFrame;
float _throwDirection[3]; // Holster's throw: the aim direction while GetTickCount is before _throwUntilTick
DWORD _throwUntilTick;    // 0 = no throw direction
float _netThrowAim[3];    // net game: where the soldier is to face for a throw, until _netThrowAimUntilTick
DWORD _netThrowAimUntilTick; // 0 = none
// Declared here, defined further down (C++ needs a declaration before the first use).
void ComputeHandWorld(float* matrix);
void Multiply(float* product, const float* first, const float* second);
void InvertRigid(float* inverse, const float* matrix);

// ---- Muzzle of the first-person weapon: engine addresses and state ----

// unsigned HashName(const char*), cdecl, case-insensitive: the hash the game keys model nodes and bones by.
// Observed in the WeaponClass property parser (offsets.md, first-person rows). GameBuild::Address::HashName,
// Feature::Muzzle (with FirstPersonRenderables).
/// WeaponClass: pointer to the first-person model (HighResGeometry). Observed in the WeaponClass property parser
/// 0x243500.
constexpr uintptr_t OFFSET_weaponClassFirstPersonModel = 0x40;
/// Model: pointer to its node table {matrices*, ?, ?, hashes*, count}: one model-space 4x4 matrix and one name hash
/// per node. Observed in Model::FindNode 0x2882B0.
constexpr uintptr_t OFFSET_modelNodeTable = 0xC0;
/// Arms pose (FirstPersonRenderable + OFFSET_firstPersonSkeleton): first of its bone matrices, 4x4 each. The pose's
/// first word points to its definition {bone records*, bone count}. Observed with the bone log (FindBoneMatrix).
constexpr uintptr_t OFFSET_skeletonBoneMatrices = 0x10;
/// Bone record in a pose definition: size, and the offset of the bone's name hash (HashName). Observed, same log.
constexpr uintptr_t BONE_RECORD_SIZE = 0x70;
constexpr uintptr_t OFFSET_boneRecordHash = 0x5C;
// What the muzzle lookup found for the latest weapon class, logged once by AimerSetHook.
bool _shouldLogMuzzle;
unsigned _muzzleNodeIndex;
void* _muzzleModel;
void* _muzzleWeaponClass;
uintptr_t _muzzleVtables[2]; // vtables of weapon class and model, as offsets from the exe base

/// A first-person model whose barrel does not run along its +z (pistols, headset run 15: the aiming line 5-10 degrees
/// right of the barrel). The model is turned on the hand so that its barrel lies along the controller's aim (idren2023
/// 2026-10-17: aim and aiming line stay the controller's for every weapon). Axes measured from the
/// mod tools' source meshes (2026-10-17): the x centre of the barrel's vertices in slices along z; +x is the player's
/// left (the model is turned 180 degrees about y onto the hand).
struct BarrelCorrection
{
    const char* modelName;
    float axis[3];   // the barrel's direction in the model's space (unit)
    bool hasMuzzle;  // muzzle replaces the hp_fire node's position
    float muzzle[3]; // model space, metres
};

constexpr BarrelCorrection BARREL_CORRECTIONS[] = {
    {"all_1st_weap_inf_pistol", {0.0778f, 0.0f, 0.9970f}, true, {0.031f, 0.060f, 0.390f}},
    {"cis_1st_weap_inf_pistol", {0.0320f, 0.0f, 0.9995f}, false, {0, 0, 0}},
    {"rep_1st_weap_inf_pistol_model", {0.0292f, 0.0f, 0.9996f}, false, {0, 0, 0}},
    // Headset run 16 (idren2023): off the barrel too. Muzzle, top rail and scope nodes agree on about 1.9 degrees.
    {"imp_1st_weap_inf_pistol", {0.0333f, 0.0f, 0.9994f}, false, {0, 0, 0}},
};
const BarrelCorrection* _barrelCorrection = nullptr; // valid while _hasBarrelCorrection
bool _hasBarrelCorrection = false;

/// Turns a weapon model matrix (model to world) about the model's origin so that its barrel axis becomes its +z:
/// matrix = R x matrix, R the rotation about y taking the axis to +z (row vectors). The weapon in hand only.
void AlignBarrel(float* weaponMatrix)
{
    if (!_hasBarrelCorrection)
    {
        return;
    }
    const float* barrel = _barrelCorrection->axis;
    const float length = sqrtf(barrel[0] * barrel[0] + barrel[2] * barrel[2]);
    if (length < 1e-6f)
    {
        return;
    }
    const float sine = barrel[0] / length, cosine = barrel[2] / length;
    const float rotation[16] = {cosine, 0, sine, 0, 0, 1, 0, 0, -sine, 0, cosine, 0, 0, 0, 0, 1};
    float turned[16];
    Multiply(turned, rotation, weaponMatrix);
    memcpy(weaponMatrix, turned, sizeof(turned));
}

// The muzzle of the first-person weapon model, in the model's own frame: the position of its "hp_fire" node.
// Chain: FirstPersonRenderable -> soldier -> Aimer -> current weapon -> its class; class+0x40 is the first-person model
// (HighResGeometry). The node is found in the model's node table by the hash of its name (0x3007F0 hashes a
// name), and its position is taken from the node's model-space matrix, so it does not matter what it hangs on.
// Looked up once per weapon class. Game thread only. False (muzzle unchanged) if the chain breaks or the model has no
// hp_fire node.
bool GetMuzzleInWeaponFrame(float* muzzle)
{
    // static locals are set up once and keep their values between calls (like a static field in C#).
    static void* cachedWeaponClass = nullptr;
    static bool isCached = false;
    static float cachedMuzzle[3];
    if (!GameBuild::IsAvailable(GameBuild::Feature::Muzzle))
    {
        return false;
    }
    void* firstPersonRenderable =
        *reinterpret_cast<void**>(GameBuild::Absolute(GameBuild::Address::FirstPersonRenderables));
    if (!firstPersonRenderable || IsBadReadPtr(firstPersonRenderable, 0x1650))
    {
        return false;
    }
    // FirstPersonRenderable+0x1610 is the soldier's Controllable part (soldier + 0x250); the soldier's Aimer is at
    // soldier + 0x8F0 and holds the current weapon at +0x128; the weapon's class is at weapon + 0x64.
    void* controllable =
        *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(firstPersonRenderable) + OFFSET_firstPersonControllable);
    if (!controllable || IsBadReadPtr(controllable, 0x900))
    {
        return false;
    }
    const uintptr_t soldier = reinterpret_cast<uintptr_t>(controllable) - OFFSET_soldierControllable;
    void* weapon = *reinterpret_cast<void**>(soldier + OFFSET_soldierAimer + OFFSET_aimerWeapon);
    if (!weapon || IsBadReadPtr(weapon, 0x70))
    {
        return false;
    }
    void* weaponClass = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(weapon) + OFFSET_weaponClass);
    if (!weaponClass || IsBadReadPtr(weaponClass, 0x48))
    {
        return false;
    }
    if (weaponClass != cachedWeaponClass)
    {
        cachedWeaponClass = weaponClass;
        isCached = false;
        void* model =
            *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(weaponClass) + OFFSET_weaponClassFirstPersonModel);
        unsigned nodeIndex = 0;
        // Sanity: weapon and class must be C++ objects of the game (vtable in the exe's read-only data). A lambda:
        // a local function, like a C# lambda.
        const auto isGameObject = [](const void* object)
        {
            return GameBuild::IsInReadOnlyData(*reinterpret_cast<const uintptr_t*>(object));
        };
        if (model && !IsBadReadPtr(model, 0xC4) && isGameObject(weapon) && isGameObject(weaponClass))
        {
            // The model's node table (model+0xC0): +0x00 array of 4x4 matrices, one per node, in the model's own
            // space (the game's FindNode 0x2882B0 derives parent-relative positions from pairs of them);
            // +0x0C array of name hashes; +0x10 node count.
            // The game's HashName, called through a function pointer made from its address.
            using HashNameFunction = unsigned(__cdecl*)(const char*);
            static const unsigned FIRE_NODE_HASH =
                reinterpret_cast<HashNameFunction>(GameBuild::Absolute(GameBuild::Address::HashName))("hp_fire");
            const uintptr_t* table =
                *reinterpret_cast<const uintptr_t**>(reinterpret_cast<uintptr_t>(model) + OFFSET_modelNodeTable);
            if (table && !IsBadReadPtr(table, 0x14))
            {
                const float* matrices = reinterpret_cast<const float*>(table[0]);
                const unsigned* hashes = reinterpret_cast<const unsigned*>(table[3]);
                const unsigned count = static_cast<unsigned>(table[4]);
                if (count && count < 1024 && !IsBadReadPtr(hashes, count * 4) && !IsBadReadPtr(matrices, count * 64))
                {
                    for (unsigned node = 0; node < count; ++node)
                    {
                        if (hashes[node] != FIRE_NODE_HASH)
                        {
                            continue;
                        }
                        const float* nodeMatrix = matrices + node * 16;
                        cachedMuzzle[0] = nodeMatrix[12];
                        cachedMuzzle[1] = nodeMatrix[13];
                        cachedMuzzle[2] = nodeMatrix[14];
                        isCached = true;
                        nodeIndex = node; // logged as the node's index
                        break;
                    }
                }
            }
        }
        // Pistols whose barrel is not along the model's +z (headset run 15: the aiming line 5-10 degrees right of the
        // barrel on all but the Imperial pistol): the barrel's axis, and for the Rebel pistol the muzzle (its hp_fire
        // node sits at the model's origin; the hp_fire marker box in the source model is at the muzzle).
        _hasBarrelCorrection = false;
        if (isCached || model)
        {
            using HashNameFunction = unsigned(__cdecl*)(const char*);
            const auto hashName = reinterpret_cast<HashNameFunction>(GameBuild::Absolute(GameBuild::Address::HashName));
            for (const BarrelCorrection& correction : BARREL_CORRECTIONS)
            {
                if (Zoom::HasFirstPersonNode(reinterpret_cast<uintptr_t>(weaponClass), hashName(correction.modelName)))
                {
                    _barrelCorrection = &correction;
                    _hasBarrelCorrection = true;
                    if (correction.hasMuzzle)
                    {
                        memcpy(cachedMuzzle, correction.muzzle, sizeof(correction.muzzle));
                        isCached = true;
                    }
                    Log::Info("aim", "barrel correction for %s", correction.modelName);
                    break;
                }
            }
        }
        _muzzleWeaponClass = weaponClass;
        const uintptr_t exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        _muzzleVtables[0] = *reinterpret_cast<uintptr_t*>(weaponClass) - exeBase;
        _muzzleVtables[1] = model && !IsBadReadPtr(model, 4) ? *reinterpret_cast<uintptr_t*>(model) - exeBase : 0;
        _shouldLogMuzzle = true;
        _muzzleNodeIndex = nodeIndex;
        _muzzleModel = model;
    }
    if (isCached)
    {
        muzzle[0] = cachedMuzzle[0];
        muzzle[1] = cachedMuzzle[1];
        muzzle[2] = cachedMuzzle[2];
    }
    return isCached;
}

/// True while the super battle droid's barrels were worked out within the last half second.
bool IsBarrelFresh()
{
    // (signed: the tick has bit 0 set, so it can be 1 ms ahead of this clock; run562: half the calls were refused)
    return _barrelTick && Holster::IsSuperBattleDroid() && static_cast<LONG>(GetTickCount() - _barrelTick) < 500;
}

// The game picks the reticle's target (the aiming line's friend / enemy colour) along the soldier's own aim
// direction, soldier+0x33C, which its aim function writes just before Aimer::Set (offsets "Reticle target relation",
// 2026-10-13): with our aim it kept following the body and the view (headset run 12, PLAN 23). It gets our direction
// too; the search then runs along it next frame. Only for an Aimer embedded in an EntitySoldier (at +0x8F0).
void SetSoldierAimDirection(void* aimer, const float* direction)
{
    static uintptr_t checkedAimer = 0;
    static bool isSoldierAimer = false;
    const uintptr_t address = reinterpret_cast<uintptr_t>(aimer);
    if (address != checkedAimer)
    {
        checkedAimer = address;
        isSoldierAimer =
            address > OFFSET_soldierAimer && Holster::IsObjectType(address - OFFSET_soldierAimer, "EntitySoldier");
    }
    // Off (b175) unless a test asks: the game's first-person camera follows this direction. For every soldier the
    // character's heading is measured from that camera (Stereo HoldViewHeading), so with the hand's direction here the
    // character-follow loop never settled and the character spun (headset run 13, all classes but the droid, which
    // had it off since b171); the droid's camera alternated between it and the body (run177, run180, run183). The
    // aiming line's friend / enemy colour therefore follows the body's aim again (PLAN 23 reopened).
    if (isSoldierAimer && _shouldWriteAim)
    {
        memcpy(reinterpret_cast<void*>(address - OFFSET_soldierAimer + OFFSET_soldierAimDirection), direction, 12);
    }
    // Instead (b178, PLAN 48): the target search alone gets the hand's direction, through the redirected call of its
    // direction getter (TargetSearchDirectionHook); the camera keeps reading the game's +0x33C.
    if (isSoldierAimer)
    {
        memcpy(_targetSearchDirection, direction, sizeof(_targetSearchDirection));
        _targetSearchControllable = address - OFFSET_soldierAimer + OFFSET_soldierControllable;
        _targetSearchTick = GetTickCount();
    }
}

// Replaces the call of the aim direction getter 0x8FF00 (`lea eax, [ecx+0xEC]`: Controllable+0xEC = soldier+0x33C) in
// the target search of PlayerController::Update (0x1F77E0, offsets "Reticle target relation"), which picks the
// reticle's target and with it the aiming line's red / green. For the player's soldier, while the hand aims, it returns
// the hand's direction kept by SetSoldierAimDirection; otherwise the game's. Writing +0x33C itself turned the camera
// (b175). thiscall without arguments: ecx = the Controllable, edx unused; game thread. general-regs-only: no float
// register is touched (the caller keeps floats in xmm0 around it).
__attribute__((target("general-regs-only")))
const uint8_t* __fastcall TargetSearchDirectionHook(const uint8_t* controllable, void* edx)
{
    (void)edx;
    if (reinterpret_cast<uintptr_t>(controllable) == _targetSearchControllable &&
        GetTickCount() - _targetSearchTick < TARGET_SEARCH_FRESH_MILLISECONDS)
    {
        return reinterpret_cast<const uint8_t*>(_targetSearchDirection);
    }
    return controllable + OFFSET_controllableAimDirection;
}

// Points the target search's call of the direction getter at TargetSearchDirectionHook (once, at the Aimer hook's
// install). The call is checked to still lead to the getter's code before it is changed.
void RedirectTargetSearchDirection()
{
    uint8_t* const call =
        reinterpret_cast<uint8_t*>(GameBuild::Absolute(GameBuild::Address::TargetSearchDirectionCall));
    int32_t relative = 0;
    memcpy(&relative, call + 1, sizeof(relative));
    const uint8_t* getter = call + CALL_LENGTH + relative;
    if (getter[0] == JUMP_OPCODE) // the game calls through a jump thunk
    {
        memcpy(&relative, getter + 1, sizeof(relative));
        getter += CALL_LENGTH + relative;
    }
    if (call[0] != CALL_OPCODE || memcmp(getter, DIRECTION_GETTER_BYTES, sizeof(DIRECTION_GETTER_BYTES)) != 0)
    {
        Log::Warning("aim", "target search direction call not as expected: aiming line colour follows the body");
        return;
    }
    uint8_t bytes[CALL_LENGTH] = {CALL_OPCODE};
    relative = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(&TargetSearchDirectionHook) - reinterpret_cast<uintptr_t>(call + CALL_LENGTH));
    memcpy(bytes + 1, &relative, sizeof(relative));
    DWORD oldProtection = 0;
    if (!VirtualProtect(call, CALL_LENGTH, PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        Log::Warning("aim", "target search direction call could not be redirected");
        return;
    }
    memcpy(call, bytes, CALL_LENGTH);
    VirtualProtect(call, CALL_LENGTH, oldProtection, &oldProtection);
    FlushInstructionCache(GetCurrentProcess(), call, CALL_LENGTH);
    Log::Info("aim", "target search follows the hand's direction");
}

// Hook on Aimer::Set (game thread, called by the soldier update with the values derived from the camera). For the
// player's Aimer the origin and direction are replaced: zoomed, from the head; with a hand (controller or test pose),
// from the gun in the hand; with the test offset, the game's direction turned. Every other Aimer (other soldiers,
// vehicles) passes through unchanged. aimer arrives in ECX (thiscall), edx is unused.
void __fastcall AimerSetHook(void* aimer, void* edx, const float* origin, const float* direction)
{
    const bool isHandAiming = (_isTestHandOn || _isTracked[0]) && _hasBaseFrame && !_isDroideka;
    if ((!_isOffsetOn && !isHandAiming) || reinterpret_cast<uintptr_t>(aimer) != _playerAimer)
    {
        _originalAimerSet(aimer, edx, origin, direction);
        return;
    }
    float aimDirection[3], fireOrigin[3];
    // Zoomed with a weapon that has no lens (whole view magnified), if switched on: aim where the head looks, so the
    // hand no longer has to hold still (idren2023, 2026-10-07). Weapons with a lens always aim with the weapon, and the
    // scope window follows it (idren2023, 2026-10-12; run117).
    float headCamera[16];
    if (isHandAiming && _shouldAimWithHeadWhenZoomed && Zoom::IsFullView() && Stereo::GetZoomedHeadCamera(headCamera))
    {
        {
            FpGuard guard;
            aimDirection[0] = -headCamera[8];
            aimDirection[1] = -headCamera[9];
            aimDirection[2] = -headCamera[10];
        }
        static bool hasSaid = false;
        if (!hasSaid)
        {
            hasSaid = true;
            Log::Info("aim", "zoomed, aiming with the head");
        }
        // The shot also starts on that line (0.3 m ahead of the point between the eyes): the game puts its reticle
        // where a shot from the fire origin would land, so with the origin at the gun in the hand the reticle sat far
        // off the scope's centre at short range (run81) and the shot missed the centre (headset run 8).
        float fireStart[3];
        {
            FpGuard guard;
            for (int axis = 0; axis < 3; ++axis)
            {
                fireStart[axis] = headCamera[12 + axis] + aimDirection[axis] * ZOOMED_FIRE_START_AHEAD_METERS;
            }
        }
        SetSoldierAimDirection(aimer, aimDirection);
        _originalAimerSet(aimer, edx, fireStart, aimDirection);
        return;
    }
    if (isHandAiming)
    {
        float muzzle[3];
        const bool hasMuzzle = GetMuzzleInWeaponFrame(muzzle); // calls game code: outside the FpGuard block
        {
            FpGuard guard;
            float handWorld[16];
            ComputeHandWorld(handWorld);
            aimDirection[0] = -handWorld[8];
            aimDirection[1] = -handWorld[9];
            aimDirection[2] = -handWorld[10]; // forward = minus the back row
            if (_isWeaponOnGrip && _weaponWorldTick && GetTickCount() - _weaponWorldTick < 500)
            {
                // the weapon sits in our hand on the grip pose: shots go where its barrel points (a model looks
                // along +z; the barrel's own axis where BARREL_CORRECTIONS has one: the rebel pistol's is 4.5 degrees
                // off +z), not along the aim pose
                const float barrelZ[3] = {0, 0, 1};
                const float* barrel = _hasBarrelCorrection ? _barrelCorrection->axis : barrelZ;
                float direction[3];
                for (int axis = 0; axis < 3; ++axis)
                {
                    direction[axis] = barrel[0] * _weaponWorld[axis] + barrel[1] * _weaponWorld[4 + axis] +
                                      barrel[2] * _weaponWorld[8 + axis];
                }
                const float length =
                    sqrtf(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
                if (length > 1e-6f)
                {
                    aimDirection[0] = direction[0] / length;
                    aimDirection[1] = direction[1] / length;
                    aimDirection[2] = direction[2] / length;
                }
            }
            const bool isThrowing = _throwUntilTick && static_cast<LONG>(GetTickCount() - _throwUntilTick) < 0;
            if (isThrowing)
            {
                memcpy(aimDirection, _throwDirection, sizeof(_throwDirection)); // a throw goes where the hand swung
            }
            // The origin the game passes is where bolts start (Aimer+0x7C, read by the fire code 0x2461A0). Move it
            // the same way the weapon model is moved: express it in the game's weapon matrix, re-emit it from ours.
            float originInWeapon[3] = {0, 0, 0};
            bool isMoved = false;
            const uintptr_t renderables = GameBuild::Absolute(GameBuild::Address::FirstPersonRenderables);
            void* firstPersonRenderable = renderables ? *reinterpret_cast<void**>(renderables) : nullptr;
            if (firstPersonRenderable && !IsBadReadPtr(firstPersonRenderable, 0x1650))
            {
                const float* gameWeapon = reinterpret_cast<const float*>(
                    reinterpret_cast<uintptr_t>(firstPersonRenderable) + OFFSET_firstPersonWeaponMatrix);
                const float turn[16] =
                    {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1}; // a model looks along +z, a pose along -z
                float inverseGameWeapon[16], ourWeapon[16];
                InvertRigid(inverseGameWeapon, gameWeapon);
                for (int axis = 0; axis < 3; ++axis)
                {
                    originInWeapon[axis] = origin[0] * inverseGameWeapon[axis] +
                                           origin[1] * inverseGameWeapon[4 + axis] +
                                           origin[2] * inverseGameWeapon[8 + axis] + inverseGameWeapon[12 + axis];
                }
                if (originInWeapon[0] * originInWeapon[0] + originInWeapon[1] * originInWeapon[1] +
                        originInWeapon[2] * originInWeapon[2] <
                    MAXIMUM_ORIGIN_IN_WEAPON_SQUARED_METERS)
                {
                    // Start the bolt at the muzzle (the weapon model's hp_fire node) when the model has one; else
                    // keep the game's own point, carried along with the weapon.
                    const float* fireStart = hasMuzzle ? muzzle : originInWeapon;
                    memcpy(_fireStartInWeapon, fireStart, sizeof(_fireStartInWeapon));
                    _hasFireStartInWeapon = true;
                    Multiply(ourWeapon, turn, handWorld);
                    AlignBarrel(ourWeapon);
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        fireOrigin[axis] = fireStart[0] * ourWeapon[axis] + fireStart[1] * ourWeapon[4 + axis] +
                                           fireStart[2] * ourWeapon[8 + axis] + ourWeapon[12 + axis];
                    }
                    isMoved = true;
                }
                if (IsBarrelFresh())
                {
                    // The super battle droid: from between its barrels, along them (every one of its weapons).
                    // With our arm the weapon was also moved so the hand sits on the controller's grip: the weapon
                    // as last drawn, put into the current base frame (PLAN 60 step 4e, run557: the line started at
                    // the aim pose, between the arms).
                    if (!(_isWeaponOnGrip && GetWeaponWorldWithin(ourWeapon, BARREL_LIFETIME_MILLISECONDS)))
                    {
                        Multiply(ourWeapon, turn, handWorld);
                    }
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        fireOrigin[axis] = _barrelMuzzleInWeapon[0] * ourWeapon[axis] +
                                           _barrelMuzzleInWeapon[1] * ourWeapon[4 + axis] +
                                           _barrelMuzzleInWeapon[2] * ourWeapon[8 + axis] + ourWeapon[12 + axis];
                        aimDirection[axis] = _barrelAxisInWeapon[0] * ourWeapon[axis] +
                                             _barrelAxisInWeapon[1] * ourWeapon[4 + axis] +
                                             _barrelAxisInWeapon[2] * ourWeapon[8 + axis];
                    }
                    isMoved = true;
                }
            }
            if (isThrowing)
            {
                // A throw leaves from the hand itself (headset run 13: the ordnance appeared away from the hand): the
                // fire code puts the ordnance at Aimer+0x7C (offsets "Thrown ordnance").
                fireOrigin[0] = handWorld[12];
                fireOrigin[1] = handWorld[13];
                fireOrigin[2] = handWorld[14];
                isMoved = true;
            }
            if (!isMoved)
            {
                fireOrigin[0] = origin[0];
                fireOrigin[1] = origin[1];
                fireOrigin[2] = origin[2];
            }
            if (_shouldLogMuzzle)
            {
                _shouldLogMuzzle = false;
                if (hasMuzzle)
                {
                    Log::Info(
                        "aim",
                        "weapon model %p: hp_fire at %.3f %.3f %.3f (node %u)",
                        _muzzleModel,
                        static_cast<double>(muzzle[0]),
                        static_cast<double>(muzzle[1]),
                        static_cast<double>(muzzle[2]),
                        _muzzleNodeIndex);
                }
                else
                {
                    Log::Info(
                        "aim",
                        "weapon class %p (vtable +0x%x) model %p (vtable +0x%x): no hp_fire node found, bolt "
                        "start stays at the game's point",
                        _muzzleWeaponClass,
                        static_cast<unsigned>(_muzzleVtables[0]),
                        _muzzleModel,
                        static_cast<unsigned>(_muzzleVtables[1]));
                }
            }
            if (_aimLogsLeft > 0)
            {
                InterlockedDecrement(&_aimLogsLeft);
                Log::Info(
                    "aim",
                    "hand direction %.3f %.3f %.3f (game %.3f %.3f %.3f), origin %.2f %.2f %.2f -> %.2f %.2f %.2f "
                    "(%s; in the weapon's frame %.3f %.3f %.3f)",
                    static_cast<double>(aimDirection[0]),
                    static_cast<double>(aimDirection[1]),
                    static_cast<double>(aimDirection[2]),
                    static_cast<double>(direction[0]),
                    static_cast<double>(direction[1]),
                    static_cast<double>(direction[2]),
                    static_cast<double>(origin[0]),
                    static_cast<double>(origin[1]),
                    static_cast<double>(origin[2]),
                    static_cast<double>(fireOrigin[0]),
                    static_cast<double>(fireOrigin[1]),
                    static_cast<double>(fireOrigin[2]),
                    isMoved ? "moved" : "kept",
                    static_cast<double>(originInWeapon[0]),
                    static_cast<double>(originInWeapon[1]),
                    static_cast<double>(originInWeapon[2]));
            }
        }
        {
            memcpy(_stockFireOrigin, origin, sizeof(_stockFireOrigin));
            memcpy(_fireLineFrom, fireOrigin, sizeof(fireOrigin));
            memcpy(_fireLineDirection, aimDirection, sizeof(aimDirection));
            _fireLineTick = GetTickCount();
        }
        SetSoldierAimDirection(aimer, aimDirection);
        _originalAimerSet(aimer, edx, fireOrigin, aimDirection);
        return;
    }
    // Test offset ("aim <yaw> <pitch>"): the game's own direction turned.
    {
        FpGuard guard;
        const float x = direction[0], y = direction[1], z = direction[2];
        const float yaw = _offsetYawRadians, pitch = _offsetPitchRadians;
        // Turn about the world's up axis (engine space: y up, -z forward, right-handed).
        const float cosine = cosf(yaw), sine = sinf(yaw);
        const float headingX = x * cosine + z * sine, headingZ = -x * sine + z * cosine;
        // Raise by the pitch offset, keeping the heading.
        const float horizontalLength = sqrtf(headingX * headingX + headingZ * headingZ);
        float elevationRadians = atan2f(y, horizontalLength) + pitch;
        constexpr float MAXIMUM_ELEVATION_RADIANS = 1.553f; // 89 degrees
        if (elevationRadians > MAXIMUM_ELEVATION_RADIANS)
        {
            elevationRadians = MAXIMUM_ELEVATION_RADIANS;
        }
        if (elevationRadians < -MAXIMUM_ELEVATION_RADIANS)
        {
            elevationRadians = -MAXIMUM_ELEVATION_RADIANS;
        }
        const float cosineElevation = cosf(elevationRadians),
                    scale = horizontalLength > 1e-6f ? cosineElevation / horizontalLength : 0.0f;
        aimDirection[0] = headingX * scale;
        aimDirection[1] = sinf(elevationRadians);
        aimDirection[2] = headingZ * scale;
        if (_aimLogsLeft > 0)
        {
            InterlockedDecrement(&_aimLogsLeft);
            Log::Info(
                "aim",
                "aimer %p origin %.2f %.2f %.2f dir %.3f %.3f %.3f -> %.3f %.3f %.3f (caller +0x%x)",
                aimer,
                static_cast<double>(origin[0]),
                static_cast<double>(origin[1]),
                static_cast<double>(origin[2]),
                static_cast<double>(x),
                static_cast<double>(y),
                static_cast<double>(z),
                static_cast<double>(aimDirection[0]),
                static_cast<double>(aimDirection[1]),
                static_cast<double>(aimDirection[2]),
                static_cast<unsigned>(
                    reinterpret_cast<uintptr_t>(__builtin_return_address(0)) -
                    reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr))));
        }
    }
    SetSoldierAimDirection(aimer, aimDirection);
    _originalAimerSet(aimer, edx, origin, aimDirection);
}

// ---- First-person model (arms and weapon) ----
// FirstPersonRenderable (vtable 0x417A60) draws the arms model with the matrix at +0x1570 and the weapon with the
// one at +0x15B0, both row-vector 4x4. Its render method [4] (0x130320) is hooked: for the duration of the call the
// two matrices are replaced by ones moved and turned about the eye (test command "fp") or placed on the controllers,
// then put back.
// void FirstPersonRenderable::Render(unsigned flags), thiscall, render thread: vtable slot 4 (offsets.md,
// first-person rows). GameBuild::Address::FirstPersonRender, Feature::FirstPersonModel.
/// void __thiscall FirstPersonRenderable::Render(unsigned flags), received as __fastcall (see top).
using FirstPersonRenderFunction = void(__fastcall*)(void* self, void* edx, unsigned flags);
/// The game's FirstPersonRenderable::Render, called through MinHook's trampoline.
FirstPersonRenderFunction _originalFirstPersonRender;
bool _isFirstPersonHookInstalled;
// Test "fp": arms and weapon moved and turned about the eye.
volatile bool _isFirstPersonOffsetOn;
volatile float _firstPersonXMeters, _firstPersonYMeters, _firstPersonZMeters, _firstPersonYawRadians,
    _firstPersonPitchRadians;       // right / up / back in eye space; yaw left positive, pitch up positive
volatile uintptr_t _redCamera;      // the engine camera (RedCamera), refreshed once per presented frame
volatile LONG _firstPersonLogsLeft; // how many more matrix logs to write (Interlocked counter)

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

// Inverse of a rotation + translation matrix (row vectors, position in row 3).
void InvertRigid(float* inverse, const float* matrix)
{
    float result[16] = {
        matrix[0],
        matrix[4],
        matrix[8],
        0,
        matrix[1],
        matrix[5],
        matrix[9],
        0,
        matrix[2],
        matrix[6],
        matrix[10],
        0,
        0,
        0,
        0,
        1};
    for (int column = 0; column < 3; ++column)
    {
        result[12 + column] =
            -(matrix[12] * result[column] + matrix[13] * result[4 + column] + matrix[14] * result[8 + column]);
    }
    memcpy(inverse, result, sizeof(result));
}

// Logs a 4x4 matrix: its three axes, its position and its last column.
void LogMatrix(const char* name, const float* matrix)
{
    Log::Info(
        "fp",
        "%s  x %.3f %.3f %.3f  y %.3f %.3f %.3f  z %.3f %.3f %.3f  pos %.3f %.3f %.3f  w %.2f %.2f %.2f %.2f",
        name,
        static_cast<double>(matrix[0]),
        static_cast<double>(matrix[1]),
        static_cast<double>(matrix[2]),
        static_cast<double>(matrix[4]),
        static_cast<double>(matrix[5]),
        static_cast<double>(matrix[6]),
        static_cast<double>(matrix[8]),
        static_cast<double>(matrix[9]),
        static_cast<double>(matrix[10]),
        static_cast<double>(matrix[12]),
        static_cast<double>(matrix[13]),
        static_cast<double>(matrix[14]),
        static_cast<double>(matrix[3]),
        static_cast<double>(matrix[7]),
        static_cast<double>(matrix[11]),
        static_cast<double>(matrix[15]));
}

// ---- Hand pose, two-handed grip and weapon handling ----

DWORD _kickTick = 0; // GetTickCount of the last shot (bit 0 set so it is never 0), for the recoil kick; 0 = none
double NowMilliseconds();
bool _isTwoHandedAllowed = true, _isTwoHanded = false; // swbf-vr.ini: two_handed; the grip is held right now
float _gripDirectionInGun[3]; // where the left hand was, in the gun's frame, when it took hold (unit vector)
float _gripLengthMeters = 0;
float _leftHandInGun[16];        // the left controller's pose in the gun's frame when it took hold
int _weaponWeight = 3;           // swbf-vr.ini: weapon_weight (0 off, 1 light, 2 medium, 3 heavy)
bool _hasVirtualStock = false;   // swbf-vr.ini: virtual_stock
DWORD _leftAtGunTick = 0;        // GetTickCount when the left hand was last at the gun (bit 0 set so it is never 0)
bool _isStockPinned = false;     // two-handed with the virtual stock: the gun pivots on the shoulder
float _shoulderInGun[3];         // the shoulder in the gun's frame: the gun is rigid with it while pinned
float _gripFromShoulderInGun[3]; // direction from the shoulder to where the left hand took hold, in the gun's frame
float _stockOrientation[16];     // the gun's pose of the previous frame while pinned (its roll is carried on)

/// Virtual stock v3 (b142): the pivot lies this far behind the right controller along the gun's barrel (metres; a
/// guess: with a 30 cm hold it puts the pivot about 60 cm from the left hand, like b129's shoulder).
constexpr float STOCK_BEHIND_GRIP_METERS = 0.30f;
/// The direction from the right controller to the pivot is taken at the grip and then turns only with the right
/// controller's heading (b176, idren2023 after headset run 13: only the trigger hand moves the pivot, never the foregrip
/// hand; the 20 degree leash of b142, which let the barrel drag the pivot, is gone). Its pitch stays as taken, so the
/// trigger hand's wrist does not swing the pivot up and down.
float _stockBackAtGrip[3]; // that direction at the grip (unit, tracking space)
float _stockGripYaw = 0;   // the right controller's heading at the grip (radians)
float _stockBack[3];       // the direction this frame
/// The stock needs this much distance between the pivot and the left hand's hold.
constexpr float SHOULDER_MINIMUM_REACH_METERS = 0.15f;

/// Left grip button: pressed further than this takes hold, and the grip lasts while it stays above the lower value.
constexpr float GRIP_TAKE_HOLD_PRESS = 0.7f;
constexpr float GRIP_KEEP_HOLD_PRESS = 0.4f;
/// The grip lets go when the hands are closer or further apart than these.
constexpr float GRIP_MINIMUM_DISTANCE_METERS = 0.08f;
constexpr float GRIP_MAXIMUM_DISTANCE_METERS = 0.70f;
/// Where the left hand must be to take hold: this far ahead of the right hand, and at most this far off the barrel.
constexpr float GRIP_MINIMUM_AHEAD_METERS = 0.10f;
constexpr float GRIP_MAXIMUM_AHEAD_METERS = 0.50f;
constexpr float GRIP_MAXIMUM_ASIDE_METERS = 0.15f;
/// The left hand counts as at the gun (no wrist hologram: it came up at every sniper foregrip, headset run 11) while it
/// is in this looser region around the barrel, holds the grip button or is two-handed, and this long after.
constexpr float AT_GUN_MINIMUM_AHEAD_METERS = 0.0f;
constexpr float AT_GUN_MAXIMUM_AHEAD_METERS = 0.80f;
constexpr float AT_GUN_MAXIMUM_ASIDE_METERS = 0.22f;
constexpr DWORD AT_GUN_LINGER_MILLISECONDS = 700;

/// Virtual stock: puts the pivot behind the right hand in the gun's frame and keeps the direction from it to the
/// left hand's hold.
/// @param leftInGun the left hand relative to the gun's origin, in the gun's frame.
/// @return false if the hold is too close to the pivot (no stock then).
bool TakeStock(const float* leftInGun)
{
    _shoulderInGun[0] = 0;
    _shoulderInGun[1] = 0;
    _shoulderInGun[2] = STOCK_BEHIND_GRIP_METERS; // +z is the gun's back
    float reach[3];
    for (int row = 0; row < 3; ++row)
    {
        reach[row] = leftInGun[row] - _shoulderInGun[row];
    }
    const float reachMeters = sqrtf(reach[0] * reach[0] + reach[1] * reach[1] + reach[2] * reach[2]);
    if (reachMeters < SHOULDER_MINIMUM_REACH_METERS)
    {
        return false;
    }
    for (int row = 0; row < 3; ++row)
    {
        _gripFromShoulderInGun[row] = reach[row] / reachMeters;
    }
    Log::Info("aim", "virtual stock: pivot %.0f cm from the hold", static_cast<double>(reachMeters * 100));
    return true;
}

/// The left hand's foregrip on the gun (headset run 18, idren2023: the left hand slid up and down the barrel; a fixed
/// grip place, the barrel brought onto the left controller): this share of the way from the weapon model's origin to
/// its muzzle, this far under the barrel line (model space).
constexpr float FOREGRIP_ALONG_MUZZLE = 0.55f, FOREGRIP_BELOW_METERS = 0.04f;
/// A stock left hand nearer the right hand than this along the barrel is not on the fore-end (a pistol's free hand).
constexpr float STOCK_FOREGRIP_MINIMUM_AHEAD_METERS = 0.10f;

// The stock first-person animation's hands on the current weapon (idren2023 2026-11-05: the foregrip is where the stock
// view puts the left hand): bone_l_wrist [0] and bone_r_wrist [1] in the weapon model's space, from the weapon's
// resting pose. Written by the render hook at every on-foot draw, read when the left hand takes hold.
SRWLOCK _stockWristsLock = SRWLOCK_INIT;
bool _hasStockWrists = false;
float _stockWristsInWeapon[2][16];

// The stock left hand's place as the left controller's pose in the right hand pose's frame (as _leftHandInGun): the
// stock right wrist's relation to the right hand, mirrored onto the left (as PlaceLeftArm), taken off the stock left
// wrist. False without the stock hands or when the left one is not ahead on the gun.
bool StockForegripInPose(const float* handWorld, const float* weaponWorld, float* pose)
{
    float wrists[2][16];
    AcquireSRWLockShared(&_stockWristsLock);
    const bool hasWrists = _hasStockWrists;
    memcpy(wrists, _stockWristsInWeapon, sizeof(wrists));
    ReleaseSRWLockShared(&_stockWristsLock);
    if (!hasWrists)
    {
        return false;
    }
    float inverse[16], gripInWeapon[16], wristInHand[16], leftGripInWeapon[16], leftGripWorld[16];
    InvertRigid(inverse, weaponWorld);
    Multiply(gripInWeapon, handWorld, inverse);
    InvertRigid(inverse, gripInWeapon);
    Multiply(wristInHand, wrists[1], inverse);
    for (int column = 0; column < 4; ++column)
    {
        wristInHand[8 + column] = -wristInHand[8 + column]; // diag(1, 1, -1) on the left: the local z row
    }
    for (int row = 0; row < 4; ++row)
    {
        wristInHand[row * 4] = -wristInHand[row * 4]; // mirror(x) on the right: every x component
    }
    InvertRigid(inverse, wristInHand);
    Multiply(leftGripInWeapon, inverse, wrists[0]);
    Multiply(leftGripWorld, leftGripInWeapon, weaponWorld);
    InvertRigid(inverse, handWorld);
    Multiply(pose, leftGripWorld, inverse);
    return -pose[14] > STOCK_FOREGRIP_MINIMUM_AHEAD_METERS;
}

// The foregrip in the right hand pose's frame (as `leftInGun` below), from the weapon as last drawn; false without a
// muzzle or a drawn weapon, or when it falls outside where a left hand can take hold. foregrip: 16 floats; its
// rotation rows are set only when *isStock (the stock animation's left hand), else the 55 % rule's point only.
bool ForegripInPose(const float* right, float* foregrip, bool* isStock)
{
    float weaponWorld[16];
    *isStock = false;
    if (!GetWeaponWorld(weaponWorld))
    {
        Log::Info("aim", "foregrip: no drawn weapon");
        return false;
    }
    {
        float handWorld[16];
        Multiply(handWorld, right, _baseFrame);
        float pose[16];
        if (StockForegripInPose(handWorld, weaponWorld, pose))
        {
            const float length = sqrtf(pose[12] * pose[12] + pose[13] * pose[13] + pose[14] * pose[14]);
            if (length > GRIP_MINIMUM_DISTANCE_METERS && length < 1.0f)
            {
                memcpy(foregrip, pose, sizeof(pose));
                *isStock = true;
                Log::Info(
                    "aim",
                    "foregrip %.0f %.0f %.0f cm from the right hand (the stock left hand)",
                    static_cast<double>(pose[12] * 100),
                    static_cast<double>(pose[13] * 100),
                    static_cast<double>(-pose[14] * 100));
                return true;
            }
        }
    }
    if (!_hasFireStartInWeapon)
    {
        Log::Info("aim", "foregrip: no stock hand, no muzzle known");
        return false;
    }
    foregrip += 12; // the 55 % rule: the point only
    const float model[3] = {
        _fireStartInWeapon[0], _fireStartInWeapon[1] - FOREGRIP_BELOW_METERS, _fireStartInWeapon[2] * FOREGRIP_ALONG_MUZZLE};
    float world[3], handWorld[16], inverse[16];
    for (int axis = 0; axis < 3; ++axis)
    {
        world[axis] = model[0] * weaponWorld[axis] + model[1] * weaponWorld[4 + axis] + model[2] * weaponWorld[8 + axis] +
                      weaponWorld[12 + axis];
    }
    Multiply(handWorld, right, _baseFrame);
    InvertRigid(inverse, handWorld);
    for (int axis = 0; axis < 3; ++axis)
    {
        foregrip[axis] = world[0] * inverse[axis] + world[1] * inverse[4 + axis] + world[2] * inverse[8 + axis] +
                         inverse[12 + axis];
    }
    const float length = sqrtf(foregrip[0] * foregrip[0] + foregrip[1] * foregrip[1] + foregrip[2] * foregrip[2]);
    const bool isUsable = length > GRIP_MINIMUM_DISTANCE_METERS && length < 1.0f;
    Log::Info(
        "aim",
        "foregrip %.0f %.0f %.0f cm from the right hand (%s)",
        static_cast<double>(foregrip[0] * 100),
        static_cast<double>(foregrip[1] * 100),
        static_cast<double>(-foregrip[2] * 100),
        isUsable ? "used" : "not used");
    return isUsable;
}

// Two-handed grip. With the left grip button held while the left hand is on the fore-end (10 to 50 cm ahead of the
// right hand, within 15 cm of the barrel line) the gun turns about the right hand so that the place the left hand
// took hold of keeps pointing at the left hand: the direction comes from two hands 30 cm apart instead of one
// wrist, which steadies it. The roll still comes from the right wrist. Changes _trackedPoses[0] (and locks the left
// hand's pose, _trackedPoses[1], onto the gun).
void ApplyTwoHandedGrip(bool isLeftTracked)
{
    Xr::ControllerInput pad;
    Xr::GetControllerInput(&pad);
    float* right = _trackedPoses[0];
    const float* left = _trackedPoses[1];
    const bool isGripping =
        pad.isValid && (_isTwoHanded ? pad.grip[1] > GRIP_KEEP_HOLD_PRESS : pad.grip[1] > GRIP_TAKE_HOLD_PRESS);
    const float leftOffset[3] = {left[12] - right[12], left[13] - right[13], left[14] - right[14]};
    float leftInGun[3];
    for (int row = 0; row < 3; ++row)
    {
        leftInGun[row] =
            leftOffset[0] * right[row * 4] + leftOffset[1] * right[row * 4 + 1] + leftOffset[2] * right[row * 4 + 2];
    }
    const float distanceMeters =
        sqrtf(leftInGun[0] * leftInGun[0] + leftInGun[1] * leftInGun[1] + leftInGun[2] * leftInGun[2]);
    {
        const float aheadMeters = -leftInGun[2],
                    asideMeters = sqrtf(leftInGun[0] * leftInGun[0] + leftInGun[1] * leftInGun[1]);
        const bool isAtGun =
            isLeftTracked && !Holster::IsSuperBattleDroid() && (_isTwoHanded || (pad.isValid && pad.grip[1] > GRIP_KEEP_HOLD_PRESS) ||
                              (aheadMeters > AT_GUN_MINIMUM_AHEAD_METERS && aheadMeters < AT_GUN_MAXIMUM_AHEAD_METERS &&
                               asideMeters < AT_GUN_MAXIMUM_ASIDE_METERS));
        if (isAtGun)
        {
            _leftAtGunTick = GetTickCount() | 1;
        }
        static bool wasAtGun = false;
        if (isAtGun != wasAtGun)
        {
            wasAtGun = isAtGun;
            Log::Info(
                "aim",
                "left hand %s the gun (%.0f cm ahead, %.0f cm off the barrel line)",
                isAtGun ? "at" : "away from",
                static_cast<double>(aheadMeters * 100),
                static_cast<double>(asideMeters * 100));
        }
    }
    if (!_isTwoHandedAllowed || !isLeftTracked || !isGripping || Holster::IsSuperBattleDroid() || // (its arms are apart)
        distanceMeters < GRIP_MINIMUM_DISTANCE_METERS ||
        distanceMeters > GRIP_MAXIMUM_DISTANCE_METERS)
    {
        if (_isTwoHanded)
        {
            _isTwoHanded = false;
            _isStockPinned = false;
            Log::Info("aim", "two-handed grip released");
        }
        return;
    }
    const float direction[3] = {
        leftInGun[0] / distanceMeters,
        leftInGun[1] / distanceMeters,
        leftInGun[2] / distanceMeters};
    if (!_isTwoHanded)
    {
        const float aheadMeters = -leftInGun[2],
                    asideMeters = sqrtf(leftInGun[0] * leftInGun[0] + leftInGun[1] * leftInGun[1]);
        if (aheadMeters < GRIP_MINIMUM_AHEAD_METERS || aheadMeters > GRIP_MAXIMUM_AHEAD_METERS ||
            asideMeters > GRIP_MAXIMUM_ASIDE_METERS)
        {
            return;
        }
        _isTwoHanded = true;
        memcpy(_gripDirectionInGun, direction, sizeof(direction));
        _gripLengthMeters = distanceMeters;
        { // the left hand's pose in the gun's frame at this moment: its model stays there while the grip lasts
            float inverseRight[16];
            InvertRigid(inverseRight, right);
            Multiply(_leftHandInGun, left, inverseRight);
        }
        // The fixed foregrip where the gun has one: the gun turns so that it points at the left hand, and the hand's
        // model sits on it, wherever along the fore-end the hand took hold.
        float foregripPose[16];
        bool isStockForegrip = false;
        const bool hasForegrip = ForegripInPose(right, foregripPose, &isStockForegrip);
        if (hasForegrip)
        {
            const float* foregrip = foregripPose + 12;
            const float length =
                sqrtf(foregrip[0] * foregrip[0] + foregrip[1] * foregrip[1] + foregrip[2] * foregrip[2]);
            for (int row = 0; row < 3; ++row)
            {
                _gripDirectionInGun[row] = foregrip[row] / length;
                leftInGun[row] = foregrip[row];
            }
            _gripLengthMeters = length;
            if (isStockForegrip)
            {
                memcpy(_leftHandInGun, foregripPose, sizeof(foregripPose)); // the stock hand's turn too
            }
            else
            {
                memcpy(_leftHandInGun + 12, foregrip, 3 * sizeof(float));
            }
        }
        _isStockPinned = _hasVirtualStock && TakeStock(leftInGun);
        if (_isStockPinned)
        {
            memcpy(_stockOrientation, right, sizeof(_stockOrientation));
            memcpy(_stockBackAtGrip, right + 8, sizeof(_stockBackAtGrip));
            memcpy(_stockBack, right + 8, sizeof(_stockBack));
            _stockGripYaw = atan2f(right[8], right[10]);
        }
        Log::Info(
            "aim",
            "two-handed grip: left hand %.0f cm ahead, %.0f cm off the barrel line%s%s",
            static_cast<double>(aheadMeters * 100),
            static_cast<double>(asideMeters * 100),
            hasForegrip ? "; on the foregrip" : "",
            _isStockPinned ? "; virtual stock: right hand ignored" : "");
        return;
    }
    // Virtual stock (b119; b129 shoulder; v3 b142, idren2023 after headset run 12): while two-handed the right
    // controller only places the pivot, a point STOCK_BEHIND_GRIP_METERS behind it (direction: _stockBack, b176), and
    // gives the roll about the barrel; its pitch and yaw are ignored, and so is the left controller's rotation. The
    // gun turns about the pivot so that the hold keeps pointing at the left hand: a hand's shake over the ~60 cm from
    // the pivot turns the gun about half as much as over the 30 cm between the hands.
    float aimed[3] = {direction[0], direction[1], direction[2]};
    const float* gripDirection = _gripDirectionInGun;
    float shoulder[3], controller[12];
    const bool isShouldered = _isStockPinned;
    if (isShouldered)
    {
        memcpy(controller, right, sizeof(controller));
        memcpy(right, _stockOrientation, 12 * sizeof(float));
        // The grip-time direction turned about the vertical by the right controller's change of heading
        // (controller[8..10] is its back axis); 'controller' holds the controller's own pose this frame.
        const float yawChange = atan2f(controller[8], controller[10]) - _stockGripYaw;
        const float cosineYaw = cosf(yawChange), sineYaw = sinf(yawChange);
        _stockBack[0] = _stockBackAtGrip[0] * cosineYaw + _stockBackAtGrip[2] * sineYaw;
        _stockBack[1] = _stockBackAtGrip[1];
        _stockBack[2] = -_stockBackAtGrip[0] * sineYaw + _stockBackAtGrip[2] * cosineYaw;
        for (int axis = 0; axis < 3; ++axis)
        {
            shoulder[axis] = right[12 + axis] + _stockBack[axis] * STOCK_BEHIND_GRIP_METERS;
        }
        const float toLeft[3] = {left[12] - shoulder[0], left[13] - shoulder[1], left[14] - shoulder[2]};
        float inGun[3];
        for (int row = 0; row < 3; ++row)
        {
            inGun[row] = toLeft[0] * right[row * 4] + toLeft[1] * right[row * 4 + 1] + toLeft[2] * right[row * 4 + 2];
        }
        const float length = sqrtf(inGun[0] * inGun[0] + inGun[1] * inGun[1] + inGun[2] * inGun[2]);
        if (length > GRIP_MINIMUM_DISTANCE_METERS)
        {
            for (int row = 0; row < 3; ++row)
            {
                aimed[row] = inGun[row] / length;
            }
            gripDirection = _gripFromShoulderInGun;
        }
    }
    // Rotation (in the gun's frame) that takes the grip point's direction to where the left hand is now (axis-angle:
    // the axis is the cross product of the two directions, its length the sine of the angle). Turns about the gun's
    // origin; with the stock the origin is put back on the sphere around the shoulder below.
    const float* aimedDirection = aimed;
    float axis[3] = {
        gripDirection[1] * aimedDirection[2] - gripDirection[2] * aimedDirection[1],
        gripDirection[2] * aimedDirection[0] - gripDirection[0] * aimedDirection[2],
        gripDirection[0] * aimedDirection[1] - gripDirection[1] * aimedDirection[0]};
    const float sine = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]),
                cosine = gripDirection[0] * aimedDirection[0] + gripDirection[1] * aimedDirection[1] +
                         gripDirection[2] * aimedDirection[2];
    if (sine >= 1e-5f)
    {
        for (float& component : axis)
        {
            component /= sine;
        }
        const float x = axis[0], y = axis[1], z = axis[2], oneMinusCosine = 1 - cosine;
        // Row-vector form (v' = v * R): the transpose of the usual column-vector rotation matrix.
        const float rotation[16] = {
            oneMinusCosine * x * x + cosine,
            oneMinusCosine * x * y + sine * z,
            oneMinusCosine * x * z - sine * y,
            0,
            oneMinusCosine * x * y - sine * z,
            oneMinusCosine * y * y + cosine,
            oneMinusCosine * y * z + sine * x,
            0,
            oneMinusCosine * x * z + sine * y,
            oneMinusCosine * y * z - sine * x,
            oneMinusCosine * z * z + cosine,
            0,
            0,
            0,
            0,
            1};
        float turned[16];
        Multiply(turned, rotation, right);
        memcpy(right, turned, sizeof(turned));
    }
    if (isShouldered)
    {
        // Roll from the right controller: its up axis, made square to the barrel (back axis), becomes the gun's up.
        const float* back = right + 8;
        const float along = controller[4] * back[0] + controller[5] * back[1] + controller[6] * back[2];
        float up[3] = {
            controller[4] - along * back[0],
            controller[5] - along * back[1],
            controller[6] - along * back[2]};
        const float upLength = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
        if (upLength > 0.2f)
        {
            for (float& component : up)
            {
                component /= upLength;
            }
            right[4] = up[0];
            right[5] = up[1];
            right[6] = up[2];
            right[0] = up[1] * back[2] - up[2] * back[1]; // right = up x back
            right[1] = up[2] * back[0] - up[0] * back[2];
            right[2] = up[0] * back[1] - up[1] * back[0];
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            right[12 + axis] = shoulder[axis] - _shoulderInGun[0] * right[axis] - _shoulderInGun[1] * right[4 + axis] -
                               _shoulderInGun[2] * right[8 + axis];
        }
        static DWORD logTick = 0;
        if (GetTickCount() - logTick > 1000)
        {
            logTick = GetTickCount();
            Log::Info(
                "aim",
                "virtual stock: barrel %.2f %.2f %.2f, gun up %.2f %.2f %.2f, pivot %.0f %.0f %.0f cm",
                static_cast<double>(-right[8]),
                static_cast<double>(-right[9]),
                static_cast<double>(-right[10]),
                static_cast<double>(right[4]),
                static_cast<double>(right[5]),
                static_cast<double>(right[6]),
                static_cast<double>(shoulder[0] * 100),
                static_cast<double>(shoulder[1] * 100),
                static_cast<double>(shoulder[2] * 100));
        }
    }
    if (_isStockPinned)
    {
        memcpy(_stockOrientation, right, sizeof(_stockOrientation));
    }
    // The left hand's model is locked where it took hold (it slid about on the gun before: headset run 8); the real
    // hand can only move along the line the gun now points down, which is hardly seen.
    float locked[16];
    Multiply(locked, _leftHandInGun, right);
    memcpy(_trackedPoses[1], locked, sizeof(locked));
}

/// Two hands carry the weapon more easily: the lag is multiplied by this.
constexpr float TWO_HANDED_LAG_FACTOR = 0.6f;
/// Sniper rifles: the heaviest setting's lag times this.
constexpr float SNIPER_LAG_FACTOR = 2.0f;

// Weight (b94, Handling.cpp): the weapon's pose lags behind the right hand's; arms and hands are drawn from the
// weapon, so they stay on it. Two hands carry it more easily (0.6 of the lag). Runs after
// ApplyTwoHandedGrip on the fresh controller poses; changes _trackedPoses[0] (and the locked left hand).
// Named ApplyWeaponHandling, not Handling, so the name Handling is free for the Handling namespace.
void ApplyWeaponHandling()
{
    static double lastMilliseconds = 0;
    const double nowMilliseconds = NowMilliseconds();
    const float stepSeconds =
        lastMilliseconds > 0 ? static_cast<float>((nowMilliseconds - lastMilliseconds) * 0.001) : 0.0f;
    lastMilliseconds = nowMilliseconds;
    static const float POSITION_LAG_SECONDS[4] = {0, 0.006f, 0.012f, 0.022f},
                       ROTATION_LAG_SECONDS[4] = {0, 0.009f, 0.018f, 0.032f}; // seconds per stage, by weapon weight
    float positionLagSeconds = POSITION_LAG_SECONDS[_weaponWeight & 3],
          rotationLagSeconds = ROTATION_LAG_SECONDS[_weaponWeight & 3];
    // Sniper rifles (a sniper scope in the weapon's class): twice the heaviest setting's lag whatever the setting
    // (PLAN 58 m, headset run 17: a steadier aim), before the two hands' share.
    Zoom::WeaponZoom weaponZoom;
    if (_weaponWeight > 0 && Zoom::GetWeaponZoom(weaponZoom) && weaponZoom.isSniperScope)
    {
        positionLagSeconds = POSITION_LAG_SECONDS[3] * SNIPER_LAG_FACTOR;
        rotationLagSeconds = ROTATION_LAG_SECONDS[3] * SNIPER_LAG_FACTOR;
    }
    if (_isTwoHanded)
    {
        positionLagSeconds *= TWO_HANDED_LAG_FACTOR;
        rotationLagSeconds *= TWO_HANDED_LAG_FACTOR;
    }
    if (positionLagSeconds <= 0 && rotationLagSeconds <= 0)
    {
        Handling::Reset();
        return;
    }
    Handling::ApplyWeight(_trackedPoses[0], stepSeconds, positionLagSeconds, rotationLagSeconds);
    if (_isTwoHanded)
    {
        float locked[16];
        Multiply(locked, _leftHandInGun, _trackedPoses[0]);
        memcpy(_trackedPoses[1], locked, sizeof(locked));
    }
}

bool _isMagazineWanted = false; // Controls asks for the reload hold (SetLeftHandOnMagazine)
bool _isOnMagazine = false;     // the hold has been taken: the drawn left hand is on the magazine
float _leftHandOnMagazine[16];  // the left hand's pose in the gun's frame while it holds the magazine
float _magazinePullMeters = 0;  // how far the left controller is from the magazine while holding it

/// Where the left controller holds the magazine, in the gun's frame (metres right, up, back): the centre of the
/// reload zone (Gesture.cpp). One guess for all weapons.
constexpr float MAGAZINE_IN_GUN[3] = {0.0f, -0.10f, -0.15f};

// Reload hold (b118): while Controls asks for it, the left hand's model is locked onto the magazine under the gun
// (its turn in the gun's frame as when it took hold, its position MAGAZINE_IN_GUN) whatever the controller does.
// Runs after ApplyWeaponHandling on the fresh controller poses; changes _trackedPoses[1].
void ApplyMagazineHold(bool isLeftTracked)
{
    if (!_isMagazineWanted || !isLeftTracked)
    {
        _isOnMagazine = false;
        _magazinePullMeters = 0;
        return;
    }
    float inverseRight[16], leftInGun[16];
    InvertRigid(inverseRight, _trackedPoses[0]);
    Multiply(leftInGun, _trackedPoses[1], inverseRight);
    if (!_isOnMagazine)
    {
        memcpy(_leftHandOnMagazine, leftInGun, sizeof(leftInGun));
        memcpy(&_leftHandOnMagazine[12], MAGAZINE_IN_GUN, sizeof(MAGAZINE_IN_GUN));
        _isOnMagazine = true;
    }
    const float pullX = leftInGun[12] - MAGAZINE_IN_GUN[0], pullY = leftInGun[13] - MAGAZINE_IN_GUN[1],
                pullZ = leftInGun[14] - MAGAZINE_IN_GUN[2];
    _magazinePullMeters = sqrtf(pullX * pullX + pullY * pullY + pullZ * pullZ);
    float locked[16];
    Multiply(locked, _leftHandOnMagazine, _trackedPoses[0]);
    memcpy(_trackedPoses[1], locked, sizeof(locked));
}

// Hand pose in world space: rows right, up, back, position (same convention as the camera). From the right
// controller when it is tracked, else from the test pose ("hand").
void ComputeHandWorld(float* matrix)
{
    if (_isTracked[0])
    {
        Multiply(matrix, _trackedPoses[0], _baseFrame);
        return;
    }
    const float cosineYaw = cosf(_testHandYawRadians), sineYaw = sinf(_testHandYawRadians),
                cosinePitch = cosf(_testHandPitchRadians), sinePitch = sinf(_testHandPitchRadians);
    const float pitchRotation[16] =
        {1, 0, 0, 0, 0, cosinePitch, sinePitch, 0, 0, -sinePitch, cosinePitch, 0, 0, 0, 0, 1};
    const float yawRotation[16] = {
        cosineYaw,
        0,
        -sineYaw,
        0,
        0,
        1,
        0,
        0,
        sineYaw,
        0,
        cosineYaw,
        0,
        _testHandXMeters,
        _testHandYMeters,
        _testHandZMeters,
        1};
    float eye[16];
    Multiply(eye, pitchRotation, yawRotation);
    Multiply(matrix, eye, _baseFrame);
}

// Left hand: its own pose, applied to the two left arm bones of the first-person skeleton.
// Skeleton instance S = *(FirstPersonRenderable+0x830): [S+0] definition ([def+0] bone records of 0x70 bytes, name
// hash at +0x5C; [def+4] bone count); bone matrices at S+0x10, 0x40 bytes each, in the arms model's space.
// Test "lhand": the left hand's own pose (right / up / back of the base frame; yaw left positive, pitch up positive).
volatile bool _isTestLeftHandOn;
volatile float _testLeftHandXMeters, _testLeftHandYMeters, _testLeftHandZMeters, _testLeftHandYawRadians,
    _testLeftHandPitchRadians;
volatile LONG _leftHandLogsLeft; // how many more left-hand logs to write (Interlocked counter)

// A pose in world space from a position and two angles relative to the base frame (the test poses).
void ComputePoseWorld(float* matrix, float x, float y, float z, float yawRadians, float pitchRadians)
{
    const float cosineYaw = cosf(yawRadians), sineYaw = sinf(yawRadians), cosinePitch = cosf(pitchRadians),
                sinePitch = sinf(pitchRadians);
    const float pitchRotation[16] =
        {1, 0, 0, 0, 0, cosinePitch, sinePitch, 0, 0, -sinePitch, cosinePitch, 0, 0, 0, 0, 1};
    const float yawRotation[16] = {cosineYaw, 0, -sineYaw, 0, 0, 1, 0, 0, sineYaw, 0, cosineYaw, 0, x, y, z, 1};
    float eye[16];
    Multiply(eye, pitchRotation, yawRotation);
    Multiply(matrix, eye, _baseFrame);
}

// Finds a bone's matrix in the first-person skeleton by name hash; null if absent.
float* FindBoneMatrix(void* firstPersonRenderable, unsigned hash, bool shouldLogAll)
{
    void* skeleton =
        *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(firstPersonRenderable) + OFFSET_firstPersonSkeleton);
    if (!skeleton || IsBadReadPtr(skeleton, 0x10))
    {
        return nullptr;
    }
    const uintptr_t* definition = *reinterpret_cast<const uintptr_t**>(skeleton);
    if (!definition || IsBadReadPtr(definition, 8))
    {
        return nullptr;
    }
    const unsigned count = static_cast<unsigned>(definition[1]);
    if (!count || count > 64 || IsBadReadPtr(reinterpret_cast<void*>(definition[0]), count * BONE_RECORD_SIZE) ||
        IsBadReadPtr(skeleton, OFFSET_skeletonBoneMatrices + count * 0x40))
    {
        return nullptr;
    }
    float* found = nullptr;
    for (unsigned bone = 0; bone < count; ++bone)
    {
        const unsigned boneHash =
            *reinterpret_cast<const unsigned*>(definition[0] + bone * BONE_RECORD_SIZE + OFFSET_boneRecordHash);
        float* matrix =
            reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(skeleton) + OFFSET_skeletonBoneMatrices + bone * 0x40);
        if (shouldLogAll)
        {
            Log::Info(
                "fp",
                "bone %u hash 0x%08x  x %.2f %.2f %.2f  z %.2f %.2f %.2f  pos %.3f %.3f %.3f",
                bone,
                boneHash,
                static_cast<double>(matrix[0]),
                static_cast<double>(matrix[1]),
                static_cast<double>(matrix[2]),
                static_cast<double>(matrix[8]),
                static_cast<double>(matrix[9]),
                static_cast<double>(matrix[10]),
                static_cast<double>(matrix[12]),
                static_cast<double>(matrix[13]),
                static_cast<double>(matrix[14]));
        }
        if (boneHash == hash)
        {
            found = matrix;
        }
    }
    return found;
}

// The game's name hash (HashName, 0x3007F0 on Steam) of a node or bone name; 0 if this game build lacks it.
unsigned HashName(const char* name)
{
    using HashNameFunction = unsigned(__cdecl*)(const char*);
    if (!GameBuild::IsAvailable(GameBuild::Feature::Muzzle))
    {
        return 0;
    }
    return reinterpret_cast<HashNameFunction>(GameBuild::Absolute(GameBuild::Address::HashName))(name);
}

// True if two matrices are equal up to rounding (sum of the absolute differences of all 16 elements).
bool IsNear(const float* first, const float* second)
{
    float difference = 0;
    for (int element = 0; element < 16; ++element)
    {
        difference += fabsf(first[element] - second[element]);
    }
    return difference < 0.006f; // rounding at world coordinates of a few hundred metres alone gives about 0.001
}

volatile LONG _firstPersonDrawCount = 0; // first-person draws since the last report
bool _isInFirstPersonRender = false;     // inside the game's first-person draw (with a hand placed)
/// Super battle droid (it has no left arm, PLAN 13): its first-person model is drawn a second time, mirrored, on the
/// left controller (idren2023, 2026-10-13). Decided at Present, read during the frame (IsMirrorDrawn).
bool _isMirrorWanted = false;
/// Mirror about the weapon's own left-right axis.
/// FirstPersonRenderable+0x1600: the Weapon it draws; on it +0x20 its world matrix (written by its draw) and +0xC8 the
/// time its muzzle flash ends (offsets "First-person weapon draw", 2026-10-13).
constexpr uintptr_t OFFSET_firstPersonWeapon = 0x1600;
constexpr uintptr_t OFFSET_weaponWorldMatrix = 0x20;
constexpr uintptr_t OFFSET_weaponFlashEnd = 0xC8;
constexpr float MIRROR_X[16] = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
/// The super battle droid's arm rolled a quarter turn about its barrels (model +z), the back of its fist from the model's
/// +y (up) to its -x (the shooter's right), where every human weapon's grip has the back of the hand (BodyArm grips,
/// measured 2026-11-08): headset run 19, idren2023: its hand was perpendicular to the real one. Rows: x -> y, y -> -x. The
/// weapon model turns with it, so the game's hp_fire stays at our barrels.
/// The left arm (mirrored about x after this) takes the opposite turn (run663 / idren2023: the same turn left it upside
/// down).
constexpr float DROID_ROLL[16] = {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
constexpr float DROID_ROLL_LEFT[16] = {0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

void RollDroidArm(float* weaponMatrix, const void* armClass, bool isLeft)
{
    if (Holster::IsSuperBattleDroid() && BodyArm::IsWanted(reinterpret_cast<uintptr_t>(armClass)))
    {
        float rolled[16];
        Multiply(rolled, isLeft ? DROID_ROLL_LEFT : DROID_ROLL, weaponMatrix);
        memcpy(weaponMatrix, rolled, sizeof(rolled));
    }
}

// The class of the weapon the player holds (see GetMuzzleInWeaponFrame for the chain), or null.
void* CurrentWeapon()
{
    const uintptr_t renderables = GameBuild::Absolute(GameBuild::Address::FirstPersonRenderables);
    void* firstPersonRenderable = renderables ? *reinterpret_cast<void**>(renderables) : nullptr;
    if (!firstPersonRenderable || IsBadReadPtr(firstPersonRenderable, 0x1650))
    {
        return nullptr;
    }
    void* controllable =
        *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(firstPersonRenderable) + OFFSET_firstPersonControllable);
    if (!controllable || IsBadReadPtr(controllable, 0x900))
    {
        return nullptr;
    }
    void* weapon = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(controllable) - OFFSET_soldierControllable + OFFSET_soldierAimer +
        OFFSET_aimerWeapon);
    if (!weapon || IsBadReadPtr(weapon, 0x70))
    {
        return nullptr;
    }
    return weapon;
}

void* CurrentWeaponClass()
{
    void* weapon = CurrentWeapon();
    return weapon ? *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(weapon) + OFFSET_weaponClass) : nullptr;
}

// All bone matrices of the first-person skeleton (arms-model space), or null. See FindBoneMatrix for the layout.
float* FindBones(void* firstPersonRenderable, unsigned* count)
{
    void* skeleton =
        *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(firstPersonRenderable) + OFFSET_firstPersonSkeleton);
    if (!skeleton || IsBadReadPtr(skeleton, 0x10))
    {
        return nullptr;
    }
    const uintptr_t* definition = *reinterpret_cast<const uintptr_t**>(skeleton);
    if (!definition || IsBadReadPtr(definition, 8))
    {
        return nullptr;
    }
    const unsigned total = static_cast<unsigned>(definition[1]);
    if (!total || total > 64 || IsBadReadPtr(skeleton, OFFSET_skeletonBoneMatrices + total * 0x40))
    {
        return nullptr;
    }
    *count = total;
    return reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(skeleton) + OFFSET_skeletonBoneMatrices);
}

// With a tracked right hand the arms and the weapon are drawn in one fixed pose per weapon, placed by the hand:
// no game animation (idle, recoil, reload, jump, crouch, roll) moves them (idren2023, 2026-10-07: after such
// animations the rifle sat beside the hand for a while). The pose is the weapon's resting pose: where the weapon
// sits in the arms model (K0, weaponInArms) and all bone matrices, taken once per weapon class when the game has held
// the weapon still for 72 draws (about 0.5 s). Until then a first sample serves (taken after 12 still draws) and the
// bones still animate.
struct RestingPose
{
    void* weaponClass;      // the weapon class this pose belongs to
    bool isUsed;            // the slot holds a pose
    bool hasSample;         // a first sample is there (taken after FIRST_STABLE_DRAWS still draws)
    bool isFinal;           // the final sample is there (after FINAL_STABLE_DRAWS), with the bones
    float weaponInArms[16]; // K0: where the weapon sits in the arms model at rest
    bool hasWrists;         // wristsInWeapon is set (with the final sample)
    float wristsInWeapon[2][16]; // bone_l_wrist, bone_r_wrist at rest in the weapon model's space (the foregrip)
    unsigned boneCount;
    float bones[64][16]; // every bone matrix of the arms at rest (arms-model space)
};

/// How many weapon classes keep a resting pose; the oldest slot is reused.
constexpr unsigned RESTING_POSE_COUNT = 8;

RestingPose _restingPoses[RESTING_POSE_COUNT];
unsigned _nextRestingPose = 0;
volatile LONG _shouldForgetRestingPoses = 0; // set by ForgetRestingPoses (any thread), taken by the draw hook

// The resting pose slot of a weapon class; a fresh (empty) slot if it has none yet.
RestingPose* FindRestingPose(void* weaponClass)
{
    for (RestingPose& pose : _restingPoses)
    {
        if (pose.isUsed && pose.weaponClass == weaponClass)
        {
            return &pose;
        }
    }
    RestingPose* pose = &_restingPoses[_nextRestingPose++ % RESTING_POSE_COUNT];
    pose->weaponClass = weaponClass;
    pose->isUsed = true;
    pose->hasSample = false;
    pose->isFinal = false;
    pose->hasWrists = false;
    pose->boneCount = 0;
    return pose;
}

// The most recent final resting pose of another weapon class, or nullptr: it places a weapon that has no resting
// pose of its own yet (b122: the first swap to a weapon showed the game's own lower-and-raise animation, off the
// hand, run123) and tells a resting sample from one taken while the weapon is lowered.
const RestingPose* FindOtherFinalPose(const void* weaponClass)
{
    const RestingPose* found = nullptr;
    for (unsigned step = 1; step <= RESTING_POSE_COUNT && !found; ++step)
    {
        const RestingPose& pose = _restingPoses[(_nextRestingPose + RESTING_POSE_COUNT - step) % RESTING_POSE_COUNT];
        if (pose.isUsed && pose.isFinal && pose.weaponClass != weaponClass)
        {
            found = &pose;
        }
    }
    return found;
}

/// A resting sample must lie this close to another weapon's resting pose (in the arms model): resting weapons were
/// 13 cm apart, the lowered weapon in the middle of a swap 60 cm away (run123).
constexpr float RESTING_SAMPLE_MAXIMUM_METERS = 0.30f;

// True if matrix is a usable placement: finite, three unit-length axes, and (maximumOffsetMeters > 0) no further than
// that from the origin. The game's first-person matrices are not valid on the first draw after a spawn; using them
// once poisoned the resting pose with NaN, which made every later draw take 75 ms and show nothing (headset run 4,
// 2026-10-07). (matrix[element] == matrix[element] is false only for NaN.)
bool IsRigid(const float* matrix, float maximumOffsetMeters)
{
    for (int element = 0; element < 16; ++element)
    {
        if (!(matrix[element] == matrix[element]) || matrix[element] > 1e6f || matrix[element] < -1e6f)
        {
            return false;
        }
    }
    for (int row = 0; row < 3; ++row)
    {
        const float lengthSquared = matrix[row * 4] * matrix[row * 4] + matrix[row * 4 + 1] * matrix[row * 4 + 1] +
                                    matrix[row * 4 + 2] * matrix[row * 4 + 2];
        if (lengthSquared < 0.98f || lengthSquared > 1.02f)
        {
            return false;
        }
    }
    if (maximumOffsetMeters > 0 && matrix[12] * matrix[12] + matrix[13] * matrix[13] + matrix[14] * matrix[14] >
                                       maximumOffsetMeters * maximumOffsetMeters)
    {
        return false;
    }
    return true;
}

// Test (headset run 3): the first-person draw took 80 ms per eye with tracked controllers. While a controller is
// tracked the hook cycles through four modes, 6 s each, and logs the time of each: 0 both hands placed (normal),
// 1 left hand left to the game, 2 right hand (arms and weapon) left to the game, 3 nothing changed.
/// How long each test mode runs.
constexpr DWORD FIRST_PERSON_TEST_MODE_MILLISECONDS = 6000;
unsigned _firstPersonTestMode = 0;
DWORD _firstPersonTestSince = 0;
double _firstPersonTestGameMilliseconds = 0, _firstPersonTestOurMilliseconds = 0,
       _firstPersonTestGameMaximumMilliseconds = 0;
unsigned _firstPersonTestDraws = 0;
bool _isFirstPersonAutoTestOn =
    false; // was on in b56 only; found NaN matrices from a bad first sample (see IsRigid below)

// Time from the performance counter, milliseconds.
double NowMilliseconds()
{
    static LARGE_INTEGER frequency;
    if (!frequency.QuadPart)
    {
        QueryPerformanceFrequency(&frequency);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
}

// Logs one test mode's draw times and the matrices of the last draw (forearm may be null).
void LogFirstPersonTest(const float* arms, const float* weapon, const float* forearm)
{
    FpGuard guard;
    auto axisLength = [](const float* row)
    {
        return static_cast<double>(sqrtf(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]));
    };
    Log::Info(
        "fp",
        "test mode %u (%s): %u draws, game's draw %.1f ms each (max %.1f), our part %.2f ms each",
        _firstPersonTestMode,
        _firstPersonTestMode == 0   ? "both hands placed"
        : _firstPersonTestMode == 1 ? "left hand left to the game"
        : _firstPersonTestMode == 2 ? "right hand left to the game"
                                    : "nothing changed",
        _firstPersonTestDraws,
        _firstPersonTestDraws ? _firstPersonTestGameMilliseconds / _firstPersonTestDraws : 0.0,
        _firstPersonTestGameMaximumMilliseconds,
        _firstPersonTestDraws ? _firstPersonTestOurMilliseconds / _firstPersonTestDraws : 0.0);
    Log::Info(
        "fp",
        "  as drawn last: arms axes %.3f %.3f %.3f pos %.2f %.2f %.2f | weapon axes %.3f %.3f %.3f pos %.2f %.2f "
        "%.2f | base pos %.2f %.2f %.2f",
        axisLength(arms),
        axisLength(arms + 4),
        axisLength(arms + 8),
        static_cast<double>(arms[12]),
        static_cast<double>(arms[13]),
        static_cast<double>(arms[14]),
        axisLength(weapon),
        axisLength(weapon + 4),
        axisLength(weapon + 8),
        static_cast<double>(weapon[12]),
        static_cast<double>(weapon[13]),
        static_cast<double>(weapon[14]),
        static_cast<double>(_baseFrame[12]),
        static_cast<double>(_baseFrame[13]),
        static_cast<double>(_baseFrame[14]));
    if (forearm)
    {
        Log::Info(
            "fp",
            "  left forearm bone (arms space): axes %.3f %.3f %.3f pos %.2f %.2f %.2f",
            axisLength(forearm),
            axisLength(forearm + 4),
            axisLength(forearm + 8),
            static_cast<double>(forearm[12]),
            static_cast<double>(forearm[13]),
            static_cast<double>(forearm[14]));
    }
}

/// Recoil kick of the gun model: how long it lasts, how far it turns up and how far it moves up and back at the
/// start (it fades out with the square of the remaining time).
constexpr DWORD KICK_DURATION_MILLISECONDS = 140;
constexpr float KICK_ANGLE_RADIANS = 0.07f;
constexpr float KICK_UP_METERS = 0.004f;
constexpr float KICK_BACK_METERS = 0.025f;
/// Zoomed: this share of the kick's angle, and no move.
constexpr float KICK_ZOOMED_ANGLE_SHARE = 0.3f;

/// The droid's weapon model matrix moved so that its hp_fire sits at the barrels' opening, its z along them:
/// weapon = translate(-hpFire) x turn(z -> barrel axis) x translate(muzzle) x target (row vectors, weapon frame).
void PlaceOnBarrels(float* weapon, const float* target, const float* hpFire)
{
    const float* axisTo = _barrelAxisInWeapon;
    const float length = sqrtf(axisTo[0] * axisTo[0] + axisTo[1] * axisTo[1] + axisTo[2] * axisTo[2]);
    if (length < 0.5f)
    {
        return;
    }
    const float to[3] = {axisTo[0] / length, axisTo[1] / length, axisTo[2] / length};
    // Rotation taking (0, 0, 1) to `to`: axis = z x to, angle from the dot product (row-vector form).
    float rotation[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float x = -to[1], y = to[0]; // (0,0,1) x to = (-to.y, to.x, 0)
    const float sine = sqrtf(x * x + y * y), cosine = to[2];
    if (sine > 1e-5f)
    {
        const float ux = x / sine, uy = y / sine, oneMinusCosine = 1 - cosine;
        // column-vector Rodrigues matrix for axis (ux, uy, 0), transposed for row vectors
        rotation[0] = cosine + ux * ux * oneMinusCosine;
        rotation[1] = ux * uy * oneMinusCosine;
        rotation[2] = -uy * sine;
        rotation[4] = ux * uy * oneMinusCosine;
        rotation[5] = cosine + uy * uy * oneMinusCosine;
        rotation[6] = ux * sine;
        rotation[8] = uy * sine;
        rotation[9] = -ux * sine;
        rotation[10] = cosine;
    }
    // M = translate(-hpFire) x rotation x translate(muzzle): rows 0-2 from the rotation, origin = muzzle - hpFire x R.
    float placing[16];
    memcpy(placing, rotation, sizeof(placing));
    for (int axis = 0; axis < 3; ++axis)
    {
        placing[12 + axis] =
            _barrelMuzzleInWeapon[axis] + to[axis] * FLASH_AHEAD_OF_OPENING_METERS -
            (hpFire[0] * rotation[axis] + hpFire[1] * rotation[4 + axis] + hpFire[2] * rotation[8 + axis]);
    }
    float moved[16];
    Multiply(moved, placing, target);
    if (IsRigid(moved, 0))
    {
        memcpy(weapon, moved, sizeof(moved));
    }
}

// The game's first-person draw; with an empty right hand (Holster), or while the game's swap still shows the weapon
// the hand let go of (Holster::IsSwapPending: Holster draws the one taken), the weapon object is taken out for the
// call, so the arms are drawn without the weapon.
void DrawGameFirstPerson(void* self, void* edx, unsigned flags)
{
    if (!Holster::IsHandEmpty() && !Holster::IsSwapPending(self))
    {
        _originalFirstPersonRender(self, edx, flags);
        return;
    }
    void** weaponObject = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(self) + OFFSET_firstPersonWeaponObject);
    void* saved = *weaponObject;
    *weaponObject = nullptr;
    _originalFirstPersonRender(self, edx, flags);
    *weaponObject = saved;
}

// ---- Arms on the tank levers (PLAN 54 a2, idren2023 2026-10-17: arms and hands only) ----
// In a vehicle the first-person draw shows the seat's cockpit model instead of the arms (the renderable's model and
// skeleton are the cockpit's). The soldier's arms are drawn there from a copy taken in the last on-foot draws: the
// arms model, every bone as placed (arms-model space) and where the arms model sat relative to the right hand. In the
// tank the arms model is put back at that place relative to the right hand on its lever, the left forearm and wrist
// are set from the left hand as on foot, and the model is drawn with a bone pose table of our own, filled by the
// game's own functions (GameBuild PoseTableInit / PoseTableSet; the arms draw 0x1306E0 builds its table the same way).

/// The copy of the arms (main thread; the game updates and draws on that one thread).
struct ArmsCopy
{
    bool isValid;
    void* model;           // the arms model (FirstPersonRenderable +0x15FC on foot)
    uintptr_t modelVtable; // its vtable when copied: checked before each use
    unsigned boneCount;
    unsigned hashes[64];
    float bones[64][16];     // arms-model space, as drawn on foot
    float gameBones[64][16]; // the same before Aim placed the left forearm and wrist (the game's animation)
    float armsInRightHand[16];
    int forearmBone, wristBone; // left forearm and wrist in hashes[], or -1
    int rightWristBone;         // bone_r_wrist, or -1
    int rightForearmBone;       // bone_r_forearm, or -1
    float forearmLengthMeters;
    bool hasPalm;
    float palmInHand[3]; // the right fist's centre in the right hand pose's frame (where the hand closes round a grip)
};

/// The right fist's centre in bone_r_wrist's frame: the centroid of the hand mesh (imp_inf_trooper_r_hand) of the
/// first-person arms, rep_1st_arctrooper.msh in the mod tools (the skeleton is shared by the stock arms; the hand
/// closes round a rifle's pistol grip there, 11 cm ahead of the wrist). The arms' hp_weapons is the weapon's attach
/// point, 6 cm above and ahead of the fist, and its runtime matrix is not in the bones' space (b255 log: 0.58 m off).
constexpr float FIST_IN_WRIST[3] = {-0.023f, 0.015f, -0.108f};
ArmsCopy _armsCopy = {};
/// FirstPersonRenderable: the arms model (offsets.md first-person rows; the draws 0x1303C0 and 0x1306E0).
constexpr uintptr_t OFFSET_firstPersonArmsModel = 0x15FC;
/// The game's pose table is 0x408 bytes (its stack slot in 0x1306E0 ends where the next local starts); ours has room.
constexpr size_t POSE_TABLE_BYTES = 0x500;
using PoseTableInitFunction = void*(__thiscall*)(void* table);
using PoseTableSetFunction = void(__thiscall*)(void* table, unsigned hash, const float* matrix);
/// A model's draw, vtable slot 1: world matrix, pose table, light, flags, 0 (as Holster's).
using ArmsDrawFunction =
    void(__thiscall*)(void* model, const float* matrix, void* pose, void* light, unsigned flags, int unknown);
/// The arms draw's flags: the render flags with these bits set (0x1306E0).
constexpr unsigned ARMS_DRAW_FLAGS = 0x80001;

// On foot, with the right hand placed, just before the game draws the arms (FirstPersonRenderHook): the copy.
// bones: as placed; gameBones: as the game animated them.
void CopyArms(void* self, const float* arms, const float* bones, const float* gameBones, unsigned boneCount)
{
    const uintptr_t renderable = reinterpret_cast<uintptr_t>(self);
    void* const model = *reinterpret_cast<void**>(renderable + OFFSET_firstPersonArmsModel);
    void* skeleton = *reinterpret_cast<void**>(renderable + OFFSET_firstPersonSkeleton);
    if (!model || !bones || !boneCount || boneCount > 64 || !_isTracked[0] || IsBadReadPtr(model, 4) || !skeleton ||
        IsBadReadPtr(skeleton, 4))
    {
        return;
    }
    const uintptr_t* definition = *reinterpret_cast<const uintptr_t**>(skeleton);
    if (!definition || IsBadReadPtr(definition, 8) || static_cast<unsigned>(definition[1]) != boneCount ||
        IsBadReadPtr(reinterpret_cast<void*>(definition[0]), boneCount * BONE_RECORD_SIZE))
    {
        return;
    }
    static const unsigned FOREARM_HASH = HashName("bone_l_forearm"), WRIST_HASH = HashName("bone_l_wrist"),
                          RIGHT_WRIST_HASH = HashName("bone_r_wrist"), RIGHT_FOREARM_HASH = HashName("bone_r_forearm");
    ArmsCopy& copy = _armsCopy;
    copy.model = model;
    copy.modelVtable = *reinterpret_cast<const uintptr_t*>(model);
    copy.boneCount = boneCount;
    copy.forearmBone = copy.wristBone = copy.rightWristBone = copy.rightForearmBone = -1;
    for (unsigned bone = 0; bone < boneCount; ++bone)
    {
        const unsigned hash =
            *reinterpret_cast<const unsigned*>(definition[0] + bone * BONE_RECORD_SIZE + OFFSET_boneRecordHash);
        copy.hashes[bone] = hash;
        copy.forearmBone = hash == FOREARM_HASH ? static_cast<int>(bone) : copy.forearmBone;
        copy.wristBone = hash == WRIST_HASH ? static_cast<int>(bone) : copy.wristBone;
        copy.rightWristBone = hash == RIGHT_WRIST_HASH ? static_cast<int>(bone) : copy.rightWristBone;
        copy.rightForearmBone = hash == RIGHT_FOREARM_HASH ? static_cast<int>(bone) : copy.rightForearmBone;
    }
    memcpy(copy.bones, bones, boneCount * 64);
    memcpy(copy.gameBones, gameBones, boneCount * 64);
    FpGuard fp;
    float handWorld[16], inverse[16];
    Multiply(handWorld, _trackedPoses[0], _baseFrame);
    InvertRigid(inverse, handWorld);
    Multiply(copy.armsInRightHand, arms, inverse);
    const auto distance = [](const float* first, const float* second)
    {
        const float dx = second[12] - first[12], dy = second[13] - first[13], dz = second[14] - first[14];
        return sqrtf(dx * dx + dy * dy + dz * dz);
    };
    if (copy.forearmBone >= 0 && copy.wristBone >= 0)
    {
        copy.forearmLengthMeters = distance(copy.gameBones[copy.forearmBone], copy.gameBones[copy.wristBone]);
    }
    copy.hasPalm = copy.rightWristBone >= 0;
    float wristInHand[3] = {};
    if (copy.hasPalm)
    {
        // Fist: its place in the wrist's frame, through the wrist bone (arms-model space), into the hand's frame.
        const float* wrist = copy.bones[copy.rightWristBone];
        float fist[3];
        for (int axis = 0; axis < 3; ++axis)
        {
            fist[axis] = FIST_IN_WRIST[0] * wrist[axis] + FIST_IN_WRIST[1] * wrist[4 + axis] +
                         FIST_IN_WRIST[2] * wrist[8 + axis] + wrist[12 + axis];
        }
        const float* toHand = copy.armsInRightHand;
        for (int axis = 0; axis < 3; ++axis)
        {
            copy.palmInHand[axis] =
                fist[0] * toHand[axis] + fist[1] * toHand[4 + axis] + fist[2] * toHand[8 + axis] + toHand[12 + axis];
            wristInHand[axis] = wrist[12] * toHand[axis] + wrist[13] * toHand[4 + axis] + wrist[14] * toHand[8 + axis] +
                                toHand[12 + axis];
        }
    }
    if (!copy.isValid)
    {
        Log::Info(
            "fp",
            "arms copied for the levers (%u bones; left forearm %s, %.3f m; in the right hand's frame: wrist %.3f %.3f "
            "%.3f, fist %.3f %.3f %.3f)",
            boneCount,
            copy.forearmBone >= 0 && copy.wristBone >= 0 ? "found" : "not found",
            static_cast<double>(copy.forearmLengthMeters),
            static_cast<double>(wristInHand[0]),
            static_cast<double>(wristInHand[1]),
            static_cast<double>(wristInHand[2]),
            static_cast<double>(copy.palmInHand[0]),
            static_cast<double>(copy.palmInHand[1]),
            static_cast<double>(copy.palmInHand[2]));
    }
    copy.isValid = true;
}

// The left forearm and wrist on the left hand, as the mirror image of how the right hand holds its grip (run231: placed
// by the wrist with a guessed palm offset, the left hand floated above and ahead of its lever; the right hand, placed
// by the arms' on-foot rifle relation, sat right). X = the right wrist bone in the right hand's frame; mirrored across
// the hand's x plane, with the left skeleton's own axes (the msh: the right arm's bones are the left's turned half way
// round y, its wrist at -z where the left's is at +z): X' = diag(1, 1, -1) x X x mirror(x). The left wrist = X' x the
// left hand; the forearm sits the forearm's length behind it (-z), turned as the wrist (as on foot: the arms end at
// the elbow, so there is nothing to bend). World space in, arms-model space out. FpGuard held.
void PlaceLeftArm(
    const ArmsCopy& copy,
    const float* arms,
    const float* rightWorld,
    const float* leftRest,
    const float* leftFrame,
    float (*bones)[16])
{
    float rightWrist[16], inverse[16], wristInHand[16], leftWorld[16], leftWrist[16], forearm[16];
    Multiply(rightWrist, bones[copy.rightWristBone], arms);
    InvertRigid(inverse, rightWorld);
    Multiply(wristInHand, rightWrist, inverse);
    for (int column = 0; column < 4; ++column)
    {
        wristInHand[8 + column] = -wristInHand[8 + column]; // diag(1, 1, -1) on the left: the local z row
    }
    for (int row = 0; row < 4; ++row)
    {
        wristInHand[row * 4] = -wristInHand[row * 4]; // mirror(x) on the right: every x component
    }
    Multiply(leftWorld, leftRest, leftFrame);
    Multiply(leftWrist, wristInHand, leftWorld);
    const float behind[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -copy.forearmLengthMeters, 1};
    Multiply(forearm, behind, leftWrist);
    InvertRigid(inverse, arms);
    Multiply(bones[copy.wristBone], leftWrist, inverse);
    Multiply(bones[copy.forearmBone], forearm, inverse);
}

/// A forearm on a held turret grip (PLAN 58 u, idren2023 2026-10-20: the forearms must read as joined to the upper arms
/// and the shoulders while the gun pitches). The first-person arms end at the elbow, and a forearm trailing straight
/// behind the hand swings with the handle; instead it points from the elbow of a two-bone arm (shoulder below the
/// eyes, bent down and out) to the wrist. Shoulder and upper arm: an adult's proportions, not measured on the model.
constexpr float SHOULDER_BELOW_EYES_METERS = 0.24f;
constexpr float SHOULDER_HALF_WIDTH_METERS = 0.19f;
constexpr float SHOULDER_BEHIND_EYES_METERS = 0.07f;
constexpr float UPPER_ARM_METERS = 0.30f;

// The forearm bone (arms-model space) turned about the wrist, by the least rotation, from where it lies to the line
// from the solved elbow to the wrist; the wrist stays. The bone's own axes are not assumed (the source model's right
// forearm reaches its wrist along -z, the left along +z). FpGuard held.
void ConnectForearm(int hand, const float* arms, int forearmBone, int wristBone, float (*bones)[16])
{
    float head[16];
    if (forearmBone < 0 || wristBone < 0 || !(Stereo::GetHeadWorld(head) || Stereo::GetLastHeadWorld(head)))
    {
        return;
    }
    const auto dot = [](const float* a, const float* b)
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    const auto normalize = [&dot](float* v)
    {
        const float size = sqrtf(dot(v, v));
        if (size < 1e-4f)
        {
            return false;
        }
        v[0] /= size;
        v[1] /= size;
        v[2] /= size;
        return true;
    };
    float wrist[16], forearm[16];
    Multiply(wrist, bones[wristBone], arms);
    Multiply(forearm, bones[forearmBone], arms);
    float current[3] = {wrist[12] - forearm[12], wrist[13] - forearm[13], wrist[14] - forearm[14]};
    const float length = sqrtf(dot(current, current));
    // The head's level right and back (camera convention: rows right, up, back).
    float right[3] = {head[0], 0.0f, head[2]}, back[3] = {head[8], 0.0f, head[10]};
    if (!normalize(current) || !normalize(right) || !normalize(back))
    {
        return;
    }
    const float side = hand == 0 ? 1.0f : -1.0f;
    float shoulder[3], direction[3];
    for (int axis = 0; axis < 3; ++axis)
    {
        shoulder[axis] = head[12 + axis] + side * SHOULDER_HALF_WIDTH_METERS * right[axis] +
                         SHOULDER_BEHIND_EYES_METERS * back[axis];
    }
    shoulder[1] -= SHOULDER_BELOW_EYES_METERS;
    for (int axis = 0; axis < 3; ++axis)
    {
        direction[axis] = wrist[12 + axis] - shoulder[axis];
    }
    const float distance = sqrtf(dot(direction, direction));
    if (!normalize(direction))
    {
        return;
    }
    // The elbow on the circle where both bones meet, on the side toward down, out and a little back; out of reach the
    // arm is straight.
    float pole[3] = {side * 0.5f * right[0] + 0.3f * back[0], -1.0f, side * 0.5f * right[2] + 0.3f * back[2]};
    const float along = dot(pole, direction);
    for (int axis = 0; axis < 3; ++axis)
    {
        pole[axis] -= along * direction[axis];
    }
    const float upper = UPPER_ARM_METERS;
    const float reach = fminf(fmaxf(distance, fabsf(upper - length) + 0.01f), upper + length);
    const float toCentre = (upper * upper - length * length + reach * reach) / (2.0f * reach);
    const float radius = normalize(pole) ? sqrtf(fmaxf(upper * upper - toCentre * toCentre, 0.0f)) : 0.0f;
    float wanted[3];
    for (int axis = 0; axis < 3; ++axis)
    {
        const float elbow = shoulder[axis] + toCentre * direction[axis] + radius * pole[axis];
        wanted[axis] = wrist[12 + axis] - elbow;
    }
    if (!normalize(wanted))
    {
        return;
    }
    // The least rotation taking `current` onto `wanted` (Rodrigues, about their cross product), applied to the axes.
    float about[3] = {
        current[1] * wanted[2] - current[2] * wanted[1],
        current[2] * wanted[0] - current[0] * wanted[2],
        current[0] * wanted[1] - current[1] * wanted[0]};
    const float sine = sqrtf(dot(about, about)), cosine = dot(current, wanted);
    if (sine > 1e-4f)
    {
        normalize(about);
        for (int row = 0; row < 3; ++row)
        {
            float* v = forearm + row * 4;
            const float across[3] = {
                about[1] * v[2] - about[2] * v[1],
                about[2] * v[0] - about[0] * v[2],
                about[0] * v[1] - about[1] * v[0]};
            const float onAxis = dot(about, v) * (1.0f - cosine);
            for (int axis = 0; axis < 3; ++axis)
            {
                v[axis] = v[axis] * cosine + across[axis] * sine + about[axis] * onAxis;
            }
        }
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        forearm[12 + axis] = wrist[12 + axis] - length * wanted[axis];
    }
    float inverse[16];
    InvertRigid(inverse, arms);
    Multiply(bones[forearmBone], forearm, inverse);
}

// A hand's pose in a seat with handles (tank levers, bike grips, the fighter tanks' gunner station): on its handle
// while held, else the controller's; false when the seat has none or the hand is not tracked.
bool GetSeatHandPose(int hand, float* restPose)
{
    if (!(Levers::GetHandPose(hand, restPose) || Handlebars::GetHandPose(hand, restPose) ||
          GunnerSeat::GetHandPose(hand, restPose) || Flight::GetHandPose(hand, restPose) ||
          WalkerDrive::GetHandPose(hand, restPose)))
    {
        return false;
    }
    // A free hand (on no handle): the seat modules hand back Gesture's copy of the controller pose, taken at the last
    // Present, but the arms are drawn in this frame's base frame (BeforeRender). That copy is a frame old, and on the
    // first frame in a seat it is in the resting space from before the recentre on entry (Vehicle::OnPresent recentres
    // after Gesture::OnPresent took it): the hands were drawn displaced for a frame and lagged after ("hands spotty on
    // entering vehicles", headset run 19). This frame's pose instead, located with the head the eyes are drawn from.
    const bool isOnHandle = Levers::IsHolding(hand) || Handlebars::IsHolding(hand) || GunnerSeat::IsHolding(hand) ||
                            Flight::IsHolding(hand) || WalkerDrive::IsHolding(hand);
    float fresh[16];
    if (!isOnHandle && Xr::GetHandPose(hand & 1, fresh))
    {
        memcpy(restPose, fresh, sizeof(fresh));
    }
    return true;
}

// On a bike or STAP with a unit whose hands are ours (PLAN 61 j): each hand round its grip's bar (BodyArm DrawOnBar)
// while it holds it, else on its controller with the controller poses, as on foot. False when the stock arms are
// drawn instead (no hands of ours for this unit).
bool DrawOurBikeHands()
{
    if (!BodyArm::IsEmptyHandWanted() || !_hasBaseFrame)
    {
        return false;
    }
    void* device = D3dSurvey::PresentingDevice();
    Xr::ControllerInput pad{};
    Xr::GetControllerInput(&pad);
    for (int hand = 0; hand < 2; ++hand)
    {
        float bar[16], shoulder[3], twist = 0, grip[16];
        if (Handlebars::GetBarFrame(hand, bar, shoulder, &twist))
        {
            BodyArm::DrawOnBar(device, bar, shoulder, twist, hand == 1);
        }
        else if (Xr::GetGripPose(hand, grip))
        {
            FpGuard fp;
            float gripWorld[16];
            Multiply(gripWorld, grip, _baseFrame);
            const float trigger = pad.isValid ? pad.trigger[hand] : 0.0f;
            if (hand == 0)
            {
                BodyArm::DrawEmptyHand(device, gripWorld, trigger);
            }
            else
            {
                BodyArm::DrawLeft(device, gripWorld, trigger, false);
            }
        }
    }
    return true;
}

// In any other seat with handles (tank levers, a flyer's stick and throttle, a walker's lever, a gunner's grips) with a
// unit whose hands are ours (idren2023 2026-11-08: our hands in every vehicle, not only on the bikes): each hand where the
// seat puts it, placed as DrawArmsOnLevers places the stock arms (on a lever in the cockpit's frame, else in the base
// frame), our hand drawn round it as round the controller's handle (the seat modules put the controller's pose where
// the handle runs through the fist), closed while it holds its handle, else posed by its trigger. False when the
// stock arms are drawn instead (no hands of ours for this unit, or the seat has no handles).
bool DrawOurSeatHands()
{
    float probe[16];
    if (!BodyArm::IsEmptyHandWanted() || !_hasBaseFrame || !GetSeatHandPose(0, probe))
    {
        return false;
    }
    void* device = D3dSurvey::PresentingDevice();
    Xr::ControllerInput pad{};
    Xr::GetControllerInput(&pad);
    float cockpit[16];
    const bool hasCockpit = CockpitShell::GetPilotFrame(cockpit);
    for (int hand = 0; hand < 2; ++hand)
    {
        float pose[16], gripPose[16], aimPose[16];
        if (!GetSeatHandPose(hand, pose) || !Xr::GetGripPose(hand, gripPose) || !Xr::GetHandPose(hand, aimPose))
        {
            continue;
        }
        const bool isOnLever =
            Levers::IsHolding(hand) || Flight::IsHolding(hand) || (hand == 0 && WalkerDrive::IsHolding(0));
        const bool isHeld = isOnLever || GunnerSeat::IsHolding(hand) || WalkerDrive::IsHolding(hand);
        FpGuard fp;
        float handWorld[16], inverseAim[16], gripInAim[16], gripWorld[16];
        Multiply(handWorld, pose, isOnLever && hasCockpit ? cockpit : _baseFrame);
        InvertRigid(inverseAim, aimPose);
        Multiply(gripInAim, gripPose, inverseAim);
        Multiply(gripWorld, gripInAim, handWorld);
        const float trigger = isHeld ? 1.0f : pad.isValid ? pad.trigger[hand] : 0.0f;
        if (hand == 0)
        {
            BodyArm::DrawEmptyHand(device, gripWorld, trigger);
        }
        else
        {
            BodyArm::DrawLeft(device, gripWorld, trigger, false);
        }
    }
    return true;
}

// In a tank's pilot seat, after the cockpit and the levers, or on a bike after its model: the copied arms with the
// hands on the levers or grips.
void DrawArmsOnLevers(unsigned flags)
{
    ArmsCopy& copy = _armsCopy;
    float right[16], left[16];
    if (!copy.isValid || !GameBuild::IsAvailable(GameBuild::Feature::VehicleHands) || !GetSeatHandPose(0, right) ||
        IsBadReadPtr(copy.model, 4) || *reinterpret_cast<const uintptr_t*>(copy.model) != copy.modelVtable)
    {
        return;
    }
    static float bones[64][16];
    float arms[16];
    {
        FpGuard fp;
        // A hand on a tank lever is placed in the cockpit's frame, as the lever is drawn (Levers::Draw); else in the
        // base frame.
        float cockpit[16];
        const bool hasCockpit = CockpitShell::GetPilotFrame(cockpit);
        const float* rightFrame = hasCockpit && (Levers::IsHolding(0) || Flight::IsHolding(0) || WalkerDrive::IsHolding(0)) ? cockpit : _baseFrame;
        const float* leftFrame = hasCockpit && (Levers::IsHolding(1) || Flight::IsHolding(1)) ? cockpit : _baseFrame;
        float rightWorld[16];
        Multiply(rightWorld, right, rightFrame);
        Multiply(arms, copy.armsInRightHand, rightWorld);
        memcpy(bones, copy.bones, copy.boneCount * 64);
        if (copy.forearmBone >= 0 && copy.wristBone >= 0 && copy.rightWristBone >= 0 && GetSeatHandPose(1, left))
        {
            PlaceLeftArm(copy, arms, rightWorld, left, leftFrame, bones);
        }
        if (GunnerSeat::IsHolding(0))
        {
            ConnectForearm(0, arms, copy.rightForearmBone, copy.rightWristBone, bones);
        }
        if (GunnerSeat::IsHolding(1) && GetSeatHandPose(1, left))
        {
            ConnectForearm(1, arms, copy.forearmBone, copy.wristBone, bones);
        }
    }
    alignas(16) static unsigned char table[POSE_TABLE_BYTES];
    const auto init = reinterpret_cast<PoseTableInitFunction>(GameBuild::Absolute(GameBuild::Address::PoseTableInit));
    const auto set = reinterpret_cast<PoseTableSetFunction>(GameBuild::Absolute(GameBuild::Address::PoseTableSet));
    init(table);
    for (unsigned bone = 0; bone < copy.boneCount; ++bone)
    {
        set(table, copy.hashes[bone], bones[bone]);
    }
    void* light = reinterpret_cast<void*>(GameBuild::Absolute(GameBuild::Address::ModelLight));
    const auto draw = (*reinterpret_cast<ArmsDrawFunction**>(copy.model))[1];
    draw(copy.model, arms, table, light, flags | ARMS_DRAW_FLAGS, 0);
}

/// Gunner seat log: at most this often.
constexpr DWORD GUNNER_LOG_MILLISECONDS = 1000;
/// Test `gunner frame 0`: the game's first-person model of a gunner seat is left out.
bool _isGunnerFrameOn = true;

/// Gunner and passenger seat diagnostic (PLAN 56): at a seat change and then once a second for a few seconds, where the
/// game camera sits in the hull's level frame (right, up, forward of the vehicle) and in the turret's own frame
/// (MountedTurret +0x210 rows), and where the seat's first-person model is drawn relative to the camera.
void LogGunnerSeat(uintptr_t renderable, int state)
{
    static uintptr_t loggedSeat = 0;
    static int logsLeft = 0;
    static DWORD lastLog = 0;
    const uintptr_t seatControllable = Vehicle::CurrentSeatControllable();
    if (seatControllable != loggedSeat)
    {
        loggedSeat = seatControllable;
        logsLeft = 4;
    }
    const DWORD now = GetTickCount();
    float camera[16], hull[16], turret[16];
    if (logsLeft <= 0 || now - lastLog < GUNNER_LOG_MILLISECONDS || !Stereo::GetGameCamera(camera) ||
        !Vehicle::GetHullMatrix(hull))
    {
        return;
    }
    --logsLeft;
    lastLog = now;
    FpGuard fp;
    const bool hasTurret = Vehicle::GetTurretMatrix(turret);
    const float hullLevel = sqrtf(hull[8] * hull[8] + hull[10] * hull[10]);
    const float forwardX = hull[8] / hullLevel, forwardZ = hull[10] / hullLevel;
    const float offset[3] = {camera[12] - hull[12], camera[13] - hull[13], camera[14] - hull[14]};
    const float turretOffset[3] = {camera[12] - turret[12], camera[13] - turret[13], camera[14] - turret[14]};
    float inverseCamera[16], model[16];
    InvertRigid(inverseCamera, camera);
    Multiply(model, reinterpret_cast<const float*>(renderable + OFFSET_firstPersonArmsMatrix), inverseCamera);
    Log::Info(
        "fp",
        "gunner seat (%s) state %d: camera in the hull right %.2f up %.2f ahead %.2f; in the turret %s %.2f %.2f "
        "%.2f; turret origin in the hull right %.2f up %.2f ahead %.2f; model in the camera %.2f %.2f %.2f",
        Vehicle::CurrentStockName() ? Vehicle::CurrentStockName() : "not stock",
        state,
        static_cast<double>(-offset[0] * forwardZ + offset[2] * forwardX),
        static_cast<double>(offset[1]),
        static_cast<double>(offset[0] * forwardX + offset[2] * forwardZ),
        hasTurret ? "x y z" : "(none)",
        static_cast<double>(turretOffset[0] * turret[0] + turretOffset[1] * turret[1] + turretOffset[2] * turret[2]),
        static_cast<double>(turretOffset[0] * turret[4] + turretOffset[1] * turret[5] + turretOffset[2] * turret[6]),
        static_cast<double>(turretOffset[0] * turret[8] + turretOffset[1] * turret[9] + turretOffset[2] * turret[10]),
        static_cast<double>(-(turret[12] - hull[12]) * forwardZ + (turret[14] - hull[14]) * forwardX),
        static_cast<double>(turret[13] - hull[13]),
        static_cast<double>((turret[12] - hull[12]) * forwardX + (turret[14] - hull[14]) * forwardZ),
        static_cast<double>(model[12]),
        static_cast<double>(model[13]),
        static_cast<double>(model[14]));
}

/// FirstPersonRenderable: what it draws (int): 0 the soldier's arms and weapon; 1 and 3 a model at the arms matrix
/// only, 2 the same plus a weapon at the weapon matrix (cockpits and turret guns of vehicles, at their hp_camera).
/// Observed in Render 0x130320's jump table (0x130388).
constexpr uintptr_t OFFSET_firstPersonState = 0x160C;
/// Cockpit log: at most this often, and this many lines per seat.
constexpr DWORD COCKPIT_LOG_MILLISECONDS = 1000;
constexpr int COCKPIT_LOGS_PER_SEAT = 20;

// In a vehicle (state 1-3) the game draws the seat's cockpit or gun model at its hp_camera, built from its own camera
// (0x131EA0, 0x1328C0), and in first person that camera follows the seat's aim, which follows the head (Vehicle): the
// cockpit would swing with the head's pitch (run219, AAT). In a pilot seat the model is turned from the game camera's
// frame onto the hull's, about the camera's position: cockpit x inverse(game camera) x hull camera, where the hull
// camera has the hull's axes (right, up, back) at the game camera's position. Hovers and walkers: the hull's heading
// only, level, as the game's own camera at zero aim (run224: an AAT standing still rocked 9 to 21 degrees in pitch
// while the game's camera stayed level; with its full pitch the cockpit window tilted out of view). A gunner seat or
// turret keeps the game's placement (its model is the gun, which does turn with the aim). No hand placement in any
// vehicle state. Returns false on foot (state 0): the caller draws as for the soldier.
bool DrawVehicleFirstPerson(void* self, void* edx, unsigned flags)
{
    const uintptr_t renderable = reinterpret_cast<uintptr_t>(self);
    const int state = *reinterpret_cast<const int*>(renderable + OFFSET_firstPersonState);
    if (state == 0)
    {
        return false;
    }
    const Vehicle::Seat seat = Vehicle::CurrentSeat();
    // A bike or STAP: its own third-person model round the rider instead of the game's stretched first-person model,
    // then the arms (on the grips while held). A mount (tauntaun, Kaadu): the game draws the animal itself (Handlebars'
    // visibility hook), the stock first-person model left out, the arms on the reins.
    if ((seat == Vehicle::Seat::Hover || seat == Vehicle::Seat::Walker) &&
        Vehicle::CurrentDrive() == Vehicle::Drive::Handlebars && Handlebars::DrawHull(flags))
    {
        if (!DrawOurBikeHands())
        {
            DrawArmsOnLevers(flags);
        }
        return true;
    }
    const bool isPilot = seat == Vehicle::Seat::Hover || seat == Vehicle::Seat::Flyer || seat == Vehicle::Seat::Walker;
    if (seat == Vehicle::Seat::Turret || seat == Vehicle::Seat::Passenger)
    {
        LogGunnerSeat(renderable, state);
        if (seat == Vehicle::Seat::Turret && GunnerSeat::DrawTurret(flags))
        {
            // Our gunner's station in place of the game's targeting frame (fighter tanks, PLAN 56), then the arms
            // (on its grips while held).
            if (!DrawOurSeatHands())
            {
                DrawArmsOnLevers(flags);
            }
            return true;
        }
        if (!_isGunnerFrameOn)
        {
            return true; // test: the game's gunner model (the stock tanks' targeting frames) left out
        }
    }
    float gameCamera[16], hull[16];
    if (!isPilot || !Stereo::GetGameCamera(gameCamera) || !Vehicle::GetHullMatrix(hull))
    {
        _originalFirstPersonRender(self, edx, flags);
        return true;
    }
    float* cockpit = reinterpret_cast<float*>(renderable + OFFSET_firstPersonArmsMatrix);
    float* weapon = reinterpret_cast<float*>(renderable + OFFSET_firstPersonWeaponMatrix);
    float savedCockpit[16], savedWeapon[16], inverseGame[16], correction[16];
    memcpy(savedCockpit, cockpit, sizeof(savedCockpit));
    memcpy(savedWeapon, weapon, sizeof(savedWeapon));
    {
        FpGuard fp;
        // Hull camera: back = -forward, right = up x back, up again = back x right (unit, right-handed as the game's
        // camera: row 0 right, 1 up, 2 back).
        const float hullElevation = asinf(hull[9] > 1.0f ? 1.0f : hull[9] < -1.0f ? -1.0f : hull[9]);
        if (seat != Vehicle::Seat::Flyer)
        {
            const float level = sqrtf(hull[8] * hull[8] + hull[10] * hull[10]);
            if (level > 0.1f)
            {
                hull[8] /= level;
                hull[10] /= level;
            }
            hull[9] = 0.0f;
            hull[4] = hull[6] = 0.0f;
            hull[5] = 1.0f;
        }
        const float back[3] = {-hull[8], -hull[9], -hull[10]};
        float right[3] = {
            hull[5] * back[2] - hull[6] * back[1],
            hull[6] * back[0] - hull[4] * back[2],
            hull[4] * back[1] - hull[5] * back[0]};
        const float length = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
        if (length < 0.5f)
        {
            _originalFirstPersonRender(self, edx, flags);
            return true;
        }
        for (float& value : right)
        {
            value /= length;
        }
        const float up[3] = {
            back[1] * right[2] - back[2] * right[1],
            back[2] * right[0] - back[0] * right[2],
            back[0] * right[1] - back[1] * right[0]};
        const float hullCamera[16] = {
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
            gameCamera[12],
            gameCamera[13],
            gameCamera[14],
            1};
        InvertRigid(inverseGame, gameCamera);
        Multiply(correction, inverseGame, hullCamera);
        // A stock tank's shell seats the pilot closer to the window than the game's eye (PLAN 57 e, idren2023 2026-10-18;
        // ShellDesigns::PilotEye): the cockpit model (and the shell drawn with its matrix) is moved back by the
        // difference, in the model's own axes, so that the shell's pilot eye comes to the camera.
        float seatedCockpit[16], seatedWeapon[16], pilotEye[3];
        memcpy(seatedCockpit, savedCockpit, sizeof(seatedCockpit));
        memcpy(seatedWeapon, savedWeapon, sizeof(seatedWeapon));
        if (seat == Vehicle::Seat::Walker)
        {
            // A walker's cockpit held still round the eye (headset run 19, idren2023: "rather have the world out the window
            // jump around but be stable in the seat"; run627: the game swings the AT-ST's cockpit model up to 25 cm
            // against its camera with each stride): its place in the camera's frame is averaged over
            // WALKER_COCKPIT_SECONDS and the cockpit (and the weapon with it) drawn there.
            static float average[3] = {};
            static bool hasAverage = false;
            static DWORD lastTick = 0;
            const DWORD tick = GetTickCount();
            const float seconds = lastTick ? static_cast<float>(tick - lastTick) * 0.001f : 0.0f;
            lastTick = tick;
            float local[3];
            const float offset[3] = {savedCockpit[12] - gameCamera[12], savedCockpit[13] - gameCamera[13],
                                     savedCockpit[14] - gameCamera[14]};
            for (int row = 0; row < 3; ++row)
            {
                local[row] = offset[0] * gameCamera[row * 4] + offset[1] * gameCamera[row * 4 + 1] +
                             offset[2] * gameCamera[row * 4 + 2];
            }
            if (!hasAverage || seconds > 0.5f)
            {
                memcpy(average, local, sizeof(average));
                hasAverage = true;
            }
            const float share = 1.0f - expf(-seconds / WALKER_COCKPIT_SECONDS);
            float shift[3] = {};
            for (int row = 0; row < 3; ++row)
            {
                average[row] += (local[row] - average[row]) * share;
                const float change = average[row] - local[row];
                for (int column = 0; column < 3; ++column)
                {
                    shift[column] += change * gameCamera[row * 4 + column];
                }
            }
            for (int column = 0; column < 3; ++column)
            {
                seatedCockpit[12 + column] += shift[column];
                seatedWeapon[12 + column] += shift[column];
            }
        }
        if (ShellDesigns::PilotEye(Vehicle::CurrentStockName(), pilotEye))
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                const float offset = LeverPlacement::COCKPIT_EYE[axis] - pilotEye[axis];
                for (int column = 0; column < 3; ++column)
                {
                    seatedCockpit[12 + column] += offset * savedCockpit[axis * 4 + column];
                    seatedWeapon[12 + column] += offset * savedCockpit[axis * 4 + column];
                }
            }
        }
        Multiply(cockpit, seatedCockpit, correction);
        Multiply(weapon, seatedWeapon, correction);
        static Vehicle::Seat loggedSeat = Vehicle::Seat::None;
        static int logsLeft = 0;
        static DWORD lastLog = 0;
        if (seat != loggedSeat)
        {
            loggedSeat = seat;
            logsLeft = COCKPIT_LOGS_PER_SEAT;
        }
        const DWORD now = GetTickCount();
        if (logsLeft > 0 && now - lastLog >= COCKPIT_LOG_MILLISECONDS)
        {
            --logsLeft;
            lastLog = now;
            // The cockpit in the game camera's frame (constant if the game builds it from that camera), and the
            // game camera's and the hull's forward elevation (degrees).
            float local[16];
            Multiply(local, savedCockpit, inverseGame);
            constexpr float DEGREES = 57.29578f;
            Log::Info(
                "fp",
                "cockpit state %d: in game camera pos %.3f %.3f %.3f z %.3f %.3f %.3f; elevation camera %.1f hull %.1f",
                state,
                static_cast<double>(local[12]),
                static_cast<double>(local[13]),
                static_cast<double>(local[14]),
                static_cast<double>(local[8]),
                static_cast<double>(local[9]),
                static_cast<double>(local[10]),
                static_cast<double>(asinf(-gameCamera[9]) * DEGREES),
                static_cast<double>(hullElevation * DEGREES));
        }
    }
    // Openings the shell cuts in the game's cockpit model (the combat speeder's front right window), then the model.
    CockpitShell::CutOpenings(D3dSurvey::PresentingDevice(), cockpit, Vehicle::CurrentStockName());
    if (!ShellDesigns::IsWholeCockpit(Vehicle::CurrentStockName())) // a starfighter's shell is its whole cockpit
    {
        _originalFirstPersonRender(self, edx, flags);
    }
    // The stock tank's cabin round the cockpit, with the cockpit's own matrix; then the levers and the arms.
    CockpitShell::Draw(D3dSurvey::PresentingDevice(), cockpit, Vehicle::CurrentStockName());
    Levers::Draw(D3dSurvey::PresentingDevice()); // after the cockpit, in the same pass (Levers.h)
    Flight::Draw(D3dSurvey::PresentingDevice()); // a flyer's stick, throttle and lever (Flight.h)
    WalkerDrive::Draw(D3dSurvey::PresentingDevice()); // a walker's throttle lever (WalkerDrive.h)
    ControllerHolo::Draw(D3dSurvey::PresentingDevice()); // the controllers' holograms (ControllerHolo.h)
    if (!DrawOurSeatHands())
    {
        DrawArmsOnLevers(flags);
    }
    memcpy(cockpit, savedCockpit, sizeof(savedCockpit));
    memcpy(weapon, savedWeapon, sizeof(savedWeapon));
    return true;
}

// Hook on FirstPersonRenderable::Render (render thread, once per eye pass while the first-person model is shown).
// self (in ECX, thiscall) is the FirstPersonRenderable; edx is unused. Places the weapon and arms on the right hand
// (or moves them by the "fp" test offset) and the left forearm and hand on the left hand, calls the game's draw, and
// puts every matrix the game had back, so the game's own state is untouched after the call.
void __fastcall FirstPersonRenderHook(void* self, void* edx, unsigned flags)
{
    if (Stereo::IsScopePass())
    {
        return; // the scope's camera sits on the fire line: the weapon would only block its view
    }
    InterlockedIncrement(&_firstPersonDrawCount);
    if (DrawVehicleFirstPerson(self, edx, flags))
    {
        return;
    }
    const uintptr_t camera = _redCamera;
    const bool isTesting = _isFirstPersonAutoTestOn && (_isTracked[0] || _isTracked[1]) && _hasBaseFrame;
    const unsigned mode = isTesting ? _firstPersonTestMode : 0;
    const bool isHandPlaced = (_isTestHandOn || _isTracked[0]) && _hasBaseFrame && !(mode & 2) && !_isDroideka;
    const bool isOffsetPlaced = !isHandPlaced && _isFirstPersonOffsetOn && camera;
    const bool isLeftHandPlaced = (_isTestLeftHandOn || _isTracked[1]) && _hasBaseFrame && !(mode & 1) && !_isDroideka;
    const double startMilliseconds = NowMilliseconds();
    if (!isHandPlaced && !isOffsetPlaced && !isLeftHandPlaced)
    {
        DrawGameFirstPerson(self, edx, flags);
        Holster::Draw(self, flags);
        if (isTesting)
        {
            const double drawMilliseconds = NowMilliseconds() - startMilliseconds;
            _firstPersonTestGameMilliseconds += drawMilliseconds;
            if (drawMilliseconds > _firstPersonTestGameMaximumMilliseconds)
            {
                _firstPersonTestGameMaximumMilliseconds = drawMilliseconds;
            }
            ++_firstPersonTestDraws;
            if (GetTickCount() - _firstPersonTestSince >= FIRST_PERSON_TEST_MODE_MILLISECONDS)
            {
                LogFirstPersonTest(
                    reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(self) + OFFSET_firstPersonArmsMatrix),
                    reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(self) + OFFSET_firstPersonWeaponMatrix),
                    nullptr);
                _firstPersonTestMode = (_firstPersonTestMode + 1) & 3;
                _firstPersonTestSince = GetTickCount();
                _firstPersonTestGameMilliseconds = _firstPersonTestOurMilliseconds =
                    _firstPersonTestGameMaximumMilliseconds = 0;
                _firstPersonTestDraws = 0;
            }
        }
        return;
    }

    float* arms = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(self) + OFFSET_firstPersonArmsMatrix);
    float* weapon = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(self) + OFFSET_firstPersonWeaponMatrix);
    float savedArms[16], savedWeapon[16], savedLeftArmBones[2][16];
    float mirroredArms[16], mirroredWeapon[16]; // the droid's left arm (see _isMirrorWanted)
    bool hasMirror = false;
    float* leftArmBones[2] = {nullptr, nullptr};
    static float savedAllBones[64][16]; // every bone as the game animated it, put back after the draw
    unsigned boneCount = 0;
    float* allBones = isHandPlaced ? FindBones(self, &boneCount) : nullptr;
    if (allBones)
    {
        memcpy(savedAllBones, allBones, boneCount * 64);
    }
    void* weaponClass = isHandPlaced ? CurrentWeaponClass() : nullptr;
    // what our arm holds: the game's weapon, or the secondary Holster draws in the hand (the game's weapon is still the
    // primary then; 2026-11-01: grips for the grenade, mines, time bomb, health / ammo, recon droid, fusion cutter)
    const uintptr_t heldSecondary = isHandPlaced && Holster::IsHandEmpty() ? Holster::HeldSecondaryClass() : 0;
    void* armClass = heldSecondary ? reinterpret_cast<void*>(heldSecondary) : weaponClass;
    bool isFrozen = false;
    memcpy(savedArms, arms, sizeof(savedArms));
    memcpy(savedWeapon, weapon, sizeof(savedWeapon));
    if (isLeftHandPlaced)
    {
        static const unsigned FOREARM_HASH = HashName("bone_l_forearm"), WRIST_HASH = HashName("bone_l_wrist");
        const bool shouldLogAll = _leftHandLogsLeft > 0;
        leftArmBones[0] = FindBoneMatrix(self, FOREARM_HASH, shouldLogAll);
        leftArmBones[1] = FindBoneMatrix(self, WRIST_HASH, false);
        if (shouldLogAll)
        {
            InterlockedDecrement(&_leftHandLogsLeft);
            Log::Info(
                "fp",
                "left forearm bone %p (hash 0x%08x), wrist %p (0x%08x)",
                static_cast<void*>(leftArmBones[0]),
                FOREARM_HASH,
                static_cast<void*>(leftArmBones[1]),
                WRIST_HASH);
        }
        if (!leftArmBones[0] || !leftArmBones[1])
        {
            leftArmBones[0] = leftArmBones[1] = nullptr;
        }
        else
        {
            memcpy(savedLeftArmBones[0], leftArmBones[0], 64);
            memcpy(savedLeftArmBones[1], leftArmBones[1], 64);
        }
    }
    {
        FpGuard guard;
        const float turn[16] =
            {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1}; // a model looks along +z, a pose along -z
        float inverse[16];
        if (isHandPlaced)
        {
            // K = the weapon in the arms model's space: where the game's animation holds it (idle, recoil, reload).
            // K0 = its resting value: the last K that stayed unchanged for 20 calls. The arms model is placed so
            // that the resting weapon sits exactly on the hand pose; the animation then plays around it.
            static float lastWeaponInArms[16];
            static int stableDraws = 0;
            static void* lastWeaponClass = nullptr;
            static int drawsSinceChange = 0; // draws since the weapon class changed, for the swap log below
            float weaponInArms[16], handWorld[16], target[16];
            if (InterlockedExchange(&_shouldForgetRestingPoses, 0))
            {
                for (RestingPose& pose : _restingPoses)
                {
                    pose.isUsed = false;
                }
                Log::Info("fp", "resting poses forgotten, taking them again");
            }
            RestingPose* rest = FindRestingPose(weaponClass);
            if (weaponClass != lastWeaponClass)
            {
                lastWeaponClass = weaponClass;
                stableDraws = 0;
                drawsSinceChange = 0;
            }
            InvertRigid(inverse, savedArms);
            Multiply(weaponInArms, savedWeapon, inverse);
            // Only a sane sample may become the resting pose: the weapon within 2 m of the arms model's origin.
            const bool isSane = IsRigid(savedArms, 0) && IsRigid(savedWeapon, 0) && IsRigid(weaponInArms, 2.0f);
            if (isSane)
            {
                stableDraws = IsNear(weaponInArms, lastWeaponInArms) ? stableDraws + 1 : 0;
                memcpy(lastWeaponInArms, weaponInArms, sizeof(weaponInArms));
                // b93: the first sample waits until the weapon has held still for 12 draws, and the final one comes
                // after 72 (it was: first sane draw, and 144). After a weapon change the first sane draw is in the
                // middle of the draw animation, and in a fight the weapon rarely holds still for a second, so that
                // sample stayed and the weapon sat off the hand (idren2023, headset run 9; cause inferred, not
                // reproduced).
                // b122: the weapon class changes when the swap begins and the lowered weapon then holds still for
                // about 20 draws, so b93's first sample was often taken there (run123); a sample far from another
                // weapon's resting pose is refused during the swap (the first 150 draws after the change; the raise
                // ended after about 125 in run123), so a weapon held differently still gets its own pose later.
                constexpr int FIRST_STABLE_DRAWS = 12, FINAL_STABLE_DRAWS = 72, SWAP_DRAWS = 150;
                const RestingPose* other = drawsSinceChange < SWAP_DRAWS ? FindOtherFinalPose(weaponClass) : nullptr;
                bool isNearRest = true;
                if (other)
                {
                    const float dx = weaponInArms[12] - other->weaponInArms[12],
                                dy = weaponInArms[13] - other->weaponInArms[13],
                                dz = weaponInArms[14] - other->weaponInArms[14];
                    isNearRest =
                        dx * dx + dy * dy + dz * dz < RESTING_SAMPLE_MAXIMUM_METERS * RESTING_SAMPLE_MAXIMUM_METERS;
                }
                // Thrown items (grenades, mines, time bombs, health / ammo) never get a resting pose of their own:
                // after a throw the game's animation parks the weapon low beside the arm and holds it still, and that
                // was taken as their pose, so the arm was drawn in the game's after-throw pose whenever one was held
                // (PLAN 58 k, headset run 17; run390: both at -0.18 -0.41 0.39, the launcher's at -0.30 -0.23 0.35).
                // They borrow another weapon's, as before their first sample.
                const char* weaponType = Holster::ObjectTypeName(reinterpret_cast<uintptr_t>(CurrentWeapon()));
                const bool isThrown = weaponType && (!strcmp(weaponType, ".?AVWeaponGrenade@@") ||
                                                     !strcmp(weaponType, ".?AVWeaponDispenser@@"));
                if (isThrown && rest->hasSample)
                {
                    rest->hasSample = rest->isFinal = false; // one taken before b353 (or by a stray draw): dropped
                }
                if (!isThrown && isNearRest && ((!rest->hasSample && stableDraws >= FIRST_STABLE_DRAWS) ||
                                   (!rest->isFinal && stableDraws >= FINAL_STABLE_DRAWS)))
                {
                    rest->isFinal = stableDraws >= FINAL_STABLE_DRAWS;
                    rest->hasSample = true;
                    memcpy(rest->weaponInArms, weaponInArms, sizeof(weaponInArms));
                    if (rest->isFinal && allBones && boneCount <= 64)
                    {
                        rest->boneCount = boneCount;
                        memcpy(rest->bones, savedAllBones, boneCount * 64);
                        // the stock hands at rest in the weapon's space: the two-handed grip's foregrip
                        static const unsigned LEFT_WRIST_HASH = HashName("bone_l_wrist"),
                                              RIGHT_WRIST_HASH = HashName("bone_r_wrist");
                        const float* leftWrist = FindBoneMatrix(self, LEFT_WRIST_HASH, false);
                        const float* rightWrist = FindBoneMatrix(self, RIGHT_WRIST_HASH, false);
                        rest->hasWrists = false;
                        if (leftWrist && rightWrist && leftWrist >= allBones && rightWrist >= allBones &&
                            leftWrist < allBones + boneCount * 16 && rightWrist < allBones + boneCount * 16)
                        {
                            float weaponInverse[16];
                            InvertRigid(weaponInverse, weaponInArms);
                            Multiply(rest->wristsInWeapon[0], savedAllBones[(leftWrist - allBones) / 16], weaponInverse);
                            Multiply(rest->wristsInWeapon[1], savedAllBones[(rightWrist - allBones) / 16], weaponInverse);
                            rest->hasWrists = IsRigid(rest->wristsInWeapon[0], 2.0f) && IsRigid(rest->wristsInWeapon[1], 2.0f);
                            Log::Info(
                                "fp",
                                "stock hands at rest in the weapon: left %.3f %.3f %.3f, right %.3f %.3f %.3f%s",
                                static_cast<double>(rest->wristsInWeapon[0][12]),
                                static_cast<double>(rest->wristsInWeapon[0][13]),
                                static_cast<double>(rest->wristsInWeapon[0][14]),
                                static_cast<double>(rest->wristsInWeapon[1][12]),
                                static_cast<double>(rest->wristsInWeapon[1][13]),
                                static_cast<double>(rest->wristsInWeapon[1][14]),
                                rest->hasWrists ? "" : " (not rigid: unused)");
                        }
                    }
                    Log::Info(
                        "fp",
                        "resting pose for weapon class %p %s: weapon at %.3f %.3f %.3f in the arms model, %u bones",
                        weaponClass,
                        rest->isFinal ? "taken" : "first sample",
                        static_cast<double>(weaponInArms[12]),
                        static_cast<double>(weaponInArms[13]),
                        static_cast<double>(weaponInArms[14]),
                        rest->isFinal ? rest->boneCount : 0u);
                }
            }
            { // the current weapon's stock hands for the foregrip (StockForegripInPose)
                AcquireSRWLockExclusive(&_stockWristsLock);
                _hasStockWrists = rest && rest->hasWrists;
                if (_hasStockWrists)
                {
                    memcpy(_stockWristsInWeapon, rest->wristsInWeapon, sizeof(_stockWristsInWeapon));
                }
                ReleaseSRWLockExclusive(&_stockWristsLock);
            }
            ComputeHandWorld(handWorld);
            float restHand[16], kickedHand[16];
            memcpy(restHand, handWorld, sizeof(restHand));
            bool isKicked = false;
            { // Recoil kick (Feedback.cpp tells when a shot leaves): the model jumps back 2.5 cm and up 4 degrees and
                // settles within 140 ms. The model only: the aim is not moved.
                const DWORD sinceMilliseconds = GetTickCount() - _kickTick;
                if (_kickTick && sinceMilliseconds < KICK_DURATION_MILLISECONDS)
                {
                    const float kickFade =
                                    (1.0f - static_cast<float>(sinceMilliseconds) /
                                                static_cast<float>(KICK_DURATION_MILLISECONDS)),
                                kickFadeSquared = kickFade * kickFade;
                    // Zoomed, the kick only tilts the gun a little: moving it back brought the lens nearer the eye,
                    // and the scope picture's field grew and shrank with every shot (the bowcaster, headset run 15).
                    const bool isZoomed = Zoom::IsZoomed();
                    const float kickAngleRadians =
                                    KICK_ANGLE_RADIANS * kickFadeSquared * (isZoomed ? KICK_ZOOMED_ANGLE_SHARE : 1.0f),
                                cosine = cosf(kickAngleRadians), sine = sinf(kickAngleRadians);
                    const float kick[16] = {
                        1,
                        0,
                        0,
                        0,
                        0,
                        cosine,
                        sine,
                        0,
                        0,
                        -sine,
                        cosine,
                        0,
                        0,
                        isZoomed ? 0.0f : KICK_UP_METERS * kickFadeSquared,
                        isZoomed ? 0.0f : KICK_BACK_METERS * kickFadeSquared,
                        1};
                    float kicked[16];
                    Multiply(kicked, kick, handWorld);
                    memcpy(handWorld, kicked, sizeof(kicked));
                    memcpy(kickedHand, kicked, sizeof(kickedHand));
                    isKicked = true;
                }
            }
            // A weapon with no resting pose yet borrows the last one taken for another weapon.
            const RestingPose* placing = rest->hasSample ? rest : FindOtherFinalPose(weaponClass);
            if (isSane && placing && IsRigid(handWorld, 0))
            {
                float newArms[16];
                // The weapon points where the controller points (its barrel on the aim pose); with our arm (BodyArm)
                // it is then moved so the point our hand closes round sits on the controller's grip pose, the middle
                // of its handle in the real palm (the grip in the aim pose's frame, carried with the hand world so
                // handling and the kick still apply), turned by the arm's tilt about that point (idren2023 2026-10-23:
                // the barrel with the controller, the hand on the gun); without it the weapon's origin stays on the
                // aim pose as before.
                Multiply(target, turn, handWorld);
                AlignBarrel(target);
                RollDroidArm(target, armClass, false);
                float gripPoint[3], gripPose[16], aimPose[16];
                _isWeaponOnGrip =
                    BodyArm::GetGripPointInModel(reinterpret_cast<uintptr_t>(armClass), gripPoint) &&
                    Xr::GetGripPose(0, gripPose) && Xr::GetHandPose(0, aimPose);
                if (_isWeaponOnGrip)
                {
                    float inverseAim[16], gripInAim[16], gripWorld[16];
                    InvertRigid(inverseAim, aimPose);
                    Multiply(gripInAim, gripPose, inverseAim);
                    Multiply(gripWorld, gripInAim, handWorld);
                    // the tilt: about the model's x through the grip point (rows: translate, rotate, translate back)
                    const float tiltRadians = BodyArm::GripTiltDegrees() * 3.14159265f / 180.0f;
                    if (tiltRadians != 0.0f)
                    {
                        const float cosine = cosf(tiltRadians), sine = sinf(tiltRadians);
                        const float y = gripPoint[1], z = gripPoint[2];
                        const float tilt[16] = {
                            1, 0, 0, 0, 0, cosine, sine, 0, 0, -sine, cosine, 0,
                            0, y - (y * cosine - z * sine), z - (y * sine + z * cosine), 1};
                        float tilted[16];
                        Multiply(tilted, tilt, target);
                        memcpy(target, tilted, sizeof(tilted));
                    }
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        const float now = gripPoint[0] * target[axis] + gripPoint[1] * target[4 + axis] +
                                          gripPoint[2] * target[8 + axis] + target[12 + axis];
                        target[12 + axis] += gripWorld[12 + axis] - now;
                    }
                    // A held item (grenade, mine, time bomb, canister, recon droid; headset run 19): the hand stays
                    // on the controller and the item is fitted into it, not the hand moved onto the item.
                    float itemWorld[16];
                    if (BodyArm::GetItemWorld(reinterpret_cast<uintptr_t>(armClass), gripWorld, itemWorld))
                    {
                        memcpy(target, itemWorld, sizeof(itemWorld));
                    }
                }
                InvertRigid(inverse, placing->weaponInArms);
                Multiply(newArms, inverse, target);
                if (IsRigid(newArms, 0) && IsRigid(target, 0))
                {
                    memcpy(arms, newArms, sizeof(newArms));
                    memcpy(weapon, target, sizeof(target)); // the weapon exactly on the hand, no animation
                    memcpy(_weaponWorld, target, sizeof(target));
                    memcpy(_steadyWeaponWorld, target, sizeof(target));
                    if (isKicked)
                    {
                        // weapon = A x kicked hand, so without the kick: weapon x kicked^-1 x rest hand
                        float inverseKicked[16], unkick[16];
                        InvertRigid(inverseKicked, kickedHand);
                        Multiply(unkick, inverseKicked, restHand);
                        Multiply(_steadyWeaponWorld, target, unkick);
                    }
                    memcpy(_weaponWorldBase, _baseFrame, sizeof(_weaponWorldBase));
                    _weaponWorldTick = GetTickCount();
                    if (_isMirrorWanted)
                    {
                        // The same model mirrored in the weapon's frame and put on the left controller: weapon =
                        // M * turn * left, arms = K0^-1 * weapon (as for the right hand, with M in between).
                        float leftWorld[16], leftTarget[16];
                        if (_isTracked[1])
                        {
                            Multiply(leftWorld, _trackedPoses[1], _baseFrame);
                        }
                        else
                        {
                            ComputePoseWorld(
                                leftWorld,
                                _testLeftHandXMeters,
                                _testLeftHandYMeters,
                                _testLeftHandZMeters,
                                _testLeftHandYawRadians,
                                _testLeftHandPitchRadians);
                        }
                        Multiply(leftTarget, turn, leftWorld);
                        Multiply(mirroredWeapon, MIRROR_X, leftTarget);
                        InvertRigid(inverse, placing->weaponInArms);
                        Multiply(mirroredArms, inverse, mirroredWeapon);
                        hasMirror = IsRigid(leftTarget, 0);
                    }
                    if (placing->isFinal && allBones && placing->boneCount == boneCount)
                    {
                        memcpy(allBones, placing->bones, boneCount * 64);
                        isFrozen = true;
                    }
                    // (the blaster's hp_fire in its model's root frame; the tri-shot and the rocket have no model:
                    // the same point, as our grip puts it there for every weapon)
                    float ourHpFire[3] = {-0.0597347f, 0.0242992f, -0.0178361f};
                    if (Holster::IsSuperBattleDroid() && BodyArm::IsWanted(reinterpret_cast<uintptr_t>(armClass)) &&
                        (GetMuzzleInWeaponFrame(ourHpFire) || true))
                    {
                        // Our arm (PLAN 60 step 4e): its grip puts the weapon model's hp_fire 3.5 cm ahead of our
                        // barrels' muzzles, along them (the model's +z): the opening is that far back along +z, the
                        // holoscope's emitter halfway along the barrels and a little above them. The weapon is not
                        // moved (our arm is drawn from it).
                        constexpr float EMITTER_BACK_METERS = 0.135f, EMITTER_UP_METERS = 0.017f; // 96 / 12 mm of ours
                        for (int axis = 0; axis < 3; ++axis)
                        {
                            _barrelAxisInWeapon[axis] = axis == 2 ? 1.0f : 0.0f;
                            _barrelMuzzleInWeapon[axis] =
                                ourHpFire[axis] - (axis == 2 ? FLASH_AHEAD_OF_OPENING_METERS : 0.0f);
                            _barrelEmitterInWeapon[axis] = ourHpFire[axis] +
                                                           (axis == 2   ? -EMITTER_BACK_METERS
                                                            : axis == 1 ? EMITTER_UP_METERS
                                                                        : 0.0f);
                        }
                        if (!_barrelTick)
                        {
                            Log::Info(
                                "aim",
                                "super battle droid: our arm's barrels, muzzle %.3f %.3f %.3f in the weapon's frame",
                                static_cast<double>(_barrelMuzzleInWeapon[0]),
                                static_cast<double>(_barrelMuzzleInWeapon[1]),
                                static_cast<double>(_barrelMuzzleInWeapon[2]));
                        }
                        _barrelTick = GetTickCount() | 1;
                        // Our left arm (no guns) placed as the right one, mirrored: the weapon model on the left
                        // controller (turn x left pose, its barrel along that aim), moved so its grip point, mirrored,
                        // sits on the left controller's grip pose; then mirrored in the weapon's frame (BodyArm draws
                        // its *_l parts there).
                        _hasOurLeftWeapon = false;
                        if (BodyArm::IsLeftWanted())
                        {
                            float leftWorld[16], leftTarget[16], gripPoint[3], leftGrip[16], leftAim[16];
                            if (_isTracked[1])
                            {
                                Multiply(leftWorld, _trackedPoses[1], _baseFrame);
                            }
                            else
                            {
                                ComputePoseWorld(
                                    leftWorld,
                                    _testLeftHandXMeters,
                                    _testLeftHandYMeters,
                                    _testLeftHandZMeters,
                                    _testLeftHandYawRadians,
                                    _testLeftHandPitchRadians);
                            }
                            Multiply(leftTarget, turn, leftWorld);
                            AlignBarrel(leftTarget);
                            RollDroidArm(leftTarget, armClass, true);
                            if (BodyArm::GetGripPointInModel(reinterpret_cast<uintptr_t>(armClass), gripPoint) &&
                                Xr::GetGripPose(1, leftGrip) && Xr::GetHandPose(1, leftAim))
                            {
                                float inverseAim[16], gripInAim[16], gripWorld[16];
                                InvertRigid(inverseAim, leftAim);
                                Multiply(gripInAim, leftGrip, inverseAim);
                                Multiply(gripWorld, gripInAim, leftWorld);
                                for (int axis = 0; axis < 3; ++axis)
                                {
                                    const float now = -gripPoint[0] * leftTarget[axis] + gripPoint[1] * leftTarget[4 + axis] +
                                                      gripPoint[2] * leftTarget[8 + axis] + leftTarget[12 + axis];
                                    leftTarget[12 + axis] += gripWorld[12 + axis] - now;
                                }
                            }
                            Multiply(_ourLeftWeapon, MIRROR_X, leftTarget);
                            _hasOurLeftWeapon = IsRigid(leftTarget, 0);
                        }
                    }
                    else if (Holster::IsSuperBattleDroid())
                    {
                        // The droid's barrels in the weapon's frame, from the forearm bone as drawn (bone -> arms
                        // model -> world -> weapon).
                        static const unsigned FOREARM_HASH = HashName("bone_r_forearm");
                        const float* forearmBone = FindBoneMatrix(self, FOREARM_HASH, false);
                        if (forearmBone && IsRigid(forearmBone, 2.0f))
                        {
                            float forearmWorld[16], inverseTarget[16], forearmInWeapon[16];
                            Multiply(forearmWorld, forearmBone, newArms);
                            InvertRigid(inverseTarget, target);
                            Multiply(forearmInWeapon, forearmWorld, inverseTarget);
                            for (int axis = 0; axis < 3; ++axis)
                            {
                                const auto point = [&](const float* p)
                                {
                                    return p[0] * forearmInWeapon[axis] + p[1] * forearmInWeapon[4 + axis] +
                                           p[2] * forearmInWeapon[8 + axis] + forearmInWeapon[12 + axis];
                                };
                                _barrelMuzzleInWeapon[axis] = point(BARREL_MUZZLE_IN_FOREARM);
                                _barrelEmitterInWeapon[axis] = point(BARREL_MIDDLE_IN_FOREARM);
                                _barrelAxisInWeapon[axis] = BARREL_AXIS_IN_FOREARM[0] * forearmInWeapon[axis] +
                                                            BARREL_AXIS_IN_FOREARM[1] * forearmInWeapon[4 + axis] +
                                                            BARREL_AXIS_IN_FOREARM[2] * forearmInWeapon[8 + axis];
                            }
                            if (!_barrelTick)
                            {
                                Log::Info(
                                    "aim",
                                    "super battle droid barrels in the weapon's frame: muzzle %.3f %.3f %.3f, axis "
                                    "%.3f %.3f %.3f",
                                    static_cast<double>(_barrelMuzzleInWeapon[0]),
                                    static_cast<double>(_barrelMuzzleInWeapon[1]),
                                    static_cast<double>(_barrelMuzzleInWeapon[2]),
                                    static_cast<double>(_barrelAxisInWeapon[0]),
                                    static_cast<double>(_barrelAxisInWeapon[1]),
                                    static_cast<double>(_barrelAxisInWeapon[2]));
                            }
                            _barrelTick = GetTickCount() | 1;
                            // The game draws the muzzle flash with the weapon model at its hp_fire, which hangs at the
                            // wrist, 3-4 cm off the barrels' opening and 13 degrees off their line: the weapon model
                            // alone (the arms are already placed) is moved and turned so that hp_fire lies on the
                            // opening, along the barrels (b160, idren2023). It carries nothing else that is seen.
                            float hpFire[3];
                            if (GetMuzzleInWeaponFrame(hpFire))
                            {
                                PlaceOnBarrels(weapon, target, hpFire);
                            }
                        }
                    }
                }
            }
            else
            {
                static unsigned loggedCount = 0;
                if (loggedCount++ < 5)
                {
                    Log::Info(
                        "fp",
                        "hand placement skipped this draw (game matrices sane: %d, resting pose: %d)",
                        isSane ? 1 : 0,
                        placing ? 1 : 0);
                }
            }
            // Weapon swap (PLAN item 5): every 8th draw of the first 160 after a change, how the game's weapon-in-arms
            // compares with the resting pose and whether the bones were frozen.
            if (drawsSinceChange < 160 && (drawsSinceChange++ & 7) == 0)
            {
                Log::Info(
                    "fp",
                    "swap draw %d class %p: still %d, K %.3f %.3f %.3f, K0 %s %.3f %.3f %.3f, bones %s",
                    drawsSinceChange - 1,
                    weaponClass,
                    stableDraws,
                    static_cast<double>(weaponInArms[12]),
                    static_cast<double>(weaponInArms[13]),
                    static_cast<double>(weaponInArms[14]),
                    rest->isFinal     ? "final"
                    : rest->hasSample ? "sample"
                    : placing         ? "borrowed"
                                      : "none",
                    static_cast<double>(placing ? placing->weaponInArms[12] : 0),
                    static_cast<double>(placing ? placing->weaponInArms[13] : 0),
                    static_cast<double>(placing ? placing->weaponInArms[14] : 0),
                    isFrozen ? "frozen" : "animated");
            }
            if (_firstPersonLogsLeft > 0)
            {
                InterlockedDecrement(&_firstPersonLogsLeft);
                LogMatrix("base", _baseFrame);
                LogMatrix("game weapon", savedWeapon);
                LogMatrix("hand weapon", weapon);
            }
        }
        else if (isOffsetPlaced)
        {
            const float* cameraMatrix = reinterpret_cast<const float*>(camera + OFFSET_cameraMatrix); // camera to world
            const float cosineYaw = cosf(_firstPersonYawRadians), sineYaw = sinf(_firstPersonYawRadians),
                        cosinePitch = cosf(_firstPersonPitchRadians), sinePitch = sinf(_firstPersonPitchRadians);
            const float pitchRotation[16] =
                {1, 0, 0, 0, 0, cosinePitch, sinePitch, 0, 0, -sinePitch, cosinePitch, 0, 0, 0, 0, 1};
            const float yawRotation[16] = {
                cosineYaw,
                0,
                -sineYaw,
                0,
                0,
                1,
                0,
                0,
                sineYaw,
                0,
                cosineYaw,
                0,
                _firstPersonXMeters,
                _firstPersonYMeters,
                _firstPersonZMeters,
                1};
            float offset[16], world[16];
            Multiply(offset, pitchRotation, yawRotation);
            InvertRigid(inverse, cameraMatrix);
            Multiply(world, inverse, offset);
            Multiply(world, world, cameraMatrix);
            if (_firstPersonLogsLeft > 0)
            {
                InterlockedDecrement(&_firstPersonLogsLeft);
                LogMatrix("camera", cameraMatrix);
                LogMatrix("arms", arms);
                LogMatrix("weapon", weapon);
            }
            Multiply(arms, savedArms, world);
            Multiply(weapon, savedWeapon, world);
        }
        if (leftArmBones[0])
        {
            // The wrist goes to the pose's position and the forearm trails behind it along the pose's direction.
            float leftPose[16], wristInForearm[16], forearm[16];
            if (_isTracked[1])
            {
                Multiply(leftPose, _trackedPoses[1], _baseFrame);
            }
            else
            {
                ComputePoseWorld(
                    leftPose,
                    _testLeftHandXMeters,
                    _testLeftHandYMeters,
                    _testLeftHandZMeters,
                    _testLeftHandYawRadians,
                    _testLeftHandPitchRadians);
            }
            InvertRigid(inverse, savedLeftArmBones[0]);
            Multiply(
                wristInForearm,
                savedLeftArmBones[1],
                inverse); // the wrist bone relative to the forearm bone, as animated
            const float forearmLengthMeters = sqrtf(
                wristInForearm[12] * wristInForearm[12] + wristInForearm[13] * wristInForearm[13] +
                wristInForearm[14] * wristInForearm[14]);
            leftPose[12] += leftPose[8] * forearmLengthMeters;
            leftPose[13] += leftPose[9] * forearmLengthMeters;
            leftPose[14] += leftPose[10] * forearmLengthMeters; // back from the wrist to the elbow
            Multiply(forearm, turn, leftPose);                  // world space
            InvertRigid(inverse, arms);
            Multiply(leftArmBones[0], forearm, inverse); // into the arms model's space (the matrix it is drawn with)
            // The hand is rigid with the forearm, at the forearm's length along it: the pose alone decides where
            // it is and how it is turned, so no animation (recoil, reload) moves the left hand any more.
            const float along[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, forearmLengthMeters, 1};
            Multiply(leftArmBones[1], along, leftArmBones[0]);
            if (!IsRigid(leftArmBones[0], 0) || !IsRigid(leftArmBones[1], 0))
            {
                memcpy(leftArmBones[0], savedLeftArmBones[0], 64);
                memcpy(leftArmBones[1], savedLeftArmBones[1], 64);
            } // never draw with a broken bone
            if (_leftHandLogsLeft > 0)
            {
                InterlockedDecrement(&_leftHandLogsLeft);
                Log::Info("fp", "left forearm length %.3f", static_cast<double>(forearmLengthMeters));
                LogMatrix("game forearm", savedLeftArmBones[0]);
                LogMatrix("game wrist", savedLeftArmBones[1]);
                LogMatrix("our forearm", leftArmBones[0]);
                LogMatrix("arms model", arms);
            }
        }
    }
    if (isHandPlaced && allBones)
    {
        CopyArms(self, arms, allBones, &savedAllBones[0][0], boneCount); // for the arms on the tank levers
    }
    // Our own right arm (BodyArm, PLAN 51 B) in place of the stock one for the weapons we fitted it to: the stock
    // right forearm, glove and cuff stud are collapsed to a point in the game's draw (each bone's matrix scaled to
    // zero about its origin; the left arm is drawn as before), and ours is drawn right after, holding the weapon where
    // the game drew it. The bones are put back after the draw, as for a frozen pose.
    // (also round a secondary Holster draws in the hand: before b467 the empty hand was drawn beside it)
    const bool isOurArm = isHandPlaced && allBones &&
                          (!Holster::IsHandEmpty() || heldSecondary || Holster::IsSuperBattleDroid()) &&
                          BodyArm::IsWanted(reinterpret_cast<uintptr_t>(armClass));
    // the empty hand ours too (idren2023 2026-10-23: the controller poses in the game), on the controller's grip pose
    float emptyGripWorld[16];
    bool isOurEmptyHand = false;
    if (isHandPlaced && allBones && Holster::IsHandEmpty() && !isOurArm && BodyArm::IsEmptyHandWanted())
    {
        float gripPose[16], aimPose[16];
        if (Xr::GetGripPose(0, gripPose) && Xr::GetHandPose(0, aimPose))
        {
            FpGuard guard;
            float handWorld[16], inverseAim[16], gripInAim[16];
            ComputeHandWorld(handWorld);
            InvertRigid(inverseAim, aimPose);
            Multiply(gripInAim, gripPose, inverseAim);
            Multiply(emptyGripWorld, gripInAim, handWorld);
            isOurEmptyHand = true;
        }
    }
    // Our left arm (PLAN 51 B, idren2023 2026-10-24): the right one mirrored on the left controller's grip pose, on foot,
    // the stock left forearm and glove collapsed as the right ones.
    float leftGripWorld[16];
    bool isOurLeftArm = false;
    if (isLeftHandPlaced && allBones && BodyArm::IsLeftWanted() && Vehicle::CurrentSeat() == Vehicle::Seat::OnFoot)
    {
        float gripPose[16], base[16];
        if (Xr::GetGripPose(1, gripPose) && GetBaseFrame(base))
        {
            FpGuard guard;
            float aimPose[16];
            if ((_isTwoHanded || _isOnMagazine) && _isTracked[1] && Xr::GetHandPose(1, aimPose))
            {
                // Held on the gun (foregrip or magazine): the hand goes where _trackedPoses[1] was locked, not where
                // the controller is (idren2023 2026-11-05: our left hand slid along the gun and off it; only the stock
                // arm, which reads _trackedPoses[1], was locked). The grip pose keeps its offset from the aim pose.
                float inverse[16], gripInAim[16], lockedGrip[16];
                InvertRigid(inverse, aimPose);
                Multiply(gripInAim, gripPose, inverse);
                Multiply(lockedGrip, gripInAim, _trackedPoses[1]);
                Multiply(leftGripWorld, lockedGrip, base);
            }
            else
            {
                Multiply(leftGripWorld, gripPose, base);
            }
            isOurLeftArm = true;
        }
    }
    if (isOurLeftArm)
    {
        static const unsigned STOCK_LEFT_ARM_HASHES[] = {
            HashName("bone_l_forearm"),
            HashName("bone_l_wrist"),
            HashName("imp_inf_trooper_l_forearm"),
            HashName("imp_inf_trooper_l_hand")};
        for (const unsigned hash : STOCK_LEFT_ARM_HASHES)
        {
            float* bone = FindBoneMatrix(self, hash, false);
            if (bone)
            {
                memset(bone, 0, 12 * sizeof(float)); // rows x, y, z zero; the origin (row 3) stays
                bone[15] = 1;
            }
        }
    }
    if (isOurArm || isOurEmptyHand)
    {
        static const unsigned STOCK_RIGHT_ARM_HASHES[] = {
            HashName("bone_r_forearm"),
            HashName("bone_r_wrist"),
            HashName("imp_inf_trooper_r_forearm"),
            HashName("imp_inf_trooper_r_hand")};
        for (const unsigned hash : STOCK_RIGHT_ARM_HASHES)
        {
            float* bone = FindBoneMatrix(self, hash, false);
            if (bone)
            {
                memset(bone, 0, 12 * sizeof(float)); // rows x, y, z zero; the origin (row 3) stays
                bone[15] = 1;
            }
        }
    }
    const double gameDrawStartMilliseconds = NowMilliseconds();
    _isInFirstPersonRender = true;
    DrawGameFirstPerson(self, edx, flags);
    Holster::Draw(self, flags);
    if (isOurEmptyHand)
    {
        Xr::ControllerInput pad{};
        Xr::GetControllerInput(&pad);
        BodyArm::DrawEmptyHand(D3dSurvey::PresentingDevice(), emptyGripWorld, pad.isValid ? pad.trigger[0] : 0.0f);
    }
    if (isOurLeftArm)
    {
        Xr::ControllerInput pad{};
        Xr::GetControllerInput(&pad);
        if (!(isOurArm && _hasOurLeftWeapon &&
              BodyArm::DrawLeftForearmWeapon(
                  D3dSurvey::PresentingDevice(),
                  _ourLeftWeapon,
                  reinterpret_cast<uintptr_t>(armClass),
                  pad.isValid ? pad.trigger[1] : 0.0f)))
        {
            BodyArm::DrawLeft(
                D3dSurvey::PresentingDevice(),
                leftGripWorld,
                pad.isValid ? pad.trigger[1] : 0.0f,
                Holster::IsHoldingCanister());
        }
    }
    if (isOurArm)
    {
        Xr::ControllerInput pad{};
        Xr::GetControllerInput(&pad);
        BodyArm::Draw(
            D3dSurvey::PresentingDevice(),
            weapon,
            reinterpret_cast<uintptr_t>(armClass),
            pad.isValid ? pad.trigger[0] : 0.0f);
    }
    if (hasMirror)
    {
        memcpy(arms, mirroredArms, sizeof(mirroredArms));
        memcpy(weapon, mirroredWeapon, sizeof(mirroredWeapon));
        // The weapon's draw (Weapon::Render 0x243000, on the Weapon at renderable+0x1600) also draws the muzzle flash
        // while weapon+0xC8 (the flash's end time) lies ahead, and copies its matrix to weapon+0x20: for the copy the
        // flash time is cleared and both are put back afterwards (offsets "First-person weapon draw").
        const uintptr_t gameWeapon =
            *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(self) + OFFSET_firstPersonWeapon);
        const bool canGuard =
            gameWeapon && !IsBadWritePtr(reinterpret_cast<void*>(gameWeapon), OFFSET_weaponFlashEnd + 4);
        float savedFlashEnd = 0, savedWeaponWorld[16];
        if (canGuard)
        {
            savedFlashEnd = *reinterpret_cast<float*>(gameWeapon + OFFSET_weaponFlashEnd);
            memcpy(savedWeaponWorld, reinterpret_cast<void*>(gameWeapon + OFFSET_weaponWorldMatrix), 64);
            *reinterpret_cast<float*>(gameWeapon + OFFSET_weaponFlashEnd) = 0.0f;
        }
        DrawGameFirstPerson(self, edx, flags); // the droid's left arm
        if (canGuard)
        {
            *reinterpret_cast<float*>(gameWeapon + OFFSET_weaponFlashEnd) = savedFlashEnd;
            memcpy(reinterpret_cast<void*>(gameWeapon + OFFSET_weaponWorldMatrix), savedWeaponWorld, 64);
        }
        static bool hasSaid = false;
        if (!hasSaid)
        {
            hasSaid = true;
            Log::Info("fp", "super battle droid: left arm drawn mirrored on the left controller");
        }
    }
    _isInFirstPersonRender = false;
    if (isTesting)
    {
        const double drawMilliseconds = NowMilliseconds() - gameDrawStartMilliseconds;
        _firstPersonTestGameMilliseconds += drawMilliseconds;
        _firstPersonTestOurMilliseconds += gameDrawStartMilliseconds - startMilliseconds;
        if (drawMilliseconds > _firstPersonTestGameMaximumMilliseconds)
        {
            _firstPersonTestGameMaximumMilliseconds = drawMilliseconds;
        }
        ++_firstPersonTestDraws;
        if (!_firstPersonTestSince)
        {
            _firstPersonTestSince = GetTickCount();
        }
        if (GetTickCount() - _firstPersonTestSince >= FIRST_PERSON_TEST_MODE_MILLISECONDS)
        {
            LogFirstPersonTest(arms, weapon, leftArmBones[0]);
            _firstPersonTestMode = (_firstPersonTestMode + 1) & 3;
            _firstPersonTestSince = GetTickCount();
            _firstPersonTestGameMilliseconds = _firstPersonTestOurMilliseconds =
                _firstPersonTestGameMaximumMilliseconds = 0;
            _firstPersonTestDraws = 0;
        }
    }
    { // Where the first-person forearms begin (their bones' origins, the elbow ends), for the full body's upper arms
        // (Body.cpp, headset run 15: the body's upper arms and these forearms did not meet).
        static const unsigned RIGHT_FOREARM_HASH = HashName("bone_r_forearm");
        const float* forearms[2] = {FindBoneMatrix(self, RIGHT_FOREARM_HASH, false), leftArmBones[0]};
        for (int hand = 0; hand < 2; ++hand)
        {
            if (forearms[hand] && IsRigid(forearms[hand], 2.0f))
            {
                const float* bone = forearms[hand];
                for (int axis = 0; axis < 3; ++axis)
                {
                    _elbowWorld[hand][axis] =
                        bone[12] * arms[axis] + bone[13] * arms[4 + axis] + bone[14] * arms[8 + axis] + arms[12 + axis];
                }
                _elbowTick[hand] = GetTickCount();
            }
        }
    }
    memcpy(arms, savedArms, sizeof(savedArms));
    memcpy(weapon, savedWeapon, sizeof(savedWeapon));
    if (leftArmBones[0])
    {
        memcpy(leftArmBones[0], savedLeftArmBones[0], 64);
        memcpy(leftArmBones[1], savedLeftArmBones[1], 64);
    }
    if (isFrozen || isOurArm || isOurEmptyHand || isOurLeftArm)
    {
        memcpy(allBones, savedAllBones, boneCount * 64);
    }
}

// Installs the hook on FirstPersonRenderable::Render once (after checking the game's first bytes there).
bool InstallFirstPersonHook()
{
    if (_isFirstPersonHookInstalled)
    {
        return true;
    }
    if (!GameBuild::IsAvailable(GameBuild::Feature::FirstPersonModel))
    {
        static bool hasLogged = false;
        if (!hasLogged)
        {
            hasLogged = true;
            Log::Warning(
                "fp",
                "not installed: %s is off for this game build",
                GameBuild::FeatureName(GameBuild::Feature::FirstPersonModel));
        }
        return false;
    }
    void* target = reinterpret_cast<void*>(GameBuild::Absolute(GameBuild::Address::FirstPersonRender));
    MH_STATUS status = MH_Initialize();
    if (status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED)
    {
        status = MH_CreateHook(
            target,
            reinterpret_cast<void*>(&FirstPersonRenderHook),
            reinterpret_cast<void**>(&_originalFirstPersonRender));
    }
    if (status == MH_OK)
    {
        status = MH_EnableHook(target);
    }
    Log::Info("fp", "FirstPersonRenderable::Render hook: %s", MH_StatusToString(status));
    _isFirstPersonHookInstalled = status == MH_OK;
    return _isFirstPersonHookInstalled;
}

// Installs the hook on Aimer::Set once (after checking the game's first bytes there).
bool InstallAimerHook()
{
    if (_isAimerHookInstalled)
    {
        return true;
    }
    if (!GameBuild::IsAvailable(GameBuild::Feature::Aim))
    {
        static bool hasLogged = false;
        if (!hasLogged)
        {
            hasLogged = true;
            Log::Warning(
                "aim",
                "not installed: %s is off for this game build",
                GameBuild::FeatureName(GameBuild::Feature::Aim));
        }
        return false;
    }
    void* target = reinterpret_cast<void*>(GameBuild::Absolute(GameBuild::Address::AimerSet));
    MH_STATUS status = MH_Initialize();
    if (status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED)
    {
        status =
            MH_CreateHook(target, reinterpret_cast<void*>(&AimerSetHook), reinterpret_cast<void**>(&_originalAimerSet));
    }
    if (status == MH_OK)
    {
        status = MH_EnableHook(target);
    }
    Log::Info("aim", "Aimer::Set hook: %s", MH_StatusToString(status));
    _isAimerHookInstalled = status == MH_OK;
    if (_isAimerHookInstalled)
    {
        RedirectTargetSearchDirection();
    }
    return _isAimerHookInstalled;
}

} // namespace

// ---- Public functions (Aim.h) ----

// Any thread. Degrees; yaw left positive, pitch up positive. isOn = false gives the aim back to the game.
void SetOffset(bool isOn, float yawDegrees, float pitchDegrees)
{
    if (isOn && !InstallAimerHook())
    {
        return;
    }
    _offsetYawRadians = yawDegrees * DEGREES_TO_RADIANS;
    _offsetPitchRadians = pitchDegrees * DEGREES_TO_RADIANS;
    _aimLogsLeft = isOn ? 3 : 0;
    _isOffsetOn = isOn;
    Log::Info(
        "aim",
        "%s, yaw %.1f pitch %.1f degrees",
        isOn ? "offset" : "off",
        static_cast<double>(yawDegrees),
        static_cast<double>(pitchDegrees));
}

// Just before the two eye passes are drawn: take the controller poses and the base frame of this very frame.
// They were located together with the head pose the eyes are drawn from, so hands and view cannot drift apart
// when the player walks or turns (before b59 the hands used the previous frame's poses and base: idren2023 saw them
// jitter, most with smooth turning).
void BeforeRender()
{
    if (!Xr::IsRunning())
    {
        return;
    }
    FpGuard guard;
    for (int hand = 0; hand < 2; ++hand)
    {
        float pose[16];
        if (_isTracked[hand] && Xr::GetHandPose(hand, pose))
        {
            memcpy(_trackedPoses[hand], pose, sizeof(pose));
        }
    }
    if (_isTracked[0])
    {
        ApplyTwoHandedGrip(_isTracked[1]);
        ApplyWeaponHandling();
        ApplyMagazineHold(_isTracked[1]);
    }
    float base[16];
    if ((_isTracked[0] || _isTracked[1]) && Stereo::GetBaseCamera(base))
    {
        memcpy(_baseFrame, base, sizeof(_baseFrame));
        _hasBaseFrame = true;
    }
}

// Main thread, once per presented frame.
void OnPresent()
{
    if (!Stereo::IsSceneShown())
    {
        // menus and loading (a level load frees the models): copy the arms anew. Kept over a respawn (headset run 18:
        // a soldier spawns empty-handed, b354, so often had no on-foot arms draw to copy before entering a ship, and
        // was drawn without hands); the copy's model is checked by its vtable at each use.
        _armsCopy.isValid = false;
    }
    _isMirrorWanted = Holster::IsSuperBattleDroid() && (_isTracked[1] || _isTestLeftHandOn) && _shouldMirrorDroid &&
                      !BodyArm::IsLeftWanted(); // (our own left arm in its place, PLAN 60 step 4e)
    // Controllers (real or simulated) take over from the test commands as soon as they are tracked.
    bool isTrackedNow[2] = {false, false};
    if (Xr::IsRunning())
    {
        for (int hand = 0; hand < 2; ++hand)
        {
            float pose[16];
            {
                FpGuard guard;
                isTrackedNow[hand] = Xr::GetHandPose(hand, pose);
            }
            if (isTrackedNow[hand])
            {
                memcpy(_trackedPoses[hand], pose, sizeof(pose));
            }
        }
        if ((isTrackedNow[0] || isTrackedNow[1]) && (!_isAimerHookInstalled || !_isFirstPersonHookInstalled))
        {
            InstallAimerHook();
            InstallFirstPersonHook();
        }
        if (isTrackedNow[0] && !_isAimerHookInstalled)
        {
            isTrackedNow[0] = false;
        }
        if (!_isFirstPersonHookInstalled)
        {
            isTrackedNow[0] = isTrackedNow[1] = false;
        }
    }
    if (isTrackedNow[0])
    {
        FpGuard guard;
        ApplyTwoHandedGrip(isTrackedNow[1]);
        ApplyWeaponHandling();
        ApplyMagazineHold(isTrackedNow[1]);
    }
    for (int hand = 0; hand < 2; ++hand)
    {
        if (isTrackedNow[hand] != _isTracked[hand])
        {
            Log::Info(
                "hand",
                "%s hand %s",
                hand == 0 ? "right" : "left",
                isTrackedNow[hand] ? "follows its controller" : "released");
        }
        _isTracked[hand] = isTrackedNow[hand];
    }
    const bool isHandAiming = _isTestHandOn || isTrackedNow[0], isLeftHandPlaced = _isTestLeftHandOn || isTrackedNow[1];
    { // Every 2 s with a tracked controller: is the first-person model drawn at all, and where are the hands?
        static DWORD lastLogTick = 0;
        const DWORD now = GetTickCount();
        if ((isTrackedNow[0] || isTrackedNow[1]) && now - lastLogTick >= HAND_LOG_INTERVAL_MILLISECONDS)
        {
            lastLogTick = now;
            FpGuard guard;
            const float* rightPose = _trackedPoses[0];
            const float* leftPose = _trackedPoses[1];
            Log::Info(
                "fp",
                "%ld first-person draws in 2 s; from the resting head (cm right/up/forward): right hand %.0f %.0f "
                "%.0f, left hand %.0f %.0f %.0f; base frame %s",
                InterlockedExchange(&_firstPersonDrawCount, 0),
                static_cast<double>(rightPose[12] * 100),
                static_cast<double>(rightPose[13] * 100),
                static_cast<double>(-rightPose[14] * 100),
                static_cast<double>(leftPose[12] * 100),
                static_cast<double>(leftPose[13] * 100),
                static_cast<double>(-leftPose[14] * 100),
                _hasBaseFrame ? "ok" : "missing");
        }
    }
    if (_isOffsetOn || isHandAiming || isLeftHandPlaced)
    {
        // The droideka has no Aimer at the soldier's place, and its arms are the game's (see _isDroideka).
        static uintptr_t checkedPlayer = 0;
        const uintptr_t player = EngineSurvey::Resolve("soldier");
        if (player != checkedPlayer)
        {
            checkedPlayer = player;
            const bool isDroideka = player && Holster::IsObjectType(player, "EntityDroideka");
            if (isDroideka != _isDroideka)
            {
                Log::Info("aim", "%s", isDroideka ? "droideka: the game aims and draws the arms" : "not a droideka");
            }
            _isDroideka = isDroideka;
        }
        _playerAimer = _isDroideka ? 0 : EngineSurvey::Resolve("aimer");
    }
    if (_isFirstPersonOffsetOn || isHandAiming || isLeftHandPlaced)
    {
        _redCamera = EngineSurvey::Resolve("red");
    }
    if (isHandAiming || isLeftHandPlaced)
    {
        float baseCamera[16];
        if (Stereo::GetBaseCamera(baseCamera))
        {
            memcpy(_baseFrame, baseCamera, sizeof(_baseFrame));
            _hasBaseFrame = true;
        }
        else if (const uintptr_t camera = _redCamera)
        {
            memcpy(_baseFrame, reinterpret_cast<const void*>(camera + OFFSET_cameraMatrix), sizeof(_baseFrame));
            _hasBaseFrame = true;
        }
        else
        {
            _hasBaseFrame = false;
        }
    }
}

// Any thread. One pose places the weapon and arms and gives the aim direction: centimetres right / up / forward of
// the base frame's origin (the eye; in VR the resting head), degrees yaw (left positive) and pitch (up positive).
// The game's own look is about 23 -30 58 0 0. This is the form a motion controller's pose will arrive in.
void SetHand(
    bool isOn,
    float xCentimeters,
    float yCentimeters,
    float forwardCentimeters,
    float yawDegrees,
    float pitchDegrees)
{
    if (isOn && (!InstallAimerHook() || !InstallFirstPersonHook()))
    {
        return;
    }
    _testHandXMeters = xCentimeters * METERS_PER_CENTIMETER;
    _testHandYMeters = yCentimeters * METERS_PER_CENTIMETER;
    _testHandZMeters = -forwardCentimeters * METERS_PER_CENTIMETER;
    _testHandYawRadians = yawDegrees * DEGREES_TO_RADIANS;
    _testHandPitchRadians = pitchDegrees * DEGREES_TO_RADIANS;
    _firstPersonLogsLeft = isOn ? 1 : 0;
    _aimLogsLeft = isOn ? 3 : 0;
    _isTestHandOn = isOn;
    Log::Info(
        "hand",
        "%s, %.0f %.0f %.0f cm, yaw %.1f pitch %.1f",
        isOn ? "on" : "off",
        static_cast<double>(xCentimeters),
        static_cast<double>(yCentimeters),
        static_cast<double>(forwardCentimeters),
        static_cast<double>(yawDegrees),
        static_cast<double>(pitchDegrees));
}

// Any thread. Moves and turns the first-person arms and weapon about the eye: centimetres right / up / back,
// degrees yaw (left positive) and pitch (up positive). isOn = false: as the game draws them.
void SetFirstPerson(
    bool isOn,
    float xCentimeters,
    float yCentimeters,
    float zCentimeters,
    float yawDegrees,
    float pitchDegrees)
{
    if (isOn && !InstallFirstPersonHook())
    {
        return;
    }
    _firstPersonXMeters = xCentimeters * METERS_PER_CENTIMETER;
    _firstPersonYMeters = yCentimeters * METERS_PER_CENTIMETER;
    _firstPersonZMeters = zCentimeters * METERS_PER_CENTIMETER;
    _firstPersonYawRadians = yawDegrees * DEGREES_TO_RADIANS;
    _firstPersonPitchRadians = pitchDegrees * DEGREES_TO_RADIANS;
    _firstPersonLogsLeft = isOn ? 2 : 0;
    _isFirstPersonOffsetOn = isOn;
    Log::Info(
        "fp",
        "%s, offset %.0f %.0f %.0f cm, yaw %.1f pitch %.1f",
        isOn ? "moved" : "off",
        static_cast<double>(xCentimeters),
        static_cast<double>(yCentimeters),
        static_cast<double>(zCentimeters),
        static_cast<double>(yawDegrees),
        static_cast<double>(pitchDegrees));
}

// Any thread. The left forearm and hand follow their own pose, same units and base frame as SetHand; the
// pose's position is the wrist.
void SetLeftHand(
    bool isOn,
    float xCentimeters,
    float yCentimeters,
    float forwardCentimeters,
    float yawDegrees,
    float pitchDegrees)
{
    if (isOn && !InstallFirstPersonHook())
    {
        return;
    }
    _testLeftHandXMeters = xCentimeters * METERS_PER_CENTIMETER;
    _testLeftHandYMeters = yCentimeters * METERS_PER_CENTIMETER;
    _testLeftHandZMeters = -forwardCentimeters * METERS_PER_CENTIMETER;
    _testLeftHandYawRadians = yawDegrees * DEGREES_TO_RADIANS;
    _testLeftHandPitchRadians = pitchDegrees * DEGREES_TO_RADIANS;
    _leftHandLogsLeft = isOn ? 2 : 0;
    _isTestLeftHandOn = isOn;
    Log::Info(
        "lhand",
        "%s, %.0f %.0f %.0f cm, yaw %.1f pitch %.1f",
        isOn ? "on" : "off",
        static_cast<double>(xCentimeters),
        static_cast<double>(yCentimeters),
        static_cast<double>(forwardCentimeters),
        static_cast<double>(yawDegrees),
        static_cast<double>(pitchDegrees));
}

// Test: logs which way the base frame faces (degrees, growing to the right), to measure turns.
void LogHeading()
{
    if (!_hasBaseFrame)
    {
        Log::Info("heading", "no base frame");
        return;
    }
    FpGuard guard;
    const double heading =
        atan2(-static_cast<double>(_baseFrame[8]), static_cast<double>(_baseFrame[10])) * RADIANS_TO_DEGREES;
    float characterRadians = 0;
    if (Stereo::GetCharacterYaw(&characterRadians))
    {
        Log::Info(
            "heading",
            "%.2f, character %.1f to the right of the view",
            heading,
            static_cast<double>(characterRadians) * RADIANS_TO_DEGREES);
        return;
    }
    Log::Info("heading", "%.2f (view turns with the character)", heading);
}

// Forget the resting poses (wrong one taken during an animation): they are taken again within a second or so.
void ForgetRestingPoses()
{
    InterlockedExchange(&_shouldForgetRestingPoses, 1);
}

bool GetBarrelMiddleWorld(float* position)
{
    // The weapon in the current base frame (GetWeaponWorld): placed at the last first-person draw, often the frame
    // before, its raw place left the aiming line moving round the muzzle while walking (PLAN 58 l, headset run 17).
    float weaponWorld[16];
    const bool isFresh = IsBarrelFresh();
    const bool hasWeapon = isFresh && GetWeaponWorldWithin(weaponWorld, BARREL_LIFETIME_MILLISECONDS);
    { // (diagnosis, b489: the droid's ammunition sign jumped between the barrels and the hand's place)
        static DWORD windowStart = 0;
        static int counts[3] = {};
        ++counts[!isFresh ? 0 : !hasWeapon ? 1 : 2];
        const DWORD now = GetTickCount();
        if (now - windowStart > 2000)
        {
            if (Holster::IsSuperBattleDroid() && (counts[0] || counts[1])) // (only when some were refused)
            {
                Log::Info("aim", "barrel middle: %d stale barrels (age %lu ms), %d no weapon (age %lu ms), %d ok",
                          counts[0], _barrelTick ? now - _barrelTick : 0ul, counts[1],
                          _weaponWorldTick ? now - _weaponWorldTick : 0ul, counts[2]);
            }
            windowStart = now;
            counts[0] = counts[1] = counts[2] = 0;
        }
    }
    if (!hasWeapon)
    {
        return false;
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        position[axis] = _barrelEmitterInWeapon[0] * weaponWorld[axis] +
                         _barrelEmitterInWeapon[1] * weaponWorld[4 + axis] +
                         _barrelEmitterInWeapon[2] * weaponWorld[8 + axis] + weaponWorld[12 + axis];
    }
    return true;
}

bool GetBarrelOpeningWorld(float* position, float* direction)
{
    // The weapon in the current base frame (GetWeaponWorld): placed at the last first-person draw, often the frame
    // before, its raw place left the aiming line moving round the muzzle while walking (PLAN 58 l, headset run 17).
    float weaponWorld[16];
    if (!IsBarrelFresh() || !GetWeaponWorldWithin(weaponWorld, BARREL_LIFETIME_MILLISECONDS, true))
    {
        return false;
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        position[axis] = _barrelMuzzleInWeapon[0] * weaponWorld[axis] +
                         _barrelMuzzleInWeapon[1] * weaponWorld[4 + axis] +
                         _barrelMuzzleInWeapon[2] * weaponWorld[8 + axis] + weaponWorld[12 + axis];
        direction[axis] = _barrelAxisInWeapon[0] * weaponWorld[axis] +
                          _barrelAxisInWeapon[1] * weaponWorld[4 + axis] +
                          _barrelAxisInWeapon[2] * weaponWorld[8 + axis];
    }
    return true;
}

void SetGunnerFrameTest(bool isOn)
{
    _isGunnerFrameOn = isOn;
    Log::Info("aim", "gunner seats: the game's first-person model %s", isOn ? "drawn" : "left out");
}

void SetDroidTest(bool shouldWriteAim, bool shouldMirror)
{
    _shouldWriteAim = shouldWriteAim;
    _shouldMirrorDroid = shouldMirror;
    Log::Info("aim", "droid test: aim write %d, mirrored arm %d", shouldWriteAim ? 1 : 0, shouldMirror ? 1 : 0);
}

bool IsFireLineFromOpening()
{
    return IsBarrelFresh();
}

bool GetBarrelEmitter(float* emitter, float* axis)
{
    if (!IsBarrelFresh())
    {
        return false;
    }
    memcpy(emitter, _barrelEmitterInWeapon, sizeof(_barrelEmitterInWeapon));
    memcpy(axis, _barrelAxisInWeapon, sizeof(_barrelAxisInWeapon));
    return true;
}

bool IsMirrorDrawn()
{
    return _isMirrorWanted;
}

bool IsInFirstPersonRender()
{
    return _isInFirstPersonRender;
}

void SetScopeHeadAim(bool isOn)
{
    _shouldAimWithHeadWhenZoomed = isOn;
    Log::Info("aim", "head aiming while zoomed %s", isOn ? "on" : "off");
}

// Heading of a tracked controller relative to the character's facing, radians, positive to the right. False if not
// tracked.
bool GetHandYaw(int hand, float* yawRadians)
{
    if (!_isTracked[hand])
    {
        return false;
    }
    FpGuard guard;
    const float forwardX = -_trackedPoses[hand][8],
                forwardZ = -_trackedPoses[hand][10]; // forward in resting-pose space (x right, -z ahead)
    if (forwardX * forwardX + forwardZ * forwardZ < 0.04f)
    {
        return false; // pointing almost straight up or down
    }
    *yawRadians = atan2f(forwardX, -forwardZ);
    return true;
}

void Kick()
{
    _kickTick = GetTickCount() | 1;
}

void SetTwoHandedAllowed(bool isAllowed)
{
    _isTwoHandedAllowed = isAllowed;
    Log::Info("aim", "two-handed grip %s", isAllowed ? "allowed" : "off");
}

bool IsTwoHanded()
{
    return _isTwoHanded;
}

bool IsLeftHandAtGun()
{
    const DWORD tick = _leftAtGunTick;
    // Signed: the tick has bit 0 set and is written on another thread, so it can be 1 ms ahead of this clock (run134:
    // the unsigned difference wrapped and the hologram flickered on).
    return tick && static_cast<long>(GetTickCount() - tick) < static_cast<long>(AT_GUN_LINGER_MILLISECONDS);
}

bool InstallFirstPersonModelHook()
{
    return InstallFirstPersonHook();
}

void SetThrowDirection(const float* direction, unsigned milliseconds)
{
    memcpy(_throwDirection, direction, sizeof(_throwDirection));
    _throwUntilTick = (GetTickCount() + milliseconds) | 1;
}

void SetNetThrowAim(const float* direction, unsigned milliseconds)
{
    memcpy(_netThrowAim, direction, sizeof(_netThrowAim));
    _netThrowAimUntilTick = (GetTickCount() + milliseconds) | 1;
}

bool GetNetThrowAim(float* direction)
{
    if (!_netThrowAimUntilTick || static_cast<LONG>(GetTickCount() - _netThrowAimUntilTick) >= 0)
    {
        return false;
    }
    memcpy(direction, _netThrowAim, sizeof(_netThrowAim));
    return true;
}

bool GetPalmInHand(float* offset)
{
    if (!_armsCopy.isValid || !_armsCopy.hasPalm)
    {
        return false;
    }
    memcpy(offset, _armsCopy.palmInHand, 3 * sizeof(float));
    return true;
}

bool GetBaseFrame(float* matrix)
{
    if (!_hasBaseFrame)
    {
        return false;
    }
    memcpy(matrix, _baseFrame, sizeof(_baseFrame));
    return true;
}

void SetLeftHandOnMagazine(bool isOn)
{
    _isMagazineWanted = isOn;
    if (!isOn)
    {
        _isOnMagazine = false;
        _magazinePullMeters = 0;
    }
}

float MagazinePullMeters()
{
    return _magazinePullMeters;
}

void SetWeaponHandling(int weight, bool hasVirtualStock)
{
    _weaponWeight = weight < 0 ? 0 : weight > 3 ? 3 : weight;
    _hasVirtualStock = hasVirtualStock;
    Log::Info(
        "aim",
        "weapon weight %d (0 off .. 3 heavy), virtual stock %s",
        _weaponWeight,
        hasVirtualStock ? "on" : "off");
}

bool GetHandWorld(int hand, float* matrix)
{
    if (!_isTracked[hand & 1] || !_hasBaseFrame)
    {
        return false;
    }
    FpGuard guard;
    Multiply(matrix, _trackedPoses[hand & 1], _baseFrame);
    return true;
}

// For Sight.cpp: the line the hand-held weapon shoots along, if a hand aimed within the last frames.
bool GetFirstPersonElbowWorld(int hand, float* position)
{
    if (hand < 0 || hand > 1 || !_elbowTick[hand] ||
        GetTickCount() - _elbowTick[hand] > FIRE_LINE_LIFETIME_MILLISECONDS)
    {
        return false;
    }
    memcpy(position, _elbowWorld[hand], sizeof(_elbowWorld[hand]));
    return true;
}

uintptr_t GetWeapon()
{
    return reinterpret_cast<uintptr_t>(CurrentWeapon());
}

uintptr_t GetWeaponClass()
{
    return reinterpret_cast<uintptr_t>(CurrentWeaponClass());
}

bool GetWeaponWorld(float* matrix)
{
    return GetWeaponWorldWithin(matrix, FIRE_LINE_LIFETIME_MILLISECONDS);
}

bool GetWeaponWorldWithin(float* matrix, DWORD lifetimeMilliseconds, bool isSteady)
{
    const float* const weaponWorld = isSteady ? _steadyWeaponWorld : _weaponWorld;
    if (!_weaponWorldTick || static_cast<LONG>(GetTickCount() - _weaponWorldTick) > static_cast<LONG>(lifetimeMilliseconds))
    {
        return false;
    }
    // The weapon was placed at the last first-person draw, often the frame before (the scope pass comes first): the
    // player moving or turning between them moved the lens against the head, and the scope picture's field (lens size
    // over its distance from the eye) grew and shrank while walking (headset run 16, all lens weapons). The hand's
    // place in the base frame is kept and put into the current base frame: weapon = W x B_then^-1 x B_now.
    float baseNow[16];
    if (Stereo::GetBaseCamera(baseNow) && IsRigid(_weaponWorldBase, 0))
    {
        FpGuard guard;
        float inverse[16], inBase[16];
        InvertRigid(inverse, _weaponWorldBase);
        Multiply(inBase, weaponWorld, inverse);
        Multiply(matrix, inBase, baseNow);
        return true;
    }
    memcpy(matrix, weaponWorld, sizeof(_weaponWorld));
    return true;
}

bool IsDroideka()
{
    return _isDroideka;
}

bool GetTargetSearchDirection(float* direction)
{
    if (!_targetSearchTick || GetTickCount() - _targetSearchTick >= TARGET_SEARCH_FRESH_MILLISECONDS)
    {
        return false;
    }
    memcpy(direction, _targetSearchDirection, sizeof(_targetSearchDirection));
    return true;
}

bool GetDrawnFireLine(float* from, float* direction)
{
    float weaponWorld[16];
    if (!_hasFireStartInWeapon || !GetFireLine(from, direction) ||
        !GetWeaponWorldWithin(weaponWorld, FIRE_LINE_LIFETIME_MILLISECONDS, true))
    {
        return false;
    }
    FpGuard guard;
    for (int axis = 0; axis < 3; ++axis)
    {
        from[axis] = _fireStartInWeapon[0] * weaponWorld[axis] + _fireStartInWeapon[1] * weaponWorld[4 + axis] +
                     _fireStartInWeapon[2] * weaponWorld[8 + axis] + weaponWorld[12 + axis];
    }
    if (_isWeaponOnGrip)
    {
        // shots go along the barrel as drawn (as the aimer hook sends them)
        const float barrelZ[3] = {0, 0, 1};
        const float* barrel = _hasBarrelCorrection ? _barrelCorrection->axis : barrelZ;
        float length = 0;
        for (int axis = 0; axis < 3; ++axis)
        {
            direction[axis] = barrel[0] * weaponWorld[axis] + barrel[1] * weaponWorld[4 + axis] +
                              barrel[2] * weaponWorld[8 + axis];
            length += direction[axis] * direction[axis];
        }
        length = sqrtf(length);
        if (length < 1e-6f)
        {
            return false;
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            direction[axis] /= length;
        }
    }
    return true;
}

bool GetStockFireOrigin(float* origin)
{
    if (!_fireLineTick || GetTickCount() - _fireLineTick > FIRE_LINE_LIFETIME_MILLISECONDS)
    {
        return false;
    }
    memcpy(origin, _stockFireOrigin, sizeof(_stockFireOrigin));
    return true;
}

bool GetFireLine(float* from, float* direction)
{
    if (!_fireLineTick || GetTickCount() - _fireLineTick > FIRE_LINE_LIFETIME_MILLISECONDS)
    {
        return false;
    }
    memcpy(from, _fireLineFrom, sizeof(_fireLineFrom));
    memcpy(direction, _fireLineDirection, sizeof(_fireLineDirection));
    return true;
}

} // namespace Aim

} // namespace SwbfVr
