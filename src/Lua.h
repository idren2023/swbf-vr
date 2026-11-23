#pragma once

/// Lua text run in the game's own interpreters (Lua 4.0.1), and the hook that sends the game's console output to our
/// log (Lua.cpp). The game has two interpreters: the menus' ("shell") and the running mission's.

#include <cstddef>

namespace SwbfVr
{

namespace Lua
{

/// Queues one chunk for the main thread and waits until it has run (at most 5 s). Any thread except the main thread
/// (script commands "lua" / "luas").
/// @param isShell true for the menus' interpreter, false for the mission's.
/// @param code the Lua text; at most 599 characters are used.
/// @return lua_dobuffer's status (0 = ran, >0 = Lua error); -1 no such interpreter now, -2 different game version,
///         -3 no frame presented within 5 s.
int Run(bool isShell, const char* code);

/// Hooks the game's console output (ConsolePrintf), so console commands log their text instead of crashing. Call once.
void Install();

/// Main thread, once per presented frame (Present hook): runs the chunk queued by Run, if any.
void OnPresent();

/// Main thread: the interpreter's lua_State.
/// @param isShell true for the menus' interpreter, false for the mission's.
/// @return the lua_State, null if there is none now or the game's Lua functions were not found.
void* CurrentState(bool isShell);

/// Main thread: compiles and runs a chunk now.
/// @param isShell true for the menus' interpreter, false for the mission's.
/// @param code the Lua text (need not end with a zero).
/// @param size its length in bytes.
/// @return lua_dobuffer's status (0 = ran), -1 = no such interpreter now.
int Execute(bool isShell, const char* code, size_t size);

/// Main thread: makes a C function callable from Lua under a global name.
/// @param isShell true for the menus' interpreter, false for the mission's.
/// @param name the Lua global's name.
/// @param function the function (cdecl, gets the lua_State, returns the number of results).
/// @return false when there is no such interpreter now.
bool Register(bool isShell, const char* name, int(__cdecl* function)(void* state));

/// Inside a function registered with Register: one argument as text.
/// @param state the lua_State the function was called with.
/// @param index the argument's number, from 1.
/// @return the text, null if the argument is not a string or a number.
const char* ToString(void* state, int index);

} // namespace Lua

} // namespace SwbfVr
