#pragma once

/// DrawState: a complete, known fixed-function state for the mod's own draws inside the game's frame. Our draws used
/// to set only the states they thought of and inherit the rest from whatever the game drew last; the sampler's mip
/// limits were among the rest, so our arm and body textures were at times sampled at a tiny mip and drawn as one flat
/// colour (runs 813-820: a pale glove, a grey hand, depending on the game's draw order and our code layout). Call
/// Reset after capturing the state block the draw restores, then set what the draw needs on top.

#include <windows.h>
#include <d3d9.h>

namespace SwbfVr
{

namespace DrawState
{

/// No shaders; every texture stage, sampler and render state that changes how a fixed-function draw looks set to the
/// Direct3D default, except: z test and write on (LESSEQUAL), no culling, lighting off, linear filtering with mips.
/// Textures unbound on every stage; stage 0 modulates texture and diffuse, the others are off.
void Reset(IDirect3DDevice9* device);

} // namespace DrawState

} // namespace SwbfVr
