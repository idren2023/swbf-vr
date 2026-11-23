// Squad.cpp: squad commands by hand gesture (PLAN 33, idren2023 2026-10-13).
//
// The game (static reading, 2026-10-14, docs/offsets.md "Squad commands"): the soldier update 0x11ABB8 builds a
// SquadMessage on a press of one of its four squad buttons and calls SquadCommand 0x2208F0, which plays the voice
// line, shows the HUD's squad command display and sends the message to every AI soldier of the player's team within
// a radius (45 m for commands 0-1, 30 m otherwise; three times that in a vehicle; field-context Follow Me skips AI
// in vehicles). So Follow Me already reaches every friendly nearby, not only the one under the crosshair, and the
// game's own limits apply. We call the same function with the same message.
//
// Gestures, per hand, in the torso frame (Gesture::GetTorso: origin at the head, upright, turned like the body):
//   arm      the grip pressed while the hand is empty and raised: 30 cm below to 30 cm above the head, at least
//            20 cm in front of it (the ears stay the ear menu's; 5 cm until headset run 18), within 75 cm across (b206; 22 / 25 / 10 cm before:
//            hard to find in headset run 14). A light tick when an empty hand comes into that space, a firmer one when
//            armed. Once armed, only taking hold of something disarms (not passing an ear), and the ear menu stays
//            shut (IsArmed).
//   strokes  while armed, a stroke starts when the hand moves faster than 0.45 m/s and ends below 0.25 m/s. A
//            stroke that has gone 20 cm forward, nearly straight (path at most 1.2 times the distance, sideways and
//            vertical at most half of it), at a peak above 1.2 m/s, is a jab: Move Out. One that has gone 18 cm
//            back toward the face the same way, at a peak above 1.2 m/s, is a beckon: Follow Me (idren2023
//            2026-10-14). One that has gone 18 cm down the same way at a peak above 0.9 m/s is a pull: Hold Position.
//            One that has gone 20 cm across in front of the face toward the other side (the right hand to the left,
//            the left hand to the right) at a peak above 1.2 m/s is a sweep: At Ease (b206, idren2023 after headset run
//            14: the horizontal circle of b191 set off other actions).
// One command per grip: letting go re-arms. Headset run 14: all four worked with either hand (b191's circle).
// Headset run 15 (idren2023): too hard to trigger. Default since then: jabs in the HUD's four directions (the game's
// squad display: up Move Out, left At Ease, right Follow Me, down Hold Position; down = back toward the face), 15 cm
// at a peak above 0.9 m/s, nearly straight. The strokes above stay as the "immersive" set, with b191's slow circle
// (the hand's path in the horizontal plane turning 320 degrees over at least 30 cm with under 15 cm of height change,
// at any speed) for At Ease instead of the sweep. While a hand is armed, the game's own squad
// display is shown (SquadCommandGroup::Show every frame), so the player sees which way is which.

#include "Squad.h"

