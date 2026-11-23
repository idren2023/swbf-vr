#pragma once

/// How the weapon follows the hands: weapon weight (the virtual stock is in Aim.cpp). Pure arithmetic, no game or Windows calls,
/// so it can be called from any thread; the mod calls it from Aim.cpp on the thread that reads the controller poses.
///
/// Poses are the mod's 4x4 row-vector matrices in resting-pose space, passed as pointers to 16 floats (the convention
/// of the whole mod until it has a matrix type): x right, y up, -z ahead, metres; rows 0..2 are the frame's right, up
/// and back axes, row 3 its position.

namespace SwbfVr
{

namespace Handling
{

/// Forgets the weapon's lagging pose, so that the next ApplyWeight starts exactly on the hand. Call when the weight
/// is switched off or the weapon changes.
void Reset();

/// Weapon weight: replaces `poseMatrix` (where the right hand is now) by where the weapon is now, lagging behind the
/// hand through two first-order lags in a row. It starts gently, never overshoots and ends exactly on the hand.
/// @param poseMatrix the right hand's pose on entry, the weapon's pose on return (16 floats).
/// @param elapsedSeconds time since the previous call; 0 or more than 0.25 s starts again on the hand.
/// @param positionTimeConstantSeconds time constant of each position stage; 0 = position follows at once.
/// @param rotationTimeConstantSeconds time constant of each rotation stage; 0 = rotation follows at once.
void ApplyWeight(
    float* poseMatrix,
    float elapsedSeconds,
    float positionTimeConstantSeconds,
    float rotationTimeConstantSeconds);

} // namespace Handling

} // namespace SwbfVr
