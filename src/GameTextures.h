#pragma once

/// GameTextures: our arm and body textures (BodyArm, PlayerBody), made at game start from the player's own game
/// textures instead of being compiled in (docs/research/runtime-assets.md step 4). Each is a recipe on the game's
/// textures (regions resized by integer bilinear, recoloured, re-mapped or filled: the base) plus our difference per
/// channel modulo 256 (tools/texpatch.py, build/gen/TexturePatches.inc). When a game texture is not the one the
/// difference was made against (another build, a mod), the base alone is used.

#include <string>

namespace SwbfVr
{

namespace GameTextures
{

/// Builds every texture from the level files in `lvlFolder` (ending in a slash). Once at start; a summary for the log.
std::string Build(const std::string& lvlFolder);

/// The texture made for this table name (BODY_ARM_TEXTURE_0, PLAYER_BODY_TEXELS_1, ...): 0xAARRGGBB rows from the top;
/// null when it could not be made.
const unsigned* Pixels(const char* table, int& width, int& height);

} // namespace GameTextures

} // namespace SwbfVr
