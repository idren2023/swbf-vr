// LockOn.cpp: the homing launchers' lock-on in the scope picture (idren2023 2026-10-20; see LockOn.h).
//
// What the game does (static reading 2026-10-20, offsets "Lock-on"): every frame PlayerController's target search
// (0x1F7770) scores each candidate with 0x2233B0 against the soldier's aim ray (Controllable+4 from, +0xEC along: the
// soldier's own aim, not the drawn weapon): a candidate t metres along the ray (0 < t <= LockOnRange + its radius) is
// inside when its distance from the ray is at most t x angle + its radius, a cone (the angle in radians used as its
// slope), LockOnAngle until locked, LockOffAngle after. The weapon keeps the target as a handle (+0x110 object,
// +0x114 serial, valid while the object's +0xB8 is that serial) and the lock as its charge: +0xB8 seconds held so
// far, +0xBC the class's LockTime; locked when the first reaches the second (slot 45), the HUD's bar is their
// ratio. A launcher locks only when its ordnance homes (WeaponLauncherClass+0x239).
//
// Drawn here over the scope pass's scene (depth off), styled after the reticle textures (reticule_rocket: twin rails,
// 45 deg chamfers, a bright core in a half-transparent dark band, soft edged, drawn from profile textures (b351);
// idren2023 2026-10-20), in red: the cone as a protractor 60 m out round
// the search ray (ticks every 10 deg, no circle, twin rails at the four points; the reticle's blue, red while a vehicle is
// in the lock zone) whose ticks light clockwise as the lock builds; four chamfered twin-rail brackets that start at the cone's width turned 30 deg and close square
// onto the target; once locked the ring bright and a chamfered diamond with four chevrons pulsing on the target. The
// target's marks follow it and stay quieter than the reticle (TARGET_*).

// Makes d3d9.h declare the Direct3D interfaces as C structs with an explicit vtable (device->lpVtbl->Method(device,
// ...)), the form the rest of the mod uses. Has to be a macro: d3d9.h reads it.
#define CINTERFACE

#include "LockOn.h"

#include "Aim.h"
#include "EngineSurvey.h"
#include "FpGuard.h"
#include "Holster.h"
#include "Log.h"
#include "Stereo.h"

