#pragma once

/// The game's texture table (Textures.cpp, docs/offsets.md "Zoom"): every texture the game has loaded, by name hash
/// (TextureByHash 0x74290). Diagnostic for now: script commands `textures` (log the table and which code looks up
/// which texture during one frame) and `texsave <hash> ...` (write a texture's top level to a file).

namespace SwbfVr
{

namespace Textures
{

/// Installs the lookup hook (feature Zoom of GameBuild, which carries TextureByHash). Called by Stereo::Install.
void Install();

/// Once per game frame, at the start of Render (between frames): carries out the requests below.
void BeforeFrame();

/// Script thread: at the next frame, log every table entry, then log the lookups made during that frame (caller and
/// hash, counted).
void RequestDump();

/// Script thread: at the next frame, write the top level of that texture to `swbf-vr-tex-<hash>.bin` (20-byte header:
/// "TEX0", width, height, D3DFORMAT, pitch, then the locked rows as they are).
/// @param hash the texture's name hash (FNV-1a of the name, each byte | 0x20).
void RequestSave(unsigned hash);

/// A loaded 2D texture of the game by its name (as its models name it, without extension; any case), for our own
/// draws with the game's look (LeverModel, CockpitShell). Main thread.
/// @param name the texture's name.
/// @return its IDirect3DTexture9, or null while it is not loaded (or the lookup is unavailable).
void* FindByName(const char* name);

} // namespace Textures

} // namespace SwbfVr
