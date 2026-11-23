// Body.cpp: the player's own body in first person (PLAN 51): VR Settings "full_body" and the script command body.
//
// The game (static reading, 2026-10-14, docs/offsets.md "Soldier body draw"):
//   0x113690 EntitySoldier render, thiscall on soldier+0x17C (vtable 0x416620 slot 3), three stack arguments
//            (ret 0xC): a flag byte, a float, one unknown. It works out a skip flag: 1 when 0x10FFF0 (asked for the
//            view's object) says yes, else the flag argument (cmove at 0x1136E3), stores it at this+0x900 and in
//            [ebp-0x20]; a set flag jumps past the body's draw at 0x113CF9.
//   0x216E50 SoldierAnimator::GetBones(out), thiscall, ret 4: the pose of this frame copied into a bone set on the
//            render's stack (calls at 0x113FC0 / 0x113FF8, both after the skip). The set: +0x00 count, +0x04 a hash
//            table of 0x100 words, the first 0x80 the bones' name hashes (HashName), the second 0x80 pointers to
//            their 4x4 matrices (lookup 0x300890 via 0x8C230; insert 0x8D1E0 stores the pointer into the pose).
//            The model draws (model vtable slot 1 at 0x114B5D / 0x114B91) read the matrices through these pointers.
// What this stage does, player's soldier only: logs the effective skip flag, whether GetBones ran, and the bones'
// translations next to the soldier's position (to tell model space from world space). Run 198: the skip flag is 0
// in first person and the bones are built; only the shadow shows (alpha 0, see the visibility branch). "draw 1"
// takes the game's alpha argument instead, for the player's call; "head 0" points the head's slot at
// our copy of its matrix with the rotation part zero (the head's vertices collapse to its joint); "hand" points the
// right hand's slot at a moved copy. The bone set lives on the game's stack for one call: nothing to restore.
// "place" (PLAN 51 step 3) moves every model draw of the player's render (body, its third-person weapon, the shadow)
// by one world offset so the neck bone sits a set distance behind and below the point between the eyes: the eye then
// looks out of the front of the head instead of from inside the chest. Horizontal only unless asked for.
// "arms 1" (step 4) bends both arms to the controllers by two-bone IK at the body's draw, in model space (+y up, +z
// forward, the right side at -x; each bone points along its own +x, run198): shoulder fixed, elbow from the lengths
// of the game's pose and a pole down and outward, the hand bone at the controller, its grip turned as the
// controller is turned from a level forward hold; the
// first-person arms are not drawn then ("arms 2" keeps them, to compare). "arms 3" (b206, the full body's default since
// headset run 14: the body's arms were far too low-poly) draws the first-person arms (forearms and gloves at the
// controllers, as without the body) and keeps only the body's upper arms, bent to the elbow; its forearms and hands
// are collapsed onto the elbow.
// Head IK (b206, PLAN 51 step 5, idren2023 after headset run 14): the neck's target is a fixed point in the head's own
// frame (10 cm behind and 12 cm below the eyes), so it stays put while the head turns or nods about it. The body's
// feet stay where they are while that target moves less than LEAN_RADIUS_METERS from them (an anchor in the base
// frame, which moves and turns with the soldier); further, the body is dragged along. The upper body (everything from
// bone_a_spine up) is then turned about bone_a_spine so the neck reaches the target: leaning and bending show.
// All hooks: MinHook, main thread (the game's Render, also when Stereo calls it once per eye).

#include "Body.h"

#include "Aim.h"
#include "D3dSurvey.h"
#include "EngineSurvey.h"
#include "FpGuard.h"
#include "GameBuild.h"
#include "GunnerSeat.h"
#include "Handlebars.h"
#include "Log.h"
#include "PlayerBody.h"
#include "Stereo.h"
#include "Vehicle.h"
#include "Xr.h"
#include "engine.h"

#include <windows.h>

