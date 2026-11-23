#pragma once

/// Call counters and call-stack capture for game functions (Probe.cpp). Diagnostic: driven by the script commands
/// "probe <rva hex> [name]", "probes" and "stack".

#include <cstdint>

namespace SwbfVr
{

namespace Probe
{

/// Hooks a game function with a probe that counts its calls, notes up to four `this` values (ecx) and logs the call
/// stack of its first two calls. At most 48 probes; a function already probed is accepted again.
/// @param rva the function's address relative to Battlefront.exe's base; must be in the code section.
/// @param name a name for the log lines; empty or null = "fn".
/// @return true when the probe is installed (or already was).
bool Add(uintptr_t rva, const char* name);

/// Logs one "PROBE" line per probe (calls, calls per frame, `this` values) and starts the counts again.
void Report();

/// Logs one "STACK" line: the return addresses into the game's code found above a stack pointer, as offsets from
/// the exe's base (at most 48, from the first 3000 stack words).
/// @param label what the line is about.
/// @param stackPointer where on the stack the walk starts (esp, or a frame address).
void LogStack(const char* label, uintptr_t stackPointer);

} // namespace Probe

} // namespace SwbfVr
