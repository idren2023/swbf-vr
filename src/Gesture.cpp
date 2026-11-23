// Gesture.cpp: where the hands are relative to the head and the body, for controls that are done by motion.
//
// Everything is worked out in the resting-pose space the headset code already uses (x right, y up, -z ahead,
// origin at the resting head, metres). Two frames are derived each presented frame:
//   head   the head's pose (ears, eye)
//   torso  under the head, upright, turned like the body. The body's heading is not tracked, so it is
//          estimated: it follows the head's heading only when the head has turned more than 40 degrees away from
//          it, which keeps hips and chest in place while the player looks around.
// A hand is "in a zone" when its controller's aim-pose origin is within the zone's radius of the zone's centre; it
// leaves at 1.3 times the radius. Zones (centres in metres, in the frame named):
//   ears   head   (+-0.10, -0.02,  0.02)  r 0.14
//   eye    head   ( 0.05, -0.10, -0.22)   r 0.12, right hand only, and the weapon must point where the head looks
//   chest  torso  (+-0.15, -0.30, -0.12)  r 0.15
//   hips   torso  (+-0.22, -0.60, -0.02)  r 0.18
//   under the gun: left hand, in the right hand's frame (0, -0.10, -0.15), r 0.11
// Also per hand: palm up (the controller rolled so that the palm side faces up) and the grip button.
// None of the numbers has been tried in a headset; they are in one table to be tuned there.

#include "Gesture.h"

#include "FpGuard.h"
#include "Log.h"
#include "Stereo.h"
#include "Xr.h"

#include <windows.h>

#include <cmath>
#include <cstring>

