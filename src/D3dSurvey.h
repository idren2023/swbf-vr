#pragma once

/// D3dSurvey: the Direct3D 9 device hooks (Present, Reset, SetTransform, the draw calls, ...) that drive Stereo and
/// the per-frame subsystems, plus a diagnostic frame tracer that writes one frame's device calls to
/// swbf-vr-trace.txt (D3dSurvey.cpp).
///
/// The hooks run on the game's main thread, which is also its render thread (the game presents from the thread that
/// runs Update and Render).

#include <cstdint>

namespace SwbfVr
{

namespace D3dSurvey
{

/// Frames presented so far: every Present that reached the real Present (the left-eye pass, whose Present Stereo
/// skips, is not counted).
/// @return the number of presented frames.
uint32_t FrameCount();

/// Logs the call stack of the next Present (script command "stack").
void RequestPresentStack();

/// Test ("drawlog <n>"): logs the draw calls and primitives of each of the next passes (one per Present call), by
/// kind: plain, indexed and the user-pointer (UP) variants.
/// @param count how many passes.
void StartDrawLog(unsigned count);

/// The device the game presents with.
/// @return the game's presenting IDirect3DDevice9, or null before the first Present.
void* PresentingDevice();

/// Redirects the game's Direct3DCreate9 import so the device is seen, and hooked, as it is created. Only the first
/// successful call does anything; DllMain calls it, and again the first DirectInput8Create in case the loader had
/// not resolved the import yet.
/// @param when where the call comes from, for the log.
void Install(const char* when);

/// Traces the next frame into swbf-vr-trace.txt (script command "trace").
void RequestTrace();

/// The game's own lighting of its models (PLAN 57 l: each map defines its own): kept from the draws whose stage-0
/// texture is this one (CockpitShell: its cockpit's texture). Render thread.
/// @param texture an IDirect3DBaseTexture9, or null: none.
void WatchLightOf(void* texture);

/// The game's lighting of the last draw with the watched texture (within the last second): its colour scale, ambient
/// colour, sun colour (RGB 0..1) and the direction the sunlight travels (world space), as the game's lit model
/// shaders get them (vertex shader constants c18-c21; runs 356-357). False when none was seen.
bool GetWatchedLight(float* scale, float* ambient, float* sunColour, float* sunDirection);

/// Logs the shader constants, stage-0 textures, vertex shaders and draws from the next first-person pass to its
/// Present (script command "shaderdump"; PLAN 57 l: how the game lights its first-person models).
void RequestShaderDump();

} // namespace D3dSurvey

} // namespace SwbfVr
