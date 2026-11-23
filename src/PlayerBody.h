#pragma once

/// PlayerBody: our own model of the player's body, one mesh on one skeleton (PLAN Next session, idren2023 2026-10-26: the
/// stock rebel soldier's body with our arms and hands, tools/body_assemble.py), drawn by Body's model draw hook in place
/// of the game's soldier while the full body is on. Every bone is posed by us: the body sized from the configured
/// height, the hips from the headset, the spine to the neck below the eyes, the arms by IK to the hands, the hands and
/// fingers as BodyArm poses them on the controllers (BodyArm then draws nothing), the legs to the feet of the game's
/// walk. Skinned on the CPU, with our 4x textures. Data: tools/body_export_player.py -> build/gen/PlayerBody.inc (never
/// in git; without it IsWanted is false). Script `playerbody 0|1|log`.
///
/// All functions run on the game's main thread, except Configure (script thread).

namespace SwbfVr
{

namespace PlayerBody
{

/// What a draw of our body takes from the game and from Body.
struct Inputs
{
    const float* gameMatrix;     // the soldier draw's matrix as the game gave it (rows, model -> world)
    const float* gameFoot[2];    // the game's foot and toe bones in this draw (its model space, rows; right, left)
    const float* gameToe[2];     //   or null
    float anchorWorld[3];        // where the neck goes, horizontally (Body's placement: follows the head, leans)
    float heightCentimeters;     // VR Settings "height"
};

/// Whether our body is built in and switched on.
bool IsWanted();

/// Whether our body drew the hands lately (BodyArm then only poses them).
bool IsDrawingHands();

/// Poses and draws the body into the current eye image.
/// @param device the game's IDirect3DDevice9.
/// @return whether it was drawn.
bool Draw(void* device, const Inputs& inputs);

/// Script command `playerbody 0|1|log`.
/// @param arguments the text after `playerbody`.
void Configure(const char* arguments);

/// Releases what lives on the device (before a reset).
void OnReset();

} // namespace PlayerBody

} // namespace SwbfVr
