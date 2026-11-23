#pragma once

/// Crash log: writes the details of fatal exceptions to swbf-vr-crash.log, and where the render thread is when frames
/// stop coming without a crash ("NO FRAME") (Crash.cpp). Read the "dinput8.dll+0x..." lines with the debug DLL of the
/// same build (re/builds/<id>/, llvm-symbolizer).

namespace SwbfVr
{

namespace Crash
{

/// Creates swbf-vr-crash.log, installs the exception handlers and starts the no-frame watcher thread. Call once.
/// @param buildId the mod's build id, written in the file's first line.
void Install(const char* buildId);

/// Call once per presented frame (Present hook, render thread). If the beats stop for 4 s, the render thread's
/// position is logged.
void Heartbeat();

} // namespace Crash

} // namespace SwbfVr