#include "Aim.h"
#include "Feedback.h"
#include "FpGuard.h"
#include "GameBuild.h"
#include "Gesture.h"
#include "Holster.h"
#include "HudDraws.h"
#include "Log.h"
#include "Net.h"
#include "Xr.h"
#include "engine.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace SwbfVr
{

namespace Squad
{

namespace
{

/// void __cdecl SquadCommand(SquadMessage*), 0x2208F0.
using SquadCommandFunction = void(__cdecl*)(void* message);
/// SquadMessage* __thiscall SquadMessage::SquadMessage(int command, void* issuer), 0x10C0B0.
using SquadMessageFunction = void*(__thiscall*)(void* message, int command, void* issuer);

/// Controllable: the player object the squad message names as its issuer (the soldier update passes
/// [Controllable+0xCC], getter 0x8C310). Static reading, 2026-10-14.
constexpr uintptr_t OFFSET_controllableIssuer = 0xCC;
/// Entity: its generation, the second half of an entity handle (getter 0x74AC0 on entity+0xB4). Static reading.
constexpr uintptr_t OFFSET_entityGeneration = 0xB8;
/// SquadMessage: the entity handle Follow Me carries (the soldier itself; 0x10C100 at 0x11B1CC).
constexpr uintptr_t OFFSET_messageHandle = 0xC;
/// Room for a SquadMessage (0x14 bytes used; the soldier update reserves more on its stack).
constexpr size_t MESSAGE_SIZE = 0x40;
constexpr uintptr_t SOLDIER_SIZE = 0xB50;

/// The raised space, torso frame (metres; y up, z back).
/// Headset run 18 (idren2023: hard to use, fighting the ear menu): at least 20 cm ahead of the head (was 5), clear of the
/// ear zones, which now reach 9 cm ahead (Gesture).
constexpr float RAISED_LOWEST = -0.30f, RAISED_HIGHEST = 0.30f, RAISED_NEAREST_AHEAD = 0.20f, RAISED_WIDEST = 0.75f;
/// Strokes: speeds that start and end one (m/s), measured over this time (s).
constexpr float STROKE_START_SPEED = 0.45f, STROKE_END_SPEED = 0.25f, SPEED_WINDOW_SECONDS = 0.1f;
constexpr float JAB_DISTANCE = 0.20f, JAB_PEAK_SPEED = 1.2f;
constexpr float BECKON_DISTANCE = 0.18f, BECKON_PEAK_SPEED = 1.2f;
constexpr float PULL_DISTANCE = 0.18f, PULL_PEAK_SPEED = 0.9f;
/// Direction set: one jab in any of the four directions.
constexpr float DIRECTION_JAB_DISTANCE = 0.15f, DIRECTION_JAB_PEAK_SPEED = 0.9f;
/// Circle (immersive set): sample spacing, total turn (radians, 320 degrees), path and height limits (metres).
constexpr float CIRCLE_SAMPLE = 0.025f, CIRCLE_TURN = 5.585f, CIRCLE_PATH = 0.30f, CIRCLE_HEIGHT = 0.15f;
constexpr float PI = 3.14159265f, TWO_PI = 6.2831853f;
/// StatusDisplay: its SquadCommandGroup (offsets "Displays"); Show's context for commands on foot.
constexpr uintptr_t OFFSET_statusDisplaySquadGroup = 0x30D0;
constexpr int SQUAD_CONTEXT_FIELD = 0;
using SquadDisplayShowFunction = void(__thiscall*)(void* group, int context, int highlighted);
/// A stroke's path at most this times its distance; the other two directions at most this share of the main one.
constexpr float STRAIGHT_PATH_FACTOR = 1.2f, STRAIGHT_SIDE_SHARE = 0.5f;
/// Position history for the speed: enough for 0.1 s at 300 presented frames per second.
constexpr int HISTORY_SIZE = 32;

struct Sample
{
    double seconds;
    float position[3];
};

struct HandState
{
    bool wasRaisedAndEmpty;
    bool wasGripped;
    bool wasAtGunOpen; // left hand: on or near the gun in the last frame without the grip held
    bool isArmed;
    bool isDone; // a command was given with this grip
    Sample history[HISTORY_SIZE];
    int historyCount, historyNext;
    // stroke
    bool isStroking;
    float strokeStart[3], strokePrevious[3], strokePath, strokePeakSpeed;
    // circle
    float circleSample[3], circleTurn, circlePath, circleLowest, circleHighest;
    bool hasCircleHeading;
    float circleHeading;
};

HandState _hands[2];
bool _isImmersiveSet = false; // swbf-vr.ini: squad_gestures
volatile LONG _requested = -1;

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

float Length(const float* vector)
{
    return sqrtf(vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2]);
}

// Position of a point (rest space) in a frame given as a camera-style matrix (rows right, up, back, position).
void ToLocal(const float* frameMatrix, const float* point, float* localPoint)
{
    const float offset[3] = {point[0] - frameMatrix[12], point[1] - frameMatrix[13], point[2] - frameMatrix[14]};
    for (int axis = 0; axis < 3; ++axis)
    {
        localPoint[axis] = offset[0] * frameMatrix[axis * 4] + offset[1] * frameMatrix[axis * 4 + 1] +
                           offset[2] * frameMatrix[axis * 4 + 2];
    }
}

const char* CommandName(Command command)
{
    switch (command)
    {
    case Command::MoveOut:
        return "Move Out";
    case Command::AtEase:
        return "At Ease";
    case Command::HoldPosition:
        return "Hold Position";
    case Command::FollowMe:
        return "Follow Me";
    }
    return "?";
}

// The player's soldier (on foot), or 0.
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
    if (!controllable ||
        IsBadReadPtr(reinterpret_cast<void*>(controllable - OFFSET_soldierControllable), SOLDIER_SIZE) ||
        !Holster::IsObjectType(controllable, "EntitySoldier"))
    {
        return 0;
    }
    return controllable - OFFSET_soldierControllable;
}

// Gives a command to the game as the soldier's squad button does (0x11B15E-0x11B1F8).
bool Give(Command command)
{
    if (!GameBuild::IsAvailable(GameBuild::Feature::Squad))
    {
        Log::Info("squad", "%s not given: squad commands are off for this game build", CommandName(command));
        return false;
    }
    const uintptr_t soldier = PlayerSoldier();
    if (!soldier)
    {
        Log::Info("squad", "%s not given: the player is not a soldier on foot", CommandName(command));
        return false;
    }
    if (Net::IsNetGame())
    {
        // SquadCommand called here would only play the voice line: in the client world it returns before the order
        // is given. The order reaches the host as the game's own squad button of this turn: the control state's
        // bits 8, 9, 10, 11 are the commands 1, 3, 0, 2 (docs/research/net/events.md).
        static const int BUTTON_OF_COMMAND[4] = {10, 8, 11, 9};
        const int number = static_cast<int>(command);
        if (number < 0 || number > 3)
        {
            return false;
        }
        Net::PressButton(BUTTON_OF_COMMAND[number]);
        Log::Info("squad", "%s given as the game's squad button (network game)", CommandName(command));
        return true;
    }
    void* const issuer = *reinterpret_cast<void**>(soldier + OFFSET_soldierControllable + OFFSET_controllableIssuer);
    if (!issuer)
    {
        Log::Info("squad", "%s not given: the soldier has no player object", CommandName(command));
        return false;
    }
    alignas(16) uint8_t message[MESSAGE_SIZE] = {};
    const auto construct =
        reinterpret_cast<SquadMessageFunction>(GameBuild::Absolute(GameBuild::Address::SquadMessage));
    construct(message, static_cast<int>(command), issuer);
    if (command == Command::FollowMe)
    { // the handle of the one to follow: the soldier itself (its vfunc 8 returns `this`)
        uint32_t* const handle = reinterpret_cast<uint32_t*>(message + OFFSET_messageHandle);
        handle[0] = static_cast<uint32_t>(soldier);
        handle[1] = *reinterpret_cast<uint32_t*>(soldier + OFFSET_entityGeneration);
    }
    const auto squadCommand =
        reinterpret_cast<SquadCommandFunction>(GameBuild::Absolute(GameBuild::Address::SquadCommand));
    squadCommand(message);
    Log::Info("squad", "%s given", CommandName(command));
    return true;
}

void ResetMotion(HandState& state, const float* position)
{
    state.historyCount = state.historyNext = 0;
    state.isStroking = false;
    memcpy(state.circleSample, position, sizeof(state.circleSample));
    state.circleTurn = state.circlePath = 0;
    state.circleLowest = state.circleHighest = position[1];
    state.hasCircleHeading = false;
}

// Immersive set: At Ease by a circle in the horizontal plane, at any speed (b191). `position` in the torso frame.
bool ReadCircle(HandState& state, const float* position)
{
    state.circleLowest = position[1] < state.circleLowest ? position[1] : state.circleLowest;
    state.circleHighest = position[1] > state.circleHighest ? position[1] : state.circleHighest;
    const float acrossX = position[0] - state.circleSample[0], acrossZ = position[2] - state.circleSample[2];
    const float across = sqrtf(acrossX * acrossX + acrossZ * acrossZ);
    if (across < CIRCLE_SAMPLE)
    {
        return false;
    }
    const float heading = atan2f(acrossX, -acrossZ);
    if (state.hasCircleHeading)
    {
        float turn = heading - state.circleHeading;
        while (turn > PI)
        {
            turn -= TWO_PI;
        }
        while (turn < -PI)
        {
            turn += TWO_PI;
        }
        state.circleTurn += turn;
    }
    state.hasCircleHeading = true;
    state.circleHeading = heading;
    state.circlePath += across;
    memcpy(state.circleSample, position, sizeof(state.circleSample));
    return fabsf(state.circleTurn) >= CIRCLE_TURN && state.circlePath >= CIRCLE_PATH &&
           state.circleHighest - state.circleLowest < CIRCLE_HEIGHT;
}

// Shows the game's squad display (all four commands, none highlighted) for this frame: it fades unless shown again.
void ShowSquadDisplay()
{
    static bool hasLoggedFailure = false, hasLoggedShow = false;
    const uintptr_t display = HudDraws::GetStatusDisplay();
    const uintptr_t group = display ? display + OFFSET_statusDisplaySquadGroup : 0;
    if (!GameBuild::IsAvailable(GameBuild::Feature::Squad) || !group ||
        IsBadReadPtr(reinterpret_cast<void*>(group), 0xC0 * 5) || !Holster::IsObjectType(display, "StatusDisplay"))
    {
        if (!hasLoggedFailure)
        {
            hasLoggedFailure = true;
            Log::Info(
                "squad",
                "squad display not shown: no StatusDisplay seen yet (display %08x)",
                static_cast<unsigned>(display));
        }
        return;
    }
    if (!hasLoggedShow)
    {
        hasLoggedShow = true;
        Log::Info("squad", "squad display shown (StatusDisplay %08x)", static_cast<unsigned>(display));
    }
    const auto show =
        reinterpret_cast<SquadDisplayShowFunction>(GameBuild::Absolute(GameBuild::Address::SquadDisplayShow));
    show(reinterpret_cast<void*>(group), SQUAD_CONTEXT_FIELD, -1);
}

// The command a moving armed hand makes, or -1. `position` is in the torso frame.
int ReadMotion(HandState& state, const float* position, double now)
{
    // Speed over the last SPEED_WINDOW_SECONDS (the oldest kept sample at least that old).
    Sample& newest = state.history[state.historyNext];
    newest.seconds = now;
    memcpy(newest.position, position, sizeof(newest.position));
    state.historyNext = (state.historyNext + 1) % HISTORY_SIZE;
    if (state.historyCount < HISTORY_SIZE)
    {
        ++state.historyCount;
    }
    const Sample* reference = nullptr;
    for (int age = 1; age < state.historyCount; ++age)
    {
        const Sample& sample = state.history[(state.historyNext - 1 - age + 2 * HISTORY_SIZE) % HISTORY_SIZE];
        reference = &sample;
        if (now - sample.seconds >= SPEED_WINDOW_SECONDS)
        {
            break;
        }
    }
    float speed = 0;
    if (reference && now > reference->seconds)
    {
        const float moved[3] = {
            position[0] - reference->position[0],
            position[1] - reference->position[1],
            position[2] - reference->position[2]};
        speed = Length(moved) / static_cast<float>(now - reference->seconds);
    }

    // Strokes: jab forward, beckon back, pull down, sweep across.
    if (!state.isStroking && speed > STROKE_START_SPEED && reference)
    {
        state.isStroking = true;
        memcpy(state.strokeStart, reference->position, sizeof(state.strokeStart));
        memcpy(state.strokePrevious, reference->position, sizeof(state.strokePrevious));
        state.strokePath = 0;
        state.strokePeakSpeed = 0;
    }
    if (state.isStroking)
    {
        const float step[3] = {
            position[0] - state.strokePrevious[0],
            position[1] - state.strokePrevious[1],
            position[2] - state.strokePrevious[2]};
        state.strokePath += Length(step);
        memcpy(state.strokePrevious, position, sizeof(state.strokePrevious));
        state.strokePeakSpeed = speed > state.strokePeakSpeed ? speed : state.strokePeakSpeed;
        const float moved[3] = {
            position[0] - state.strokeStart[0],
            position[1] - state.strokeStart[1],
            position[2] - state.strokeStart[2]};
        const float distance = Length(moved);
        const bool isStraight = state.strokePath <= STRAIGHT_PATH_FACTOR * distance;
        const float forward = -moved[2], down = -moved[1];
        if (!_isImmersiveSet)
        {
            // The HUD's four directions in the horizontal plane: the main one at least DIRECTION_JAB_DISTANCE, the
            // other two at most half of it.
            const float right = moved[0];
            const bool isAlong = fabsf(forward) > fabsf(right);
            const float main = isAlong ? fabsf(forward) : fabsf(right), side = isAlong ? fabsf(right) : fabsf(forward);
            if (isStraight && main >= DIRECTION_JAB_DISTANCE && state.strokePeakSpeed >= DIRECTION_JAB_PEAK_SPEED &&
                fabsf(moved[1]) <= STRAIGHT_SIDE_SHARE * main && side <= STRAIGHT_SIDE_SHARE * main)
            {
                const Command command = isAlong     ? (forward > 0 ? Command::MoveOut : Command::HoldPosition)
                                        : right > 0 ? Command::FollowMe
                                                    : Command::AtEase;
                return static_cast<int>(command);
            }
            if (speed < STROKE_END_SPEED)
            {
                state.isStroking = false;
            }
            return -1;
        }
        if (isStraight && forward >= JAB_DISTANCE && state.strokePeakSpeed >= JAB_PEAK_SPEED &&
            fabsf(moved[0]) <= STRAIGHT_SIDE_SHARE * forward && fabsf(moved[1]) <= STRAIGHT_SIDE_SHARE * forward)
        {
            return static_cast<int>(Command::MoveOut);
        }
        if (isStraight && -forward >= BECKON_DISTANCE && state.strokePeakSpeed >= BECKON_PEAK_SPEED &&
            fabsf(moved[0]) <= STRAIGHT_SIDE_SHARE * -forward && fabsf(moved[1]) <= STRAIGHT_SIDE_SHARE * -forward)
        {
            return static_cast<int>(Command::FollowMe);
        }
        if (isStraight && down >= PULL_DISTANCE && state.strokePeakSpeed >= PULL_PEAK_SPEED &&
            fabsf(moved[0]) <= STRAIGHT_SIDE_SHARE * down && fabsf(moved[2]) <= STRAIGHT_SIDE_SHARE * down)
        {
            return static_cast<int>(Command::HoldPosition);
        }
        if (speed < STROKE_END_SPEED)
        {
            state.isStroking = false;
        }
    }
    if (_isImmersiveSet && ReadCircle(state, position))
    {
        return static_cast<int>(Command::AtEase);
    }
    return -1;
}

} // namespace

