// Zoom.cpp: the game's zoom kept off the main camera in VR (docs/ZOOM.md section 2 and 5, offsets.md "Zoom").
//
// How the game zooms (read from the image, ZOOM.md section 2): the zoom button steps Weapon+0xC0 through the weapon
// class's levels. Every Update the chase camera asks the soldier for its zoom (Trackable slot 2, GetZoom 0x118330:
// Weapon+0xC0 while the soldier is zoomed, else 1), ramps the camera's own factor (RedCamera+0xA0) toward it, and
// RedCamera::SetZoom divides the camera's tangents by it. The ramp reads the current factor back from the camera, so
// SetZoom itself cannot be held at 1 (the ramp would never settle); GetZoom can: answering 1 there leaves the camera
// unzoomed as if the player had not zoomed, while the soldier and the weapon stay zoomed (turn rate, the zoomed
// flag, the first-person view of ZoomFirstPerson). In the headset the eye images then keep the headset's field of
// view (a zoomed camera narrowed them and the runtime showed a small box: headset run 10).

#include "Zoom.h"

#include "Aim.h"
#include "FpGuard.h"
#include "GameBuild.h"
#include "Log.h"

#include <windows.h>

#include <MinHook.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace SwbfVr
{

namespace Zoom
{

namespace
{

// GetZoom's `this` is the soldier's Trackable part, 0x44C into the soldier (offsets.md "Zoom").
/// Trackable: the soldier's zoomed flag (soldier+0x26C).
constexpr intptr_t OFFSET_trackableIsZoomed = -0x1E0;
/// Trackable: weapon slots (soldier+0xA30) and the index of the one in hand (soldier+0xA5C).
constexpr intptr_t OFFSET_trackableWeapons = 0x5E4;
constexpr intptr_t OFFSET_trackableWeaponIndex = 0x610;
/// Weapon: its WeaponClass; its current zoom factor and turn divisor.
constexpr uintptr_t OFFSET_weaponClass = 0x64;
constexpr uintptr_t OFFSET_weaponZoomFactor = 0xC0;
constexpr uintptr_t OFFSET_weaponTurnDivisor = 0xC4;
/// WeaponClass (parser 0x243500): ReticuleTexture and ScopeTexture name hashes, ZoomMin, ZoomMax, ZoomRate,
/// ZoomFirstPerson (byte), SniperScope (byte).
constexpr uintptr_t OFFSET_classReticleHash = 0x48;
constexpr uintptr_t OFFSET_classScopeHash = 0x4C;
constexpr uintptr_t OFFSET_classZoomMinimum = 0x6C;
constexpr uintptr_t OFFSET_classZoomMaximum = 0x70;
constexpr uintptr_t OFFSET_classZoomRate = 0x74;
constexpr uintptr_t OFFSET_classZoomFirstPerson = 0x80;
constexpr uintptr_t OFFSET_classSniperScope = 0xD4;
/// The game's texture object (0x3C bytes, constructor 0x282B80, filled from GetLevelDesc(0) at 0x2835E0): height,
/// width, kind (1 = 2D, 2 = cube, 3 = volume) and its IDirect3DBaseTexture9*. offsets.md "Zoom".
constexpr uintptr_t OFFSET_textureHeight = 0x04;
constexpr uintptr_t OFFSET_textureWidth = 0x08;
constexpr uintptr_t OFFSET_textureKind = 0x18;
constexpr uintptr_t OFFSET_textureD3d = 0x1C;
constexpr int TEXTURE_KIND_2D = 1;

/// The patch site in the soldier's first-person camera (slot 1, 0x118030): `jne` (0F 85 rel32, 6 bytes, at
/// 0x1181A5) that skips the per-frame update of the first-person arms and weapon (0x1316B0) while the soldier is
/// zoomed. The game keeps drawing them (FirstPersonRenderable::Render, 2 calls per frame) but with no update they
/// are not seen (run102 captures; run103 probes: update 1 call per frame unzoomed, none zoomed). Six NOPs keep the
/// update going. (The similar check in slot 5, 0x118598, is not on this path: no call came from it in run103.)
/// The draw itself (0x1306E0, called by FirstPersonRenderable::Render) has its own `jne` of the same shape at
/// 0x13072C after 0x1301D0, which reads Controllable+0x1C = soldier+0x26C: NOPed too. Before it, the draw asks
/// 0x12FFC0(view) whether the first person is hidden, which is true while the soldier's camera slot (soldier+0xA64,
/// 0x1101F0) is 5 or 8; zooming sets 5 (run106). That call (0x130713) is redirected to IsHiddenHook, which answers
/// "not hidden" while VR keeps the zoom off the main camera and passes the game's answer through otherwise.
constexpr int CALL_LENGTH = 5;
constexpr uint8_t CALL_OPCODE = 0xE8;
constexpr int HIDE_JUMP_LENGTH = 6;
constexpr uint8_t HIDE_JUMP_OPCODE[2] = {0x0F, 0x85};
constexpr uint8_t NO_OPERATION_BYTES[HIDE_JUMP_LENGTH] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};

/// First-person weapon model (WeaponClass +0x40) and its node table (model +0xC0: +0x0C name hashes, +0x10 count);
/// the same reads as Aim's hp_fire lookup.
constexpr uintptr_t OFFSET_classFirstPersonModel = 0x40;
constexpr uintptr_t OFFSET_modelNodeTable = 0xC0;

/// A stock first-person weapon model with a lens at the eyepiece: its name, the lens radius and the lens centres in
/// model space (metres, z forward), from the bounds of the lens geometry in the mod tools' msh files (docs/ZOOM.md
/// section 2, table 5). The bowcasters have two scopes, both drawn with the one scope picture.
struct LensModel
{
    const char* name;
    float radiusMeters;
    int lensCount;
    float centres[2][3];
    int outlineCount = 0;                  // 0: a round glass of the radius
    const float (*outline)[2] = nullptr; // else its corners in model x, y (metres), round the first centre
    float tiltDegrees = 0;               // the screen's tilt in the picture's frame (Lens::tiltDegrees)
    bool isScreen = false;               // a display seen by both eyes (Lens::isScreen)
};

/// The CIS sniper rifle's screen (node "scroll", cis_1st_cockpit_fx): a flat elongated octagon 7.2 by 2.6 cm facing
/// the eye at z 0.0235, right of the barrel.
constexpr float CIS_SNIPER_SCREEN[][2] = {
    {0.0892f, 0.0985f},
    {0.0940f, 0.1059f},
    {0.1112f, 0.1113f},
    {0.1387f, 0.1113f},
    {0.1564f, 0.1059f},
    {0.1613f, 0.0985f},
    {0.1564f, 0.0911f},
    {0.1389f, 0.0857f},
    {0.1114f, 0.0857f},
    {0.0940f, 0.0911f}};
/// The CIS launcher's screen: the dark face of its sight box (z -0.2009, behind the box's rim at -0.2093), about 7.4
/// by 3.1 cm, tilted 10.3 degrees (its long edges, seen from behind), two corners cut; a red line (node "lighting") lies on it at z -0.2025.
constexpr float CIS_LAUNCHER_SCREEN[][2] = {
    {0.1296f, 0.1068f},
    {0.1946f, 0.0950f},
    {0.2031f, 0.0987f},
    {0.2066f, 0.1181f},
    {0.2002f, 0.1260f},
    {0.1352f, 0.1377f}};
/// The Republic launcher's eyepiece window: the opening of its box's rear rim (z -0.2517), 4.7 by 3.9 cm, two corners
/// cut; the green glass (node "cube") sits in it at z -0.2466.
constexpr float REP_LAUNCHER_WINDOW[][2] = {
    {0.1647f, 0.1291f},
    {0.2031f, 0.1276f},
    {0.2111f, 0.1337f},
    {0.2121f, 0.1592f},
    {0.2046f, 0.1659f},
    {0.1662f, 0.1675f}};

/// The game keeps only the root node (named like the model) and hardpoints of a model at run time (run110: the sniper
/// rifle has 2 nodes, the lens is merged away), so the lens is recognised by the model's name. Every other model (CIS
/// and Republic rifles, Republic pistol, mod weapons) gets the whole view magnified.
constexpr LensModel LENS_MODELS[] = {
    {"all_1st_weap_inf_bowcaster", 0.0115f, 2, {{0.066f, 0.129f, -0.152f}, {-0.068f, 0.129f, -0.152f}}},
    {"all_1st_weap_inf_launcher", 0.018f, 1, {{0.198f, 0.104f, -0.205f}}},
    {"all_1st_weap_inf_pistol", 0.019f, 1, {{-0.006f, 0.110f, -0.160f}}},
    {"all_1st_weap_inf_rifle", 0.025f, 1, {{0.001f, 0.127f, -0.133f}}},
    {"all_1st_weap_inf_sniperrifle", 0.017f, 1, {{0.0f, 0.134f, -0.121f}}},
    // the screens (idren2023 2026-11-22: "little more oval/rectangular screens"): the picture just in front of each, over
    // the launcher's red line; the msh's root node of the launcher is cis_1st_weap_launcher (which name the game keeps
    // is not known: both listed)
    {"cis_1st_weap_inf_launcher", 0.0390f, 1, {{0.1686f, 0.1153f, -0.2030f}}, 6, CIS_LAUNCHER_SCREEN, 10.3f, true},
    {"cis_1st_weap_launcher", 0.0390f, 1, {{0.1686f, 0.1153f, -0.2030f}}, 6, CIS_LAUNCHER_SCREEN, 10.3f, true},
    {"cis_1st_weap_inf_pistol", 0.013f, 1, {{-0.005f, 0.095f, -0.131f}}},
    {"cis_1st_weap_inf_sniperrifle", 0.0361f, 1, {{0.1252f, 0.0985f, 0.0225f}}, 10, CIS_SNIPER_SCREEN, 0.0f, true},
    // the eyepiece glass: the flat 16-sided disc all_launcher_eyepeice1 (radius 2.78-2.82 cm; idren2023 2026-11-22: the
    // shock trooper's launcher used the projected picture instead of its glass)
    {"imp_1st_weap_inf_launcher", 0.027f, 1, {{0.220f, 0.103f, -0.225f}}},
    {"imp_1st_weap_inf_pistol", 0.016f, 1, {{-0.037f, 0.039f, -0.135f}}},
    {"imp_1st_weap_inf_rifle", 0.019f, 1, {{0.0f, 0.131f, -0.143f}}},
    {"imp_1st_weap_inf_sniperrifle", 0.017f, 1, {{0.0f, 0.154f, -0.122f}}},
    // the eyepiece window, over its green glass (run 843: it had the projected picture); the msh's root node is
    // main_lambert (no other stock first-person model has it)
    {"main_lambert", 0.0237f, 1, {{0.1884f, 0.1476f, -0.2475f}}, 6, REP_LAUNCHER_WINDOW},
    {"rep_1st_weap_inf_launcher", 0.0237f, 1, {{0.1884f, 0.1476f, -0.2475f}}, 6, REP_LAUNCHER_WINDOW},
    {"rep_1st_weap_inf_sniperrifle", 0.017f, 1, {{0.0f, 0.154f, -0.122f}}},
    {"wok_1st_weap_inf_bowcaster", 0.0115f, 2, {{0.066f, 0.129f, -0.150f}, {-0.067f, 0.129f, -0.150f}}}};

/// Weapons without a lens get a projected picture where the rifles have their eyepiece, larger than a glass, instead
/// of the whole view magnified (headset run 12: awful). PLAN 26, b148; the size was right in headset run 14. Lens
/// weapons' pictures fit their glass exactly (b206, headset run 14: 1.5 times the glass was too big).
constexpr float LENS_PROJECTION_SCALE = 1.5f;
// b176: 2 cm lower and a sixth smaller (headset run 13: a little high and a little too big).
constexpr LensModel PROJECTED_LENS = {"(projected)", 0.0185f, 1, {{0.0f, 0.11f, -0.13f}}};
/// The droid's picture floats this many picture radii above its barrels.
constexpr float PROJECTED_EMITTER_BELOW = 1.2f;
/// The holoscope's emitter (b177, idren2023 after run185: projected forward, away from the eye) lies this far behind the
/// picture toward the eye and this many picture radii under its centre, so the beam opens forward onto it.
constexpr float PROJECTOR_BEHIND_METERS = 0.06f;
constexpr float PROJECTOR_BELOW = 0.3f;
/// The super battle droid's picture: this far back from its barrels' emitter toward the eye, this big (b158).
constexpr float DROID_PICTURE_BACK_METERS = 0.04f;    // b160: from the barrels' middle (the picture stays put)
constexpr float DROID_PICTURE_RADIUS_METERS = 0.034f; // b176: a sixth smaller (headset run 13)

/// Zoom counts as on while GetZoom asked for more than this within ZOOM_HOLD_MILLISECONDS (it is not asked in a
/// vehicle or while dead, and the value must not stick then).
constexpr float ZOOMED_FACTOR_MINIMUM = 1.001f;
constexpr DWORD ZOOM_HOLD_MILLISECONDS = 200;

/// GetZoom (soldier Trackable slot 2): void __thiscall GetZoom(float* zoom). Called by the chase camera's Update on
/// the main thread.
using GetZoomFunction = void(__thiscall*)(void* self, float* zoom);

GetZoomFunction _originalGetZoom = nullptr;
bool _isInstalled = false;
constexpr int HIDE_JUMP_COUNT = 2;                              // update skip, draw skip
uint8_t* _hideJump[HIDE_JUMP_COUNT] = {};                       // the patch sites, null when the feature is off
uint8_t _hideJumpBytes[HIDE_JUMP_COUNT][HIDE_JUMP_LENGTH] = {}; // the game's own bytes there
bool _isHidePatched = false;

using IsHiddenFunction = uint8_t(__cdecl*)(int view);
IsHiddenFunction _originalIsHidden = nullptr;

// Replaces the first-person draw's call of 0x12FFC0 (cdecl, returns a bool in AL). Render thread.
uint8_t __cdecl IsHiddenHook(int view)
{
    const uint8_t isHidden = _originalIsHidden(view);
    return isHidden && _isHidePatched && IsZoomed() ? 0 : isHidden;
}

float _targetFactor = 1.0f;  // what the game wants (1 = not zoomed)
DWORD _targetTick = 0;       // when GetZoom last asked for more than 1
WeaponZoom _weaponZoom = {}; // the weapon in hand, read while zoomed
bool _hasWeaponZoom = false;
/// The reticle and scope overlay hashes last logged.
unsigned _loggedReticleHash = 0;
unsigned _loggedScopeHash = 0;
const LensModel* _lensModel = nullptr; // the weapon in hand in LENS_MODELS, null without a known lens
bool _shouldUseLens = true; // scope picture on the weapon's lens (B2), else in the window (swbf-vr.ini: scope_view)
float _windowHalfTan = 0.57735027f; // tan(30 degrees): a 60 degree window (idren2023: 20 far too narrow, 60 right)
float _scopeFactor = 1.0f;          // the scope view's ramped factor (StepFactor)
DWORD _scopeFactorTick = 0;
const void* _loggedClass = nullptr; // last WeaponClass logged
float _loggedFactor = 1.0f;         // last level logged

/// A pointer to a game C++ object: its first word points to a vtable in the exe's read-only data.
bool IsGameObject(uintptr_t pointer)
{
    return pointer >= 0x10000 && !IsBadReadPtr(reinterpret_cast<const void*>(pointer), 4) &&
           GameBuild::IsInReadOnlyData(*reinterpret_cast<const uintptr_t*>(pointer));
}

template <typename T> T Read(uintptr_t base, uintptr_t offset)
{
    T value;
    memcpy(&value, reinterpret_cast<const void*>(base + offset), sizeof(T));
    return value;
}

/// The node name hashes of a weapon class's first-person model (the root node carries the model's name).
/// @return the number of hashes (0 if they cannot be read); *hashes is set to the first.

unsigned ModelNodes(uintptr_t model, uintptr_t* hashes); // below

unsigned ModelNodeHashes(uintptr_t weaponClass, uintptr_t* hashes)
{
    // Headset run 16 crash: in a vehicle seat the soldier chain yields a garbage class pointer (0xFF251E14).
    if (!GameBuild::IsAvailable(GameBuild::Feature::Muzzle) || weaponClass < 0x10000 ||
        IsBadReadPtr(reinterpret_cast<const void*>(weaponClass + OFFSET_classFirstPersonModel), 4))
    {
        return 0;
    }
    return ModelNodes(Read<uintptr_t>(weaponClass, OFFSET_classFirstPersonModel), hashes);
}

// A runtime model's node name hashes: their array and count (0 when it cannot be read).
unsigned ModelNodes(uintptr_t model, uintptr_t* hashes)
{
    if (model < 0x10000 || IsBadReadPtr(reinterpret_cast<const void*>(model), OFFSET_modelNodeTable + 4))
    {
        return 0;
    }
    const uintptr_t table = Read<uintptr_t>(model, OFFSET_modelNodeTable);
    if (table < 0x10000 || IsBadReadPtr(reinterpret_cast<const void*>(table), 0x14))
    {
        return 0;
    }
    *hashes = Read<uintptr_t>(table, 0x0C);
    const unsigned count = Read<unsigned>(table, 0x10);
    if (!count || count >= 1024 || IsBadReadPtr(reinterpret_cast<const void*>(*hashes), count * 4))
    {
        return 0;
    }
    return count;
}

/// The weapon class's first-person model in LENS_MODELS (its root node carries the name), or nullptr if it is not
/// there or cannot be read.
const LensModel* FindLensModel(uintptr_t weaponClass)
{
    uintptr_t hashes = 0;
    const unsigned count = ModelNodeHashes(weaponClass, &hashes);
    if (!count)
    {
        return nullptr;
    }
    using HashNameFunction = unsigned(__cdecl*)(const char*);
    const auto hashName = reinterpret_cast<HashNameFunction>(GameBuild::Absolute(GameBuild::Address::HashName));
    // The root node carries the model's name, or the name with "_mesh" (the Wookiee's bowcaster, run115).
    for (const LensModel& lensModel : LENS_MODELS)
    {
        char meshName[64];
        snprintf(meshName, sizeof(meshName), "%s_mesh", lensModel.name);
        const unsigned nameHashes[2] = {hashName(lensModel.name), hashName(meshName)};
        for (unsigned node = 0; node < count; ++node)
        {
            const unsigned nodeHash = Read<unsigned>(hashes, 4 * node);
            if (nodeHash == nameHashes[0] || nodeHash == nameHashes[1])
            {
                return &lensModel;
            }
        }
    }
    // Not on the list: the node hashes, to be named offline (tools/texnames.py names any hash in a log line).
    char line[160] = "";
    for (unsigned node = 0, length = 0; node < count && node < 8 && length < sizeof(line) - 12; ++node)
    {
        length += static_cast<unsigned>(
            snprintf(line + length, sizeof(line) - length, " 0x%08x", Read<unsigned>(hashes, 4 * node)));
    }
    Log::Info("zoom", "first-person model has no lens name; nodes:%s", line);
    return nullptr;
}

/// Reads the weapon in hand and its class through the Trackable; logs once per weapon class and per level.
void ReadWeapon(uintptr_t trackable, float factor)
{
    _hasWeaponZoom = false;
    const int index = Read<int>(trackable, OFFSET_trackableWeaponIndex);
    if (index < 0 || index > 8)
    {
        return;
    }
    const uintptr_t weapon = Read<uintptr_t>(trackable, OFFSET_trackableWeapons + 4 * index);
    if (!IsGameObject(weapon))
    {
        return;
    }
    const uintptr_t weaponClass = Read<uintptr_t>(weapon, OFFSET_weaponClass);
    if (!IsGameObject(weaponClass))
    {
        return;
    }
    _weaponZoom.minimumFactor = Read<float>(weaponClass, OFFSET_classZoomMinimum);
    _weaponZoom.maximumFactor = Read<float>(weaponClass, OFFSET_classZoomMaximum);
    _weaponZoom.rate = Read<float>(weaponClass, OFFSET_classZoomRate);
    _weaponZoom.isFirstPersonZoom = Read<uint8_t>(weaponClass, OFFSET_classZoomFirstPerson) != 0;
    _weaponZoom.isSniperScope = Read<uint8_t>(weaponClass, OFFSET_classSniperScope) != 0;
    _weaponZoom.reticleHash = Read<unsigned>(weaponClass, OFFSET_classReticleHash);
    _weaponZoom.scopeHash = Read<unsigned>(weaponClass, OFFSET_classScopeHash);
    const bool isNewClass = reinterpret_cast<const void*>(weaponClass) != _loggedClass;
    if (isNewClass)
    {
        _lensModel = FindLensModel(weaponClass);
        _weaponZoom.hasLens = _lensModel != nullptr;
    }
    _hasWeaponZoom = true;
    const bool isNewLevel = factor > _loggedFactor * 1.01f || factor < _loggedFactor * 0.99f;
    if (isNewClass)
    {
        _loggedClass = reinterpret_cast<const void*>(weaponClass);
        Log::Info(
            "zoom",
            "weapon class %p: ZoomMin %.2f ZoomMax %.2f ZoomRate %.2f ZoomFirstPerson %d SniperScope %d reticle "
            "hash 0x%08x scope hash 0x%08x, lens model %s",
            reinterpret_cast<const void*>(weaponClass),
            static_cast<double>(_weaponZoom.minimumFactor),
            static_cast<double>(_weaponZoom.maximumFactor),
            static_cast<double>(_weaponZoom.rate),
            _weaponZoom.isFirstPersonZoom ? 1 : 0,
            _weaponZoom.isSniperScope ? 1 : 0,
            _weaponZoom.reticleHash,
            _weaponZoom.scopeHash,
            _lensModel ? _lensModel->name : "no lens: whole view magnified");
    }
    if (isNewClass || isNewLevel)
    {
        _loggedFactor = factor;
        Log::Info(
            "zoom",
            "level: factor %.2f (Weapon+0xC0 %.2f, turn divisor %.2f)",
            static_cast<double>(factor),
            static_cast<double>(Read<float>(weapon, OFFSET_weaponZoomFactor)),
            static_cast<double>(Read<float>(weapon, OFFSET_weaponTurnDivisor)));
    }
}

// Hook of the soldier's GetZoom (thiscall: `self` arrives in ECX, the unused `edx` parameter takes EDX). The game's
// own function runs first (it also remembers the value in the soldier, +0x640 of the Trackable); in VR the answer is
// then replaced by 1 and the real one kept.
void __fastcall GetZoomHook(void* self, void* /*edx*/, float* zoom)
{
    _originalGetZoom(self, zoom);
    if (!_isHidePatched) // the patch is in exactly while VR runs (BeforeFrame)
    {
        return;
    }
    FpGuard fp; // our float maths must not disturb the game's floating-point state
    const float factor = *zoom;
    const bool isZoomed = factor > ZOOMED_FACTOR_MINIMUM;
    const bool wasZoomed = IsZoomed();
    if (isZoomed)
    {
        _targetFactor = factor;
        _targetTick = GetTickCount();
        ReadWeapon(reinterpret_cast<uintptr_t>(self), factor);
        *zoom = 1.0f;
    }
    else
    {
        _targetFactor = 1.0f;
        _targetTick = 0;
    }
    if (isZoomed != wasZoomed)
    {
        const bool isSoldierZoomed = Read<uint8_t>(reinterpret_cast<uintptr_t>(self), OFFSET_trackableIsZoomed) != 0;
        Log::Info(
            "zoom",
            "%s (factor %.2f, soldier zoomed flag %d); main camera kept at 1",
            isZoomed ? "on" : "off",
            static_cast<double>(factor),
            isSoldierZoomed ? 1 : 0);
        if (!isZoomed)
        {
            _loggedFactor = 1.0f;
        }
    }
}

bool WriteCode(uint8_t* target, const uint8_t* bytes, size_t length = HIDE_JUMP_LENGTH)
{
    DWORD oldProtection = 0;
    if (!VirtualProtect(target, length, PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        return false;
    }
    memcpy(target, bytes, length);
    VirtualProtect(target, length, oldProtection, &oldProtection);
    FlushInstructionCache(GetCurrentProcess(), target, length);
    return true;
}

} // namespace

namespace
{

/// A texture of the weapon in hand while zoomed, looked up in the game's texture table (TextureByHash); logs once per
/// hash.
void* LookupTexture(unsigned hash, const char* what, unsigned& loggedHash, unsigned& width, unsigned& height)
{
    width = height = 0;
    if (!IsZoomed() || !_hasWeaponZoom || hash == 0 || !GameBuild::IsAvailable(GameBuild::Feature::Zoom))
    {
        return nullptr;
    }
    using TextureByHashFunction = uintptr_t(__cdecl*)(unsigned);
    const uintptr_t texture =
        reinterpret_cast<TextureByHashFunction>(GameBuild::Absolute(GameBuild::Address::TextureByHash))(hash);
    const bool isTwoDimensional = texture != 0 && Read<int>(texture, OFFSET_textureKind) == TEXTURE_KIND_2D;
    void* const d3dTexture = isTwoDimensional ? Read<void*>(texture, OFFSET_textureD3d) : nullptr;
    if (d3dTexture)
    {
        width = Read<unsigned>(texture, OFFSET_textureWidth);
        height = Read<unsigned>(texture, OFFSET_textureHeight);
    }
    if (hash != loggedHash)
    {
        loggedHash = hash;
        Log::Info(
            "zoom",
            "%s texture 0x%08x: %s %ux%u",
            what,
            hash,
            d3dTexture ? "found" : (texture ? "not 2D" : "not loaded"),
            width,
            height);
    }
    return d3dTexture;
}

} // namespace

void Install()
{
    if (_isInstalled)
    {
        return;
    }
    _isInstalled = true;
    if (!GameBuild::IsAvailable(GameBuild::Feature::Zoom))
    {
        Log::Warning(
            "zoom",
            "not installed (%s is off for this game build): zooming narrows the headset's view",
            GameBuild::FeatureName(GameBuild::Feature::Zoom));
        return;
    }
    const GameBuild::Address sites[HIDE_JUMP_COUNT] = {
        GameBuild::Address::FirstPersonZoomHide,
        GameBuild::Address::FirstPersonDrawZoomSkip};
    uint8_t* found[HIDE_JUMP_COUNT] = {};
    for (int i = 0; i < HIDE_JUMP_COUNT; ++i)
    {
        found[i] = reinterpret_cast<uint8_t*>(GameBuild::Absolute(sites[i]));
        if (memcmp(found[i], HIDE_JUMP_OPCODE, sizeof(HIDE_JUMP_OPCODE)) != 0)
        {
            Log::Warning(
                "zoom",
                "first-person hide jump %d not as expected (%02x %02x): not installed",
                i,
                found[i][0],
                found[i][1]);
            return;
        }
        memcpy(_hideJumpBytes[i], found[i], HIDE_JUMP_LENGTH);
    }
    void* const target = reinterpret_cast<void*>(GameBuild::Absolute(GameBuild::Address::SoldierGetZoom));
    MH_Initialize();
    MH_STATUS status =
        MH_CreateHook(target, reinterpret_cast<void*>(&GetZoomHook), reinterpret_cast<void**>(&_originalGetZoom));
    if (status == MH_OK)
    {
        status = MH_EnableHook(target);
    }
    if (status != MH_OK)
    {
        Log::Error("zoom", "GetZoom hook failed: %s", MH_StatusToString(status));
        return;
    }
    // The hidden check's call: pointed at IsHiddenHook once, here (before any frame); the hook passes the game's
    // answer through while VR is off.
    uint8_t* const call = reinterpret_cast<uint8_t*>(GameBuild::Absolute(GameBuild::Address::FirstPersonHiddenCall));
    if (call[0] != CALL_OPCODE)
    {
        Log::Warning("zoom", "first-person hidden call not as expected (%02x): not redirected", call[0]);
    }
    else
    {
        int32_t relative = 0;
        memcpy(&relative, call + 1, sizeof(relative));
        _originalIsHidden = reinterpret_cast<IsHiddenFunction>(call + CALL_LENGTH + relative);
        uint8_t bytes[CALL_LENGTH] = {CALL_OPCODE};
        relative = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&IsHiddenHook) - reinterpret_cast<uintptr_t>(call + CALL_LENGTH));
        memcpy(bytes + 1, &relative, sizeof(relative));
        if (!WriteCode(call, bytes, CALL_LENGTH))
        {
            Log::Warning("zoom", "first-person hidden call could not be redirected");
        }
    }
    memcpy(_hideJump, found, sizeof(_hideJump));
    Log::Info("zoom", "GetZoom hook installed");
}