#include <MinHook.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace SwbfVr
{

namespace Body
{

namespace
{

// Addresses: GameBuild feature FullBody (SoldierRender, SoldierRenderEnd, GetBones, SoldierVisibilityBranch,
// ModelDraw, FirstPersonArmsDrawReturn, WeaponModelDrawReturn); the notes below give the Steam RVAs.
// 0x113690 soldier render, 0x115673 its end, 0x216E50 GetBones.
// 0x288680 RedModel draw (vtable 0x42C6A0 slot 1): thiscall (matrix, bone set, colour, flags, override), ret 0x14.
// 0x130858 return address of the first-person arms' model draw in FirstPersonRenderable::Render (call at 0x130855).
// 0x243046 return address of Weapon::Render's (0x243000, vtable slot 0x5C) model draw: inside the player's render
// that is the third-person weapon (called at 0x114C34 / 0x114CB6), not drawn while the body is placed.
/// The jne 0x113710 (75 09) after the visibility check 0x115F50 -> 0x228270, which is false for the view's own
/// player in a first-person camera: falling through sets the draw's alpha to 0 (the model draw 0x288680 then skips
/// every segment and draws only the shadow volume; sub-agent, static, 2026-10-14). jmp (EB 09) instead takes the
/// alpha from the float argument, as for everyone else.
constexpr uint8_t VISIBILITY_BRANCH_GAME[2] = {0x75, 0x09};
constexpr uint8_t VISIBILITY_BRANCH_ALWAYS[2] = {0xEB, 0x09};
/// EntitySoldier render's `this` is the soldier + this; the effective skip flag is kept at this + 0x900.
constexpr uintptr_t OFFSET_soldierRenderable = 0x17C;
constexpr uintptr_t OFFSET_renderableSkipFlag = 0x900;
/// Bone set: count, then the hash table (keys, then values).
constexpr uintptr_t OFFSET_boneSetKeys = 0x4;
constexpr unsigned BONE_SET_SLOTS = 0x80;
constexpr uintptr_t OFFSET_boneSetValues = OFFSET_boneSetKeys + uintptr_t{BONE_SET_SLOTS} * 4;

/// The third-person bones (rep_inf_* msh files of the mod tools).
constexpr const char* BONE_NAMES[] = {
    "bone_root",       "bone_pelvis",     "bone_a_spine",    "bone_b_spine",   "bone_ribcage", "bone_neck",
    "bone_head",       "bone_l_clavicle", "bone_l_upperarm", "bone_l_forearm", "bone_l_hand",  "bone_r_clavicle",
    "bone_r_upperarm", "bone_r_forearm",  "bone_r_hand",     "bone_l_thigh",   "bone_l_calf",  "bone_l_foot",
    "bone_l_toe",      "bone_r_thigh",    "bone_r_calf",     "bone_r_foot",    "bone_r_toe",   "hp_weapons"};
constexpr unsigned BONE_NAME_COUNT = sizeof(BONE_NAMES) / sizeof(BONE_NAMES[0]);
constexpr unsigned NECK_BONE = 5;
constexpr unsigned HEAD_BONE = 6;
constexpr unsigned RIGHT_HAND_BONE = 14;
/// Upper arm, forearm, hand per controller (Aim's numbering: 0 right, 1 left).
constexpr unsigned ARM_BONES[2][3] = {{12, 13, 14}, {8, 9, 10}};
/// Model-space x of the side each arm is on (right at -x).
constexpr float ARM_SIDE[2] = {-1.0f, 1.0f};
/// "arms 3": the body's upper arm may be stretched or shortened this far to reach the first-person forearm.
constexpr float ELBOW_STRETCH_MINIMUM = 0.7f, ELBOW_STRETCH_MAXIMUM = 1.5f;
/// The spine's pivot for the head IK, and every bone that turns with the upper body (BONE_NAMES indices: the spine
/// bones, ribcage, neck, head, both clavicles and arms, hp_weapons).
constexpr unsigned SPINE_PIVOT_BONE = 2;
constexpr unsigned UPPER_BODY_BONES[] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 23};
constexpr unsigned UPPER_BODY_BONE_COUNT = sizeof(UPPER_BODY_BONES) / sizeof(UPPER_BODY_BONES[0]);
/// The feet stay planted while the neck's target is within this of them, horizontally (a lean); beyond it they follow.
constexpr float LEAN_RADIUS_METERS = 0.25f;
/// The upper body turns at most this far (radians, 60 degrees) toward the neck's target.
constexpr float MAXIMUM_SPINE_TURN_RADIANS = 1.047f;
/// The upper body's shoulders are squared (turned about the vertical through the spine's pivot) to the way between
/// the body's own forward and the head's (run447: the game's rifle stance has them yawed ~45 deg, the right shoulder
/// 33 cm back, the right hand out of reach), at most this far (radians, 75 degrees).
constexpr float MAXIMUM_SQUARE_RADIANS = 1.309f;
/// Height mode 2's scale: the soldier's neck above its feet when standing in the game's rifle stance (model units:
/// bone_neck 1.42, run449; the msh's rest pose has 1.72, the stance bends the knees), and a
/// person's eye height over their height (anthropometry: 0.936).
constexpr float SOLDIER_STANDING_NECK_METERS = 1.42f;
constexpr float EYE_HEIGHT_SHARE = 0.936f;
/// Clavicles (BONE_NAMES indices, per controller as ARM_BONES): a hand beyond REACH_SHARE of its arm's length turns its
/// collarbone toward it (the shoulder moves to the target) by at most MAXIMUM_SHRUG_RADIANS (30 degrees).
constexpr unsigned CLAVICLE_BONES[2] = {11, 7};
constexpr float REACH_SHARE = 0.97f;
constexpr float MAXIMUM_SHRUG_RADIANS = 0.524f;
/// Bones whose translation the summary line shows: root, pelvis, head, left hand, right hand, hp_weapons.
constexpr unsigned SUMMARY_BONES[] = {0, 1, 6, 10, 14, 23};
constexpr unsigned SUMMARY_LINE_LIMIT = 30;

/// void __thiscall EntitySoldier render(flag, float, unknown), written as __fastcall: ecx = soldier + 0x17C, edx
/// unused, three stack words popped by the callee.
using SoldierRenderFunction =
    void(__fastcall*)(uintptr_t renderable, void* edx, uint32_t flag, float value, uint32_t third);
/// unsigned __thiscall SoldierAnimator::GetBones(BoneSet* out), as __fastcall; returns the bone count.
using GetBonesFunction = unsigned(__fastcall*)(uintptr_t animator, void* edx, uintptr_t boneSet);
/// void __thiscall RedModel draw(const float* matrix, BoneSet*, colour, flags, override), as __fastcall.
using ModelDrawFunction = void(__fastcall*)(
    uintptr_t model,
    void* edx,
    const float* matrix,
    uintptr_t boneSet,
    uint32_t colour,
    uint32_t flags,
    uint32_t override);

SoldierRenderFunction _originalSoldierRender = nullptr;
GetBonesFunction _originalGetBones = nullptr;
ModelDrawFunction _originalModelDraw = nullptr;
uintptr_t _gameBase = 0;
uintptr_t _soldierRenderRva = 0, _soldierRenderEndRva = 0, _firstPersonArmsDrawReturnRva = 0,
          _weaponModelDrawReturnRva = 0;
uintptr_t _playerSoldier = 0;
/// The player's soldier left out of the render (set in OnPresent, between frames; read by the render hook): in a seat
/// the game shows only in third person, where we put the eye at the rider's (the gunship's door passenger, mounts;
/// idren2023 2026-11-08: his own body, arm raised to the rail, stood in the view).
bool _shouldHidePlayer = false;
/// The player's soldier as last resolved on foot: in a seat the PlayerController's Controllable is the seat's, so
/// Resolve("soldier") gives a wrong address there (run657: the hide never matched).
uintptr_t _onFootSoldier = 0;
float _riderEye[3] = {0, 0, 0}; // where the eye is put while _shouldHidePlayer (world, game metres)
unsigned _hookCallsWhileHidden = 0;
unsigned _hiddenRenderCount = 0; // player renders left out (logged once)
float _nearestSoldier[3] = {0, 0, 0}; // while hidden: the soldier drawn nearest the eye (horizontally), world
float _nearestSoldierDistance = 1e9f;
unsigned _hiddenFrames = 0;
unsigned _boneHashes[BONE_NAME_COUNT];

// Asked for by Configure (script thread) or SetFullBody (main thread), read on the main thread.
volatile bool _isLogRequested = false;
volatile bool _isFullBodyWanted = false; // the VR Settings row "full_body"
volatile int _playerHeightCentimeters = 185; // VR Settings "height" (6 ft 1 in)
bool _isFullBodyActive = false;          // wanted, in VR and on foot with controllers: the switches below are set
volatile bool _shouldForceDraw = false;
volatile bool _shouldHideHead = false;
volatile int _handOffsetCentimeters[3] = {};
volatile bool _shouldPlace = false;
volatile int _placeHeightMode = 0; // 0 horizontal only, 1 lifted, 2 scaled about the feet
volatile int _placeBackCentimeters = 10;
volatile int _placeDownCentimeters = 12;
volatile bool _shouldDriveArms = false;
volatile bool _shouldHideFirstPersonArms = false;
volatile bool _shouldCollapseForearms = false; // "arms 3": the first-person arms are the forearms and hands
volatile bool _shouldBendSpine = false;        // head IK

bool _isInstalled = false;
bool _hasFailed = false;
bool _isInPlayerRender = false;
uint8_t* _visibilityBranch = nullptr;

// Counted between two summary lines.
unsigned _playerRenderCount = 0;
unsigned _playerBonesCount = 0;
uint32_t _lastFlagArgument = 0;
float _lastAlphaArgument = 0.0f;
uint8_t _lastSkipFlag = 0;
unsigned _lastBoneCount = 0;
float _lastTranslations[sizeof(SUMMARY_BONES) / sizeof(SUMMARY_BONES[0])][3];
bool _hasTranslations = false;
bool _hasLoggedSet = false;
unsigned _summaryCount = 0;

/// Our matrices the bone set's slots are pointed at (not in the game's heap; rewritten every call).
float _headMatrix[16];
/// Head bones the game keeps under names we cannot hash (eff_head: the Rebel pilot's head is a skinned segment weighted
/// to bone_head and eff_head, and kept its head in headset run 15; the troopers' helmets hang on bone_head alone).
constexpr int MAXIMUM_EXTRA_HEAD_BONES = 4;
constexpr float EXTRA_HEAD_BONE_RADIUS_METERS = 0.30f;
float _extraHeadMatrices[MAXIMUM_EXTRA_HEAD_BONES][16];
float _handMatrix[16];

// Placement, per player render: the neck in model space (from GetBones), the world offset worked out at the first
// model draw and applied to every later one, and the moved copy of the draw's matrix.
bool _hasNeck = false;
float _neckInModel[3];
bool _hasPlacement = false;
float _placementOffset[3];
float _placementOrigin[3]; // the body's origin (feet) of this render, the scale's fixed point
float _placementScale = 1.0f;
float _placedMatrix[16];
float _lastPlacementOffset[3];
float _lastNeckBelowEye = 0.0f; // metres, before the placement's own vertical move
unsigned _placedDrawCount = 0;

// Arms: the player's bone set of this render (from GetBones), whether the IK ran on it, our arm matrices.
uintptr_t _playerBoneSet = 0;
bool _hasArms = false;
float _armMatrices[2][3][16];
unsigned _armDrawCount = 0;
bool _wereArmsDriven = false; // the right arm was bent in the last player render (first-person arms hidden only then)
bool _isArmsDrivenThisRender = false;
float _lastArmReach[2]; // shoulder to target over the arm's length, last IK
// Head IK: the neck's target in the world (this eye pass), the feet's anchor in the base frame (x, z), the upper body.
float _neckTargetWorld[3];
bool _hasNeckTarget = false;
float _anchorInBase[2];
bool _hasAnchor = false;
bool _hasSpine = false; // the upper body was turned for this render
bool _hasOurBody = false; // PlayerBody drew our body in this render (the game's body draws skipped)
float _spineMatrices[UPPER_BODY_BONE_COUNT][16];
float _lastSpineDegrees = 0.0f;
float _lastSquareDegrees = 0.0f;
float _anchorWorld[3]; // where the neck goes this render (ComputePlacement), for PlayerBody
float _clavicleMatrices[2][4][16]; // per controller: clavicle, upper arm, forearm, hand turned with the collarbone
float _lastShrugDegrees[2];
unsigned _firstPersonPassRenderCount = 0; // player renders inside the first-person pass
unsigned _eyePassRenderCount = 0;         // player renders inside an eye pass

/// Finds a bone's slot in the set: the address of its matrix pointer, or null.
uintptr_t* FindSlot(uintptr_t boneSet, unsigned hash)
{
    unsigned* keys = reinterpret_cast<unsigned*>(boneSet + OFFSET_boneSetKeys);
    for (unsigned slot = 0; slot < BONE_SET_SLOTS; ++slot)
    {
        if (keys[slot] == hash)
        {
            return reinterpret_cast<uintptr_t*>(boneSet + OFFSET_boneSetValues) + slot;
        }
    }
    return nullptr;
}

const char* BoneName(unsigned hash)
{
    for (unsigned bone = 0; bone < BONE_NAME_COUNT; ++bone)
    {
        if (_boneHashes[bone] == hash)
        {
            return BONE_NAMES[bone];
        }
    }
    return nullptr;
}

/// Once: every key of the set with its name and translation, and the head's full matrix.
void LogBoneSet(uintptr_t boneSet)
{
    const unsigned* keys = reinterpret_cast<const unsigned*>(boneSet + OFFSET_boneSetKeys);
    const uintptr_t* values = reinterpret_cast<const uintptr_t*>(boneSet + OFFSET_boneSetValues);
    Log::Info("body", "bone set: count %u", *reinterpret_cast<const unsigned*>(boneSet));
    for (unsigned slot = 0; slot < BONE_SET_SLOTS; ++slot)
    {
        if (!keys[slot] || !values[slot] || IsBadReadPtr(reinterpret_cast<const void*>(values[slot]), 64))
        {
            continue;
        }
        const float* matrix = reinterpret_cast<const float*>(values[slot]);
        const char* name = BoneName(keys[slot]);
        Log::Info(
            "body",
            "  slot %3u %08X %-16s at %08X: x (%.2f %.2f %.2f) y (%.2f %.2f %.2f) z (%.2f %.2f %.2f) t (%.3f %.3f "
            "%.3f) w %.2f %.2f %.2f %.2f",
            slot,
            keys[slot],
            name ? name : "?",
            static_cast<unsigned>(values[slot]),
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
}

// Hook of the soldier render: for the player's soldier, notes the call and the flag, optionally lets the flag
// argument alone decide the skip, and marks the call so GetBones below knows the set is the player's.
void __fastcall SoldierRenderHook(uintptr_t renderable, void* edx, uint32_t flag, float value, uint32_t third)
{
    const bool isPlayer = _playerSoldier && renderable == _playerSoldier + OFFSET_soldierRenderable;
    if (_shouldHidePlayer && !isPlayer && !IsBadReadPtr(reinterpret_cast<const void*>(renderable - OFFSET_soldierRenderable + OFFSET_soldierBodyMatrix), 64))
    {
        // the seated passenger may not be the soldier resolved on foot (run658): any soldier standing at the eye
        ++_hookCallsWhileHidden;
        const float* body =
            reinterpret_cast<const float*>(renderable - OFFSET_soldierRenderable + OFFSET_soldierBodyMatrix);
        const float dx = body[12] - _riderEye[0], dy = body[13] - _riderEye[1], dz = body[14] - _riderEye[2];
        if (dx * dx + dz * dz < _nearestSoldierDistance)
        {
            _nearestSoldierDistance = dx * dx + dz * dz;
            memcpy(_nearestSoldier, body + 12, sizeof(_nearestSoldier));
        }
        if (dx * dx + dz * dz < 0.8f * 0.8f && dy > -2.4f && dy < 0.5f)
        {
            if (_hiddenRenderCount++ == 0)
            {
                Log::Info("body", "a soldier at the rider's eye left out (%.2f %.2f %.2f m from it)",
                          static_cast<double>(dx), static_cast<double>(dy), static_cast<double>(dz));
            }
            return;
        }
    }
    if (!isPlayer)
    {
        _originalSoldierRender(renderable, edx, flag, value, third);
        return;
    }
    if (_shouldHidePlayer && isPlayer)
    {
        if (_hiddenRenderCount++ == 0)
        {
            Log::Info("body", "the player's soldier left out (first-person view at the rider's eye)");
        }
        return;
    }
    ++_playerRenderCount;
    _firstPersonPassRenderCount += Stereo::IsFirstPersonPass() ? 1 : 0;
    _eyePassRenderCount += Stereo::CurrentEye() >= 0 ? 1 : 0;
    _lastFlagArgument = flag;
    _lastAlphaArgument = value;
    const bool shouldForce = _shouldForceDraw && _visibilityBranch;
    if (shouldForce)
    {
        memcpy(_visibilityBranch, VISIBILITY_BRANCH_ALWAYS, 2);
    }
    _isInPlayerRender = true;
    _hasNeck = false;
    _hasPlacement = false;
    _playerBoneSet = 0;
    _hasArms = false;
    _hasSpine = false;
    _hasOurBody = false;
    _originalSoldierRender(renderable, edx, flag, value, third);
    _isInPlayerRender = false;
    if (shouldForce)
    {
        memcpy(_visibilityBranch, VISIBILITY_BRANCH_GAME, 2);
    }
    _lastSkipFlag = *reinterpret_cast<uint8_t*>(renderable + OFFSET_renderableSkipFlag);
}

// Hook of GetBones: after the game filled the set, for the player's render only, notes the translations and
// applies the test changes by pointing slots at our matrices.
unsigned __fastcall GetBonesHook(uintptr_t animator, void* edx, uintptr_t boneSet)
{
    const unsigned count = _originalGetBones(animator, edx, boneSet);
    const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - _gameBase;
    if (!_isInPlayerRender || returnAddress < _soldierRenderRva || returnAddress >= _soldierRenderEndRva)
    {
        return count;
    }
    FpGuard guard; // RAII: a clean floating-point state for our copies, the game's restored at the end
    ++_playerBonesCount;
    _lastBoneCount = count;
    if (!_hasLoggedSet)
    {
        _hasLoggedSet = true;
        LogBoneSet(boneSet);
    }
    for (unsigned entry = 0; entry < sizeof(SUMMARY_BONES) / sizeof(SUMMARY_BONES[0]); ++entry)
    {
        const uintptr_t* slot = FindSlot(boneSet, _boneHashes[SUMMARY_BONES[entry]]);
        const float* matrix = slot ? reinterpret_cast<const float*>(*slot) : nullptr;
        for (int axis = 0; axis < 3; ++axis)
        {
            _lastTranslations[entry][axis] = matrix ? matrix[12 + axis] : 0.0f;
        }
    }
    _hasTranslations = true;
    _playerBoneSet = boneSet;
    if (const uintptr_t* neck = FindSlot(boneSet, _boneHashes[NECK_BONE]); neck && *neck)
    {
        memcpy(_neckInModel, reinterpret_cast<const float*>(*neck) + 12, sizeof(_neckInModel));
        _hasNeck = true;
    }
    if (_shouldHideHead)
    {
        uintptr_t* slot = FindSlot(boneSet, _boneHashes[HEAD_BONE]);
        if (slot && *slot)
        {
            memcpy(_headMatrix, reinterpret_cast<const float*>(*slot), sizeof(_headMatrix));
            for (int element = 0; element < 12; ++element)
            {
                if (element % 4 != 3)
                {
                    _headMatrix[element] = 0.0f;
                }
            }
            *slot = reinterpret_cast<uintptr_t>(_headMatrix);
            // Every unnamed bone near the head and above the neck collapses too, onto the head's joint.
            unsigned* keys = reinterpret_cast<unsigned*>(boneSet + OFFSET_boneSetKeys);
            uintptr_t* values = reinterpret_cast<uintptr_t*>(boneSet + OFFSET_boneSetValues);
            int extraCount = 0;
            for (unsigned index = 0; index < BONE_SET_SLOTS && extraCount < MAXIMUM_EXTRA_HEAD_BONES; ++index)
            {
                if (!keys[index] || !values[index] || BoneName(keys[index]) ||
                    IsBadReadPtr(reinterpret_cast<const void*>(values[index]), 64))
                {
                    continue;
                }
                const float* bone = reinterpret_cast<const float*>(values[index]);
                float distanceSquared = 0;
                for (int axis = 0; axis < 3; ++axis)
                {
                    const float difference = bone[12 + axis] - _headMatrix[12 + axis];
                    distanceSquared += difference * difference;
                }
                if (distanceSquared > EXTRA_HEAD_BONE_RADIUS_METERS * EXTRA_HEAD_BONE_RADIUS_METERS ||
                    (_hasNeck && bone[13] < _neckInModel[1]))
                {
                    continue;
                }
                float* collapsed = _extraHeadMatrices[extraCount++];
                memcpy(collapsed, _headMatrix, sizeof(_headMatrix));
                values[index] = reinterpret_cast<uintptr_t>(collapsed);
                static unsigned loggedKeys[MAXIMUM_EXTRA_HEAD_BONES * 4];
                static int loggedCount = 0;
                bool isLogged = false;
                for (int logged = 0; logged < loggedCount; ++logged)
                {
                    isLogged = isLogged || loggedKeys[logged] == keys[index];
                }
                if (!isLogged && loggedCount < MAXIMUM_EXTRA_HEAD_BONES * 4)
                {
                    loggedKeys[loggedCount++] = keys[index];
                    Log::Info(
                        "body",
                        "head hidden: also collapsed bone %08X, %.0f cm from the head joint",
                        keys[index],
                        static_cast<double>(sqrtf(distanceSquared) * 100));
                }
            }
        }
    }
    const int handOffset[3] = {_handOffsetCentimeters[0], _handOffsetCentimeters[1], _handOffsetCentimeters[2]};
    if (handOffset[0] || handOffset[1] || handOffset[2])
    {
        uintptr_t* slot = FindSlot(boneSet, _boneHashes[RIGHT_HAND_BONE]);
        if (slot && *slot)
        {
            memcpy(_handMatrix, reinterpret_cast<const float*>(*slot), sizeof(_handMatrix));
            for (int axis = 0; axis < 3; ++axis)
            {
                _handMatrix[12 + axis] += static_cast<float>(handOffset[axis]) * 0.01f;
            }
            *slot = reinterpret_cast<uintptr_t>(_handMatrix);
        }
    }
    return count;
}

float Dot(const float* a, const float* b);

/// The world offset (and, height mode 2, the scale about the body's origin) that puts the neck (model space, through
/// the draw's matrix) behind and below the head, from the head's level heading. False outside an eye pass or before
/// the bones of this render are known.
bool ComputePlacement(const float* matrix, float* offset)
{
    float head[16];
    if (!_hasNeck || !Stereo::GetHeadWorld(head))
    {
        return false;
    }
    float neck[3];
    for (int axis = 0; axis < 3; ++axis)
    {
        neck[axis] = _neckInModel[0] * matrix[axis] + _neckInModel[1] * matrix[4 + axis] +
                     _neckInModel[2] * matrix[8 + axis] + matrix[12 + axis];
    }
    const float back = static_cast<float>(_placeBackCentimeters) * 0.01f;
    const float down = static_cast<float>(_placeDownCentimeters) * 0.01f;
    // The neck's target: behind and below the eyes in the head's own frame (camera convention: row 1 up, row 2 back),
    // so nodding or turning the head moves the eyes about it, not the neck (head IK, b206). Before b206 the offsets
    // were level (the head's heading) and vertical, and the whole body moved with the eyes.
    for (int axis = 0; axis < 3; ++axis)
    {
        _neckTargetWorld[axis] = head[12 + axis] + head[8 + axis] * back - head[4 + axis] * down;
    }
    _hasNeckTarget = true;
    _lastNeckBelowEye = head[13] - neck[1];
    memcpy(_placementOrigin, matrix + 12, sizeof(_placementOrigin));
    _placementScale = 1.0f;
    const float neckHeight = neck[1] - _placementOrigin[1];
    if (_placeHeightMode == 2 && neckHeight > 0.3f)
    {
        // From the player's height (VR Settings "height"), not the headset: the standing eyes' height less `down` over
        // the soldier's standing neck. Ducking or the game's crouch no longer shrinks the body (and the neck's target,
        // which rises as the head pitches down, grew it 1.13 -> 1.25 in run449).
        const float eyeHeight = static_cast<float>(_playerHeightCentimeters) * 0.01f * EYE_HEIGHT_SHARE;
        const float scale = (eyeHeight - down) / SOLDIER_STANDING_NECK_METERS;
        _placementScale = scale < 0.7f ? 0.7f : (scale > 1.4f ? 1.4f : scale);
        for (int axis = 0; axis < 3; ++axis)
        {
            neck[axis] = _placementOrigin[axis] + (neck[axis] - _placementOrigin[axis]) * _placementScale;
        }
    }
    // Where the upright neck goes, horizontally: the anchor, which follows the target only beyond LEAN_RADIUS_METERS
    // (in the base frame, so it walks and turns with the soldier). Without a base frame, the target itself.
    float place[3] = {_neckTargetWorld[0], _neckTargetWorld[1], _neckTargetWorld[2]};
    float base[16];
    if (_shouldBendSpine && Aim::GetBaseFrame(base))
    {
        float relative[3], local[2];
        for (int axis = 0; axis < 3; ++axis)
        {
            relative[axis] = _neckTargetWorld[axis] - base[12 + axis];
        }
        local[0] = Dot(relative, base);     // right
        local[1] = Dot(relative, base + 8); // back
        if (!_hasAnchor)
        {
            _anchorInBase[0] = local[0];
            _anchorInBase[1] = local[1];
            _hasAnchor = true;
        }
        const float awayX = local[0] - _anchorInBase[0], awayZ = local[1] - _anchorInBase[1];
        const float away = sqrtf(awayX * awayX + awayZ * awayZ);
        if (away > LEAN_RADIUS_METERS)
        {
            _anchorInBase[0] = local[0] - awayX / away * LEAN_RADIUS_METERS;
            _anchorInBase[1] = local[1] - awayZ / away * LEAN_RADIUS_METERS;
        }
        const float up = Dot(relative, base + 4);
        for (int axis = 0; axis < 3; ++axis)
        {
            place[axis] = base[12 + axis] + base[axis] * _anchorInBase[0] + base[4 + axis] * up +
                          base[8 + axis] * _anchorInBase[1];
        }
    }
    memcpy(_anchorWorld, place, sizeof(_anchorWorld));
    offset[0] = place[0] - neck[0];
    offset[1] = _placeHeightMode == 1 ? head[13] - down - neck[1] : 0.0f;
    offset[2] = place[2] - neck[2];
    return true;
}

float Dot(const float* a, const float* b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/// Scales a 3-vector to length 1; false (vector unchanged) when it is nearly zero.
bool Normalize(float* v)
{
    const float length = sqrtf(Dot(v, v));
    if (length < 1e-5f)
    {
        return false;
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        v[axis] /= length;
    }
    return true;
}

/// The rotation (3x3 inside a 4x4, row vectors) that turns unit direction from into unit direction to by the
/// shortest way; identity for (nearly) opposite or equal directions.
void RotationBetween(const float* from, const float* to, float* rotation)
{
    const float v[3] = {
        from[1] * to[2] - from[2] * to[1],
        from[2] * to[0] - from[0] * to[2],
        from[0] * to[1] - from[1] * to[0]};
    const float c = Dot(from, to);
    memset(rotation, 0, 16 * sizeof(float));
    rotation[15] = 1.0f;
    if (c < -0.999f)
    {
        rotation[0] = rotation[5] = rotation[10] = 1.0f;
        return;
    }
    const float k = 1.0f / (1.0f + c);
    // Column form R = c I + [v]x + v v^T k; row vectors use its transpose.
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            rotation[row * 4 + column] = (row == column ? c : 0.0f) + v[row] * v[column] * k;
        }
    }
    rotation[1] += v[2];
    rotation[2] -= v[1];
    rotation[4] -= v[2];
    rotation[6] += v[0];
    rotation[8] += v[1];
    rotation[9] -= v[0];
}

/// out = bone's rotation turned by rotation (row vectors: each axis row times rotation), at position.
void TurnBone(const float* bone, const float* rotation, const float* position, float* out)
{
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            out[row * 4 + column] = bone[row * 4] * rotation[column] + bone[row * 4 + 1] * rotation[4 + column] +
                                    bone[row * 4 + 2] * rotation[8 + column];
        }
        out[row * 4 + 3] = 0.0f;
    }
    memcpy(out + 12, position, 3 * sizeof(float));
    out[15] = 1.0f;
}

/// Two-bone IK of one arm to a target in model space; points the set's three slots at our matrices. turn: the
/// controller's rotation away from a level, straight-ahead hold, in model space; the hand bone keeps the game's grip
/// on its rifle turned by it.
void BendArm(uintptr_t boneSet, int hand, const float* target, const float* turn, const float* elbowTarget)
{
    uintptr_t* slots[3];
    const float* bones[3];
    for (int part = 0; part < 3; ++part)
    {
        slots[part] = FindSlot(boneSet, _boneHashes[ARM_BONES[hand][part]]);
        if (!slots[part] || !*slots[part])
        {
            return;
        }
        bones[part] = reinterpret_cast<const float*>(*slots[part]);
    }
    const float* shoulder = bones[0] + 12;
    float upper[3], lower[3], toTarget[3];
    for (int axis = 0; axis < 3; ++axis)
    {
        upper[axis] = bones[1][12 + axis] - shoulder[axis];
        lower[axis] = bones[2][12 + axis] - bones[1][12 + axis];
        toTarget[axis] = target[axis] - shoulder[axis];
    }
    const float upperLength = sqrtf(Dot(upper, upper)), lowerLength = sqrtf(Dot(lower, lower));
    float distance = sqrtf(Dot(toTarget, toTarget));
    if (upperLength < 0.05f || lowerLength < 0.05f || !Normalize(toTarget))
    {
        return;
    }
    _lastArmReach[hand] = distance / (upperLength + lowerLength);
    const float longest = (upperLength + lowerLength) * 0.999f;
    const float shortest = fabsf(upperLength - lowerLength) + 0.01f;
    distance = distance > longest ? longest : (distance < shortest ? shortest : distance);
    // Elbow: along the shoulder-target line by a, off it by h toward the pole (down, outward, a little back).
    const float a = (upperLength * upperLength - lowerLength * lowerLength + distance * distance) / (2.0f * distance);
    const float h = sqrtf(fmaxf(0.0f, upperLength * upperLength - a * a));
    float pole[3] = {ARM_SIDE[hand] * 0.5f, -1.0f, -0.3f};
    const float along = Dot(pole, toTarget);
    for (int axis = 0; axis < 3; ++axis)
    {
        pole[axis] -= toTarget[axis] * along;
    }
    if (!Normalize(pole))
    {
        pole[0] = 0.0f;
        pole[1] = -1.0f;
        pole[2] = 0.0f;
    }
    float elbow[3], wrist[3], upperNew[3], lowerNew[3];
    for (int axis = 0; axis < 3; ++axis)
    {
        elbow[axis] = shoulder[axis] + toTarget[axis] * a + pole[axis] * h;
        wrist[axis] = shoulder[axis] + toTarget[axis] * distance;
    }
    // With the first-person forearms drawn ("arms 3"), the upper arm ends where the first-person forearm begins
    // (headset run 15: they did not meet), stretched or shortened along itself within ELBOW_STRETCH limits.
    float stretch = 1.0f;
    if (_shouldCollapseForearms && elbowTarget)
    {
        float toElbow[3] = {elbowTarget[0] - shoulder[0], elbowTarget[1] - shoulder[1], elbowTarget[2] - shoulder[2]};
        const float elbowDistance = sqrtf(Dot(toElbow, toElbow));
        if (elbowDistance > 0.05f)
        {
            stretch = elbowDistance / upperLength;
            stretch = stretch < ELBOW_STRETCH_MINIMUM   ? ELBOW_STRETCH_MINIMUM
                      : stretch > ELBOW_STRETCH_MAXIMUM ? ELBOW_STRETCH_MAXIMUM
                                                        : stretch;
            Normalize(toElbow);
            for (int axis = 0; axis < 3; ++axis)
            {
                elbow[axis] = shoulder[axis] + toElbow[axis] * upperLength * stretch;
            }
        }
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        upperNew[axis] = elbow[axis] - shoulder[axis];
        lowerNew[axis] = wrist[axis] - elbow[axis];
    }
    Normalize(upperNew);
    Normalize(lowerNew);
    float rotation[16];
    RotationBetween(bones[0], upperNew, rotation); // row 0 of a bone = its own +x = the way it points
    TurnBone(bones[0], rotation, shoulder, _armMatrices[hand][0]);
    for (int axis = 0; axis < 3; ++axis)
    {
        _armMatrices[hand][0][axis] *= stretch; // along the bone's own +x (row 0)
    }
    RotationBetween(bones[1], lowerNew, rotation);
    TurnBone(bones[1], rotation, elbow, _armMatrices[hand][1]);
    TurnBone(bones[2], turn, wrist, _armMatrices[hand][2]);
    if (_shouldCollapseForearms)
    { // the first-person arms show forearm and hand: the body's collapse onto the elbow (rotation zero, as the head)
        for (int part = 1; part < 3; ++part)
        {
            float* collapsed = _armMatrices[hand][part];
            memset(collapsed, 0, 12 * sizeof(float));
            memcpy(collapsed + 12, elbow, sizeof(elbow));
            collapsed[15] = 1.0f;
        }
    }
    for (int part = 0; part < 3; ++part)
    {
        *slots[part] = reinterpret_cast<uintptr_t>(_armMatrices[hand][part]);
    }
}

/// A hand beyond REACH_SHARE of its arm's length: turns the arm's collarbone (and the arm with it) about the
/// clavicle's joint so the shoulder moves toward the target by what is missing, at most MAXIMUM_SHRUG_RADIANS; points
/// the set's four slots at our turned copies.
void ShrugClavicle(uintptr_t boneSet, int hand, const float* target)
{
    _lastShrugDegrees[hand] = 0.0f;
    uintptr_t* slots[4];
    slots[0] = FindSlot(boneSet, _boneHashes[CLAVICLE_BONES[hand]]);
    for (int part = 0; part < 3; ++part)
    {
        slots[part + 1] = FindSlot(boneSet, _boneHashes[ARM_BONES[hand][part]]);
    }
    for (const uintptr_t* slot : slots)
    {
        if (!slot || !*slot)
        {
            return;
        }
    }
    const float* clavicle = reinterpret_cast<const float*>(*slots[0]);
    const float* shoulder = reinterpret_cast<const float*>(*slots[1]) + 12;
    const float* elbow = reinterpret_cast<const float*>(*slots[2]) + 12;
    const float* wrist = reinterpret_cast<const float*>(*slots[3]) + 12;
    float upper[3], lower[3], toTarget[3], fromJoint[3], jointToTarget[3];
    for (int axis = 0; axis < 3; ++axis)
    {
        upper[axis] = elbow[axis] - shoulder[axis];
        lower[axis] = wrist[axis] - elbow[axis];
        toTarget[axis] = target[axis] - shoulder[axis];
        fromJoint[axis] = shoulder[axis] - clavicle[12 + axis];
        jointToTarget[axis] = target[axis] - clavicle[12 + axis];
    }
    const float missing =
        sqrtf(Dot(toTarget, toTarget)) - REACH_SHARE * (sqrtf(Dot(upper, upper)) + sqrtf(Dot(lower, lower)));
    const float radius = sqrtf(Dot(fromJoint, fromJoint));
    if (missing <= 0.0f || radius < 0.03f || !Normalize(fromJoint) || !Normalize(jointToTarget))
    {
        return;
    }
    float cosine = Dot(fromJoint, jointToTarget);
    cosine = cosine > 1.0f ? 1.0f : (cosine < -1.0f ? -1.0f : cosine);
    const float full = acosf(cosine);
    float angle = missing / radius; // the shoulder's arc toward the target
    angle = angle > MAXIMUM_SHRUG_RADIANS ? MAXIMUM_SHRUG_RADIANS : angle;
    angle = angle > full ? full : angle;
    if (angle < 0.01f)
    {
        return;
    }
    const float share = angle / full;
    float to[3];
    for (int axis = 0; axis < 3; ++axis)
    {
        to[axis] = fromJoint[axis] * (1.0f - share) + jointToTarget[axis] * share;
    }
    Normalize(to);
    float rotation[16];
    RotationBetween(fromJoint, to, rotation);
    _lastShrugDegrees[hand] = angle * 57.2958f;
    float joint[3];
    memcpy(joint, clavicle + 12, sizeof(joint));
    for (int part = 0; part < 4; ++part)
    {
        const float* bone = reinterpret_cast<const float*>(*slots[part]);
        float offset[3], position[3];
        for (int axis = 0; axis < 3; ++axis)
        {
            offset[axis] = bone[12 + axis] - joint[axis];
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            position[axis] = joint[axis] + offset[0] * rotation[axis] + offset[1] * rotation[4 + axis] +
                             offset[2] * rotation[8 + axis];
        }
        TurnBone(bone, rotation, position, _clavicleMatrices[hand][part]);
        *slots[part] = reinterpret_cast<uintptr_t>(_clavicleMatrices[hand][part]);
    }
}

/// Head IK: turns the upper body about bone_a_spine so the neck points at the neck's target (model space through the
/// placed draw matrix), at most MAXIMUM_SPINE_TURN_RADIANS; points the set's upper-body slots at our turned copies.
void BendSpine(uintptr_t boneSet, const float* matrix)
{
    uintptr_t* slots[UPPER_BODY_BONE_COUNT];
    for (unsigned index = 0; index < UPPER_BODY_BONE_COUNT; ++index)
    {
        slots[index] = FindSlot(boneSet, _boneHashes[UPPER_BODY_BONES[index]]);
        if (!slots[index] || !*slots[index])
        {
            return;
        }
    }
    const uintptr_t* pivotSlot = FindSlot(boneSet, _boneHashes[SPINE_PIVOT_BONE]);
    const uintptr_t* neckSlot = FindSlot(boneSet, _boneHashes[NECK_BONE]);
    if (!pivotSlot || !neckSlot)
    {
        return;
    }
    float pivot[3], neck[3];
    memcpy(pivot, reinterpret_cast<const float*>(*pivotSlot) + 12, sizeof(pivot));
    memcpy(neck, reinterpret_cast<const float*>(*neckSlot) + 12, sizeof(neck));
    // World to model: (p - t) times the transpose of the rotation, over the scale squared.
    const float scaleSquared = Dot(matrix, matrix);
    // Square the shoulders: the line from the right shoulder to the left (model +x is the body's left) turned about
    // the vertical to the left of the way between the body's forward (+z) and the head's.
    float square[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    _lastSquareDegrees = 0.0f;
    const uintptr_t* leftShoulder = FindSlot(boneSet, _boneHashes[ARM_BONES[1][0]]);
    const uintptr_t* rightShoulder = FindSlot(boneSet, _boneHashes[ARM_BONES[0][0]]);
    float head[16];
    if (leftShoulder && *leftShoulder && rightShoulder && *rightShoulder && Stereo::GetHeadWorld(head))
    {
        const float* left = reinterpret_cast<const float*>(*leftShoulder);
        const float* right = reinterpret_cast<const float*>(*rightShoulder);
        float line[3] = {left[12] - right[12], 0.0f, left[14] - right[14]};
        float forward[3]; // the head's forward (camera convention: row 2 back) in model space, level
        for (int axis = 0; axis < 3; ++axis)
        {
            forward[axis] = -Dot(head + 8, matrix + axis * 4) / scaleSquared;
        }
        forward[1] = 0.0f;
        float between[3] = {};
        if (Normalize(line) && Normalize(forward))
        {
            between[0] = forward[0];
            between[2] = forward[2] + 1.0f;
        }
        if (Normalize(between))
        {
            float wanted[3] = {between[2], 0.0f, -between[0]}; // the left of that way
            float cosine = Dot(line, wanted);
            cosine = cosine > 1.0f ? 1.0f : (cosine < -1.0f ? -1.0f : cosine);
            const float angle = acosf(cosine);
            if (angle > MAXIMUM_SQUARE_RADIANS)
            { // part of the way (close enough to a slerp here)
                const float share = MAXIMUM_SQUARE_RADIANS / angle;
                for (int axis = 0; axis < 3; ++axis)
                {
                    wanted[axis] = line[axis] * (1.0f - share) + wanted[axis] * share;
                }
                Normalize(wanted);
            }
            RotationBetween(line, wanted, square);
            _lastSquareDegrees = (angle > MAXIMUM_SQUARE_RADIANS ? MAXIMUM_SQUARE_RADIANS : angle) * 57.2958f *
                                 (line[2] * wanted[0] - line[0] * wanted[2] < 0 ? -1.0f : 1.0f);
        }
    }
    { // the neck as the squared upper body has it
        const float offset[3] = {neck[0] - pivot[0], neck[1] - pivot[1], neck[2] - pivot[2]};
        for (int axis = 0; axis < 3; ++axis)
        {
            neck[axis] =
                pivot[axis] + offset[0] * square[axis] + offset[1] * square[4 + axis] + offset[2] * square[8 + axis];
        }
    }
    float relative[3], target[3];
    for (int axis = 0; axis < 3; ++axis)
    {
        relative[axis] = _neckTargetWorld[axis] - matrix[12 + axis];
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        target[axis] = Dot(relative, matrix + axis * 4) / scaleSquared;
    }
    float from[3], to[3];
    for (int axis = 0; axis < 3; ++axis)
    {
        from[axis] = neck[axis] - pivot[axis];
        to[axis] = target[axis] - pivot[axis];
    }
    if (!Normalize(from) || !Normalize(to))
    {
        return;
    }
    float cosine = Dot(from, to);
    cosine = cosine > 1.0f ? 1.0f : (cosine < -1.0f ? -1.0f : cosine);
    const float angle = acosf(cosine);
    if (angle > MAXIMUM_SPINE_TURN_RADIANS)
    { // part of the way: the target direction pulled back toward the upright one (close enough to a slerp here)
        const float share = MAXIMUM_SPINE_TURN_RADIANS / angle;
        for (int axis = 0; axis < 3; ++axis)
        {
            to[axis] = from[axis] * (1.0f - share) + to[axis] * share;
        }
        Normalize(to);
    }
    _lastSpineDegrees = (angle > MAXIMUM_SPINE_TURN_RADIANS ? MAXIMUM_SPINE_TURN_RADIANS : angle) * 57.2958f;
    float lean[16], rotation[16];
    RotationBetween(from, to, lean);
    memcpy(rotation, lean, sizeof(rotation));
    for (int row = 0; row < 3; ++row)
    { // squared, then leaned (row vectors: square times lean)
        for (int column = 0; column < 3; ++column)
        {
            rotation[row * 4 + column] = square[row * 4] * lean[column] + square[row * 4 + 1] * lean[4 + column] +
                                         square[row * 4 + 2] * lean[8 + column];
        }
    }
    for (unsigned index = 0; index < UPPER_BODY_BONE_COUNT; ++index)
    {
        const float* bone = reinterpret_cast<const float*>(*slots[index]);
        float offsetFromPivot[3], position[3];
        for (int axis = 0; axis < 3; ++axis)
        {
            offsetFromPivot[axis] = bone[12 + axis] - pivot[axis];
        }
        for (int axis = 0; axis < 3; ++axis)
        { // row vector times the rotation
            position[axis] = pivot[axis] + offsetFromPivot[0] * rotation[axis] +
                             offsetFromPivot[1] * rotation[4 + axis] + offsetFromPivot[2] * rotation[8 + axis];
        }
        TurnBone(bone, rotation, position, _spineMatrices[index]);
        *slots[index] = reinterpret_cast<uintptr_t>(_spineMatrices[index]);
    }
}

/// Both arms to the controllers, for the draw at matrix (model to world, rigid).
void DriveArms(uintptr_t boneSet, const float* matrix)
{
    _isArmsDrivenThisRender = false;
    for (int hand = 0; hand < 2; ++hand)
    {
        float handWorld[16];
        if (!Aim::GetHandWorld(hand, handWorld))
        {
            continue;
        }
        // World to model: (p - t) times the transpose of the rotation, over the scale squared.
        const float scaleSquared = Dot(matrix, matrix);
        float relative[3], target[3];
        for (int axis = 0; axis < 3; ++axis)
        {
            relative[axis] = handWorld[12 + axis] - matrix[12 + axis];
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            target[axis] = Dot(relative, matrix + axis * 4) / scaleSquared;
        }
        // The controller's axes in model space (camera convention: x right, y up, -z forward), then the turn from a
        // level forward hold (right = model -x, up = +y, back = -z): turn = level^T x controller.
        float controller[9];
        for (int row = 0; row < 3; ++row)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                controller[row * 3 + axis] = Dot(handWorld + row * 4, matrix + axis * 4) / scaleSquared;
            }
            Normalize(controller + row * 3);
        }
        constexpr float LEVEL[9] = {-1, 0, 0, 0, 1, 0, 0, 0, -1};
        float turn[16] = {};
        turn[15] = 1.0f;
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                turn[row * 4 + column] = LEVEL[0 * 3 + row] * controller[0 * 3 + column] +
                                         LEVEL[1 * 3 + row] * controller[1 * 3 + column] +
                                         LEVEL[2 * 3 + row] * controller[2 * 3 + column];
            }
        }
        // The first-person forearm's elbow end in model space, for "arms 3".
        float elbowWorld[3], elbowTarget[3];
        const bool hasElbow = Aim::GetFirstPersonElbowWorld(hand, elbowWorld);
        if (hasElbow)
        {
            float elbowRelative[3];
            for (int axis = 0; axis < 3; ++axis)
            {
                elbowRelative[axis] = elbowWorld[axis] - matrix[12 + axis];
            }
            for (int axis = 0; axis < 3; ++axis)
            {
                elbowTarget[axis] = Dot(elbowRelative, matrix + axis * 4) / scaleSquared;
            }
        }
        ShrugClavicle(boneSet, hand, target);
        BendArm(boneSet, hand, target, turn, hasElbow ? elbowTarget : nullptr);
        _isArmsDrivenThisRender = _isArmsDrivenThisRender || hand == 0;
    }
}