void SetGestureSet(bool isImmersive)
{
    _isImmersiveSet = isImmersive;
    Log::Info(
        "squad",
        "gestures: %s",
        isImmersive ? "immersive (jab Move Out, beckon Follow Me, pull down Hold Position, circle At Ease)"
                    : "HUD directions (jab forward Move Out, left At Ease, right Follow Me, back Hold Position)");
}

bool IsArmed(int hand)
{
    return hand >= 0 && hand < 2 && _hands[hand].isArmed && !_hands[hand].isDone;
}

void Request(Command command)
{
    InterlockedExchange(&_requested, static_cast<LONG>(command));
}

// Main thread, once per presented frame, after Gesture::OnPresent and Holster::OnPresent.
void OnPresent()
{
    FpGuard guard;
    const LONG requested = InterlockedExchange(&_requested, -1);
    if (requested >= 0)
    {
        Give(static_cast<Command>(requested));
    }
    float torso[16];
    if (!Xr::IsRunning() || !Gesture::GetTorso(torso))
    {
        memset(_hands, 0, sizeof(_hands));
        return;
    }
    const double now = NowSeconds();
    bool isAnyHandInSpace = false;
    for (int hand = 0; hand < 2; ++hand)
    {
        HandState& state = _hands[hand];
        const Gesture::Hand& gesture = Gesture::CurrentHand(hand);
        if (!gesture.isTracked)
        {
            memset(&state, 0, sizeof(state));
            continue;
        }
        float position[3];
        ToLocal(torso, &gesture.pose[12], position);
        // Empty: the right hand while the holsters leave it with nothing; the left one while it is off the gun. Aim
        // counts the left hand as at the gun whenever its grip is held (anywhere), so its nearness is taken from the
        // frames before the grip; once armed, only a two-handed grip takes it.
        if (hand == 1 && !gesture.isGripped)
        {
            state.wasAtGunOpen = Aim::IsLeftHandAtGun();
        }
        const bool isEmpty = hand == 0 ? Holster::HoldsNothing() : !state.wasAtGunOpen && !Aim::IsTwoHanded();
        const bool isAtEar = gesture.zone == Gesture::Zone::EarLeft || gesture.zone == Gesture::Zone::EarRight;
        const bool isRaised = position[1] >= RAISED_LOWEST && position[1] <= RAISED_HIGHEST &&
                              -position[2] >= RAISED_NEAREST_AHEAD &&
                              sqrtf(position[0] * position[0] + position[2] * position[2]) <= RAISED_WIDEST;
        const bool isRaisedAndEmpty = isEmpty && isRaised && !isAtEar;
        if (isRaisedAndEmpty && !state.wasRaisedAndEmpty && !gesture.isGripped)
        {
            Feedback::Pulse(hand, 0.01f, 0.15f); // the gesture space's tick
        }
        state.wasRaisedAndEmpty = isRaisedAndEmpty;

        const bool isPressed = gesture.isGripped && !state.wasGripped;
        state.wasGripped = gesture.isGripped;
        if (!gesture.isGripped)
        {
            if (state.isArmed && !state.isDone)
            {
                Log::Info("squad", "%s hand: grip let go, no command", hand ? "left" : "right");
            }
            state.isArmed = state.isDone = false;
            continue;
        }
        if (isPressed && isRaisedAndEmpty)
        {
            state.isArmed = true;
            state.isDone = false;
            ResetMotion(state, position);
            Feedback::Pulse(hand, 0.025f, 0.4f);
            Log::Info(
                "squad",
                "%s hand armed at %.0f %.0f %.0f cm (right, up, ahead of the head)",
                hand ? "left" : "right",
                static_cast<double>(position[0] * 100),
                static_cast<double>(position[1] * 100),
                static_cast<double>(-position[2] * 100));
        }
        if (!state.isArmed || state.isDone)
        {
            continue;
        }
        isAnyHandInSpace = true; // armed: the display shows which way is which (run213: shown for any free hand ahead,
                                 // also on the spawn screen and in the pause menu, it was up nearly all the time)
        if (!isEmpty)
        { // took hold of something
            state.isArmed = false;
            Log::Info(
                "squad",
                "%s hand: disarmed (%s)",
                hand ? "left" : "right",
                hand == 0            ? "holds a weapon"
                : Aim::IsTwoHanded() ? "two-handed grip"
                                     : "at the gun");
            continue;
        }
        const int command = ReadMotion(state, position, now);
        if (command >= 0)
        {
            state.isDone = true;
            if (Give(static_cast<Command>(command)))
            {
                Feedback::Pulse(hand, 0.08f, 0.8f);
            }
        }
    }
    if (isAnyHandInSpace && PlayerSoldier())
    {
        ShowSquadDisplay();
    }
}

} // namespace Squad

} // namespace SwbfVr