void BeforeFrame(bool isVrRunning)
{
    if (!_hideJump[0] || isVrRunning == _isHidePatched)
    {
        return;
    }
    bool isWritten = true;
    for (int i = 0; i < HIDE_JUMP_COUNT; ++i)
    {
        isWritten = WriteCode(_hideJump[i], isVrRunning ? NO_OPERATION_BYTES : _hideJumpBytes[i]) && isWritten;
    }
    if (isWritten)
    {
        _isHidePatched = isVrRunning;
        if (!isVrRunning)
        {
            _targetFactor = 1.0f;
            _targetTick = 0;
        }
        Log::Info(
            "zoom",
            "%s",
            isVrRunning ? "VR: zoom kept off the main camera, first-person weapon drawn while zoomed"
                        : "flat: the game's own zoom");
    }
}

bool IsZoomed()
{
    return _isHidePatched && _targetTick != 0 && GetTickCount() - _targetTick <= ZOOM_HOLD_MILLISECONDS;
}

float TargetFactor()
{
    return IsZoomed() ? _targetFactor : 1.0f;
}

float StepFactor()
{
    const DWORD now = GetTickCount();
    if (!IsZoomed())
    {
        _scopeFactor = 1.0f;
        _scopeFactorTick = 0;
        return 1.0f;
    }
    FpGuard fp;
    const float elapsedSeconds = _scopeFactorTick ? static_cast<float>(now - _scopeFactorTick) * 0.001f : 0.0f;
    _scopeFactorTick = now;
    // As the game's own ramp (ChaseCamera::Update): factor = lerp(target, current, exp(-25 dt)).
    const float keep = expf(-25.0f * (elapsedSeconds > 0.25f ? 0.25f : elapsedSeconds));
    _scopeFactor = _targetFactor + (_scopeFactor - _targetFactor) * keep;
    return _scopeFactor;
}

