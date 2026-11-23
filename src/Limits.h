#pragma once

/// Limits: larger tables for the game's renderer, needed for a wide field of view (Limits.cpp). Without them the
/// headset's view overfills the renderer's fixed tables and the game crashes or hangs (b17, b20).

namespace SwbfVr
{

namespace Limits
{

/// Installs the fill-level monitor (a hook on the renderer's queue flush that logs how close each table gets to the
/// game's original limit). Called once when the stereo hooks are installed; later calls do nothing.
void Install();

/// Replaces the renderer's tables by larger ones. Render thread, between frames (start of the Render hook); does its
/// work once, at the first frame start where all draw queues are empty.
void Apply();

} // namespace Limits

} // namespace SwbfVr
