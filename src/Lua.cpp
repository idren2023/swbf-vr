// Lua.cpp: runs Lua text inside the game's own Lua 4.0.1 states.
//
// The game keeps two script objects, one for the menus ("shell") and one for a running mission. The first field of
// each is its lua_State. Text is compiled and run with the game's lua_dobuffer, on the game's main thread (from
// the Present hook), never from our script thread: the interpreter is not thread-safe.
// What this opens: the 470 ScriptCB_* functions (docs/MODTOOLS.md section 4), among them the console commands.
#include "Lua.h"

#include "FpGuard.h"
#include "GameBuild.h"
#include "Log.h"
#include "MinHook.h"

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace SwbfVr
{

namespace Lua
{

namespace
{

// ---- Engine addresses (GameBuild; docs/offsets.md, "Lua") ----
// Feature::Lua: LuaDoBuffer, int lua_dobuffer(lua_State*, const char* text, size_t size, const char* name), cdecl
// (observed: called by the base library's dostring); MissionScript, pointer to the mission's script object
// ([object + 0] = lua_State; null in the menus); ShellScript, pointer to the menu's (shell's) script object;
// LuaRegister and LuaToString (below).
// Feature::ConsoleOutput: the game's console output, ConsolePrintf: void ConsolePrintf(Console*, const char* fmt,
// ...), cdecl, 37 callers. It formats into a buffer and calls ConsolePuts, void Console::Puts(const char*),
// thiscall. In the retail game the console object is null, so every console command that prints something crashes
// (seen: "framerate", b23). The hook sends the text to our log when there is no console.
using GameBuild::Address;
/// The real ConsolePrintf (MinHook's trampoline).
void* _originalConsolePrintf;

/// Room for one console line (the game's own buffer is 2 KB too: `sub esp, 0x800` in its first bytes).
constexpr size_t CONSOLE_TEXT_BYTES = 2048;
/// Run's statuses besides lua_dobuffer's own.
constexpr int STATUS_NO_STATE = -1;
constexpr int STATUS_DIFFERENT_VERSION = -2;
constexpr int STATUS_NO_FRAME = -3;
/// Run waits in 10 ms steps, 5 s in all, for the main thread.
constexpr int WAIT_STEP_COUNT = 500;
constexpr DWORD WAIT_STEP_MILLISECONDS = 10;
/// A lua_State is taken as valid only if this much memory is readable at its address.
constexpr size_t MINIMUM_STATE_BYTES = 64;

// Replaces ConsolePrintf (cdecl, "..." variable arguments): formats the text and, without a console, logs it.
void __cdecl ConsolePrintfHook(void* console, const char* format, ...)
{
    char text[CONSOLE_TEXT_BYTES];
    {
        // FpGuard: an RAII guard (FpGuard.h): it saves the game's floating-point state, gives vsnprintf a clean one
        // and restores the game's at the closing brace.
        FpGuard guard;
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(text, sizeof(text), format, arguments);
        va_end(arguments);
        if (!console)
        {
            Log::Info("console", "%s", text);
        }
    }
    if (console)
    {
        // __thiscall: the object is passed in ecx, as the game's member functions expect.
        using ConsolePutsFunction = void(__thiscall*)(void*, const char*);
        reinterpret_cast<ConsolePutsFunction>(GameBuild::Absolute(Address::ConsolePuts))(console, text);
    }
}

using DoBufferFunction = int(__cdecl*)(void* state, const char* text, size_t size, const char* name);

// One chunk handed from the script thread to the main thread.
struct Request
{
    char code[600];
    bool isShell;
    volatile LONG isPending; // 1 = waiting for the main thread
    int status;              // lua_dobuffer's result; negative = not run
    void* object;
    void* state;
};

Request _request;

// True if size bytes from address on are committed, readable memory within one region.
bool IsReadable(const void* address, size_t size)
{
    MEMORY_BASIC_INFORMATION memoryInfo;
    if (!address || !VirtualQuery(address, &memoryInfo, sizeof(memoryInfo)))
    {
        return false;
    }
    if (memoryInfo.State != MEM_COMMIT || (memoryInfo.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
    {
        return false;
    }
    return reinterpret_cast<uintptr_t>(address) + size <=
           reinterpret_cast<uintptr_t>(memoryInfo.BaseAddress) + memoryInfo.RegionSize;
}

} // namespace

// Script thread: queue one chunk and wait until the main thread has run it. Returns lua_dobuffer's status
// (0 = ran without error), -1 = no such state right now, -2 = different game version, -3 = no frame within 5 s.
int Run(bool isShell, const char* code)
{
    strncpy(_request.code, code, sizeof(_request.code) - 1);
    _request.code[sizeof(_request.code) - 1] = 0;
    _request.isShell = isShell;
    _request.status = STATUS_NO_FRAME;
    _request.object = _request.state = nullptr;
    InterlockedExchange(&_request.isPending, 1);
    for (int step = 0; step < WAIT_STEP_COUNT && _request.isPending; ++step)
    {
        Sleep(WAIT_STEP_MILLISECONDS);
    }
    const bool hasRun = !InterlockedExchange(&_request.isPending, 0);
    Log::Info(
        "lua",
        "%s state, object %p, state %p, status %d%s: %s",
        isShell ? "shell" : "mission",
        _request.object,
        _request.state,
        hasRun ? _request.status : STATUS_NO_FRAME,
        !hasRun                                       ? " (no frame presented)"
        : _request.status == 0                        ? ""
        : _request.status == STATUS_NO_STATE          ? " (no such state now)"
        : _request.status == STATUS_DIFFERENT_VERSION ? " (different game version)"
                                                      : " (Lua error)",
        code);
    return hasRun ? _request.status : STATUS_NO_FRAME;
}

// Main thread, once per presented frame.
void OnPresent()
{
    if (!_request.isPending)
    {
        return;
    }
    int status = STATUS_NO_STATE;
    if (!GameBuild::IsAvailable(GameBuild::Feature::Lua))
    {
        status = STATUS_DIFFERENT_VERSION;
    }
    else
    {
        // The game's global pointer to the script object; the object's first field is its lua_State.
        void** slot = reinterpret_cast<void**>(
            GameBuild::Absolute(_request.isShell ? Address::ShellScript : Address::MissionScript));
        void* object = *slot;
        _request.object = object;
        if (IsReadable(object, sizeof(void*)))
        {
            void* state = *static_cast<void**>(object);
            _request.state = state;
            if (IsReadable(state, MINIMUM_STATE_BYTES))
            {
                status = reinterpret_cast<DoBufferFunction>(
                    GameBuild::Absolute(Address::LuaDoBuffer))(state, _request.code, strlen(_request.code), "=swbf-vr");
            }
        }
    }
    _request.status = status;
    InterlockedExchange(&_request.isPending, 0);
}

// ---- direct use on the main thread (Settings.cpp: the VR settings screen) ----
// LuaRegister (0x18AF60 on Steam): void Register(lua_State*, const char* name, lua_CFunction), cdecl: the helper the
// game registers its own ScriptCB_* functions with (loop at 0x17CDE0 over the table at 0x479018). LuaToString
// (0xCB560): const char* ToString(lua_State*, int index), cdecl, used by every ScriptCB_* function that takes a string.
namespace
{
// True if the game's Lua functions were found (GameBuild, Feature::Lua); warned once.
bool IsApiKnown()
{
    static bool hasWarned = false;
    const bool isKnown = GameBuild::IsAvailable(GameBuild::Feature::Lua);
    if (!isKnown && !hasWarned)
    {
        hasWarned = true;
        Log::Warning("lua", "Lua functions not found (different game version): no VR settings screen");
    }
    return isKnown;
}
} // namespace

// The lua_State of the mission's (or the menu's) interpreter; null if there is none right now.
void* CurrentState(bool isShell)
{
    if (!IsApiKnown())
    {
        return nullptr;
    }
    void* object =
        *reinterpret_cast<void**>(GameBuild::Absolute(isShell ? Address::ShellScript : Address::MissionScript));
    if (!IsReadable(object, sizeof(void*)))
    {
        return nullptr;
    }
    void* state = *static_cast<void**>(object);
    return IsReadable(state, MINIMUM_STATE_BYTES) ? state : nullptr;
}

// Main thread only. Returns lua_dobuffer's status (0 = ran), -1 = no such interpreter now.
int Execute(bool isShell, const char* code, size_t size)
{
    void* state = CurrentState(isShell);
    if (!state)
    {
        return STATUS_NO_STATE;
    }
    return reinterpret_cast<DoBufferFunction>(GameBuild::Absolute(Address::LuaDoBuffer))(state, code, size, "=swbf-vr");
}

// Main thread only. Makes a C function callable from Lua under that global name.
bool Register(bool isShell, const char* name, int(__cdecl* function)(void* state))
{
    void* state = CurrentState(isShell);
    if (!state)
    {
        return false;
    }
    using RegisterFunction = void(__cdecl*)(void*, const char*, int(__cdecl*)(void*));
    reinterpret_cast<RegisterFunction>(GameBuild::Absolute(Address::LuaRegister))(state, name, function);
    return true;
}

// Inside a registered function: argument <index> (from 1) as text; null if it is not a string or a number.
const char* ToString(void* state, int index)
{
    using ToStringFunction = const char*(__cdecl*)(void*, int);
    return reinterpret_cast<ToStringFunction>(GameBuild::Absolute(Address::LuaToString))(state, index);
}

void Install()
{
    if (!GameBuild::IsAvailable(GameBuild::Feature::ConsoleOutput))
    {
        Log::Warning(
            "lua",
            "console output not hooked: %s is off for this game build",
            GameBuild::FeatureName(GameBuild::Feature::ConsoleOutput));
        return;
    }
    void* target = reinterpret_cast<void*>(GameBuild::Absolute(Address::ConsolePrintf));
    MH_STATUS status = MH_Initialize();
    if (status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED)
    {
        status = MH_CreateHook(target, reinterpret_cast<void*>(&ConsolePrintfHook), &_originalConsolePrintf);
    }
    if (status == MH_OK)
    {
        status = MH_EnableHook(target);
    }
    Log::Info("lua", "console output hook: %s", MH_StatusToString(status));
}

} // namespace Lua

} // namespace SwbfVr