void SetWindowDegrees(int degrees)
{
    if (degrees < 10 || degrees > 60)
    {
        return;
    }
    FpGuard fp;
    _windowHalfTan = tanf(static_cast<float>(degrees) * 0.5f * 0.017453293f);
    Log::Info("zoom", "scope window %d degrees", degrees);
}

float WindowHalfTan()
{
    return _windowHalfTan;
}

bool GetWeaponZoom(WeaponZoom& weaponZoom)
{
    if (!IsZoomed() || !_hasWeaponZoom)
    {
        return false;
    }
    weaponZoom = _weaponZoom;
    return true;
}

bool IsFullView()
{
    return IsZoomed() && _hasWeaponZoom && !_weaponZoom.hasLens && !_shouldUseLens;
}

bool IsCisWeapon(uintptr_t weaponClass)
{
    // The CIS side's first-person weapon models (mod tools' Sides/CIS/MSH) and the super battle droid's arm blaster
    // (root node 0x8D95A295, run156).
    static const char* const NAMES[] = {
        "cis_1st_weap_inf_rifle",
        "cis_1st_weap_inf_pistol",
        "cis_1st_weap_inf_sniperrifle",
        "cis_1st_weap_inf_launcher",
        "cis_1st_weap_inf_fusioncutter",
        "cis_1st_weap_inf_sonicblaster",
        "cis_1st_weap_inf_thermaldetonator"};
    if (HasFirstPersonNode(weaponClass, 0x8D95A295u))
    {
        return true;
    }
    using HashNameFunction = unsigned(__cdecl*)(const char*);
    const auto hashName = reinterpret_cast<HashNameFunction>(GameBuild::Absolute(GameBuild::Address::HashName));
    for (const char* name : NAMES)
    {
        char meshName[64];
        snprintf(meshName, sizeof(meshName), "%s_mesh", name);
        if (HasFirstPersonNode(weaponClass, hashName(name)) || HasFirstPersonNode(weaponClass, hashName(meshName)))
        {
            return true;
        }
    }
    return false;
}