#include <windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace SwbfVr
{

namespace LockOn
{

namespace
{

/// Weapon (offsets "Lock-on"): its class, the lock's seconds so far and needed, the target handle (object, serial).
constexpr uintptr_t OFFSET_weaponClass = 0x64, OFFSET_weaponLockSeconds = 0xB8, OFFSET_weaponLockNeeded = 0xBC;
constexpr uintptr_t OFFSET_weaponTarget = 0x110, OFFSET_weaponTargetSerial = 0x114;
/// The handle's object: its serial (a handle is valid while it matches).
constexpr uintptr_t OFFSET_objectSerial = 0xB8;
/// WeaponClass: LockOnRange, LockOnAngle and LockOffAngle (radians; the ODF's degrees times pi / 180).
constexpr uintptr_t OFFSET_classLockRange = 0xC8, OFFSET_classLockOnAngle = 0xCC, OFFSET_classLockOffAngle = 0xD0;
/// WeaponLauncherClass: LockTime, and whether its ordnance homes (set from OrdnanceName's class).
constexpr uintptr_t OFFSET_launcherLockTime = 0x234, OFFSET_launcherIsHoming = 0x239;
/// Soldier: its Controllable; Controllable: the aim ray's start and direction (the target search's).
constexpr uintptr_t OFFSET_soldierControllable = 0x250, OFFSET_controllableAimFrom = 0x4, OFFSET_controllableAim = 0xEC;
/// An entity's world matrix (soldiers and vehicles; the target's place if the handle's object is the entity: checked
/// as a rotation, inferred).
constexpr uintptr_t OFFSET_entityMatrix = 0x1B0;

/// The ring (idren2023 2026-10-20: no solid circle, less busy): the cone's cross-section this far along the search ray,
/// drawn as ticks only, every 10 deg pointing in from the cone's edge (every 30 deg long), lengths as shares of the
/// radius; the twin rails out at the four points: their gap (radians of arc at the ring) and how far out they start and
/// end (shares of the radius).
constexpr float RING_METERS = 60.0f;
constexpr int TICK_COUNT = 36;
constexpr float TICK_SHORT = 0.05f, TICK_LONG = 0.11f;
constexpr float RAIL_GAP = 0.05f, RAIL_START = 0.04f, RAIL_END = 0.17f;
/// The brackets: their half size round the target when locked (an angle from the scope camera, at least a metre),
/// where they start (a share of the cone's half width at the target: the lock area closing in) and how far turned (radians), their arms, chamfers
/// and the inner rail's inset (shares of the half size).
constexpr float BRACKET_HALF_TAN = 0.035f, BRACKET_LEAST_METERS = 1.0f, BRACKET_OPEN_SHARE = 0.62f; // corners (sqrt 2 out) inside the ring
constexpr float BRACKET_TURN = 0.5235988f, BRACKET_ARM = 0.42f, BRACKET_CHAMFER = 0.14f, BRACKET_RAIL = 0.1f;
/// A stroke's core half width as a tangent from the scope camera, and its outline's width in core widths.
constexpr float LINE_HALF_TAN = 0.0011f;
/// The stroke's layers, as measured in reticule_rocket (re/textures, 2026-10-20: a rail is one bright texel at alpha
/// 0xCC-0xEE between two dark texels at 0x55-0x99, nothing round them; the softness is the texture magnified): the
/// under ribbon UNDER_WIDTH core widths wide, dark (BAND_SHADE) at BAND_ALPHA, fading out smoothly over its outer
/// share from BAND_FADE on; the core ribbon bright at CORE_ALPHA, fading out from CORE_FADE on. No halo. Each ribbon's
/// ends carry the same layers (square, as the textures' ends).
constexpr float UNDER_WIDTH = 3.2f, BAND_SHADE = 0.26f, BAND_ALPHA = 0.55f, BAND_FADE = 0.25f;
constexpr float CORE_ALPHA = 0.95f, CORE_FADE = 0.0f; // soft from the middle out, as a magnified texel
constexpr int PROFILE_SIZE = 64;
/// The locked icon: its half size (times the brackets' locked half size), blinking period and the share of it lit.
constexpr float RETICLE_SIZE = 0.6f;
constexpr DWORD BLINK_MILLISECONDS = 250, BLINK_ON_MILLISECONDS = 150;
/// The brackets at the start of a lock, quieter than the reticle they move over: their strokes this much as wide,
/// their opacity this share, their core at most this lit; they grow to full as the lock builds and the reticle fades.
constexpr float TARGET_WEIGHT = 0.7f, TARGET_OPACITY = 0.6f, TARGET_MOST_LIT = 0.55f;
/// At the moment of the lock the brackets snap in from POP_SCALE times their size over POP_MILLISECONDS.
constexpr float POP_SCALE = 1.35f;
constexpr DWORD POP_MILLISECONDS = 350;
/// Colours in the reticle textures' manner (a dark outline, a core from dim to hot), from a base colour: the ring in
/// the scope reticle's blue (the game's 0x43A7F2) and red (the game's enemy 0xDF2020) while a vehicle is in the lock
/// zone (idren2023 2026-10-20); the target's marks always red.
constexpr D3DCOLOR RETICLE_BLUE = 0x43A7F2, ENEMY_RED = 0xDF2020;
/// The palette in use: outline, dim core, lit core.
struct Palette
{
    D3DCOLOR outline, dim, lit;
};
Palette _palette;
/// Weapon vtable slot 45 (0x242FE0): the game's own "locked" test. The raw log's interval.
constexpr int SLOT_IS_LOCKED = 45;
constexpr DWORD RAW_LOG_MILLISECONDS = 100;
/// ReticleOpacity counts only this long after the last scope pass.
constexpr DWORD RETICLE_FRESH_MILLISECONDS = 250;
/// The scope picture spans this many times the widest lock cone (idren2023: wider than the cone needs).
constexpr float SCOPE_MARGIN = 1.3f;
/// The test target's distance along the aim, and its build-up and locked times.
constexpr float TEST_METERS = 40.0f, TEST_RIGHT = 2.5f, TEST_UP = 1.0f;
constexpr DWORD TEST_BUILD_MILLISECONDS = 1000, TEST_HOLD_MILLISECONDS = 1000;

struct Vertex
{
    float x, y, z;
    D3DCOLOR color;
    float u, v; // across the stroke 0..1; along it 0 at an end, 0.5 from the stroke's own ends on
};

constexpr int MOST_VERTICES = 6 * 512;
/// Strokes are drawn in two lists, all outlines first, then the cores over them.
struct Stroke
{
    Vertex vertices[MOST_VERTICES];
    int count;
};
Stroke _outline, _core; // the under ribbons (band and halo), the cores over them
IDirect3DTexture9* _underTexture = nullptr; // the layers' profiles (MakeProfiles), managed pool
IDirect3DTexture9* _coreTexture = nullptr;
IDirect3DDevice9* _textureDevice = nullptr;
float _camera[16]; // the scope camera this pass (rows right, up, back, position)

IDirect3DStateBlock9* _savedState = nullptr;
IDirect3DDevice9* _stateBlockDevice = nullptr;
bool _isTest = false;
DWORD _testStartTick = 0;
float _testTarget[3];
bool _hasTestTarget = false;
uintptr_t _loggedTarget = 0; // the target last logged (a change is logged once)
bool _wasLocked = false;
bool _isRawLogOn = false;      // `lockon log 1`: the game's own lock values every RAW_LOG_MILLISECONDS
DWORD _rawLogTick = 0;
float _reticleOpacity = 1.0f;  // the scope reticle's opacity this frame (ReticleOpacity)
DWORD _reticleTick = 0;        // when it was worked out

/// The lock as read this frame.
struct Lock
{
    float from[3], aim[3];  // the aim ray
    float angle, range;     // the cone in use (radians as a slope; metres)
    bool hasTarget;
    float target[3];        // the target's place
    float progress;         // 0..1
};

template <typename T> bool Read(uintptr_t address, T* value)
{
    if (!address || IsBadReadPtr(reinterpret_cast<const void*>(address), sizeof(T)))
    {
        return false;
    }
    *value = *reinterpret_cast<const T*>(address);
    return true;
}

// The target object's place, from its world matrix if that is a rotation (else not known).
bool TargetPlace(uintptr_t object, float* place)
{
    float matrix[16];
    if (IsBadReadPtr(reinterpret_cast<const void*>(object + OFFSET_entityMatrix), sizeof(matrix)))
    {
        return false;
    }
    memcpy(matrix, reinterpret_cast<const void*>(object + OFFSET_entityMatrix), sizeof(matrix));
    for (int row = 0; row < 3; ++row)
    {
        const float* r = matrix + row * 4;
        const float length = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
        if (!(length > 0.9f && length < 1.1f))
        {
            return false;
        }
    }
    memcpy(place, matrix + 12, 3 * sizeof(float));
    return true;
}

// The class of the homing launcher in the hand, or 0.
uintptr_t HomingLauncherClass()
{
    const uintptr_t weapon = Aim::GetWeapon();
    const char* weaponType = weapon ? Holster::ObjectTypeName(weapon) : nullptr;
    uintptr_t weaponClass = 0;
    uint8_t isHoming = 0;
    float lockTime = 0;
    if (!weaponType || strcmp(weaponType, ".?AVWeaponLauncher@@") != 0 ||
        !Read(weapon + OFFSET_weaponClass, &weaponClass) || !Read(weaponClass + OFFSET_launcherIsHoming, &isHoming) ||
        !isHoming || !Read(weaponClass + OFFSET_launcherLockTime, &lockTime) || lockTime <= 0)
    {
        return 0;
    }
    return weaponClass;
}

// The player's lock this frame: false without a homing launcher in hand (or the test).
bool ReadLock(Lock& lock)
{
    const uintptr_t soldier = EngineSurvey::Resolve("soldier");
    const uintptr_t controllable = soldier ? soldier + OFFSET_soldierControllable : 0;
    if (!controllable || IsBadReadPtr(reinterpret_cast<const void*>(controllable), OFFSET_controllableAim + 12))
    {
        return false;
    }
    memcpy(lock.from, reinterpret_cast<const void*>(controllable + OFFSET_controllableAimFrom), sizeof(lock.from));
    // The search's direction: the hand's while it aims (Aim's TargetSearchDirectionHook, b178), else the soldier's.
    if (!Aim::GetTargetSearchDirection(lock.aim))
    {
        memcpy(lock.aim, reinterpret_cast<const void*>(controllable + OFFSET_controllableAim), sizeof(lock.aim));
    }
    const float length = sqrtf(lock.aim[0] * lock.aim[0] + lock.aim[1] * lock.aim[1] + lock.aim[2] * lock.aim[2]);
    if (!(length > 0.5f && length < 1.5f))
    {
        return false;
    }
    for (float& value : lock.aim)
    {
        value /= length;
    }
    if (_isTest)
    {
        const DWORD cycle = (GetTickCount() - _testStartTick) % (TEST_BUILD_MILLISECONDS + TEST_HOLD_MILLISECONDS);
        lock.angle = 6.0f * 0.01745329f;
        lock.range = 175.0f;
        lock.hasTarget = true;
        lock.progress = cycle >= TEST_BUILD_MILLISECONDS ? 1.0f : static_cast<float>(cycle) / TEST_BUILD_MILLISECONDS;
        if (!_hasTestTarget) // fixed in the world where the aim pointed at the start, so turning away shows tracking
        {
            // Off the aim (TEST_RIGHT / TEST_UP), to show the marks following a target away from the reticle.
            const float right[3] = {-lock.aim[2], 0, lock.aim[0]};
            const float rightLength = sqrtf(right[0] * right[0] + right[2] * right[2]);
            for (int axis = 0; axis < 3; ++axis)
            {
                _testTarget[axis] = lock.from[axis] + lock.aim[axis] * TEST_METERS +
                                    (rightLength > 1e-3f ? right[axis] / rightLength * TEST_RIGHT : 0.0f) +
                                    (axis == 1 ? TEST_UP : 0.0f);
            }
            _hasTestTarget = true;
        }
        memcpy(lock.target, _testTarget, sizeof(_testTarget));
        return true;
    }
    const uintptr_t weapon = Aim::GetWeapon();
    const uintptr_t weaponClass = HomingLauncherClass();
    if (!weaponClass)
    {
        return false;
    }
    float seconds = 0, needed = 0, onAngle = 0, offAngle = 0;
    uintptr_t object = 0;
    unsigned serial = 0, objectSerial = 0;
    Read(weapon + OFFSET_weaponLockSeconds, &seconds);
    Read(weapon + OFFSET_weaponLockNeeded, &needed);
    Read(weaponClass + OFFSET_classLockOnAngle, &onAngle);
    Read(weaponClass + OFFSET_classLockOffAngle, &offAngle);
    Read(weaponClass + OFFSET_classLockRange, &lock.range);
    Read(weapon + OFFSET_weaponTarget, &object);
    Read(weapon + OFFSET_weaponTargetSerial, &serial);
    lock.hasTarget = object && Read(object + OFFSET_objectSerial, &objectSerial) && objectSerial == serial &&
                     TargetPlace(object, lock.target);
    lock.progress = needed > 0 ? (seconds >= needed ? 1.0f : seconds / needed) : 0.0f;
    const bool isLocked = lock.hasTarget && lock.progress >= 1.0f;
    lock.angle = isLocked && offAngle > 0 ? offAngle : onAngle;
    if (_isRawLogOn && GetTickCount() - _rawLogTick >= RAW_LOG_MILLISECONDS)
    {
        // The game's own values, and its own verdict: the weapon's slot 45 (0x242FE0, true when +0xB8 >= +0xBC), to
        // tell that what is drawn follows the game's lock and not a reading of ours.
        _rawLogTick = GetTickCount();
        void** const vtable = *reinterpret_cast<void***>(weapon);
        using IsLockedFunction = bool(__fastcall*)(void* self, void* edx);
        const bool gameSaysLocked =
            vtable && !IsBadReadPtr(vtable + SLOT_IS_LOCKED, sizeof(void*)) &&
            reinterpret_cast<IsLockedFunction>(vtable[SLOT_IS_LOCKED])(reinterpret_cast<void*>(weapon), nullptr);
        float off = 0, along = 0; // the target's place from the search ray: metres along, degrees off
        if (lock.hasTarget)
        {
            float toTarget[3];
            for (int axis = 0; axis < 3; ++axis)
            {
                toTarget[axis] = lock.target[axis] - lock.from[axis];
                along += toTarget[axis] * lock.aim[axis];
            }
            float across = 0;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float rest = toTarget[axis] - lock.aim[axis] * along;
                across += rest * rest;
            }
            off = along > 0.1f ? atanf(sqrtf(across) / along) * 57.29578f : 0.0f;
        }
        const char* name = object ? Holster::ObjectTypeName(object) : nullptr;
        Log::Info(
            "lockon",
            "raw: handle %p serial %u (object's %u%s) %s; timer %.2f / %.2f s; game's locked %d, ours %d; target %.0f m "
            "along, %.1f deg off (cone %.1f deg)",
            reinterpret_cast<void*>(object),
            serial,
            objectSerial,
            object && objectSerial == serial ? ", valid" : "",
            name ? name : "-",
            static_cast<double>(seconds),
            static_cast<double>(needed),
            gameSaysLocked ? 1 : 0,
            isLocked ? 1 : 0,
            static_cast<double>(along),
            static_cast<double>(off),
            static_cast<double>(lock.angle * 57.29578f));
    }
    const uintptr_t shownTarget = lock.hasTarget ? object : 0;
    if (shownTarget != _loggedTarget || isLocked != _wasLocked)
    {
        _loggedTarget = shownTarget;
        _wasLocked = isLocked;
        const char* name = shownTarget ? Holster::ObjectTypeName(shownTarget) : nullptr;
        Log::Info(
            "lockon",
            "target %s (%p) at %.1f %.1f %.1f, %.2f of %.2f s%s; cone %.1f deg to %.0f m",
            shownTarget ? (name ? name : "(no type)") : "none",
            reinterpret_cast<void*>(object),
            static_cast<double>(lock.hasTarget ? lock.target[0] : 0),
            static_cast<double>(lock.hasTarget ? lock.target[1] : 0),
            static_cast<double>(lock.hasTarget ? lock.target[2] : 0),
            static_cast<double>(seconds),
            static_cast<double>(needed),
            isLocked ? ", locked" : "",
            static_cast<double>(lock.angle * 57.29578f),
            static_cast<double>(lock.range));
    }
    return true;
}

// A stroke from a to b as a ribbon facing the scope camera into `list`, half width = distance x halfTan, carried on
// past both ends by that much: three pieces, an end cap, the middle, an end cap (v 0 at the cap's end, 0.5 where the
// stroke itself ends), so the profile textures draw square layered ends and strokes meeting at a corner close it.
void Ribbon(Stroke& list, const float* a, const float* b, float halfTan, D3DCOLOR color)
{
    if (list.count + 18 > MOST_VERTICES)
    {
        return;
    }
    const float* eye = _camera + 12;
    float along[3], middle[3];
    for (int axis = 0; axis < 3; ++axis)
    {
        along[axis] = b[axis] - a[axis];
        middle[axis] = (a[axis] + b[axis]) * 0.5f - eye[axis];
    }
    float side[3] = {
        along[1] * middle[2] - along[2] * middle[1],
        along[2] * middle[0] - along[0] * middle[2],
        along[0] * middle[1] - along[1] * middle[0]};
    const float sideLength = sqrtf(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
    const float distance = sqrtf(middle[0] * middle[0] + middle[1] * middle[1] + middle[2] * middle[2]);
    const float alongLength = sqrtf(along[0] * along[0] + along[1] * along[1] + along[2] * along[2]);
    if (sideLength < 1e-6f || alongLength < 1e-6f)
    {
        return;
    }
    const float half = distance * halfTan / sideLength, extend = distance * halfTan / alongLength;
    // Four stations along: the start cap's end, a, b, the end cap's end; and their v.
    float stations[4][3];
    const float vs[4] = {0.0f, 0.5f, 0.5f, 0.0f};
    for (int axis = 0; axis < 3; ++axis)
    {
        stations[0][axis] = a[axis] - along[axis] * extend;
        stations[1][axis] = a[axis];
        stations[2][axis] = b[axis];
        stations[3][axis] = b[axis] + along[axis] * extend;
    }
    for (int piece = 0; piece < 3; ++piece)
    {
        Vertex quad[4];
        for (int corner = 0; corner < 4; ++corner)
        {
            const int station = piece + (corner >> 1);
            const float* at = stations[station];
            const float sign = (corner & 1) ? 1.0f : -1.0f;
            quad[corner] = {
                at[0] + side[0] * half * sign,
                at[1] + side[1] * half * sign,
                at[2] + side[2] * half * sign,
                color,
                (corner & 1) ? 1.0f : 0.0f,
                vs[station]};
        }
        const int order[6] = {0, 1, 2, 2, 1, 3};
        for (int index : order)
        {
            list.vertices[list.count++] = quad[index];
        }
    }
}

// Two colours (ARGB) mixed by t 0..1.
D3DCOLOR Mix(D3DCOLOR from, D3DCOLOR to, float t)
{
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    D3DCOLOR result = 0;
    for (int shift = 0; shift < 32; shift += 8)
    {
        const float x = static_cast<float>((from >> shift) & 0xFF), y = static_cast<float>((to >> shift) & 0xFF);
        result |= static_cast<D3DCOLOR>(x + (y - x) * t + 0.5f) << shift;
    }
    return result;
}

// The palette of a base colour (0xRRGGBB): the under ribbon's tint the base (its band and halo shade it, MakeProfiles),
// the dim core the base a little darker, the lit core the base half way to white.
Palette PaletteOf(D3DCOLOR base)
{
    const auto scaled = [base](float k, float toWhite)
    {
        D3DCOLOR result = 0;
        for (int shift = 0; shift < 24; shift += 8)
        {
            float channel = static_cast<float>((base >> shift) & 0xFF) * k;
            channel += (255.0f - channel) * toWhite;
            result |= static_cast<D3DCOLOR>(channel > 255 ? 255 : channel) << shift;
        }
        return result;
    };
    return {0xFF000000 | scaled(1.0f, 0), 0x90000000 | scaled(0.8f, 0), 0xFF000000 | scaled(1.0f, 0.55f)};
}

// A colour with its alpha scaled by `opacity` (0..1).
D3DCOLOR Faded(D3DCOLOR color, float opacity)
{
    const float alpha = static_cast<float>(color >> 24) * (opacity < 0 ? 0 : opacity > 1 ? 1 : opacity);
    return (static_cast<D3DCOLOR>(alpha + 0.5f) << 24) | (color & 0xFFFFFF);
}

// One stroke in the manner of the reticle textures: the under ribbon (a half-transparent dark band, soft edged)
// UNDER_WIDTH times as wide as the core, the core over it from dim to bright by `lit` (0..1); `weight` scales the widths.
void Line(const float* a, const float* b, float lit, float weight = 1.0f, float opacity = 1.0f)
{
    Ribbon(_outline, a, b, LINE_HALF_TAN * weight * UNDER_WIDTH, Faded(_palette.outline, opacity));
    Ribbon(_core, a, b, LINE_HALF_TAN * weight, Faded(Mix(_palette.dim, _palette.lit, lit), opacity));
}

// The two profile textures (PROFILE_SIZE square, A8R8G8B8, managed): u across a stroke, v along it (0 at a cap's
// end, 0.5 and on inside), the layer taken by the larger of the two distances from the middle (square ends). Under:
// dark (BAND_SHADE) at BAND_ALPHA, fading out smoothly from BAND_FADE. Core: near white at CORE_ALPHA, fading out
// from CORE_FADE. The vertex colour tints both.
bool MakeProfiles(IDirect3DDevice9* device)
{
    if (_textureDevice == device && _underTexture && _coreTexture)
    {
        return true;
    }
    if (_underTexture)
    {
        _underTexture->lpVtbl->Release(_underTexture);
        _underTexture = nullptr;
    }
    if (_coreTexture)
    {
        _coreTexture->lpVtbl->Release(_coreTexture);
        _coreTexture = nullptr;
    }
    _textureDevice = device;
    for (int kind = 0; kind < 2; ++kind)
    {
        IDirect3DTexture9** texture = kind == 0 ? &_underTexture : &_coreTexture;
        if (FAILED(device->lpVtbl->CreateTexture(
                device, PROFILE_SIZE, PROFILE_SIZE, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, texture, nullptr)))
        {
            *texture = nullptr;
            return false;
        }
        D3DLOCKED_RECT lock{};
        if (FAILED((*texture)->lpVtbl->LockRect(*texture, 0, &lock, nullptr, 0)))
        {
            return false;
        }
        for (int y = 0; y < PROFILE_SIZE; ++y)
        {
            uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lock.pBits) + y * lock.Pitch);
            const float v = (static_cast<float>(y) + 0.5f) / PROFILE_SIZE;
            const float alongDistance = v >= 0.5f ? 0.0f : 1.0f - 2.0f * v; // 0 inside, 1 at a cap's end
            for (int x = 0; x < PROFILE_SIZE; ++x)
            {
                const float u = (static_cast<float>(x) + 0.5f) / PROFILE_SIZE;
                const float acrossDistance = fabsf(2.0f * u - 1.0f);
                const float d = acrossDistance > alongDistance ? acrossDistance : alongDistance;
                // Fading out smoothly from `from` to the edge (smoothstep), as a magnified texel blends.
                const auto fade = [d](float from)
                {
                    const float t = d <= from ? 0.0f : (d - from) / (1.0f - from);
                    return 1.0f - t * t * (3.0f - 2.0f * t);
                };
                const float shade = kind == 0 ? BAND_SHADE : 1.0f - 0.2f * d; // the core a touch darker at its sides
                const float alpha = kind == 0 ? BAND_ALPHA * fade(BAND_FADE) : CORE_ALPHA * fade(CORE_FADE);
                const uint32_t a8 = static_cast<uint32_t>(alpha * 255.0f + 0.5f);
                const uint32_t c8 = static_cast<uint32_t>(shade * 255.0f + 0.5f);
                row[x] = (a8 << 24) | (c8 << 16) | (c8 << 8) | c8;
            }
        }
        (*texture)->lpVtbl->UnlockRect(*texture, 0);
    }
    return true;
}


// A point in the scope camera's plane through `centre`: x right, y up (metres), the pair turned by `turn` radians.
void InPlane(const float* centre, float x, float y, float* point, float turn = 0.0f)
{
    const float c = cosf(turn), s = sinf(turn);
    const float rx = x * c - y * s, ry = x * s + y * c;
    for (int axis = 0; axis < 3; ++axis)
    {
        point[axis] = centre[axis] + _camera[axis] * rx + _camera[4 + axis] * ry;
    }
}

// The cone as a protractor RING_METERS along the search ray, at right angles to it: a double circle, a tick every
// 5 deg pointing in (every 30 deg longer), twin rails out at the four points in line with the reticle's arms. The ticks
// light clockwise from the top as the lock builds (all of them once locked); without a target they stay dim.
void AddRing(const Lock& lock)
{
    const float distance = lock.range > 0 && lock.range < RING_METERS ? lock.range : RING_METERS;
    const float radius = distance * lock.angle;
    float centre[3], right[3], up[3];
    for (int axis = 0; axis < 3; ++axis)
    {
        centre[axis] = lock.from[axis] + lock.aim[axis] * distance;
    }
    // right = aim x world up, up = right x aim: the ring upright, its top above the aim.
    right[0] = -lock.aim[2];
    right[1] = 0;
    right[2] = lock.aim[0];
    const float rightLength = sqrtf(right[0] * right[0] + right[2] * right[2]);
    if (rightLength < 1e-3f || radius <= 0)
    {
        return;
    }
    right[0] /= rightLength;
    right[2] /= rightLength;
    up[0] = right[1] * lock.aim[2] - right[2] * lock.aim[1];
    up[1] = right[2] * lock.aim[0] - right[0] * lock.aim[2];
    up[2] = right[0] * lock.aim[1] - right[1] * lock.aim[0];
    // A point of the ring's plane `angle` clockwise from the top, `scale` times the radius out.
    const auto at = [&](float angle, float scale, float* point)
    {
        const float c = cosf(angle) * radius * scale, s = sinf(angle) * radius * scale;
        for (int axis = 0; axis < 3; ++axis)
        {
            point[axis] = centre[axis] + up[axis] * c + right[axis] * s;
        }
    };
    constexpr float TWO_PI = 6.2831853f;
    const bool isLocked = lock.hasTarget && lock.progress >= 1.0f;
    const float ringLit = isLocked ? 1.0f : lock.hasTarget ? 0.6f : 0.35f;
    _palette = PaletteOf(lock.hasTarget ? ENEMY_RED : RETICLE_BLUE);
    const int litTicks = isLocked ? TICK_COUNT : lock.hasTarget ? static_cast<int>(lock.progress * TICK_COUNT) : 0;
    for (int tick = 0; tick < TICK_COUNT; ++tick)
    {
        const bool isLong = tick % (TICK_COUNT / 12) == 0;
        const float angle = TWO_PI * static_cast<float>(tick) / TICK_COUNT;
        float from[3], to[3];
        at(angle, 1.0f, from);
        at(angle, 1.0f - (isLong ? TICK_LONG : TICK_SHORT), to);
        Line(from, to, tick < litTicks ? 1.0f : 0.25f, isLong ? 1.0f : 0.75f);
    }
    for (int point = 0; point < 4; ++point) // twin rails out at the top, right, bottom and left
    {
        const float angle = TWO_PI * static_cast<float>(point) / 4;
        for (int rail = -1; rail <= 1; rail += 2)
        {
            // Parallel: the same distance across at both ends, so the angle shrinks with the radius.
            const float across = rail * RAIL_GAP * 0.5f;
            float from[3], to[3];
            at(angle + across / (1.0f + RAIL_START), 1.0f + RAIL_START, from);
            at(angle + across / (1.0f + RAIL_END), 1.0f + RAIL_END, to);
            Line(from, to, ringLit);
        }
    }
}

// The target's brackets: chamfered corners with a twin rail inside, open as wide as the cone at the target and turned
// 30 deg, closing and squaring onto the target as the lock builds; locked, bright, with a chamfered diamond and four
// chevrons pointing in blinking on the target.
void AddTarget(const Lock& lock)
{
    float distance = 0; // along the scope camera's forward (-back)
    for (int axis = 0; axis < 3; ++axis)
    {
        distance -= (lock.target[axis] - _camera[12 + axis]) * _camera[8 + axis];
    }
    if (distance < 1.0f)
    {
        return;
    }
    float locked = distance * BRACKET_HALF_TAN;
    locked = locked < BRACKET_LEAST_METERS ? BRACKET_LEAST_METERS : locked;
    const float progress = lock.progress < 0 ? 0 : lock.progress > 1 ? 1 : lock.progress;
    const float eased = progress * progress * (3 - 2 * progress); // smoothstep: closes gently, settles
    const bool isLocked = progress >= 1.0f;
    // Open: just inside the cone's half width at the target's distance (never smaller than locked).
    float open = distance * lock.angle * BRACKET_OPEN_SHARE;
    open = open > locked ? open : locked;
    _palette = PaletteOf(ENEMY_RED);
    // The moment of the lock: the brackets snap in from POP_SCALE times their size at full strength.
    static bool wasLocked = false;
    static DWORD lockTick = 0;
    const DWORD now = GetTickCount();
    if (isLocked && !wasLocked)
    {
        lockTick = now;
    }
    wasLocked = isLocked;
    const float pop = isLocked && now - lockTick < POP_MILLISECONDS
                          ? 1.0f - static_cast<float>(now - lockTick) / POP_MILLISECONDS
                          : 0.0f;
    const float popped = pop * pop; // eases out
    // Strength grows with the lock as the scope's reticle fades out (idren2023 2026-10-20): quiet at the start (TARGET_*),
    // full once locked, when the reticle is gone and the lock alone steers the torpedo.
    const float lit = TARGET_MOST_LIT * 0.6f + (1.0f - TARGET_MOST_LIT * 0.6f) * eased;
    const float width = TARGET_WEIGHT + (1.0f - TARGET_WEIGHT) * eased;
    const float opacity = TARGET_OPACITY + (1.0f - TARGET_OPACITY) * eased;
    const float half = (locked + (open - locked) * (1.0f - eased)) * (1.0f + (POP_SCALE - 1.0f) * popped);
    const float turn = BRACKET_TURN * (1.0f - eased);
    const float arm = half * BRACKET_ARM, chamfer = half * BRACKET_CHAMFER, inset = half * BRACKET_RAIL;
    for (int corner = 0; corner < 4; ++corner)
    {
        const float sx = (corner & 1) ? 1.0f : -1.0f, sy = (corner & 2) ? 1.0f : -1.0f;
        for (int rail = 0; rail < 2; ++rail) // the corner, then its shorter twin inside
        {
            const float h = rail == 0 ? half : half - inset;
            const float a = rail == 0 ? arm : arm * 0.6f, c = rail == 0 ? chamfer : chamfer * 0.8f;
            float p[4][3];
            InPlane(lock.target, sx * (h - a), sy * h, p[0], turn);
            InPlane(lock.target, sx * (h - c), sy * h, p[1], turn);
            InPlane(lock.target, sx * h, sy * (h - c), p[2], turn);
            InPlane(lock.target, sx * h, sy * (h - a), p[3], turn);
            const float shade = rail == 0 ? lit : lit * 0.8f, weight = (rail == 0 ? 1.0f : 0.6f) * width;
            Line(p[0], p[1], shade, weight, opacity);
            Line(p[1], p[2], shade, weight, opacity);
            Line(p[2], p[3], shade, weight, opacity);
        }
    }
    // Locked: the full lock symbol (b345's), blinking hard at 4 Hz.
    if (isLocked && (now % BLINK_MILLISECONDS) < BLINK_ON_MILLISECONDS)
    {
        // The diamond: its four points (up, right, down, left) cut off, so eight corners.
        const float r = locked * RETICLE_SIZE, cut = 0.22f;
        const float points[4][2] = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}};
        float corners[8][3];
        for (int point = 0; point < 4; ++point)
        {
            const float* here = points[point];
            const float* before = points[(point + 3) % 4];
            const float* after = points[(point + 1) % 4];
            InPlane(
                lock.target,
                (here[0] + (before[0] - here[0]) * cut) * r,
                (here[1] + (before[1] - here[1]) * cut) * r,
                corners[point * 2]);
            InPlane(
                lock.target,
                (here[0] + (after[0] - here[0]) * cut) * r,
                (here[1] + (after[1] - here[1]) * cut) * r,
                corners[point * 2 + 1]);
        }
        for (int index = 0; index < 8; ++index)
        {
            Line(corners[index], corners[(index + 1) % 8], 1.0f, 1.2f);
        }
        // Four chevrons outside it, pointing in at the target.
        for (const auto& point : points)
        {
            const float dx = point[0], dy = point[1];
            const float tip = r * 1.3f, back = r * 1.65f, wing = r * 0.3f;
            float t[3], w0[3], w1[3];
            InPlane(lock.target, dx * tip, dy * tip, t);
            InPlane(lock.target, dx * back - dy * wing, dy * back + dx * wing, w0);
            InPlane(lock.target, dx * back + dy * wing, dy * back - dx * wing, w1);
            Line(w0, t, 1.0f);
            Line(t, w1, 1.0f);
        }
    }
}

} // namespace