// Hook of the model draw: inside the player's render with "place" on, draws at the matrix moved by this render's
// placement offset; everything else passes through untouched.
void __fastcall ModelDrawHook(
    uintptr_t model,
    void* edx,
    const float* matrix,
    uintptr_t boneSet,
    uint32_t colour,
    uint32_t flags,
    uint32_t override)
{
    // The hull of the player's gunner seat (GunnerSeat): left out under a cabin seat's cabin, or drawn with the stock
    // turret's bones collapsed, then restored.
    if (GunnerSeat::ShouldSkipHullDraw(boneSet))
    {
        return;
    }
    if (GunnerSeat::BeforeHullDraw(boneSet, matrix))
    {
        _originalModelDraw(model, edx, matrix, boneSet, colour, flags, override);
        GunnerSeat::AfterHullDraw();
        return;
    }
    if (_shouldHideFirstPersonArms &&
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - _gameBase == _firstPersonArmsDrawReturnRva &&
        _wereArmsDriven)
    {
        return; // the body's arms are the arms; the first-person weapon is drawn separately and stays
    }
    if (!_isInPlayerRender || (!_shouldPlace && !_shouldDriveArms) || !matrix)
    {
        _originalModelDraw(model, edx, matrix, boneSet, colour, flags, override);
        return;
    }
    if (_shouldPlace &&
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - _gameBase == _weaponModelDrawReturnRva)
    {
        return; // the first-person weapon is the weapon
    }
    const float* gameMatrix = matrix; // as the game gave it (before our placement)
    {
        FpGuard guard; // RAII: a clean floating-point state for our arithmetic, the game's restored at the end
        if (_shouldPlace && !_hasPlacement)
        {
            _hasPlacement = ComputePlacement(matrix, _placementOffset);
            if (_hasPlacement)
            {
                memcpy(_lastPlacementOffset, _placementOffset, sizeof(_lastPlacementOffset));
            }
        }
        if (_hasPlacement)
        {
            memcpy(_placedMatrix, matrix, sizeof(_placedMatrix));
            for (int element = 0; element < 12; ++element)
            {
                _placedMatrix[element] *= _placementScale;
            }
            for (int axis = 0; axis < 3; ++axis)
            {
                _placedMatrix[12 + axis] = _placementOrigin[axis] +
                                           (matrix[12 + axis] - _placementOrigin[axis]) * _placementScale +
                                           _placementOffset[axis];
            }
            matrix = _placedMatrix;
            ++_placedDrawCount;
        }
        if (_shouldBendSpine && _hasPlacement && _hasNeckTarget && !_hasSpine && boneSet && boneSet == _playerBoneSet)
        {
            _hasSpine = true; // the set is shared by this render's body draws: turn once, before the arms
            BendSpine(boneSet, matrix);
        }
        if (_shouldDriveArms && !_hasArms && boneSet && boneSet == _playerBoneSet)
        {
            _hasArms = true; // the set is shared by this render's body draws: bend once
            DriveArms(boneSet, matrix);
            _wereArmsDriven = _isArmsDrivenThisRender;
            ++_armDrawCount;
        }
        // PLAN 51 B: our body, posed by us, in place of the soldier's (every draw on the player's bone set: drawn once
        // a render)
        if (_shouldPlace && _hasPlacement && boneSet && boneSet == _playerBoneSet && PlayerBody::IsWanted())
        {
            if (!_hasOurBody)
            {
                _hasOurBody = true;
                PlayerBody::Inputs inputs = {};
                inputs.gameMatrix = gameMatrix;
                const unsigned legBones[2][2] = {{21, 22}, {17, 18}}; // foot, toe: right, left (BONE_NAMES)
                for (int side = 0; side < 2; ++side)
                {
                    const uintptr_t* foot = FindSlot(boneSet, _boneHashes[legBones[side][0]]);
                    const uintptr_t* toe = FindSlot(boneSet, _boneHashes[legBones[side][1]]);
                    inputs.gameFoot[side] = foot && *foot ? reinterpret_cast<const float*>(*foot) : nullptr;
                    inputs.gameToe[side] = toe && *toe ? reinterpret_cast<const float*>(*toe) : nullptr;
                }
                memcpy(inputs.anchorWorld, _anchorWorld, sizeof(inputs.anchorWorld));
                inputs.heightCentimeters = static_cast<float>(_playerHeightCentimeters);
                _hasOurBody = PlayerBody::Draw(D3dSurvey::PresentingDevice(), inputs);
            }
            if (_hasOurBody)
            {
                return;
            }
        }
    }
    _originalModelDraw(model, edx, matrix, boneSet, colour, flags, override);
}

