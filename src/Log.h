#pragma once

/// The mod's log, swbf-vr.log next to the DLL. One line per call:
/// `<GetTickCount> <LEVEL> <subsystem>: <text>`, for example `123456 INFO  xr: session created`.
/// All functions take a printf-style format and are safe to call from any thread.
/// Players send this file to diagnose their setup, so a line should make sense without access to their PC:
/// errors and warnings say what failed and what the mod did about it (Log.cpp).
///
/// `__attribute__((format(printf, 2, 3)))` tells the compiler that argument 2 is a printf format for the arguments
/// from 3 on, so it warns about a format that does not match its arguments.

namespace SwbfVr
{

namespace Log
{

/// Detail for development. Written only when `log_debug = 1` is in swbf-vr.ini.
/// @param subsystem the tag after the level, lower case (`xr`, `stereo`, ...); greps of the logs rely on it.
/// @param format printf-style format, followed by its arguments.
void Debug(const char* subsystem, const char* format, ...) __attribute__((format(printf, 2, 3)));

/// Normal progress: what was set up, what a test command did.
/// @param subsystem the tag after the level.
/// @param format printf-style format, followed by its arguments.
void Info(const char* subsystem, const char* format, ...) __attribute__((format(printf, 2, 3)));

/// Something did not work, and the mod carries on without it (a feature turned off, a value ignored).
/// @param subsystem the tag after the level.
/// @param format printf-style format, followed by its arguments.
void Warning(const char* subsystem, const char* format, ...) __attribute__((format(printf, 2, 3)));

/// A failure that stops a part of the mod (a hook, VR as a whole).
/// @param subsystem the tag after the level.
/// @param format printf-style format, followed by its arguments.
void Error(const char* subsystem, const char* format, ...) __attribute__((format(printf, 2, 3)));

/// Switches Debug lines on or off (swbf-vr.ini: log_debug). Off at start. A change is logged.
/// @param isEnabled true to write Debug lines.
void SetDebugEnabled(bool isEnabled);

/// Writes the first lines of the log: the mod's build and the Windows version. The game build (SHA-256 of
/// Battlefront.exe) follows from GameBuild::Identify. Call once, outside the loader lock.
/// @param buildId the mod's build id (BUILD_ID, for example "b97").
void WriteStartupHeader(const char* buildId);

} // namespace Log

} // namespace SwbfVr
