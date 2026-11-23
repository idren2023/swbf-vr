#pragma once

/// Hardware data breakpoints that record which code touches an address (Watch.cpp). Diagnostic: driven by the script
/// commands "watch <slot> <target> <w|rw>" and "unwatch"; results go to swbf-vr.log as W lines.

#include <cstdint>

namespace SwbfVr
{

namespace Watch
{

/// Notes the calling thread as the one whose accesses are watched. Call from the game's main thread (Present hook);
/// only the first call counts.
void NoteMainThread();

/// Arms one of the CPU's four data breakpoints on the main thread. A watch switches itself off after 15000 accesses.
/// @param slot which breakpoint, 0 to 3.
/// @param address the watched address; must be 4-byte aligned (4 bytes are watched).
/// @param isReadWrite true to catch reads and writes, false for writes only.
/// @param label name for the log lines.
/// @return true when the breakpoint is armed.
bool Set(unsigned slot, uintptr_t address, bool isReadWrite, const char* label);

/// Disarms all watches and logs what they saw: one line per code location, busiest first. Does nothing when no watch
/// is set.
void ClearAndReport();

} // namespace Watch

} // namespace SwbfVr