/// The soldier classes the full body (PlayerBody: the Rebel soldier, all_inf_soldier's skin) is drawn for (idren2023
/// 2026-11-21: the Rebel soldier only, and only in its own skin). all_inf_soldierjungle has the same model and no
/// OverrideTexture; the desert and urban soldiers override the texture, the vanguard has its own model, the snow
/// soldier its own first-person arms; every other unit has no full body of ours yet (PLAN 51 B).
struct BodyClass
{
    const char* name;
    bool hasFullBody;
};
constexpr BodyClass BODY_CLASSES[] = {
    {"all_inf_soldier", true},
    {"all_inf_soldierjungle", true},
    {"all_inf_soldierdesert", false},
    {"all_inf_soldierurban", false},
    {"all_inf_soldiersnow", false},
    {"all_inf_vanguard", false},
};
/// EntitySoldierClass: its ODF name's hash (FNV-1a with each byte | 0x20, the game's 0x3008F0), at the same place as
/// in the vehicle classes (offsets.md "Vehicle class name"; for soldiers run 819).
constexpr uintptr_t OFFSET_soldierClassNameHash = 0x18;

/// Whether the player's soldier is one of the classes above that has the full body; logged when the class changes.
bool HasFullBodyClass(uintptr_t soldier)
{
    static uintptr_t lastClass = 0;
    static bool lastResult = false;
    if (!soldier || IsBadReadPtr(reinterpret_cast<void*>(soldier + OFFSET_soldierClass), 4))
    {
        return false;
    }
    const uintptr_t soldierClass = *reinterpret_cast<uintptr_t*>(soldier + OFFSET_soldierClass);
    if (!soldierClass || IsBadReadPtr(reinterpret_cast<void*>(soldierClass + OFFSET_soldierClassNameHash), 4))
    {
        return false;
    }
    if (soldierClass == lastClass)
    {
        return lastResult;
    }
    const unsigned hash = *reinterpret_cast<unsigned*>(soldierClass + OFFSET_soldierClassNameHash);
    const char* name = nullptr;
    bool result = false;
    for (const BodyClass& candidate : BODY_CLASSES)
    {
        unsigned candidateHash = 0x811C9DC5u;
        for (const char* character = candidate.name; *character; ++character)
        {
            candidateHash = (candidateHash ^ (static_cast<unsigned char>(*character) | 0x20u)) * 0x01000193u;
        }
        if (candidateHash == hash)
        {
            name = candidate.name;
            result = candidate.hasFullBody;
        }
    }
    lastClass = soldierClass;
    lastResult = result;
    Log::Info(
        "body",
        "soldier class %08X (name hash %08X: %s): %s",
        static_cast<unsigned>(soldierClass),
        hash,
        name ? name : "not a Rebel soldier",
        result ? "full body available" : "no full body of ours, the arms alone");
    return result;
}

} // namespace

