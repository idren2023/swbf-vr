#pragma once

/// LockOn: the homing launchers' lock-on shown in the scope picture (idren2023 2026-10-20, after headset run 17): the
/// lock cone as a ring, four corner brackets that close round the target as the lock builds up (the game's own lock
/// time, the weapon HUD's green bar), and a blinking reticle on the target once it is locked (LockOn.cpp).
///
/// DrawInScope and OnReset run on the render thread (Stereo.cpp); SetTest on the game's main thread (Script), which is
/// the same thread.

namespace SwbfVr
{

namespace LockOn
{

/// Draws the lock-on display into the scope picture. Once per scope pass, when its scene is complete (Stereo.cpp's
/// SetNearFar hook, before the first-person pass): the scope camera's VIEW and PROJECTION are set.
/// @param device the game's IDirect3DDevice9.
void DrawInScope(void* device);

/// The scope picture's half field (a tangent) for the weapon in hand: with a homing launcher (or the test) 1.3 times
/// its widest lock cone, so the whole cone and its ring are in the picture; else 0 (the lens's own field). Render
/// thread.
float ScopeHalfTan();

/// The scope reticle's opacity (Hud): 1 normally; with a target in the lock zone it fades as the lock builds, 0 while
/// locked (idren2023 2026-10-20: the lock alone steers the torpedo). 1 when no scope pass ran lately.
float ReticleOpacity();

/// A shot left the player's weapon (Feedback, game thread): the test lock starts again (the game clears its own lock).
void OnShot();

/// `lockon log 1`: the game's own lock values (handle, timer, its own "locked" test, the target's place from the ray)
/// every 100 ms while a homing launcher's scope is up.
void SetRawLog(bool isOn);

/// Before the device is reset: releases the state block (default pool).
void OnReset();

/// Test (`lockon test 1`): with any weapon in the scope, a target 40 m along the aim, 2.5 m right and 1 m up (fixed in the world there), and a lock that builds up over
/// one second, holds locked for one second and starts again; `lockon test 0` back to the game's own lock.
void SetTest(bool isOn);

} // namespace LockOn

} // namespace SwbfVr
