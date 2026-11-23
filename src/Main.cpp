// swbf-vr: loads into Battlefront.exe as a dinput8.dll proxy.
// The game imports only DirectInput8Create from dinput8, so that is the one export we forward.

#include "Main.h"

#include "Crash.h"
#include "D3dSurvey.h"
#include "EngineSurvey.h"
#include "GameBuild.h"
#include "GameMeshes.h"
#include "GameTextures.h"
#include "Input.h"
#include "Log.h"
#include "Lua.h"
#if SWBF_VR_DEV
#include "Script.h" // src/dev: the developer's test scripts, not in the public source
#endif
#include "StockTables.h"

#include <windows.h>
#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

// BUILD_ID is set by tools/build.sh on the compiler's command line (-DBUILD_ID="b97"); a macro because only the
// command line can pass it in.
#ifndef BUILD_ID
#define BUILD_ID "dev" // NOLINT(cppcoreguidelines-macro-usage): set on the command line
#endif

namespace SwbfVr
{

namespace Main
{

namespace
{

HMODULE _module = nullptr;
HMODULE _realDirectInput = nullptr;
/// The DLL's folder, ending with a backslash; set in DllMain.
char _directory[MAX_PATH];

} // namespace

const char* ModDirectory()
{
    return _directory;
}

FILE* OpenModFile(const char* name, const char* mode)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s%s", _directory, name);
    // Shared open, so the files can be read by other programs while the game is running.
    return _fsopen(path, mode, _SH_DENYNO);
}

} // namespace Main

namespace Profiling
{

namespace
{

constexpr int STAGE_COUNT = static_cast<int>(Stage::Count);
double _milliseconds[STAGE_COUNT], _longestMilliseconds[STAGE_COUNT];
unsigned _calls[STAGE_COUNT];

} // namespace

void Add(Stage stage, double milliseconds)
{
    const int index = static_cast<int>(stage);
    _milliseconds[index] += milliseconds;
    ++_calls[index];
    if (milliseconds > _longestMilliseconds[index])
    {
        _longestMilliseconds[index] = milliseconds;
    }
}

// Average milliseconds per frame spent in each stage (and the longest single call) since the last report.
void Report(unsigned frames)
{
    static const char* const STAGE_NAMES[STAGE_COUNT] = {
        "wait",
        "begin",
        "locate",
        "readback",
        "acquire",
        "upload",
        "release",
        "end",
        "renderL",
        "renderR",
        "renderFlat",
        "renderScope",
        "present",
        "gpucopy"};
    char line[640];
    int length = 0;
    bool hasAny = false;
    for (int stage = 0; stage < STAGE_COUNT; ++stage)
    {
        if (_calls[stage])
        {
            hasAny = true;
            length += snprintf(
                line + length,
                sizeof(line) - static_cast<size_t>(length),
                " %s %.1f (max %.1f, %ux)",
                STAGE_NAMES[stage],
                _milliseconds[stage] / (frames ? frames : 1),
                _longestMilliseconds[stage],
                _calls[stage]);
        }
        _milliseconds[stage] = _longestMilliseconds[stage] = 0;
        _calls[stage] = 0;
    }
    // Which window is in front: the game slows down a lot when it is not its own.
    char title[64] = "";
    DWORD processId = 0;
    const HWND frontWindow = GetForegroundWindow();
    if (frontWindow)
    {
        GetWindowThreadProcessId(frontWindow, &processId);
        GetWindowTextA(frontWindow, title, sizeof(title));
    }
    if (hasAny)
    {
        Log::Info(
            "time",
            "ms per frame over %u frames:%s | in front: %s \"%s\"",
            frames,
            line,
            processId == GetCurrentProcessId() ? "the game" : "ANOTHER WINDOW",
            title);
    }
}

} // namespace Profiling