void DrawInScope(void* device)
{
    if (!device || !Stereo::GetScopeCamera(_camera))
    {
        return;
    }
    Lock lock{};
    _outline.count = _core.count = 0;
    {
        FpGuard fp;
        _reticleTick = GetTickCount();
        _reticleOpacity = 1.0f;
        if (!ReadLock(lock))
        {
            return;
        }
        // The scope reticle fades out as the lock builds and is gone while locked (the lock alone steers the torpedo).
        if (lock.hasTarget)
        {
            const float progress = lock.progress < 0 ? 0 : lock.progress > 1 ? 1 : lock.progress;
            _reticleOpacity = progress >= 1.0f ? 0.0f : 1.0f - progress * progress * (3 - 2 * progress);
        }
        AddRing(lock);
        if (lock.hasTarget)
        {
            AddTarget(lock);
        }
    }
    if (_outline.count + _core.count == 0)
    {
        return;
    }
    IDirect3DDevice9* const d3dDevice = static_cast<IDirect3DDevice9*>(device);
    if (_stateBlockDevice != d3dDevice)
    {
        OnReset();
        _stateBlockDevice = d3dDevice;
    }
    if (!_savedState && FAILED(d3dDevice->lpVtbl->CreateStateBlock(d3dDevice, D3DSBT_ALL, &_savedState)))
    {
        _savedState = nullptr;
        return;
    }
    if (FAILED(_savedState->lpVtbl->Capture(_savedState)))
    {
        return;
    }
    static const D3DMATRIX IDENTITY = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
    d3dDevice->lpVtbl->SetVertexShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetPixelShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetTransform(d3dDevice, D3DTS_WORLD, &IDENTITY);
    if (!MakeProfiles(d3dDevice))
    {
        _savedState->lpVtbl->Apply(_savedState);
        return;
    }
    d3dDevice->lpVtbl->SetFVF(d3dDevice, D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_LIGHTING, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_FOGENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZENABLE, D3DZB_FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZWRITEENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHATESTENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_STENCILENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_CULLMODE, D3DCULL_NONE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHABLENDENABLE, TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_COLORWRITEENABLE, 0x7);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_TEXCOORDINDEX, 0);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_SRGBTEXTURE, FALSE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    for (const Stroke* list : {&_outline, &_core}) // the bands and halos first, the cores over them
    {
        if (list->count > 0)
        {
            d3dDevice->lpVtbl->SetTexture(
                d3dDevice,
                0,
                reinterpret_cast<IDirect3DBaseTexture9*>(list == &_outline ? _underTexture : _coreTexture));
            d3dDevice->lpVtbl->DrawPrimitiveUP(
                d3dDevice, D3DPT_TRIANGLELIST, static_cast<UINT>(list->count / 3), list->vertices, sizeof(Vertex));
        }
    }
    _savedState->lpVtbl->Apply(_savedState);
    static bool hasLogged = false;
    if (!hasLogged)
    {
        hasLogged = true;
        Log::Info("lockon", "first lock-on display drawn in the scope picture (%d + %d vertices)", _outline.count, _core.count);
    }
}