bool InstallModelDrawHook()
{
    static bool hasTried = false, isOn = false;
    if (hasTried)
    {
        return isOn;
    }
    hasTried = true;
    if (!GameBuild::IsAvailable(GameBuild::Feature::FullBody))
    {
        Log::Warning("body", "model draw hook: not available on this game build");
        return false;
    }
    _gameBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    _firstPersonArmsDrawReturnRva = GameBuild::Rva(GameBuild::Address::FirstPersonArmsDrawReturn);
    _weaponModelDrawReturnRva = GameBuild::Rva(GameBuild::Address::WeaponModelDrawReturn);
    void* const modelDraw = reinterpret_cast<void*>(GameBuild::Absolute(GameBuild::Address::ModelDraw));
    MH_Initialize();
    MH_STATUS status = MH_CreateHook(
        modelDraw,
        reinterpret_cast<void*>(&ModelDrawHook),
        reinterpret_cast<void**>(&_originalModelDraw));
    if (status == MH_OK)
    {
        status = MH_EnableHook(modelDraw);
    }
    isOn = status == MH_OK;
    Log::Info("body", "model draw hook %s", isOn ? "on" : "failed");
    return isOn;
}

namespace
{

void Install()
{
    _isInstalled = true;
    if (!GameBuild::IsAvailable(GameBuild::Feature::FullBody))
    {
        _hasFailed = true;
        Log::Warning("body", "full body not available on this game build; not installed");
        return;
    }
    _gameBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    _soldierRenderRva = GameBuild::Rva(GameBuild::Address::SoldierRender);
    _soldierRenderEndRva = GameBuild::Rva(GameBuild::Address::SoldierRenderEnd);
    _firstPersonArmsDrawReturnRva = GameBuild::Rva(GameBuild::Address::FirstPersonArmsDrawReturn);
    _weaponModelDrawReturnRva = GameBuild::Rva(GameBuild::Address::WeaponModelDrawReturn);
    using HashNameFunction = unsigned(__cdecl*)(const char*);
    const auto hashName = reinterpret_cast<HashNameFunction>(GameBuild::Absolute(GameBuild::Address::HashName));
    for (unsigned bone = 0; bone < BONE_NAME_COUNT; ++bone)
    {
        _boneHashes[bone] = hashName ? hashName(BONE_NAMES[bone]) : 0;
    }
    uint8_t* const branch =
        reinterpret_cast<uint8_t*>(GameBuild::Absolute(GameBuild::Address::SoldierVisibilityBranch));
    DWORD oldProtection = 0;
    if (!memcmp(branch, VISIBILITY_BRANCH_GAME, 2) && VirtualProtect(branch, 2, PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        _visibilityBranch = branch; // stays writable: rewritten around every player render while "draw 1"
    }
    void* const render = reinterpret_cast<void*>(GameBuild::Absolute(GameBuild::Address::SoldierRender));
    void* const getBones = reinterpret_cast<void*>(GameBuild::Absolute(GameBuild::Address::GetBones));
    MH_Initialize();
    MH_STATUS status = MH_CreateHook(
        render,
        reinterpret_cast<void*>(&SoldierRenderHook),
        reinterpret_cast<void**>(&_originalSoldierRender));
    if (status == MH_OK)
    {
        status = MH_CreateHook(
            getBones,
            reinterpret_cast<void*>(&GetBonesHook),
            reinterpret_cast<void**>(&_originalGetBones));
    }
    if (status == MH_OK)
    {
        status = MH_EnableHook(render);
    }
    if (status == MH_OK && !InstallModelDrawHook())
    {
        status = MH_ERROR_NOT_CREATED;
    }
    if (status == MH_OK)
    {
        status = MH_EnableHook(getBones);
    }
    _hasFailed = status != MH_OK;
    Log::Info(
        "body",
        "hooks %s (soldier render, GetBones, model draw); visibility branch %s; head hash %08X, right hand %08X",
        status == MH_OK ? "on" : "failed",
        _visibilityBranch ? "patchable" : "not as expected",
        _boneHashes[HEAD_BONE],
        _boneHashes[RIGHT_HAND_BONE]);
}

} // namespace

void Configure(const char* arguments)
{
    int value = 0;
    int x = 0, y = 0, z = 0;
    if (!strncmp(arguments, "log", 3))
    {
        _isLogRequested = true;
    }
    else if (sscanf(arguments, "draw %d", &value) == 1)
    {
        _shouldForceDraw = value != 0;
        _isLogRequested = true;
    }
    else if (sscanf(arguments, "head %d", &value) == 1)
    {
        _shouldHideHead = value == 0;
        _isLogRequested = true;
    }
    else if (!strncmp(arguments, "place", 5))
    {
        // "place off", "place" (defaults), "place <back cm> <down cm> <height 0|1|2>"
        _shouldPlace = strncmp(arguments + 5, " off", 4) != 0;
        if (sscanf(arguments + 5, "%d %d %d", &x, &y, &z) == 3)
        {
            _placeBackCentimeters = x;
            _placeDownCentimeters = y;
            _placeHeightMode = z;
        }
        _isLogRequested = true;
    }
    else if (sscanf(arguments, "full %d", &value) == 1)
    {
        _isFullBodyWanted = value != 0; // as the VR Settings row, for a test without touching swbf-vr.ini
    }
    else if (sscanf(arguments, "arms %d", &value) == 1)
    {
        _shouldDriveArms = value != 0;
        _shouldHideFirstPersonArms = value == 1;
        _shouldCollapseForearms = value == 3;
        _isLogRequested = true;
    }
    else if (sscanf(arguments, "spine %d", &value) == 1)
    {
        _shouldBendSpine = value != 0;
        _hasAnchor = false;
        _isLogRequested = true;
    }
    else if (sscanf(arguments, "hand %d %d %d", &x, &y, &z) == 3)
    {
        _handOffsetCentimeters[0] = x;
        _handOffsetCentimeters[1] = y;
        _handOffsetCentimeters[2] = z;
        _isLogRequested = true;
    }
    Log::Info(
        "body",
        "body %s: draw %d, head %s, hand offset %d %d %d cm, place %s (back %d, down %d cm, height mode %d)",
        arguments,
        _shouldForceDraw ? 1 : 0,
        _shouldHideHead ? "hidden" : "shown",
        _handOffsetCentimeters[0],
        _handOffsetCentimeters[1],
        _handOffsetCentimeters[2],
        _shouldPlace ? "on" : "off",
        _placeBackCentimeters,
        _placeDownCentimeters,
        _placeHeightMode);
}

void SetFullBody(bool isOn)
{
    _isFullBodyWanted = isOn;
}

void SetPlayerHeight(int centimeters)
{
    _playerHeightCentimeters = centimeters < 120 ? 120 : (centimeters > 230 ? 230 : centimeters);
}

void OnPresent()
{
    float characterYaw = 0.0f;
    const bool shouldBeActive = _isFullBodyWanted && !_hasFailed && Xr::IsRunning() &&
                                Stereo::GetCharacterYaw(&characterYaw) && // on foot with controllers in VR
                                HasFullBodyClass(EngineSurvey::Resolve("soldier"));
    if (shouldBeActive != _isFullBodyActive)
    {
        // The diagnostic switches, as "body draw 1", "head 0", "place 10 12 2", "arms 1" would set them.
        _isFullBodyActive = shouldBeActive;
        _shouldForceDraw = shouldBeActive;
        _shouldHideHead = shouldBeActive;
        _placeBackCentimeters = 10;
        _placeDownCentimeters = 12;
        _placeHeightMode = 2;
        _shouldPlace = shouldBeActive;
        _shouldDriveArms = shouldBeActive;
        _shouldHideFirstPersonArms = false; // b206: the first-person arms are the forearms and hands ("arms 3")
        _shouldCollapseForearms = shouldBeActive;
        _shouldBendSpine = shouldBeActive;
        _hasAnchor = false;
        _wereArmsDriven = false;
        Log::Info("body", "full body %s", shouldBeActive ? "on" : "off");
    }
    {
        float riderEye[3];
        const bool shouldHide = !_hasFailed && Xr::IsRunning() &&
                                (GunnerSeat::GetForcedEye(riderEye) || Handlebars::GetMountEye(riderEye));
        if (shouldHide)
        {
            memcpy(_riderEye, riderEye, sizeof(_riderEye));
            // where the seated soldier really is, in the hull's model space (the eye's place is a guess per seat)
            float hull[16];
            if (++_hiddenFrames % 180 == 90 && _nearestSoldierDistance < 1e8f && Vehicle::GetHullMatrix(hull))
            {
                float eyeLocal[3], soldierLocal[3];
                for (int axis = 0; axis < 3; ++axis)
                {
                    const float* row = hull + axis * 4;
                    const float size = row[0] * row[0] + row[1] * row[1] + row[2] * row[2];
                    eyeLocal[axis] = ((_riderEye[0] - hull[12]) * row[0] + (_riderEye[1] - hull[13]) * row[1] +
                                      (_riderEye[2] - hull[14]) * row[2]) / size;
                    soldierLocal[axis] = ((_nearestSoldier[0] - hull[12]) * row[0] +
                                          (_nearestSoldier[1] - hull[13]) * row[1] +
                                          (_nearestSoldier[2] - hull[14]) * row[2]) / size;
                }
                Log::Info("body", "rider: eye in hull %.2f %.2f %.2f, nearest soldier %.2f %.2f %.2f (%.2f m level)",
                          static_cast<double>(eyeLocal[0]), static_cast<double>(eyeLocal[1]),
                          static_cast<double>(eyeLocal[2]), static_cast<double>(soldierLocal[0]),
                          static_cast<double>(soldierLocal[1]), static_cast<double>(soldierLocal[2]),
                          static_cast<double>(sqrtf(_nearestSoldierDistance)));
                _nearestSoldierDistance = 1e9f;
            }
        }
        if (shouldHide != _shouldHidePlayer)
        {
            if (!shouldHide)
            {
                Log::Info("body", "while hidden: %u other soldier renders, %u left out", _hookCallsWhileHidden,
                          _hiddenRenderCount);
            }
            _hookCallsWhileHidden = 0;
            _hiddenRenderCount = 0;
            Log::Info("body", "player's soldier %s", shouldHide ? "hidden (rider's eye view)" : "drawn again");
        }
        _shouldHidePlayer = shouldHide;
    }
    const bool isOnFoot = Vehicle::CurrentSeat() == Vehicle::Seat::OnFoot;
    if (isOnFoot)
    {
        _onFootSoldier = EngineSurvey::Resolve("soldier");
    }
    if ((!_isLogRequested && !_isFullBodyActive && !_shouldHidePlayer) || _hasFailed)
    {
        return;
    }
    _playerSoldier = isOnFoot ? _onFootSoldier : (_shouldHidePlayer ? _onFootSoldier : EngineSurvey::Resolve("soldier"));
    if (!_isInstalled)
    {
        Install();
        return;
    }
    if (!_isLogRequested)
    {
        return;
    }
    static DWORD summaryTick = 0;
    const DWORD now = GetTickCount();
    if (now - summaryTick < 1000 || _summaryCount >= SUMMARY_LINE_LIMIT)
    {
        return;
    }
    summaryTick = now;
    ++_summaryCount;
    char bones[400] = "";
    if (_hasTranslations)
    {
        int length = 0;
        for (unsigned entry = 0; entry < sizeof(SUMMARY_BONES) / sizeof(SUMMARY_BONES[0]); ++entry)
        {
            length += snprintf(
                bones + length,
                sizeof(bones) - length,
                " %s (%.2f %.2f %.2f)",
                BONE_NAMES[SUMMARY_BONES[entry]] + (SUMMARY_BONES[entry] == 23 ? 0 : 5),
                static_cast<double>(_lastTranslations[entry][0]),
                static_cast<double>(_lastTranslations[entry][1]),
                static_cast<double>(_lastTranslations[entry][2]));
        }
    }
    const float* position =
        _playerSoldier ? reinterpret_cast<const float*>(_playerSoldier + OFFSET_soldierBodyMatrix + 0x30) : nullptr;
    Log::Info(
        "body",
        "player %08X at (%.2f %.2f %.2f): renders %u (%u in eye passes, %u in first-person passes), flag argument "
        "%08X, alpha argument %.2f, skip flag %u, GetBones "
        "%u (%u bones); placed draws %u, offset (%.2f %.2f %.2f), neck %.2f below the eyes, scale %.3f; arms bent %u, "
        "reach right %.2f left %.2f; shoulders squared %.0f deg, collarbones right %.0f left %.0f deg; spine turned %.0f "
        "deg;%s",
        static_cast<unsigned>(_playerSoldier),
        static_cast<double>(position ? position[0] : 0.0f),
        static_cast<double>(position ? position[1] : 0.0f),
        static_cast<double>(position ? position[2] : 0.0f),
        _playerRenderCount,
        _eyePassRenderCount,
        _firstPersonPassRenderCount,
        _lastFlagArgument,
        static_cast<double>(_lastAlphaArgument),
        _lastSkipFlag,
        _playerBonesCount,
        _lastBoneCount,
        _placedDrawCount,
        static_cast<double>(_lastPlacementOffset[0]),
        static_cast<double>(_lastPlacementOffset[1]),
        static_cast<double>(_lastPlacementOffset[2]),
        static_cast<double>(_lastNeckBelowEye),
        static_cast<double>(_placementScale),
        _armDrawCount,
        static_cast<double>(_lastArmReach[0]),
        static_cast<double>(_lastArmReach[1]),
        static_cast<double>(_lastSquareDegrees),
        static_cast<double>(_lastShrugDegrees[0]),
        static_cast<double>(_lastShrugDegrees[1]),
        static_cast<double>(_lastSpineDegrees),
        bones);
    _armDrawCount = 0;
    _placedDrawCount = 0;
    _playerRenderCount = 0;
    _eyePassRenderCount = 0;
    _firstPersonPassRenderCount = 0;
    _playerBonesCount = 0;
    _hasTranslations = false;
}

} // namespace Body

} // namespace SwbfVr
