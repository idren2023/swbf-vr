#pragma once

/// Foliage: keeps the game's props (grass, ferns, shrubs) from flickering in the headset (idren2023 2026-11-22: "the
/// amount of flickering is making it feel/look very bad"; runs 860-869).
///
/// The game's prop generator (a per-update task, 0x201ED0) fills 32 m ground cells round the camera with prop
/// clusters from a pool sized at level load: (100 / 32 + 5)^2 = 66, capped at 254 (0x201B28). Each update it evicts
/// the clusters no view drew since the last update first, then hands the free ones to the missing cells of the
/// game camera's frustum until the pool runs out. In VR the eyes draw a wider view turned with the head, the pool runs
/// out and the clusters swap between two sets on every update (run 868, FoliageProbe: with the head turned, 27 and 38
/// clusters, 117 and 528 grass patches drawn on alternate frames): the props in view vanish every other frame. The cap
/// jump is made unconditional, so the pool is always 254 (about 40 KB of the game's own heap, rebuilt at each load).

namespace SwbfVr
{

namespace Foliage
{

/// Between frames (start of the Render hook): applies the pool patch once, before a level is loaded (the pool is
/// sized at level load).
void BeforeFrame();

} // namespace Foliage

} // namespace SwbfVr