float ScopeHalfTan()
{
    const uintptr_t weaponClass = _isTest ? 0 : HomingLauncherClass();
    float onAngle = _isTest ? 6.0f * 0.01745329f : 0, offAngle = _isTest ? 10.0f * 0.01745329f : 0;
    if (weaponClass)
    {
        FpGuard fp;
        Read(weaponClass + OFFSET_classLockOnAngle, &onAngle);
        Read(weaponClass + OFFSET_classLockOffAngle, &offAngle);
    }
    const float widest = onAngle > offAngle ? onAngle : offAngle;
    return widest > 0 ? widest * SCOPE_MARGIN : 0.0f; // the angle is the cone's slope (a tangent)
}

void OnReset()
{
    if (_savedState)
    {
        _savedState->lpVtbl->Release(_savedState);
        _savedState = nullptr;
    }
    _stateBlockDevice = nullptr;
}

float ReticleOpacity()
{
    return GetTickCount() - _reticleTick < RETICLE_FRESH_MILLISECONDS ? _reticleOpacity : 1.0f;
}

void OnShot()
{
    // The game clears its own lock at a shot (ClearLockOnFire, 1 unless the ODF says otherwise: the class constructor
    // at 0x24BE57; slot 30 0x24C070 then zeroes the timer), so the display follows by itself; only the test lock
    // starts again here.
    _testStartTick = GetTickCount();
}

void SetRawLog(bool isOn)
{
    _isRawLogOn = isOn;
    Log::Info("lockon", "raw lock log %s (every %u ms while a homing launcher is in the scope)", isOn ? "on" : "off",
              static_cast<unsigned>(RAW_LOG_MILLISECONDS));
}

void SetTest(bool isOn)
{
    _isTest = isOn;
    _testStartTick = GetTickCount();
    _hasTestTarget = false;
    Log::Info("lockon", "test target %s", isOn ? "on (40 m along the aim, 2.5 m right and 1 m up, fixed there, locks over 1 s)" : "off");
}

} // namespace LockOn

} // namespace SwbfVr
