// Jet.cpp: jet packs in VR (PLAN 36, idren2023 2026-10-13; flight by the left controller, idren2023 2026-11-07).
//
// The game knows two kinds (EntitySoldierClass, from the ODF; static reading of the exe by a sub-agent, 2026-10-13,
// docs/offsets.md "Jet packs"):
//   jump pack  (dark trooper: JetJump 21, JetPush 0): jump again in the air = one jet jump. The jump function 0x1103F0
//              adds JetJump * normalise((0, 1.1, 0) + body forward * move input) to the velocity: straight up, or up to
//              about 45 degrees ahead when the left stick is pushed forward.
//   hover      (jet trooper: JetType "hover", JetJump 5, JetPush 8): the same jump starts a hover (action 7); every
//              frame the hover code pulls the vertical speed toward about +2 m/s, adds JetPush * aim pitch * move
//              input (the aim, soldier+0x33C, is the right hand's since b147) and hands 2 * move * dot(aim, forward)
//              to the air movement 0x110BE0 as its forward input. Jump again ends it; fuel ends it.
// The hover's steering direction is the left controller's pointing direction (yaw and pitch, in the world), or the
// head's with head-based movement (Controls::SetMoveByHead, swbf-vr.ini move_direction = head). It is sampled once
// per presented frame (OnPresent) and used by the hover in the next soldier update, so both see one frame's value.
// The jump pack is unchanged (idren2023 2026-11-07: it is right as it is).
// What VR changes:
//   jump pack  the jump keeps its strength, but its angle above the ground follows the headset: looking level gives
//              the stock 45 degrees ahead, looking up 45 degrees or more goes straight up, never lower than 30 degrees
//              (looking down must not throw the player into the ground). Ahead = the body's forward, which follows the
//              left controller.
//   hover      the velocity is ours: left stick forward flies along the steering direction (pitch included: point up
//              to climb), back flies the other way, sideways flies level to its right or left; the right stick's up /
//              down adds a climb or sink (holds the height when centred and the left stick is idle, so height can be
//              changed without pointing up or down). The velocity eases toward that target (0.3 s); with both
//              sticks idle the player stops and holds the spot. The game's own forward / strafe inputs to the air
//              movement are passed as zero; fuel and the end of the hover stay the game's.
// Hooks: MinHook on the jet jump (thiscall, returns bool); the hover's call of the air movement 0x110BE0 (thiscall,
// five floats, ten callers) is redirected at its one call site in the jetting state, so the other callers are not
// touched. Both run inside the soldier update, on the main thread, as does OnPresent.

#include "Jet.h"

#include "Aim.h"
#include "EngineSurvey.h"
#include "FpGuard.h"
#include "GameBuild.h"
#include "Log.h"
#include "Net.h"
#include "Stereo.h"
#include "Xr.h"
#include "engine.h"

#include <windows.h>