void LogFirstPersonModel(uintptr_t weaponClass)
{
    uintptr_t hashes = 0;
    const unsigned count = ModelNodeHashes(weaponClass, &hashes);
    char line[160] = "";
    for (unsigned node = 0, length = 0; node < count && node < 8 && length < sizeof(line) - 12; ++node)
    {
        length += static_cast<unsigned>(
            snprintf(line + length, sizeof(line) - length, " 0x%08x", Read<unsigned>(hashes, 4 * node)));
    }
    Log::Info("zoom", "weapon class %08X: first-person model nodes:%s", static_cast<unsigned>(weaponClass), line);
}

unsigned ModelNodeList(uintptr_t model, uintptr_t* hashes)
{
    return ModelNodes(model, hashes);
}

bool ModelHasNode(uintptr_t model, unsigned nodeHash)
{
    uintptr_t hashes = 0;
    const unsigned count = ModelNodes(model, &hashes);
    for (unsigned node = 0; node < count; ++node)
    {
        if (Read<unsigned>(hashes, 4 * node) == nodeHash)
        {
            return true;
        }
    }
    return false;
}

bool HasFirstPersonNodes(uintptr_t weaponClass)
{
    uintptr_t hashes = 0;
    return ModelNodeHashes(weaponClass, &hashes) > 0;
}