namespace Main
{

bool PatchPointer(void** slot, void* value)
{
    DWORD oldProtection = 0;
    if (!VirtualProtect(static_cast<void*>(slot), sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        return false;
    }
    *slot = value;
    VirtualProtect(static_cast<void*>(slot), sizeof(void*), oldProtection, &oldProtection);
    return true;
}

namespace
{

// Runs on the game's first DirectInput8Create call, outside the loader lock.
// The Steam wrapper has decrypted the game's code by this point.
void Initialize()
{
    char systemPath[MAX_PATH];
    GetSystemDirectoryA(systemPath, MAX_PATH);
    strcat_s(systemPath, "\\dinput8.dll");
    _realDirectInput = LoadLibraryA(systemPath);
    Log::WriteStartupHeader(BUILD_ID);
    // Before anything that hooks the game: which build this is and which features its code allows.
    GameBuild::Identify();
    Log::Info(
        "init",
        "exe base %p, real dinput8 %p",
        static_cast<void*>(GetModuleHandleA(nullptr)),
        static_cast<void*>(_realDirectInput));
    Crash::Install(BUILD_ID);
    // The pieces of the game's models the shells use, from the player's own level files (nothing of the game's is
    // compiled into the mod): once, before any shell is built.
    Log::Info("stock", "%s", StockTables::Build(std::string(ModDirectory()) + "Data/_lvl_pc/").c_str());
    Log::Info("textures", "%s", GameTextures::Build(std::string(ModDirectory()) + "Data/_lvl_pc/").c_str());
    Log::Info("meshes", "%s", GameMeshes::Build(std::string(ModDirectory()) + "Data/_lvl_pc/").c_str());
    D3dSurvey::Install("first DirectInput8Create");
    EngineSurvey::Install();
#if SWBF_VR_DEV
    Script::Install();
#endif
    Lua::Install();
}

} // namespace

} // namespace Main

// The DirectInput8Create export (src/dinput8.def maps it to this name; extern "C" keeps the name free of C++
// decoration and of the namespace, WINAPI is the stdcall convention the game calls it with). Initialises the mod on
// the first call, then forwards to the system's dinput8.dll and hooks the IDirectInput8 object it returns.
extern "C" HRESULT WINAPI
// NOLINTNEXTLINE(readability-identifier-naming): the internal name src/dinput8.def exports
Proxy_DirectInput8Create(HINSTANCE instance, DWORD version, REFIID interfaceId, LPVOID* directInput, LPUNKNOWN outer)
{
    // A static local initialised by a call: the call runs once, on the first pass (thread-safe in C++).
    static bool isInitialized = (Main::Initialize(), true);
    (void)isInitialized;
    using DirectInput8CreateFunction = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    DirectInput8CreateFunction realCreate = Main::_realDirectInput
                                                ? reinterpret_cast<DirectInput8CreateFunction>(reinterpret_cast<void*>(
                                                      GetProcAddress(Main::_realDirectInput, "DirectInput8Create")))
                                                : nullptr;
    if (!realCreate)
    {
        return E_FAIL;
    }
    const HRESULT result = realCreate(instance, version, interfaceId, directInput, outer);
    if (SUCCEEDED(result) && directInput && *directInput)
    {
        Input::OnDirectInputCreated(*directInput);
    }
    return result;
}

} // namespace SwbfVr

// Outside the namespace: the C runtime's DLL entry code calls DllMain by its global name.
BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        SwbfVr::Main::_module = module;
        DisableThreadLibraryCalls(module);
        GetModuleFileNameA(module, SwbfVr::Main::_directory, MAX_PATH);
        if (char* slash = strrchr(SwbfVr::Main::_directory, '\\'))
        {
            slash[1] = 0;
        }
        else
        {
            SwbfVr::Main::_directory[0] = 0;
        }
        // Must happen before the game's own code runs, so the device is seen when it is created.
        SwbfVr::D3dSurvey::Install("DllMain");
    }
    return TRUE;
}