#include <MinHook.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace SwbfVr
{

namespace Jet
{

namespace
{

/// Jump pack: the jump's angle above the ground = this + the head's pitch, kept between the two limits (radians).
constexpr float JUMP_ANGLE_AT_LEVEL_RADIANS = 0.785398f; // 45 degrees, the stock jump with the stick pushed forward
constexpr float JUMP_ANGLE_LOWEST_RADIANS = 0.523599f;   // 30 degrees
constexpr float JUMP_ANGLE_HIGHEST_RADIANS = 1.570796f;  // straight up
/// Hover: speeds at full stick (metres per second). The stock jet trooper runs at 6.5 (MaxSpeed) and strafes at 4.5;
/// the stock hover hands the air movement twice the move input ahead.
constexpr float HOVER_AHEAD_METERS_PER_SECOND = 9.0f;
constexpr float HOVER_BACK_METERS_PER_SECOND = 5.0f;
constexpr float HOVER_SIDE_METERS_PER_SECOND = 6.0f;
constexpr float HOVER_CLIMB_METERS_PER_SECOND = 6.0f; // right stick (JetPush 8 against gravity 20 climbs slowly)
constexpr float HOVER_SINK_METERS_PER_SECOND = 6.0f;
/// Hover: our eased velocity (the soldier's carries the game's lift, added each frame before the air movement).
float _hoverVelocity[3] = {};
/// Hover: the velocity eases toward the sticks' target with this time constant (seconds).
constexpr float HOVER_EASE_SECONDS = 0.3f;
constexpr float STICK_DEAD_ZONE = 0.15f;
/// Hover log: one line this often while hovering, at most this many lines a session.
constexpr DWORD HOVER_LOG_MILLISECONDS = 250;
constexpr unsigned HOVER_LOG_LINES = 120;
constexpr float DEGREES_PER_RADIAN = 57.29578f;

/// bool __thiscall EntitySoldier::JetJump(), written as __fastcall: ecx = the soldier, edx unused, no stack
/// arguments, the result in al.
using JetJumpFunction = bool(__fastcall*)(uintptr_t soldier, void* edx);
/// void __thiscall EntitySoldier air movement(float dt, float forward, float strafe, float third, float airControl),
/// written as __fastcall (ecx = the soldier, edx unused, five floats on the stack popped by the callee, as thiscall).
using AirMoveFunction =
    void(__fastcall*)(uintptr_t soldier, void* edx, float seconds, float forward, float strafe, float third, float air);

JetJumpFunction _originalJetJump = nullptr;
AirMoveFunction _originalAirMove = nullptr;
bool _hasTried = false;
uintptr_t _playerSoldier = 0;
// This frame's input and steering direction (OnPresent; read by the hooks in the next soldier update, same thread).
float _rightStickY = 0.0f;
float _leftStickX = 0.0f;
float _leftStickY = 0.0f;
bool _isSteerByHead = false;
bool _hasSteer = false;
float _steerForward[3] = {}; // world, unit, pitch included
float _steerRight[3] = {};   // world, unit, level
float _rightSign = 1.0f;     // which of the two level perpendiculars of the forward is the source's right
bool _isHovering = false;
DWORD _hoverTick = 0; // last frame the hover code ran for the player
float _hoverTarget[3] = {};
unsigned _jumpLogCount = 0;

/// True for the player's soldier while VR runs, if its class can be read.
bool IsPlayer(uintptr_t soldier)
{
    return soldier && soldier == _playerSoldier && Xr::IsRunning() &&
           !IsBadReadPtr(reinterpret_cast<void*>(soldier + OFFSET_soldierClass), 4);
}

bool IsHoverClass(uintptr_t soldier)
{
    const uintptr_t soldierClass = *reinterpret_cast<uintptr_t*>(soldier + OFFSET_soldierClass);
    return soldierClass && !IsBadReadPtr(reinterpret_cast<void*>(soldierClass + OFFSET_soldierClassJetHover), 1) &&
           *reinterpret_cast<uint8_t*>(soldierClass + OFFSET_soldierClassJetHover) != 0;
}

float Clamp(float value, float lowest, float highest)
{
    return value < lowest ? lowest : (value > highest ? highest : value);
}

/// A stick axis with the dead zone taken out and the rest stretched to 0..1.
float ShapeStick(float value)
{
    return value > STICK_DEAD_ZONE    ? (value - STICK_DEAD_ZONE) / (1 - STICK_DEAD_ZONE)
           : value < -STICK_DEAD_ZONE ? (value + STICK_DEAD_ZONE) / (1 - STICK_DEAD_ZONE)
                                      : 0.0f;
}

/// Takes this frame's steering direction from the left controller's or the head's pose in the world (camera
/// convention: rows right, up, back, position; -back is where it points). Main thread, once per presented frame.
void SampleSteer()
{
    FpGuard guard;
    float pose[16];
    _hasSteer = _isSteerByHead ? Stereo::GetLastHeadWorld(pose) : Aim::GetHandWorld(1, pose);
    if (!_hasSteer)
    {
        return;
    }
    float forward[3] = {-pose[8], -pose[9], -pose[10]};
    const float length = sqrtf(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    if (length < 0.5f)
    {
        _hasSteer = false;
        return;
    }
    for (float& axis : forward)
    {
        axis /= length;
    }
    // Right: level and square to the forward. Which of the two perpendiculars (-z, 0, x) or (z, 0, -x) is the
    // source's right is a fixed property of the world's axes; it is read from the source's right row while that is
    // clearly level, and kept while the source is rolled on its side.
    const float levelLength = sqrtf(forward[0] * forward[0] + forward[2] * forward[2]);
    float right[3] = {pose[0], 0.0f, pose[2]};
    if (levelLength > 0.2f)
    {
        right[0] = -forward[2] / levelLength;
        right[2] = forward[0] / levelLength;
        const float agreement = right[0] * pose[0] + right[2] * pose[2];
        if (agreement > 0.3f || agreement < -0.3f)
        {
            _rightSign = agreement > 0 ? 1.0f : -1.0f;
        }
        right[0] *= _rightSign;
        right[2] *= _rightSign;
    }
    else
    {
        // Pointing nearly straight up or down: the source's own right row, levelled.
        const float rightLength = sqrtf(right[0] * right[0] + right[2] * right[2]);
        if (rightLength < 0.1f)
        {
            _hasSteer = false;
            return;
        }
        right[0] /= rightLength;
        right[2] /= rightLength;
    }
    memcpy(_steerForward, forward, sizeof(forward));
    memcpy(_steerRight, right, sizeof(right));
}

// Hook of the jet jump: the game's jump first (fuel, impulse, state), then the impulse turned to the head's angle.
bool __fastcall JetJumpHook(uintptr_t soldier, void* edx)
{
    float* velocity = reinterpret_cast<float*>(soldier + OFFSET_soldierVelocity);
    // Not in a network game: the velocity is the host's to set, and it knows nothing of the head (the jump would be
    // pulled back to the host's own). The hover's take-off stays the game's too.
    const bool isPlayer = !Net::IsNetGame() && IsPlayer(soldier) && !IsHoverClass(soldier);
    float before[3] = {};
    if (isPlayer)
    {
        memcpy(before, velocity, sizeof(before));
    }
    const bool hasJumped = _originalJetJump(soldier, edx);
    float head[16];
    if (!hasJumped || !isPlayer || !Stereo::GetHeadPose(head))
    {
        return hasJumped;
    }
    FpGuard guard; // RAII: a clean floating-point state for our arithmetic, the game's restored at the end
    const float impulse[3] = {velocity[0] - before[0], velocity[1] - before[1], velocity[2] - before[2]};
    const float strength = sqrtf(impulse[0] * impulse[0] + impulse[1] * impulse[1] + impulse[2] * impulse[2]);
    // The body's forward, level (row +0x20 of its matrix).
    const float* forwardRow = reinterpret_cast<const float*>(soldier + OFFSET_soldierBodyMatrix + 0x20);
    const float levelLength = sqrtf(forwardRow[0] * forwardRow[0] + forwardRow[2] * forwardRow[2]);
    if (strength < 0.1f || levelLength < 0.1f)
    {
        return hasJumped;
    }
    // The head's pitch: its forward is -(back row), head[8..10]; up positive.
    const float headUp = -head[9] > 1 ? 1 : (-head[9] < -1 ? -1 : -head[9]);
    float angle = JUMP_ANGLE_AT_LEVEL_RADIANS + asinf(headUp);
    angle = angle < JUMP_ANGLE_LOWEST_RADIANS ? JUMP_ANGLE_LOWEST_RADIANS : angle;
    angle = angle > JUMP_ANGLE_HIGHEST_RADIANS ? JUMP_ANGLE_HIGHEST_RADIANS : angle;
    const float ahead = strength * cosf(angle) / levelLength;
    velocity[0] = before[0] + forwardRow[0] * ahead;
    velocity[1] = before[1] + strength * sinf(angle);
    velocity[2] = before[2] + forwardRow[2] * ahead;
    if (_jumpLogCount < 10)
    {
        ++_jumpLogCount;
        Log::Info(
            "jet",
            "jump pack: %.1f m/s at %.0f degrees above the ground (head pitch %.0f; the game's was %.0f)",
            static_cast<double>(strength),
            static_cast<double>(angle * DEGREES_PER_RADIAN),
            static_cast<double>(asinf(headUp) * DEGREES_PER_RADIAN),
            static_cast<double>(asinf(impulse[1] / strength) * DEGREES_PER_RADIAN));
    }
    return hasJumped;
}

// Replaces the hover's call of the air movement: for the player hovering in VR the velocity comes from the sticks
// and the steering direction, eased toward and set before the movement (which moves the soldier with it) and again
// after (the movement may apply gravity or air control to it; the hover code starts from it next frame). The game's
// forward / strafe inputs are passed as zero, so its own air control does not push toward the body's forward.
void __fastcall HoverMoveStub(
    uintptr_t soldier,
    void* edx,
    float seconds,
    float forward,
    float strafe,
    float third,
    float air)
{
    const bool isPlayerHovering = IsPlayer(soldier) && IsHoverClass(soldier) &&
                                  *reinterpret_cast<int*>(soldier + OFFSET_soldierAction) == SOLDIER_ACTION_JETTING;
    float* velocity = reinterpret_cast<float*>(soldier + OFFSET_soldierVelocity);
    float wanted[3] = {};
    if (isPlayerHovering)
    {
        FpGuard guard; // RAII, as above; ends before the game's code runs again
        const float climb = ShapeStick(_rightStickY) *
                            (_rightStickY > 0 ? HOVER_CLIMB_METERS_PER_SECOND : HOVER_SINK_METERS_PER_SECOND);
        if (_hasSteer)
        {
            const float ahead = ShapeStick(_leftStickY);
            const float side = ShapeStick(_leftStickX);
            const float aheadSpeed = ahead * (ahead > 0 ? HOVER_AHEAD_METERS_PER_SECOND : HOVER_BACK_METERS_PER_SECOND);
            const float sideSpeed = side * HOVER_SIDE_METERS_PER_SECOND;
            for (int axis = 0; axis < 3; ++axis)
            {
                _hoverTarget[axis] = _steerForward[axis] * aheadSpeed + _steerRight[axis] * sideSpeed;
            }
            _hoverTarget[1] += climb;
            const float ease = seconds > 0 && seconds < 0.5f ? 1.0f - expf(-seconds / HOVER_EASE_SECONDS) : 0.0f;
            // Eased from our own last velocity, not the soldier's: the game's hover code adds its lift to the
            // soldier's velocity every frame before this call, and easing from that left level flight climbing
            // ~16 deg and a dive held level (run642).
            if (!_isHovering)
            {
                memcpy(_hoverVelocity, velocity, sizeof(_hoverVelocity));
            }
            for (int axis = 0; axis < 3; ++axis)
            {
                wanted[axis] = _hoverVelocity[axis] + (_hoverTarget[axis] - _hoverVelocity[axis]) * ease;
                _hoverVelocity[axis] = wanted[axis];
                velocity[axis] = wanted[axis];
            }
            forward = 0.0f;
            strafe = 0.0f;
        }
        else
        {
            // No steering source (controller not tracked): the game's ahead and sideways, the right stick's height.
            _hoverTarget[0] = _hoverTarget[2] = 0.0f;
            _hoverTarget[1] = climb;
            wanted[1] = climb;
            velocity[1] = climb;
        }
        if (!_isHovering)
        {
            Log::Info(
                "jet",
                "hover: flown by the left stick along %s",
                !_hasSteer       ? "the game's forward (no source)"
                : _isSteerByHead ? "the head"
                                 : "the left controller");
        }
        _isHovering = true;
        _hoverTick = GetTickCount();
    }
    _originalAirMove(soldier, edx, seconds, forward, strafe, third, air);
    if (isPlayerHovering)
    {
        if (_hasSteer)
        {
            memcpy(velocity, wanted, sizeof(wanted));
        }
        else
        {
            velocity[1] = wanted[1];
        }
    }
}

void Install()
{
    if (!GameBuild::IsAvailable(GameBuild::Feature::JetPacks))
    {
        Log::Warning(
            "jet",
            "jet functions not found (%s is off for this game build): jet packs as in the game",
            GameBuild::FeatureName(GameBuild::Feature::JetPacks));
        return;
    }
    void* const jetJump = reinterpret_cast<void*>(GameBuild::Absolute(GameBuild::Address::JetJump));
    MH_Initialize();
    MH_STATUS status =
        MH_CreateHook(jetJump, reinterpret_cast<void*>(&JetJumpHook), reinterpret_cast<void**>(&_originalJetJump));
    if (status == MH_OK)
    {
        status = MH_EnableHook(jetJump);
    }
    // The call site: E8 rel32, the target relative to the next instruction. Its target is kept (it may be the
    // incremental linker's jump thunk, which leads to the same function) and the call is pointed at our stub.
    uint8_t* const call = reinterpret_cast<uint8_t*>(GameBuild::Absolute(GameBuild::Address::JetHoverMoveCall));
    bool isRedirected = false;
    if (call[0] == 0xE8)
    {
        int32_t relative = 0;
        memcpy(&relative, call + 1, 4);
        _originalAirMove = reinterpret_cast<AirMoveFunction>(reinterpret_cast<uintptr_t>(call) + 5 + relative);
        const int32_t toStub =
            static_cast<int32_t>(reinterpret_cast<uintptr_t>(&HoverMoveStub) - (reinterpret_cast<uintptr_t>(call) + 5));
        DWORD oldProtection = 0;
        if (VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &oldProtection))
        {
            memcpy(call + 1, &toStub, 4);
            VirtualProtect(call + 1, 4, oldProtection, &oldProtection);
            FlushInstructionCache(GetCurrentProcess(), call, 5);
            isRedirected = true;
        }
    }
    Log::Info(
        "jet",
        "jump pack hook %s, hover call %s",
        status == MH_OK ? "on" : "failed",
        isRedirected ? "redirected" : "not redirected (not a call)");
}

// While hovering, four times a second (the first HOVER_LOG_LINES of a session): the sticks, the steering direction,
// the target velocity, the position and the direction actually flown since the last line, with the angle between
// that and the target (no world-axis convention needed to read it: small = flies where it should).
void LogHover(DWORD now)
{
    static DWORD lastTick = 0;
    static unsigned lineCount = 0;
    static float lastPosition[3] = {};
    static bool hasLastPosition = false;
    if (!_isHovering || !_playerSoldier)
    {
        hasLastPosition = false;
        return;
    }
    if (now - lastTick < HOVER_LOG_MILLISECONDS || lineCount >= HOVER_LOG_LINES)
    {
        return;
    }
    FpGuard guard;
    const float seconds = static_cast<float>(now - lastTick) / 1000.0f;
    lastTick = now;
    ++lineCount;
    const float* position = reinterpret_cast<const float*>(_playerSoldier + OFFSET_soldierBodyMatrix + 0x30);
    const float* velocity = reinterpret_cast<const float*>(_playerSoldier + OFFSET_soldierVelocity);
    float moved[3] = {};
    for (int axis = 0; axis < 3; ++axis)
    {
        moved[axis] = hasLastPosition ? position[axis] - lastPosition[axis] : 0.0f;
    }
    const bool hadLastPosition = hasLastPosition;
    memcpy(lastPosition, position, sizeof(lastPosition));
    hasLastPosition = true;
    const float movedLength = sqrtf(moved[0] * moved[0] + moved[1] * moved[1] + moved[2] * moved[2]);
    const float targetLength = sqrtf(
        _hoverTarget[0] * _hoverTarget[0] + _hoverTarget[1] * _hoverTarget[1] + _hoverTarget[2] * _hoverTarget[2]);
    float offTarget = -1.0f; // degrees between the flown and the target direction; -1 = not measurable
    if (hadLastPosition && movedLength > 0.05f && targetLength > 0.5f)
    {
        const float cosine = (moved[0] * _hoverTarget[0] + moved[1] * _hoverTarget[1] + moved[2] * _hoverTarget[2]) /
                             (movedLength * targetLength);
        offTarget = acosf(Clamp(cosine, -1.0f, 1.0f)) * DEGREES_PER_RADIAN;
    }
    const float flownPitch = movedLength > 0.05f ? asinf(Clamp(moved[1] / movedLength, -1.0f, 1.0f)) : 0.0f;
    Log::Info(
        "jet",
        "hover: stick L %.2f %.2f R %.2f; steer %s fwd (%.2f %.2f %.2f) pitch %.0f; target (%.1f %.1f %.1f); "
        "vel (%.1f %.1f %.1f); pos (%.1f %.1f %.1f); flown %.1f m/s pitch %.0f, %.0f deg off target",
        static_cast<double>(_leftStickX),
        static_cast<double>(_leftStickY),
        static_cast<double>(_rightStickY),
        !_hasSteer       ? "none"
        : _isSteerByHead ? "head"
                         : "left",
        static_cast<double>(_steerForward[0]),
        static_cast<double>(_steerForward[1]),
        static_cast<double>(_steerForward[2]),
        static_cast<double>(asinf(Clamp(_steerForward[1], -1.0f, 1.0f)) * DEGREES_PER_RADIAN),
        static_cast<double>(_hoverTarget[0]),
        static_cast<double>(_hoverTarget[1]),
        static_cast<double>(_hoverTarget[2]),
        static_cast<double>(velocity[0]),
        static_cast<double>(velocity[1]),
        static_cast<double>(velocity[2]),
        static_cast<double>(position[0]),
        static_cast<double>(position[1]),
        static_cast<double>(position[2]),
        static_cast<double>(hadLastPosition && seconds > 0 ? movedLength / seconds : 0.0f),
        static_cast<double>(flownPitch * DEGREES_PER_RADIAN),
        static_cast<double>(offTarget));
}

} // namespace

void OnPresent(float rightStickY, float leftStickX, float leftStickY, bool isSteerByHead)
{
    _rightStickY = rightStickY;
    _leftStickX = leftStickX;
    _leftStickY = leftStickY;
    _isSteerByHead = isSteerByHead;
    _playerSoldier = EngineSurvey::Resolve("soldier");
    SampleSteer();
    const DWORD now = GetTickCount();
    if (_isHovering && now - _hoverTick > 200)
    {
        _isHovering = false;
        Log::Info("jet", "hover ended");
    }
    LogHover(now);
    if (!_hasTried && Xr::IsRunning())
    {
        _hasTried = true;
        Install();
    }
}

bool IsHovering()
{
    return _isHovering;
}

} // namespace Jet

} // namespace SwbfVr
