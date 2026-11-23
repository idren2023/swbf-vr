// Handling.cpp: how the weapon follows the hands (b94). Pure arithmetic, no game or Windows calls.
//
// Weight. The weapon does not copy the controller: it is pulled toward it through two first-order lags in a row
// (a critically damped second-order follower: it starts gently, never overshoots and ends exactly on the hand).
// Position and rotation have their own time constants. The arms and hands are drawn from the weapon's pose, so
// they stay on the weapon; only the real controller runs ahead of it while it moves. idren2023's wish (headset run 9):
// the weapon should lag toward where the controllers come to rest, and the hands must never slide off it.
//
// The virtual stock (two-handed, right hand ignored) is in Aim.cpp's ApplyTwoHandedGrip since b119.
// All numbers are first guesses, not yet tried in the headset.
#include "Handling.h"

#include <math.h>
#include <string.h>

namespace SwbfVr
{

namespace Handling
{

namespace
{

// A pause or a jump (recentre, tracking found again) longer or farther than these starts the follower again.
constexpr float LONGEST_STEP_SECONDS = 0.25f;
constexpr float WEAPON_JUMP_METERS = 0.4f;

/// A rotation as a unit quaternion (x, y, z the axis times sin(angle / 2), w = cos(angle / 2)): unlike a matrix it can
/// be blended a fraction of the way toward another rotation and normalised again.
struct Quaternion
{
    float x, y, z, w;
};

/// The rotation part of a row-vector matrix as a quaternion (the standard conversion, choosing the largest of the
/// four possible divisors for precision).
Quaternion QuaternionFromMatrix(const float* matrix)
{
    // r<row><column> in column-vector convention, which is the transpose of the mod's row-vector layout.
    const float r00 = matrix[0], r01 = matrix[4], r02 = matrix[8], r10 = matrix[1], r11 = matrix[5], r12 = matrix[9],
                r20 = matrix[2], r21 = matrix[6], r22 = matrix[10];
    const float trace = r00 + r11 + r22;
    Quaternion result;
    if (trace > 0)
    {
        const float scale = sqrtf(trace + 1.0f) * 2;
        result.w = scale / 4;
        result.x = (r21 - r12) / scale;
        result.y = (r02 - r20) / scale;
        result.z = (r10 - r01) / scale;
    }
    else if (r00 > r11 && r00 > r22)
    {
        const float scale = sqrtf(1.0f + r00 - r11 - r22) * 2;
        result.w = (r21 - r12) / scale;
        result.x = scale / 4;
        result.y = (r01 + r10) / scale;
        result.z = (r02 + r20) / scale;
    }
    else if (r11 > r22)
    {
        const float scale = sqrtf(1.0f + r11 - r00 - r22) * 2;
        result.w = (r02 - r20) / scale;
        result.x = (r01 + r10) / scale;
        result.y = scale / 4;
        result.z = (r12 + r21) / scale;
    }
    else
    {
        const float scale = sqrtf(1.0f + r22 - r00 - r11) * 2;
        result.w = (r10 - r01) / scale;
        result.x = (r02 + r20) / scale;
        result.y = (r12 + r21) / scale;
        result.z = scale / 4;
    }
    return result;
}

/// Writes the rotation into the first three rows of a row-vector matrix and makes it affine (last column 0, 0, 0, 1);
/// the position row is left to the caller.
void WriteRotation(float* matrix, const Quaternion& rotation)
{
    const float x = rotation.x, y = rotation.y, z = rotation.z, w = rotation.w;
    matrix[0] = 1 - 2 * (y * y + z * z);
    matrix[4] = 2 * (x * y - z * w);
    matrix[8] = 2 * (x * z + y * w);
    matrix[1] = 2 * (x * y + z * w);
    matrix[5] = 1 - 2 * (x * x + z * z);
    matrix[9] = 2 * (y * z - x * w);
    matrix[2] = 2 * (x * z - y * w);
    matrix[6] = 2 * (y * z + x * w);
    matrix[10] = 1 - 2 * (x * x + y * y);
    matrix[3] = matrix[7] = matrix[11] = 0;
    matrix[15] = 1;
}

/// Moves `rotation` the fraction `fraction` of the way toward `target`, the short way round (q and -q are the same
/// rotation, so the target's sign is chosen to match).
void StepToward(Quaternion& rotation, const Quaternion& target, float fraction)
{
    const float sign = rotation.x * target.x + rotation.y * target.y + rotation.z * target.z + rotation.w * target.w < 0
                           ? -1.0f
                           : 1.0f;
    rotation.x += (target.x * sign - rotation.x) * fraction;
    rotation.y += (target.y * sign - rotation.y) * fraction;
    rotation.z += (target.z * sign - rotation.z) * fraction;
    rotation.w += (target.w * sign - rotation.w) * fraction;
    const float length =
        sqrtf(rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z + rotation.w * rotation.w);
    if (length > 1e-6f)
    {
        rotation.x /= length;
        rotation.y /= length;
        rotation.z /= length;
        rotation.w /= length;
    }
    else
    {
        rotation = target;
    }
}

/// One first-order lag of the weapon follower.
struct FollowerStage
{
    float position[3];
    Quaternion rotation;
};

// File-level state (game thread or render thread, never both: see the header). Not in the game's heap.
FollowerStage _stages[2]; // [0] follows the hand, [1] follows [0] and is the weapon
bool _hasStages = false;

/// The fraction of the remaining distance a first-order lag with time constant `timeConstantSeconds` covers in
/// `elapsedSeconds` (1 - e^(-t / tau)); 1 when the time constant is 0.
float LagFraction(float elapsedSeconds, float timeConstantSeconds)
{
    return timeConstantSeconds > 1e-4f ? 1.0f - expf(-elapsedSeconds / timeConstantSeconds) : 1.0f;
}

/// False if any element is NaN or implausibly large (a pose not tracked yet or broken).
bool IsFinite(const float* matrix)
{
    for (int element = 0; element < 16; ++element)
    {
        // NaN is the only value not equal to itself.
        if (!(matrix[element] == matrix[element]) || fabsf(matrix[element]) > 1e4f)
        {
            return false;
        }
    }
    return true;
}

} // namespace

void Reset()
{
    _hasStages = false;
}

void ApplyWeight(
    float* poseMatrix,
    float elapsedSeconds,
    float positionTimeConstantSeconds,
    float rotationTimeConstantSeconds)
{
    if (!IsFinite(poseMatrix))
    {
        _hasStages = false;
        return;
    }
    const Quaternion target = QuaternionFromMatrix(poseMatrix);
    const float dx = poseMatrix[12] - _stages[1].position[0], dy = poseMatrix[13] - _stages[1].position[1],
                dz = poseMatrix[14] - _stages[1].position[2];
    // First call, a pause, or a jump (recentre, tracking found again): start on the hand.
    if (!_hasStages || elapsedSeconds <= 0 || elapsedSeconds > LONGEST_STEP_SECONDS ||
        dx * dx + dy * dy + dz * dz > WEAPON_JUMP_METERS * WEAPON_JUMP_METERS)
    {
        for (FollowerStage& stage : _stages)
        {
            memcpy(stage.position, poseMatrix + 12, sizeof(stage.position));
            stage.rotation = target;
        }
        _hasStages = true;
        return;
    }
    const float positionFraction = LagFraction(elapsedSeconds, positionTimeConstantSeconds),
                rotationFraction = LagFraction(elapsedSeconds, rotationTimeConstantSeconds);
    for (int axis = 0; axis < 3; ++axis)
    {
        _stages[0].position[axis] += (poseMatrix[12 + axis] - _stages[0].position[axis]) * positionFraction;
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        _stages[1].position[axis] += (_stages[0].position[axis] - _stages[1].position[axis]) * positionFraction;
    }
    StepToward(_stages[0].rotation, target, rotationFraction);
    StepToward(_stages[1].rotation, _stages[0].rotation, rotationFraction);
    WriteRotation(poseMatrix, _stages[1].rotation);
    memcpy(poseMatrix + 12, _stages[1].position, sizeof(_stages[1].position));
}

} // namespace Handling

} // namespace SwbfVr