bool HasFirstPersonNode(uintptr_t weaponClass, unsigned nodeHash)
{
    uintptr_t hashes = 0;
    const unsigned count = ModelNodeHashes(weaponClass, &hashes);
    for (unsigned node = 0; node < count; ++node)
    {
        if (Read<unsigned>(hashes, 4 * node) == nodeHash)
        {
            return true;
        }
    }
    return false;
}

bool GetLensCentre(uintptr_t weaponClass, float* centre)
{
    // Cached per class: FindLensModel walks the model's nodes and logs a class it does not know.
    static uintptr_t cachedClass = 0;
    static const LensModel* cachedModel = nullptr;
    if (!weaponClass)
    {
        return false;
    }
    if (weaponClass != cachedClass)
    {
        cachedClass = weaponClass;
        cachedModel = FindLensModel(weaponClass);
    }
    if (!cachedModel)
    {
        return false;
    }
    memcpy(centre, cachedModel->centres[0], 3 * sizeof(float));
    return true;
}

void SetLensView(bool isOn)
{
    _shouldUseLens = isOn;
}

int GetLenses(Lens* lenses)
{
    if (!_shouldUseLens || !IsZoomed() || !_hasWeaponZoom)
    {
        return 0;
    }
    Lens& lens = lenses[0];
    if (_lensModel)
    {
        // A weapon with lenses: the picture on the glass of each lens, exactly its size (b176, idren2023 after headset
        // run 13: back from b155's hologram over the lens to the picture on the lens; holograms only for weapons
        // without one; b206: no longer 1.5 times the glass).
        const int count = _lensModel->lensCount < MAXIMUM_LENS_COUNT ? _lensModel->lensCount : MAXIMUM_LENS_COUNT;
        for (int index = 0; index < count; ++index)
        {
            memcpy(lenses[index].centre, _lensModel->centres[index], sizeof(lenses[index].centre));
            lenses[index].radiusMeters = _lensModel->radiusMeters;
            lenses[index].emitterCount = 0;
            lenses[index].emitterRadiusMeters = 0;
            lenses[index].outlineCount = 0;
            lenses[index].tiltDegrees = _lensModel->tiltDegrees;
            lenses[index].isScreen = _lensModel->isScreen;
            for (int corner = 0; corner < _lensModel->outlineCount && corner < MAXIMUM_OUTLINE_POINTS; ++corner)
            { // model x points left seen from behind (the frame's right is -x, Stereo DrawScope)
                lenses[index].outline[corner][0] =
                    -(_lensModel->outline[corner][0] - _lensModel->centres[index][0]) / _lensModel->radiusMeters;
                lenses[index].outline[corner][1] =
                    (_lensModel->outline[corner][1] - _lensModel->centres[index][1]) / _lensModel->radiusMeters;
                lenses[index].outlineCount = corner + 1;
            }
        }
        return count;
    }
    float barrelEmitter[3], barrelAxis[3];
    lens.outlineCount = 0;
    lens.tiltDegrees = 0;
    lens.isScreen = false;
    if (Aim::GetBarrelEmitter(barrelEmitter, barrelAxis))
    {
        // The super battle droid (idren2023, 2026-10-13): the light comes from between its two wrist cannon barrels; the
        // picture floats above them and back toward the eye along the barrels, as for the lens weapons.
        // The barrels' muzzles are an arm's length away, so the picture comes back toward the eye further and is larger
        // (run164: over the muzzle it was small and far; run165: 25 cm back it covered the eye).
        lens.radiusMeters = DROID_PICTURE_RADIUS_METERS;
        lens.emitterCount = 1;
        lens.emitterRadiusMeters = 0;
        for (int axis = 0; axis < 3; ++axis)
        {
            lens.centre[axis] = barrelEmitter[axis] - barrelAxis[axis] * DROID_PICTURE_BACK_METERS;
        }
        lens.centre[1] += lens.radiusMeters * PROJECTED_EMITTER_BELOW; // up along the weapon's y
        for (int axis = 0; axis < 3; ++axis)
        {
            lens.emitters[0][axis] = lens.centre[axis] - barrelAxis[axis] * PROJECTOR_BEHIND_METERS;
        }
        lens.emitters[0][1] -= lens.radiusMeters * PROJECTOR_BELOW;
        return 1;
    }
    // No lens: the picture at the rifles' eyepiece place, the emitter behind it toward the eye, projecting forward.
    lens.radiusMeters = PROJECTED_LENS.radiusMeters * LENS_PROJECTION_SCALE;
    memcpy(lens.centre, PROJECTED_LENS.centres[0], sizeof(lens.centre));
    lens.emitterCount = 1;
    lens.emitterRadiusMeters = 0;
    lens.emitters[0][0] = lens.centre[0];
    lens.emitters[0][1] = lens.centre[1] - lens.radiusMeters * PROJECTOR_BELOW;
    lens.emitters[0][2] = lens.centre[2] - PROJECTOR_BEHIND_METERS;
    return 1;
}

void* ReticleTexture(unsigned& width, unsigned& height)
{
    return LookupTexture(_weaponZoom.reticleHash, "reticle", _loggedReticleHash, width, height);
}

void* ScopeTexture(unsigned& width, unsigned& height)
{
    return LookupTexture(_weaponZoom.scopeHash, "scope overlay", _loggedScopeHash, width, height);
}

} // namespace Zoom

} // namespace SwbfVr