namespace SwbfVr
{

namespace Gesture
{

namespace
{

/// The frame a zone's centre is given in.
enum class ZoneFrame
{
    Head,
    Torso,
    RightHand
};

struct ZoneDefinition
{
    Zone zone;
    ZoneFrame frame;
    float x, y, z, radius; // centre and radius, metres
};

constexpr ZoneDefinition ZONES[] = {
    // the ears: 11 cm round (14 until headset run 18: reaching 12 cm ahead, into the squad gestures' space)
    {Zone::EarRight, ZoneFrame::Head, 0.10f, -0.02f, 0.02f, 0.11f},
    {Zone::EarLeft, ZoneFrame::Head, -0.10f, -0.02f, 0.02f, 0.11f},
    {Zone::Eye, ZoneFrame::Head, 0.05f, -0.10f, -0.22f, 0.12f},
    {Zone::ChestRight, ZoneFrame::Torso, 0.15f, -0.30f, -0.12f, 0.15f},
    {Zone::ChestLeft, ZoneFrame::Torso, -0.15f, -0.30f, -0.12f, 0.15f},
    {Zone::HipRight, ZoneFrame::Torso, 0.22f, -0.60f, -0.02f, 0.18f},
    {Zone::HipLeft, ZoneFrame::Torso, -0.22f, -0.60f, -0.02f, 0.18f},
    {Zone::UnderGun, ZoneFrame::RightHand, 0.0f, -0.10f, -0.15f, 0.11f},
    {Zone::BeltLeft, ZoneFrame::Torso, -0.12f, -0.52f, -0.20f, 0.10f}, // Holster AMMO_POSE (replaced by the box below)
};
/// The ammo canister on the belt (Holster AMMO_POSE, item_powerup_ammo at Holster's AMMO_MODEL_SCALE 0.27): a box
/// round the drawn canister itself, in the torso frame (idren2023 2026-10-24: hard to grab; the zone was a 10 cm ball on
/// the model's origin, its base, 6.5 cm under the canister's middle, and was tested at the controller's tip). The
/// canister: 6.8 x 12.9 x 6.5 cm, its middle 6.45 cm over its base; the box adds BELT_AMMO_MARGIN_METERS each way and
/// is tested at the left palm (the controller's grip pose), where the canister sits once taken.
constexpr float BELT_AMMO_CENTRE[3] = {-0.1235f, -0.4555f, -0.20f};
constexpr float BELT_AMMO_HALF_SIZE[3] = {0.034f, 0.0645f, 0.0325f};
constexpr float BELT_AMMO_MARGIN_METERS = 0.015f;
const char* const ZONE_NAMES[] = {
    "no zone",
    "right ear",
    "left ear",
    "eye",
    "right chest",
    "left chest",
    "right hip",
    "left hip",
    "under the gun",
    "left belt"};

/// A hand leaves a zone only at this factor times the zone's radius (so it does not flicker at the edge).
constexpr float ZONE_LEAVE_FACTOR = 1.3f;
/// The torso turns with the head only beyond this angle between them (40 degrees).
constexpr float TORSO_FOLLOW_RADIANS = 0.70f;
/// The head's forward axis must have at least this squared length in the horizontal plane for a heading (not
/// looking nearly straight up or down).
constexpr float MINIMUM_FORWARD_LENGTH_SQUARED = 0.04f;
constexpr float PI = 3.14159265f;
constexpr float TWO_PI = 6.2831853f;
/// Grip button: pressed beyond this to take hold, released below that to let go.
constexpr float GRIP_TAKE_HOLD_PRESS = 0.7f;
constexpr float GRIP_KEEP_HOLD_PRESS = 0.4f;
/// Palm up: the palm axis's upward component must exceed this to begin (35 degrees off straight up), and that to
/// stay (53 degrees; was 45 / 63: came up too readily, headset run 8).
constexpr float PALM_UP_TO_BEGIN = 0.82f;
constexpr float PALM_UP_TO_STAY = 0.60f;
/// Eye zone: the cosine of the angle between the weapon and the view must exceed this to enter (about 30 degrees),
/// and that to stay (37 degrees).
constexpr float EYE_ALIGNMENT_TO_ENTER = 0.87f;
constexpr float EYE_ALIGNMENT_TO_STAY = 0.80f;

Hand _hands[2];
float _torsoYawRadians = 0; // positive to the left (about +y)
bool _hasTorso = false;
float _torso[16]; // the torso frame of this presented frame, while _hasTorso
bool _isLogging = true;

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

} // namespace

const Hand& CurrentHand(int hand)
{
    return _hands[hand & 1];
}

const char* ZoneName(Zone zone)
{
    return zone >= Zone::None && zone <= Zone::BeltLeft ? ZONE_NAMES[static_cast<int>(zone)] : "?";
}

bool GetTorso(float* matrix)
{
    if (!_hasTorso)
    {
        return false;
    }
    memcpy(matrix, _torso, sizeof(_torso));
    return true;
}

void SetLogging(bool isOn)
{
    _isLogging = isOn;
}

// Main thread, once per presented frame, after Aim::OnPresent.
void OnPresent()
{
    FpGuard guard;
    float head[16], hands[2][16];
    Xr::ControllerInput input;
    Xr::GetControllerInput(&input);
    const bool hasHead = Xr::IsRunning() && Stereo::GetHeadPose(head);
    bool isTracked[2];
    for (int hand = 0; hand < 2; ++hand)
    {
        isTracked[hand] = hasHead && Xr::GetHandPose(hand, hands[hand]);
    }
    float torso[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (hasHead)
    {
        // Head heading from its forward axis; the torso follows beyond 40 degrees.
        const float forwardX = -head[8], forwardZ = -head[10];
        if (forwardX * forwardX + forwardZ * forwardZ > MINIMUM_FORWARD_LENGTH_SQUARED)
        {
            const float headYaw = atan2f(-forwardX, -forwardZ);
            if (!_hasTorso)
            {
                _torsoYawRadians = headYaw;
                _hasTorso = true;
            }
            float yawDifference = headYaw - _torsoYawRadians;
            while (yawDifference > PI)
            {
                yawDifference -= TWO_PI;
            }
            while (yawDifference < -PI)
            {
                yawDifference += TWO_PI;
            }
            const float limit = TORSO_FOLLOW_RADIANS;
            if (yawDifference > limit)
            {
                _torsoYawRadians += yawDifference - limit;
            }
            else if (yawDifference < -limit)
            {
                _torsoYawRadians += yawDifference + limit;
            }
        }
        const float cosine = cosf(_torsoYawRadians), sine = sinf(_torsoYawRadians);
        torso[0] = cosine;
        torso[2] = -sine;
        torso[8] = sine;
        torso[10] = cosine; // yaw about +y, left positive
        torso[12] = head[12];
        torso[13] = head[13];
        torso[14] = head[14];
        memcpy(_torso, torso, sizeof(_torso));
    }
    else
    {
        _hasTorso = false;
    }

    for (int hand = 0; hand < 2; ++hand)
    {
        Hand& state = _hands[hand];
        const Zone zoneBefore = state.zone;
        const bool wasPalmUp = state.isPalmUp;
        if (!isTracked[hand])
        {
            state = Hand{};
            if (zoneBefore != Zone::None && _isLogging)
            {
                Log::Info("gesture", "%s hand: tracking lost", hand ? "left" : "right");
            }
            continue;
        }
        state.isTracked = true;
        memcpy(state.pose, hands[hand], sizeof(state.pose));
        state.isGripped = input.isValid && (state.isGripped ? input.grip[hand] > GRIP_KEEP_HOLD_PRESS
                                                            : input.grip[hand] > GRIP_TAKE_HOLD_PRESS);
        // Palm: the left hand's palm faces the controller's +x, the right hand's its -x. Up = that axis points up.
        const float palmY = hand == 1 ? hands[hand][1] : -hands[hand][1];
        state.isPalmUp = state.isPalmUp ? palmY > PALM_UP_TO_STAY : palmY > PALM_UP_TO_BEGIN;
        Zone zone = Zone::None;
        float bestDistanceRatio = 1e9f;
        for (const ZoneDefinition& definition : ZONES)
        {
            if (definition.zone == Zone::UnderGun && (hand != 1 || !isTracked[0]))
            {
                continue;
            }
            if ((definition.zone == Zone::Eye && hand != 0) || (definition.zone == Zone::BeltLeft && hand != 1))
            {
                continue;
            }
            const float* frame = definition.frame == ZoneFrame::Head    ? head
                                 : definition.frame == ZoneFrame::Torso ? torso
                                                                        : hands[0];
            if (definition.zone == Zone::BeltLeft)
            {
                // the box round the canister, tested at the left palm (grip pose; the aim pose's tip without one)
                float grip[16];
                const float* palm = Xr::GetGripPose(1, grip) ? &grip[12] : &hands[hand][12];
                float palmLocal[3];
                ToLocal(frame, palm, palmLocal);
                const float margin = BELT_AMMO_MARGIN_METERS * (zoneBefore == definition.zone ? ZONE_LEAVE_FACTOR : 1.0f);
                bool isInside = true;
                for (int axis = 0; axis < 3; ++axis)
                {
                    isInside = isInside &&
                               fabsf(palmLocal[axis] - BELT_AMMO_CENTRE[axis]) <= BELT_AMMO_HALF_SIZE[axis] + margin;
                }
                if (isInside && bestDistanceRatio > 0.5f)
                {
                    zone = definition.zone;
                    bestDistanceRatio = 0.5f;
                }
                continue;
            }
            float handLocal[3];
            ToLocal(frame, &hands[hand][12], handLocal);
            const float offsetX = handLocal[0] - definition.x, offsetY = handLocal[1] - definition.y,
                        offsetZ = handLocal[2] - definition.z;
            const float distance = sqrtf(offsetX * offsetX + offsetY * offsetY + offsetZ * offsetZ);
            const float radius =
                zoneBefore == definition.zone ? definition.radius * ZONE_LEAVE_FACTOR : definition.radius;
            if (distance > radius)
            {
                continue;
            }
            if (definition.zone == Zone::Eye)
            { // the weapon must point where the head looks (within about 30 degrees; 37 to stay)
                const float alignment =
                    hands[hand][8] * head[8] + hands[hand][9] * head[9] + hands[hand][10] * head[10];
                if (alignment < (zoneBefore == definition.zone ? EYE_ALIGNMENT_TO_STAY : EYE_ALIGNMENT_TO_ENTER))
                {
                    continue;
                }
            }
            if (distance / definition.radius < bestDistanceRatio)
            {
                bestDistanceRatio = distance / definition.radius;
                zone = definition.zone;
            }
        }
        state.zone = zone;
        state.hasEntered = zone != zoneBefore && zone != Zone::None;
        if (_isLogging && zone != zoneBefore)
        {
            Log::Info(
                "gesture",
                "%s hand: %s (was %s); from the head, cm right/up/forward: %.0f %.0f %.0f",
                hand ? "left" : "right",
                ZONE_NAMES[static_cast<int>(zone)],
                ZONE_NAMES[static_cast<int>(zoneBefore)],
                static_cast<double>((hands[hand][12] - head[12]) * 100),
                static_cast<double>((hands[hand][13] - head[13]) * 100),
                static_cast<double>(-(hands[hand][14] - head[14]) * 100));
        }
        if (_isLogging && state.isPalmUp != wasPalmUp)
        {
            Log::Info("gesture", "%s hand: palm %s", hand ? "left" : "right", state.isPalmUp ? "up" : "no longer up");
        }
    }
}

} // namespace Gesture

} // namespace SwbfVr
