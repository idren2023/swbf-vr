// Xr.cpp: OpenXR output (headset), controllers and the simulated headset. Minimal by design:
//  - No openxr_loader.dll to ship: the active runtime is found through the registry (or SteamVR's install
//    folder) and loaded directly, which is all the standard loader does for an application without API layers.
//  - The game is Direct3D 9 and OpenXR needs Direct3D 11, so this module owns a small D3D11 device and the
//    eye images arrive as system-memory pixels (copied from the game's back buffer by Stereo.cpp).
// All OpenXR calls happen on the game's render thread. Callers wrap them in FpGuard.
// OpenXR structs are initialised with just their type tag, as the API intends.
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
// The OpenXR headers read these switches, so they have to be macros: Win32 and D3D11 enable the platform types used
// below; no prototypes, because every OpenXR function is looked up in the runtime at run time (no loader library).
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_NO_PROTOTYPES

#include "Xr.h"

#include "FpGuard.h"
#include "GameBuild.h"
#include "GpuTransfer.h"
#include "Log.h"
#include "Main.h"
#if SWBF_VR_DEV
#include "Script.h" // src/dev: the developer's test scripts, not in the public source
#endif

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <tlhelp32.h>

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace SwbfVr
{

namespace Xr
{

namespace
{

// ---- Constants ----

/// Degrees in one radian, and the other way round (log lines and script angles).
constexpr float DEGREES_PER_RADIAN = 57.29578f;
constexpr float RADIANS_PER_DEGREE = 0.01745329f;
/// A full turn in radians (phase of the simulated head's and hands' swing).
constexpr float FULL_TURN_RADIANS = 6.2831853f;
/// RegGetValue flag RRF_SUBKEY_WOW6464KEY: read the 64-bit registry view.
constexpr DWORD REGISTRY_64_BIT_VIEW = 0x00010000;
/// The flat virtual screen (menus, loading): its distance ahead of the resting pose and its width.
constexpr float FLAT_SCREEN_DISTANCE_METERS = 2.0f;
constexpr float FLAT_SCREEN_WIDTH_METERS = 2.4f;
/// A field of view from the runtime counts as sensible when both half-angle tangents lie between these.
constexpr float MINIMUM_SENSIBLE_TAN = 0.2f;
constexpr float MAXIMUM_SENSIBLE_TAN = 5.0f;
/// How many failed xrLocateViews or xrEndFrame calls are logged (each).
constexpr unsigned MAXIMUM_LOGGED_FAILURES = 5;
/// XrDuration is in nanoseconds.
constexpr float NANOSECONDS_PER_SECOND = 1e9f;
/// Simulated headset: half the distance between the eyes, and how far its field of view reaches up and down (44 and
/// 52 degrees, Quest-like).
constexpr float SIMULATED_HALF_EYE_DISTANCE_METERS = 0.032f;
constexpr float SIMULATED_FOV_UP_RADIANS = 0.7679f;
constexpr float SIMULATED_FOV_DOWN_RADIANS = 0.9076f;

// ---- OpenXR functions ----

// The OpenXR functions we use, looked up by name in the runtime once the instance exists (xrCreateInstance itself
// comes first, separately). An "X-macro": the list is written once, and each use defines X to expand every entry
// differently (below a function pointer field per entry, in LoadRuntime one lookup per entry). It has to be a macro:
// C++ has no other way to turn one list of names into both declarations and string literals.
// clang-format off
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): an X-macro, see above
#define XR_FUNCTIONS(X)                                                                                                \
    X(DestroyInstance)                                                                                                 \
    X(GetInstanceProperties)                                                                                           \
    X(GetSystem)                                                                                                       \
    X(GetSystemProperties)                                                                                             \
    X(GetD3D11GraphicsRequirementsKHR)                                                                                 \
    X(CreateSession)                                                                                                   \
    X(DestroySession)                                                                                                  \
    X(CreateReferenceSpace)                                                                                            \
    X(DestroySpace)                                                                                                    \
    X(EnumerateViewConfigurationViews)                                                                                 \
    X(EnumerateSwapchainFormats)                                                                                       \
    X(CreateSwapchain)                                                                                                 \
    X(DestroySwapchain)                                                                                                \
    X(EnumerateSwapchainImages)                                                                                        \
    X(AcquireSwapchainImage)                                                                                           \
    X(WaitSwapchainImage)                                                                                              \
    X(ReleaseSwapchainImage)                                                                                           \
    X(PollEvent)                                                                                                       \
    X(BeginSession)                                                                                                    \
    X(EndSession)                                                                                                      \
    X(WaitFrame)                                                                                                       \
    X(BeginFrame)                                                                                                      \
    X(EndFrame)                                                                                                        \
    X(LocateViews)                                                                                                     \
    X(StringToPath)                                                                                                    \
    X(CreateActionSet)                                                                                                 \
    X(DestroyActionSet)                                                                                                \
    X(CreateAction)                                                                                                    \
    X(SuggestInteractionProfileBindings)                                                                               \
    X(AttachSessionActionSets)                                                                                         \
    X(CreateActionSpace)                                                                                               \
    X(SyncActions)                                                                                                     \
    X(LocateSpace)                                                                                                     \
    X(GetActionStateFloat)                                                                                             \
    X(GetActionStateVector2f)                                                                                          \
    X(GetActionStateBoolean)                                                                                           \
    X(ApplyHapticFeedback)
// clang-format on

/// Pointers to the runtime's OpenXR functions, named without the "xr" prefix (_xr.CreateSession is xrCreateSession).
// NOLINTBEGIN(readability-identifier-naming): fields named after the OpenXR functions
struct Functions
{
    PFN_xrCreateInstance CreateInstance;
#define X(name) PFN_xr##name name;
    XR_FUNCTIONS(X)
#undef X
} _xr;

// NOLINTEND(readability-identifier-naming)

// ---- State ----

HMODULE _runtime = nullptr;
XrInstance _instance = XR_NULL_HANDLE;
XrSystemId _systemId = XR_NULL_SYSTEM_ID;
XrSession _session = XR_NULL_HANDLE;
XrSpace _space = XR_NULL_HANDLE; // LOCAL reference space: eyes, hands and layers are all located in it
// Controllers (see SetUpHands)
XrActionSet _actionSet = XR_NULL_HANDLE;
XrAction _poseAction = XR_NULL_HANDLE;
XrSpace _handSpaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE}; // 0 right, 1 left
XrPosef _handPoses[2];
bool _isHandTracked[2] = {false, false};
// The grip pose (/input/grip/pose: the centre of the controller's handle, in the palm), for our own arm (BodyArm):
// the hand is placed on it and the weapon on the hand.
XrAction _gripPoseAction = XR_NULL_HANDLE;
XrSpace _gripSpaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
XrPosef _gripPoses[2];
bool _isGripTracked[2] = {false, false};
bool _areHandsReady = false;
XrAction _gripAction = XR_NULL_HANDLE, _stickClickAction = XR_NULL_HANDLE, _menuAction = XR_NULL_HANDLE,
         _hapticAction = XR_NULL_HANDLE;
XrAction _triggerTouchAction = XR_NULL_HANDLE, _thumbTouchAction = XR_NULL_HANDLE; // optional (touch sensors)
XrAction _triggerAction = XR_NULL_HANDLE, _stickAction = XR_NULL_HANDLE,
         _buttonActions[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE}; // buttons: lower (A/X), upper (B/Y)
XrPath _handPaths[2] = {XR_NULL_PATH, XR_NULL_PATH};
ControllerInput _controllerInput{};
bool _isStarted = false, _isSessionRunning = false, _hasFailed = false;
// Set by the script thread (and by runtime events), picked up by Service between frames.
volatile bool _isStartRequested = false, _isStopRequested = false;
ID3D11Device* _d3d11Device = nullptr;
ID3D11DeviceContext* _d3d11Context = nullptr;
LUID _d3d11AdapterLuid{}; // the adapter _d3d11Device is on
// Images on the graphics card (GpuTransfer, b268): its shared fences and textures opened in our device.
ID3D11Device5* _d3d11Device5 = nullptr;
ID3D11DeviceContext4* _d3d11Context4 = nullptr;
ID3D11Fence *_writtenFence = nullptr, *_readFence = nullptr;

struct OpenedTexture
{
    void* handle = nullptr;
    ID3D11Texture2D* texture = nullptr;
};

OpenedTexture _sharedTextures[3]; // by GpuTransfer slot
bool _isSharedRefused = false;    // opening them failed, or another adapter: images come as pixels (until VR restarts)
int64_t _swapchainFormat = 0;     // a DXGI_FORMAT
bool _isBgra = true;              // the swapchain format has the back buffer's byte order (no channel swap)
XrFrameState _frameState{XR_TYPE_FRAME_STATE};
XrView _views[2];
bool _areViewsValid = false; // _views holds poses and field of view from the headset
uint32_t _framesSubmitted = 0;

// Resting pose: where the player's head was (position and heading) when VR started or the view was last
// recentred with the Home key. The game camera sits exactly there; head movement is measured from it.
bool _hasOrigin = false;
volatile bool _isRecenterRequested = false;
XrVector3f _originPosition{0, 0, 0};
float _originYawRadians = 0;             // about +y
volatile LONG _recenterRequestCount = 0; // recentres asked for so far (RecenterCount)

// ---- Helpers ----

/// Product of two quaternions (b is applied first, then a).
XrQuaternionf MultiplyQuaternions(const XrQuaternionf& a, const XrQuaternionf& b)
{
    return XrQuaternionf{
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

/// Rotation about +y (left positive).
XrQuaternionf YawQuaternion(float yawRadians)
{
    return XrQuaternionf{0, sinf(yawRadians * 0.5f), 0, cosf(yawRadians * 0.5f)};
}

/// Turns a vector about +y.
XrVector3f RotateYaw(float yawRadians, const XrVector3f& vector)
{
    const float cosine = cosf(yawRadians), sine = sinf(yawRadians);
    return XrVector3f{cosine * vector.x + sine * vector.z, vector.y, -sine * vector.x + cosine * vector.z};
}

/// An OpenXR swapchain and its Direct3D 11 images.
struct Swapchain
{
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<ID3D11Texture2D*> images;
    int width = 0, height = 0;
} _eyeSwapchains[2], _flatSwapchain;

/// The eye or flat image with red and blue swapped, when the runtime only offers RGBA formats.
std::vector<uint8_t> _swizzledPixels;

/// Logs a failed OpenXR call.
/// @return true when the result is a success.
bool Check(XrResult result, const char* what)
{
    if (XR_SUCCEEDED(result))
    {
        return true;
    }
    Log::Error("xr", "%s failed (%d)", what, static_cast<int>(result));
    return false;
}

// ---- Finding the runtime ----

/// @return true when the path names an existing file (not a folder).
bool FileExists(const std::string& path)
{
    const DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

/// Appends a whole file to `text`.
/// @return false when the file cannot be opened.
bool ReadTextFile(const std::string& path, std::string& text)
{
    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "rb") != 0 || !file)
    {
        return false;
    }
    char buffer[4096];
    size_t byteCount;
    while ((byteCount = fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        text.append(buffer, byteCount);
    }
    fclose(file);
    return true;
}

// Value of the first JSON string that follows `key` (handles "key": "value" and "key": ["value", ...]).
bool FindJsonString(const std::string& json, const char* key, std::string& value)
{
    size_t position = json.find(std::string("\"") + key + "\"");
    if (position == std::string::npos)
    {
        return false;
    }
    position = json.find(':', position);
    if (position == std::string::npos)
    {
        return false;
    }
    position = json.find('"', position);
    if (position == std::string::npos)
    {
        return false;
    }
    value.clear();
    for (++position; position < json.size() && json[position] != '"'; ++position)
    {
        if (json[position] == '\\' && position + 1 < json.size())
        {
            ++position;
        }
        value += json[position];
    }
    return !value.empty();
}

/// The folder part of a path, with its trailing slash; empty when there is none.
std::string DirectoryOf(const std::string& path)
{
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

/// The active OpenXR runtime's manifest path from the Khronos registry key.
/// @param viewFlag 0 for the 32-bit registry view, REGISTRY_64_BIT_VIEW for the 64-bit one.
/// @param path set to the manifest path when the result is true.
bool ReadRegistryRuntime(DWORD viewFlag, std::string& path)
{
    char value[MAX_PATH * 2];
    DWORD size = sizeof(value);
    if (RegGetValueA(
            HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Khronos\\OpenXR\\1",
            "ActiveRuntime",
            RRF_RT_REG_SZ | viewFlag,
            nullptr,
            value,
            &size) != ERROR_SUCCESS)
    {
        return false;
    }
    path = value;
    return true;
}

// Path of the manifest (JSON) of a 32-bit OpenXR runtime, or empty.
std::string FindRuntimeManifest()
{
    char environmentValue[MAX_PATH * 2];
    if (GetEnvironmentVariableA("XR_RUNTIME_JSON", environmentValue, sizeof(environmentValue)) > 0 &&
        FileExists(environmentValue))
    {
        Log::Info("xr", "runtime from XR_RUNTIME_JSON: %s", environmentValue);
        // SteamVR sets this to its 64-bit manifest for every game it sees start (first headset run, 2026-10-07:
        // loading that library fails with error 193). The 32-bit manifest lies next to it.
        std::string manifest = environmentValue;
        const size_t at = manifest.rfind("win64");
        if (at != std::string::npos)
        {
            std::string sibling = manifest;
            sibling.replace(at, 5, "win32");
            if (FileExists(sibling))
            {
                Log::Info("xr", "using the 32-bit manifest next to it: %s", sibling.c_str());
                return sibling;
            }
        }
        return manifest;
    }
    std::string path;
    if (ReadRegistryRuntime(0, path))
    {
        Log::Info("xr", "active runtime (32-bit registry): %s", path.c_str());
        if (FileExists(path))
        {
            return path;
        }
    }
    else
    {
        Log::Info("xr", "no active runtime in the 32-bit registry view");
    }
    // The 64-bit entry cannot be loaded by a 32-bit game, but SteamVR keeps its 32-bit manifest next to it.
    if (ReadRegistryRuntime(REGISTRY_64_BIT_VIEW, path))
    {
        Log::Info("xr", "active runtime (64-bit registry): %s", path.c_str());
        const std::string sibling = DirectoryOf(path) + "steamxr_win32.json";
        if (FileExists(sibling))
        {
            return sibling;
        }
    }
    char localAppData[MAX_PATH];
    if (GetEnvironmentVariableA("LOCALAPPDATA", localAppData, sizeof(localAppData)) > 0)
    {
        std::string vrPaths, steamVrFolder;
        if (ReadTextFile(std::string(localAppData) + "\\openvr\\openvrpaths.vrpath", vrPaths) &&
            FindJsonString(vrPaths, "runtime", steamVrFolder))
        {
            const std::string manifest = steamVrFolder + "\\steamxr_win32.json";
            Log::Info("xr", "SteamVR folder from openvrpaths: %s", steamVrFolder.c_str());
            if (FileExists(manifest))
            {
                return manifest;
            }
        }
    }
    return std::string();
}

/// Finds and loads the runtime library, negotiates with it as the loader would, creates the OpenXR instance and looks
/// up every function of XR_FUNCTIONS.
/// @return false when there is no usable runtime (logged).
bool LoadRuntime()
{
    const std::string manifest = FindRuntimeManifest();
    if (manifest.empty())
    {
        Log::Info("xr", "no 32-bit OpenXR runtime found");
        return false;
    }
    std::string json, library;
    if (!ReadTextFile(manifest, json) || !FindJsonString(json, "library_path", library))
    {
        Log::Warning("xr", "cannot read library_path from %s", manifest.c_str());
        return false;
    }
    for (char& character : library)
    {
        if (character == '/')
        {
            character = '\\';
        }
    }
    if (!(library.size() > 2 && (library[1] == ':' || library[0] == '\\')))
    {
        library = DirectoryOf(manifest) + library;
    }
    _runtime = LoadLibraryExA(library.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!_runtime)
    {
        Log::Error("xr", "cannot load runtime %s (error %lu)", library.c_str(), GetLastError());
        return false;
    }
    Log::Info("xr", "runtime library %s", library.c_str());
    auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
        reinterpret_cast<void*>(GetProcAddress(_runtime, "xrNegotiateLoaderRuntimeInterface")));
    if (!negotiate)
    {
        Log::Info("xr", "runtime has no xrNegotiateLoaderRuntimeInterface");
        return false;
    }
    XrNegotiateLoaderInfo loader{};
    loader.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    loader.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    loader.structSize = sizeof(loader);
    loader.minInterfaceVersion = 1;
    loader.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    loader.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    loader.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
    XrNegotiateRuntimeRequest request{};
    request.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
    request.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
    request.structSize = sizeof(request);
    if (!Check(negotiate(&loader, &request), "runtime negotiation") || !request.getInstanceProcAddr)
    {
        return false;
    }
    PFN_xrGetInstanceProcAddr getInstanceProcAddr = request.getInstanceProcAddr;
    if (!Check(
            getInstanceProcAddr(
                XR_NULL_HANDLE,
                "xrCreateInstance",
                reinterpret_cast<PFN_xrVoidFunction*>(&_xr.CreateInstance)),
            "lookup of xrCreateInstance"))
    {
        return false;
    }

    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(createInfo.applicationInfo.applicationName, "swbf-vr");
    createInfo.applicationInfo.applicationVersion = 1;
    strcpy_s(createInfo.applicationInfo.engineName, "Zero (Battlefront 2004)");
    createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    const char* extensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    createInfo.enabledExtensionCount = 1;
    createInfo.enabledExtensionNames = extensions;
    const XrResult created = _xr.CreateInstance(&createInfo, &_instance);
    if (created == XR_ERROR_RUNTIME_FAILURE)
    {
        Log::Warning("xr", "the runtime could not start (SteamVR not running or no headset connected)");
    }
    if (!Check(created, "xrCreateInstance"))
    {
        _instance = XR_NULL_HANDLE;
        return false;
    }
    // One lookup per entry of the X-macro list (see XR_FUNCTIONS); "xr" #name pastes the name into a string literal.
#define X(name)                                                                                                        \
    if (!Check(                                                                                                        \
            getInstanceProcAddr(_instance, "xr" #name, reinterpret_cast<PFN_xrVoidFunction*>(&_xr.name)),              \
            "lookup of xr" #name))                                                                                     \
        return false;
    XR_FUNCTIONS(X)
#undef X
    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(_xr.GetInstanceProperties(_instance, &properties)))
    {
        Log::Info(
            "xr",
            "instance created, runtime \"%s\" %u.%u.%u",
            properties.runtimeName,
            static_cast<unsigned>(XR_VERSION_MAJOR(properties.runtimeVersion)),
            static_cast<unsigned>(XR_VERSION_MINOR(properties.runtimeVersion)),
            static_cast<unsigned>(XR_VERSION_PATCH(properties.runtimeVersion)));
    }
    return true;
}

UINT _gameVendorId = 0, _gameDeviceId = 0; // PCI ids of the adapter the game's Direct3D 9 device is on

// ---- Direct3D 11 device on the adapter the runtime asks for ----

/// Creates _d3d11Device and _d3d11Context (dxgi.dll and d3d11.dll are loaded at run time, nothing is linked).
/// @param requirements the runtime's adapter and minimum feature level.
/// @return false when no suitable device could be made (logged).
bool CreateD3d11Device(const XrGraphicsRequirementsD3D11KHR& requirements)
{
    HMODULE dxgi = LoadLibraryA("dxgi.dll"), d3d11 = LoadLibraryA("d3d11.dll");
    using CreateFactoryFunction = HRESULT(WINAPI*)(REFIID, void**);
    using CreateDeviceFunction = HRESULT(WINAPI*)(
        IDXGIAdapter*,
        D3D_DRIVER_TYPE,
        HMODULE,
        UINT,
        const D3D_FEATURE_LEVEL*,
        UINT,
        UINT,
        ID3D11Device**,
        D3D_FEATURE_LEVEL*,
        ID3D11DeviceContext**);
    auto createFactory = dxgi ? reinterpret_cast<CreateFactoryFunction>(
                                    reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")))
                              : nullptr;
    auto createDevice = d3d11 ? reinterpret_cast<CreateDeviceFunction>(
                                    reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDevice")))
                              : nullptr;
    if (!createFactory || !createDevice)
    {
        Log::Warning("xr", "Direct3D 11 is not available");
        return false;
    }
    IDXGIFactory1* factory = nullptr;
    if (FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory)
    {
        Log::Error("xr", "CreateDXGIFactory1 failed");
        return false;
    }
    // Which graphics adapter: the one the runtime names; if it names none (or one that is not there), the one
    // the game itself renders on; failing that, the first.
    IDXGIAdapter1* adapter = nullptr;
    IDXGIAdapter1* gameAdapter = nullptr;
    IDXGIAdapter1* firstAdapter = nullptr;
    IDXGIAdapter1* candidate = nullptr;
    Log::Info(
        "xr",
        "the runtime asks for adapter %08lx:%08lx",
        static_cast<unsigned long>(requirements.adapterLuid.HighPart),
        static_cast<unsigned long>(requirements.adapterLuid.LowPart));
    for (UINT adapterIndex = 0; factory->EnumAdapters1(adapterIndex, &candidate) != DXGI_ERROR_NOT_FOUND;
         ++adapterIndex)
    {
        DXGI_ADAPTER_DESC1 description{};
        candidate->GetDesc1(&description);
        Log::Info(
            "xr",
            "adapter %u: %ls (%04x:%04x) id %08lx:%08lx%s",
            adapterIndex,
            description.Description,
            description.VendorId,
            description.DeviceId,
            static_cast<unsigned long>(description.AdapterLuid.HighPart),
            static_cast<unsigned long>(description.AdapterLuid.LowPart),
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " software" : "");
        const bool isSoftware = (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        if (!adapter && memcmp(&description.AdapterLuid, &requirements.adapterLuid, sizeof(LUID)) == 0)
        {
            adapter = candidate;
            candidate->AddRef();
        }
        if (!gameAdapter && _gameVendorId && description.VendorId == _gameVendorId &&
            description.DeviceId == _gameDeviceId)
        {
            gameAdapter = candidate;
            candidate->AddRef();
        }
        if (!firstAdapter && !isSoftware)
        {
            firstAdapter = candidate;
            candidate->AddRef();
        }
        candidate->Release();
        candidate = nullptr;
    }
    factory->Release();
    if (!adapter)
    {
        adapter = gameAdapter ? gameAdapter : firstAdapter;
        if (adapter)
        {
            adapter->AddRef();
            Log::Info(
                "xr",
                "that adapter is not present; using %s",
                gameAdapter ? "the one the game renders on" : "the first one");
        }
    }
    if (gameAdapter)
    {
        gameAdapter->Release();
    }
    if (firstAdapter)
    {
        firstAdapter->Release();
    }
    if (!adapter)
    {
        Log::Info("xr", "no graphics adapter found");
        return false;
    }
    DXGI_ADAPTER_DESC1 chosenDescription{};
    adapter->GetDesc1(&chosenDescription);
    _d3d11AdapterLuid = chosenDescription.AdapterLuid;
    const D3D_FEATURE_LEVEL featureLevels[] =
        {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT result = createDevice(
        adapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        0,
        featureLevels,
        4,
        D3D11_SDK_VERSION,
        &_d3d11Device,
        &featureLevel,
        &_d3d11Context);
    if (result == E_INVALIDARG)
    {
        result = createDevice(
            adapter,
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            0,
            featureLevels + 1,
            3,
            D3D11_SDK_VERSION,
            &_d3d11Device,
            &featureLevel,
            &_d3d11Context);
    }
    adapter->Release();
    if (FAILED(result) || !_d3d11Device)
    {
        Log::Error("xr", "D3D11CreateDevice failed 0x%lx", result);
        return false;
    }
    if (featureLevel < requirements.minFeatureLevel)
    {
        Log::Info(
            "xr",
            "Direct3D feature level 0x%x is below the runtime's minimum 0x%x",
            static_cast<unsigned>(featureLevel),
            static_cast<unsigned>(requirements.minFeatureLevel));
        return false;
    }
    Log::Info("xr", "Direct3D 11 device created, feature level 0x%x", static_cast<unsigned>(featureLevel));
    return true;
}

/// Destroys a swapchain (if it exists) and clears its record.
void ReleaseSwapchain(Swapchain& swapchain)
{
    if (swapchain.handle != XR_NULL_HANDLE)
    {
        _xr.DestroySwapchain(swapchain.handle);
    }
    swapchain = Swapchain();
}

/// Makes sure the swapchain exists at this size, creating it again when the size changed.
/// @return false when it could not be created (logged).
bool EnsureSwapchain(Swapchain& swapchain, int width, int height)
{
    if (swapchain.handle != XR_NULL_HANDLE && swapchain.width == width && swapchain.height == height)
    {
        return true;
    }
    ReleaseSwapchain(swapchain);
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags =
        XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = _swapchainFormat;
    info.sampleCount = 1;
    info.width = static_cast<uint32_t>(width);
    info.height = static_cast<uint32_t>(height);
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!Check(_xr.CreateSwapchain(_session, &info, &swapchain.handle), "xrCreateSwapchain"))
    {
        swapchain.handle = XR_NULL_HANDLE;
        return false;
    }
    uint32_t imageCount = 0;
    _xr.EnumerateSwapchainImages(swapchain.handle, 0, &imageCount, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> images(
        imageCount,
        XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (!imageCount || !Check(
                           _xr.EnumerateSwapchainImages(
                               swapchain.handle,
                               imageCount,
                               &imageCount,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
                           "xrEnumerateSwapchainImages"))
    {
        ReleaseSwapchain(swapchain);
        return false;
    }
    for (const auto& image : images)
    {
        swapchain.images.push_back(image.texture);
    }
    swapchain.width = width;
    swapchain.height = height;
    Log::Info(
        "xr",
        "swapchain %dx%d with %u images, format %d",
        width,
        height,
        imageCount,
        static_cast<int>(_swapchainFormat));
    return true;
}

// Copies BGRA pixels (the layout of the game's back buffer) into the next image of a swapchain.
// Profiling::Scope (Main.h) is an RAII timer: it starts when created and adds the time to its stage when it goes out of
// scope at the closing brace, so the extra blocks below only mark what is timed.
bool CopyToSwapchain(Swapchain& swapchain, const void* pixels, int pitch, int width, int height)
{
    if (!EnsureSwapchain(swapchain, width, height))
    {
        return false;
    }
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    bool hasWaited = false;
    {
        Profiling::Scope profile(Profiling::Stage::Acquire);
        if (!Check(_xr.AcquireSwapchainImage(swapchain.handle, &acquire, &index), "xrAcquireSwapchainImage"))
        {
            return false;
        }
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        hasWaited = Check(_xr.WaitSwapchainImage(swapchain.handle, &wait), "xrWaitSwapchainImage");
    }
    if (hasWaited && index < swapchain.images.size())
    {
        if (!_isBgra)
        { // the runtime only offers RGBA: swap red and blue on the way
            _swizzledPixels.resize(static_cast<size_t>(pitch) * height);
            const uint8_t* source = static_cast<const uint8_t*>(pixels);
            uint8_t* destination = _swizzledPixels.data();
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    const uint8_t* sourcePixel = source + static_cast<size_t>(y) * pitch + x * 4;
                    uint8_t* destinationPixel = destination + static_cast<size_t>(y) * pitch + x * 4;
                    destinationPixel[0] = sourcePixel[2];
                    destinationPixel[1] = sourcePixel[1];
                    destinationPixel[2] = sourcePixel[0];
                    destinationPixel[3] = 255;
                }
            }
            pixels = _swizzledPixels.data();
        }
        Profiling::Scope profile(Profiling::Stage::Upload);
        _d3d11Context->UpdateSubresource(swapchain.images[index], 0, nullptr, pixels, static_cast<UINT>(pitch), 0);
    }
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    {
        Profiling::Scope profile(Profiling::Stage::Release);
        _xr.ReleaseSwapchainImage(swapchain.handle, &release);
    }
    return hasWaited;
}

/// Gives up on images on the graphics card until VR restarts (they then come as pixels).
void RefuseSharedImages(const char* why, HRESULT result)
{
    if (!_isSharedRefused)
    {
        Log::Warning(
            "xr",
            "images on the graphics card refused: %s (0x%08lx); eye images through system memory",
            why,
            static_cast<unsigned long>(result));
    }
    _isSharedRefused = true;
}

/// Longest CPU wait for GpuTransfer's copy of an eye image before it goes to the swapchain (normally done by then; a
/// frame at 72 Hz is 13.9 ms).
constexpr DWORD WRITTEN_WAIT_MILLISECONDS = 25;

/// Opens GpuTransfer's fences (once per device) and the image's shared texture (again when its handle changes).
/// @return the texture in our device, or nullptr (refused, logged).
ID3D11Texture2D* OpenSharedImage(const GpuTransfer::SharedImage& image)
{
    HRESULT result = S_OK;
    if (!_readFence)
    {
        if (FAILED(
                result =
                    _d3d11Device->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&_d3d11Device5))) ||
            FAILED(
                result = _d3d11Context->QueryInterface(
                    __uuidof(ID3D11DeviceContext4),
                    reinterpret_cast<void**>(&_d3d11Context4))) ||
            FAILED(
                result = _d3d11Device5->OpenSharedFence(
                    image.written,
                    __uuidof(ID3D11Fence),
                    reinterpret_cast<void**>(&_writtenFence))) ||
            FAILED(
                result = _d3d11Device5->OpenSharedFence(
                    image.read,
                    __uuidof(ID3D11Fence),
                    reinterpret_cast<void**>(&_readFence))))
        {
            RefuseSharedImages("Direct3D 11.4 or its shared fences", result);
            return nullptr;
        }
    }
    OpenedTexture& opened = _sharedTextures[image.slot];
    if (opened.handle != image.texture)
    {
        if (opened.texture)
        {
            opened.texture->Release();
        }
        opened = OpenedTexture();
        if (FAILED(
                result = _d3d11Device5->OpenSharedResource1(
                    image.texture,
                    __uuidof(ID3D11Texture2D),
                    reinterpret_cast<void**>(&opened.texture))))
        {
            opened.texture = nullptr;
            RefuseSharedImages("its shared texture", result);
            return nullptr;
        }
        opened.handle = image.texture;
    }
    return opened.texture;
}

/// Tells GpuTransfer that its texture may be written again (in order on our queue, after any copy from it).
void SignalSharedRead(const GpuTransfer::SharedImage& image)
{
    if (_d3d11Context4 && _readFence)
    {
        _d3d11Context4->Signal(_readFence, image.fenceValue);
        _d3d11Context->Flush();
    }
}

// Copies an image on the graphics card into the next image of a swapchain: our queue waits on the card for
// GpuTransfer's copy, copies, and signals that the shared texture is free again (also when nothing was copied).
bool CopySharedToSwapchain(Swapchain& swapchain, const GpuTransfer::SharedImage& image)
{
    ID3D11Texture2D* texture = OpenSharedImage(image);
    if (!texture || !EnsureSwapchain(swapchain, image.width, image.height))
    {
        SignalSharedRead(image);
        return false;
    }
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    bool hasWaited = false;
    {
        Profiling::Scope profile(Profiling::Stage::Acquire);
        if (!Check(_xr.AcquireSwapchainImage(swapchain.handle, &acquire, &index), "xrAcquireSwapchainImage"))
        {
            SignalSharedRead(image);
            return false;
        }
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        hasWaited = Check(_xr.WaitSwapchainImage(swapchain.handle, &wait), "xrWaitSwapchainImage");
    }
    if (hasWaited && index < swapchain.images.size())
    {
        Profiling::Scope profile(Profiling::Stage::Upload);
        // GpuTransfer's copy checked on the CPU, capped, instead of a wait queued on the card (headset 2026-11-13: with
        // the card-side wait, SteamVR, which syncs to our queue in xrWaitFrame, waited on it while the copy waited on
        // Direct3D 9 work the blocked game could not issue: stalls of 8 s after zooming, then the compositor's
        // watchdog and a locked PC). Past the cap the copy goes ahead: at worst one image shows part of the next.
        if (_writtenFence->GetCompletedValue() < image.fenceValue)
        {
            static HANDLE writtenEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!writtenEvent || FAILED(_writtenFence->SetEventOnCompletion(image.fenceValue, writtenEvent)) ||
                WaitForSingleObject(writtenEvent, WRITTEN_WAIT_MILLISECONDS) != WAIT_OBJECT_0)
            {
                static int logsLeft = 10;
                if (logsLeft > 0)
                {
                    --logsLeft;
                    Log::Info("xr", "shared image %llu not written within %lu ms: copied without waiting",
                              static_cast<unsigned long long>(image.fenceValue), WRITTEN_WAIT_MILLISECONDS);
                }
            }
        }
        _d3d11Context->CopyResource(swapchain.images[index], texture);
    }
    SignalSharedRead(image);
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    {
        Profiling::Scope profile(Profiling::Stage::Release);
        _xr.ReleaseSwapchainImage(swapchain.handle, &release);
    }
    return hasWaited;
}

/// Destroys everything OpenXR and Direct3D 11 (swapchains, spaces, session, instance, device) and resets the state.
void Shutdown()
{
    if (_instance != XR_NULL_HANDLE)
    {
        if (_session != XR_NULL_HANDLE)
        {
            ReleaseSwapchain(_eyeSwapchains[0]);
            ReleaseSwapchain(_eyeSwapchains[1]);
            ReleaseSwapchain(_flatSwapchain);
            for (XrSpace& space : _handSpaces)
            {
                if (space != XR_NULL_HANDLE)
                {
                    _xr.DestroySpace(space);
                }
                space = XR_NULL_HANDLE;
            }
            for (XrSpace& space : _gripSpaces)
            {
                if (space != XR_NULL_HANDLE)
                {
                    _xr.DestroySpace(space);
                }
                space = XR_NULL_HANDLE;
            }
            if (_space != XR_NULL_HANDLE)
            {
                _xr.DestroySpace(_space);
            }
            _xr.DestroySession(_session);
        }
        _xr.DestroyInstance(_instance);
    }
    _instance = XR_NULL_HANDLE;
    _session = XR_NULL_HANDLE;
    _space = XR_NULL_HANDLE;
    for (OpenedTexture& opened : _sharedTextures)
    {
        if (opened.texture)
        {
            opened.texture->Release();
        }
        opened = OpenedTexture();
    }
    for (IUnknown** object :
         {reinterpret_cast<IUnknown**>(&_writtenFence),
          reinterpret_cast<IUnknown**>(&_readFence),
          reinterpret_cast<IUnknown**>(&_d3d11Context4),
          reinterpret_cast<IUnknown**>(&_d3d11Device5)})
    {
        if (*object)
        {
            (*object)->Release();
            *object = nullptr;
        }
    }
    _isSharedRefused = false;
    if (_d3d11Context)
    {
        _d3d11Context->Release();
        _d3d11Context = nullptr;
    }
    if (_d3d11Device)
    {
        _d3d11Device->Release();
        _d3d11Device = nullptr;
    }
    _isStarted = _isSessionRunning = false;
    _areViewsValid = _hasOrigin = false;
    _actionSet = XR_NULL_HANDLE; // destroyed with the instance
    _poseAction = _gripPoseAction = XR_NULL_HANDLE;
    _areHandsReady = _isHandTracked[0] = _isHandTracked[1] = _isGripTracked[0] = _isGripTracked[1] = false;
    _triggerAction = _stickAction = _buttonActions[0] = _buttonActions[1] = XR_NULL_HANDLE;
    _controllerInput = ControllerInput{};
}

// ---- Motion controllers: one pose action, both hands ----
// The "aim" pose of each controller (origin near its front, -z pointing where it points) is located every frame in
// the same space as the eyes. Triggers, sticks, buttons and grips are actions of the same set, read per hand in
// LocateHands; vibration is an output action.

/// Creates an action of _actionSet with one subaction path per hand.
/// @param type pose, float, vector, boolean or vibration.
/// @param name the action's internal name (lower case, as OpenXR requires).
/// @param shownName the name the runtime shows in its binding screen.
/// @param action set to the new action.
/// @return false when the runtime refused it (logged).
bool CreateHandAction(XrActionType type, const char* name, const char* shownName, XrAction* action)
{
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    info.actionType = type;
    strcpy(info.actionName, name);
    strcpy(info.localizedActionName, shownName);
    info.countSubactionPaths = 2;
    info.subactionPaths = _handPaths;
    return Check(_xr.CreateAction(_actionSet, &info, action), "xrCreateAction");
}

/// Creates the action set, its actions, the bindings for every known controller type and one pose space per hand,
/// and attaches the set to the session.
/// @return false when the controllers cannot be used (VR then runs without them).
bool SetUpHands()
{
    if (!Check(_xr.StringToPath(_instance, "/user/hand/right", &_handPaths[0]), "xrStringToPath") ||
        !Check(_xr.StringToPath(_instance, "/user/hand/left", &_handPaths[1]), "xrStringToPath"))
    {
        return false;
    }
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(setInfo.actionSetName, "gameplay");
    strcpy(setInfo.localizedActionSetName, "Gameplay");
    if (!Check(_xr.CreateActionSet(_instance, &setInfo, &_actionSet), "xrCreateActionSet"))
    {
        return false;
    }
    if (!CreateHandAction(XR_ACTION_TYPE_POSE_INPUT, "hand_pose", "Hand pose", &_poseAction) ||
        !CreateHandAction(XR_ACTION_TYPE_POSE_INPUT, "grip_pose", "Grip pose", &_gripPoseAction) ||
        !CreateHandAction(XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger", &_triggerAction) ||
        !CreateHandAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "stick", "Stick", &_stickAction) ||
        !CreateHandAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_lower", "Lower button (A / X)", &_buttonActions[0]) ||
        !CreateHandAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_upper", "Upper button (B / Y)", &_buttonActions[1]) ||
        !CreateHandAction(XR_ACTION_TYPE_FLOAT_INPUT, "grip", "Grip", &_gripAction) ||
        !CreateHandAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click", "Stick click", &_stickClickAction) ||
        !CreateHandAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu button", &_menuAction))
    {
        return false;
    }
    if (!CreateHandAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "trigger_touch", "Finger on the trigger", &_triggerTouchAction) ||
        !CreateHandAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "thumb_touch", "Thumb on the face", &_thumbTouchAction))
    {
        _triggerTouchAction = _thumbTouchAction = XR_NULL_HANDLE;
        Log::Info("xr", "no touch actions; the open hand's poses come from the buttons alone");
    }
    if (!CreateHandAction(XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Vibration", &_hapticAction))
    {
        _hapticAction = XR_NULL_HANDLE;
        Log::Info("xr", "no vibration action; continuing without haptics");
    }

    // Bindings per controller type; a runtime ignores profiles it does not know. Paths are relative to the hand.
    struct Profile
    {
        const char* name;
        const char* trigger;
        const char* stick;
        const char* lower[2];
        const char* upper[2];
        const char* grip;
        const char* click;
        const char* menu;
    }; // [0] right, [1] left; menu: left hand only

    const Profile profiles[] = {
        {"/interaction_profiles/oculus/touch_controller",
         "/input/trigger/value",
         "/input/thumbstick",
         {"/input/a/click", "/input/x/click"},
         {"/input/b/click", "/input/y/click"},
         "/input/squeeze/value",
         "/input/thumbstick/click",
         "/input/menu/click"},
        {"/interaction_profiles/valve/index_controller",
         "/input/trigger/value",
         "/input/thumbstick",
         {"/input/a/click", "/input/a/click"},
         {"/input/b/click", "/input/b/click"},
         "/input/squeeze/value",
         "/input/thumbstick/click",
         nullptr},
        {"/interaction_profiles/htc/vive_controller",
         "/input/trigger/value",
         "/input/trackpad",
         {nullptr, nullptr},
         {nullptr, nullptr},
         "/input/squeeze/click",
         "/input/trackpad/click",
         "/input/menu/click"},
        {"/interaction_profiles/microsoft/motion_controller",
         "/input/trigger/value",
         "/input/thumbstick",
         {nullptr, nullptr},
         {nullptr, nullptr},
         "/input/squeeze/click",
         "/input/thumbstick/click",
         "/input/menu/click"},
        {"/interaction_profiles/khr/simple_controller",
         "/input/select/click",
         nullptr,
         {nullptr, nullptr},
         {nullptr, nullptr},
         nullptr,
         nullptr,
         "/input/menu/click"},
    };
    const char* handNames[2] = {"/user/hand/right", "/user/hand/left"};
    unsigned accepted = 0;
    for (const Profile& profile : profiles)
    {
        XrActionSuggestedBinding bindings[48];
        uint32_t bindingCount = 0;
        // A lambda (an inline function, like a C# lambda); [&] captures the surrounding locals by reference, so it
        // appends to this loop's `bindings` and `bindingCount`.
        const auto add = [&](XrAction action, int hand, const char* input)
        {
            if (!input)
            {
                return;
            }
            char path[128];
            snprintf(path, sizeof(path), "%s%s", handNames[hand], input);
            XrPath bindingPath;
            if (XR_SUCCEEDED(_xr.StringToPath(_instance, path, &bindingPath)))
            {
                bindings[bindingCount++] = XrActionSuggestedBinding{action, bindingPath};
            }
        };
        for (int hand = 0; hand < 2; ++hand)
        {
            add(_poseAction, hand, "/input/aim/pose");
            add(_gripPoseAction, hand, "/input/grip/pose");
            add(_triggerAction, hand, profile.trigger);
            add(_stickAction, hand, profile.stick);
            add(_buttonActions[0], hand, profile.lower[hand]);
            add(_buttonActions[1], hand, profile.upper[hand]);
            add(_gripAction, hand, profile.grip);
            add(_stickClickAction, hand, profile.click);
            if (hand == 1)
            {
                add(_menuAction, hand, profile.menu);
            }
        }
        // Vibration and then the touch sensors last, so that the list without them can be offered if a runtime refuses
        // it.
        const uint32_t countWithoutHaptics = bindingCount;
        if (_hapticAction != XR_NULL_HANDLE)
        {
            for (int hand = 0; hand < 2; ++hand)
            {
                add(_hapticAction, hand, "/output/haptic");
            }
        }
        const uint32_t countWithoutTouch = bindingCount;
        // touch sensors on the controllers that have them (several bindings of one action: any of them)
        const bool isTouch = !strcmp(profile.name, "/interaction_profiles/oculus/touch_controller");
        const bool isIndex = !strcmp(profile.name, "/interaction_profiles/valve/index_controller");
        for (int hand = 0; hand < 2 && _triggerTouchAction != XR_NULL_HANDLE && (isTouch || isIndex); ++hand)
        {
            add(_triggerTouchAction, hand, "/input/trigger/touch");
            add(_thumbTouchAction, hand, "/input/thumbstick/touch");
            add(_thumbTouchAction, hand, isTouch && hand == 1 ? "/input/x/touch" : "/input/a/touch");
            add(_thumbTouchAction, hand, isTouch && hand == 1 ? "/input/y/touch" : "/input/b/touch");
            add(_thumbTouchAction, hand, isTouch ? "/input/thumbrest/touch" : "/input/trackpad/touch");
        }
        XrPath profilePath;
        if (XR_FAILED(_xr.StringToPath(_instance, profile.name, &profilePath)))
        {
            continue;
        }
        XrInteractionProfileSuggestedBinding suggest{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggest.interactionProfile = profilePath;
        suggest.countSuggestedBindings = bindingCount;
        suggest.suggestedBindings = bindings;
        XrResult result = _xr.SuggestInteractionProfileBindings(_instance, &suggest);
        if (XR_FAILED(result) && bindingCount != countWithoutTouch)
        {
            Log::Warning(
                "xr", "bindings for %s refused with the touch sensors (%d), trying without", profile.name,
                static_cast<int>(result));
            suggest.countSuggestedBindings = countWithoutTouch;
            result = _xr.SuggestInteractionProfileBindings(_instance, &suggest);
        }
        if (XR_FAILED(result) && countWithoutTouch != countWithoutHaptics)
        {
            Log::Warning(
                "xr",
                "bindings for %s refused with vibration (%d), trying without",
                profile.name,
                static_cast<int>(result));
            suggest.countSuggestedBindings = countWithoutHaptics;
            result = _xr.SuggestInteractionProfileBindings(_instance, &suggest);
        }
        if (XR_SUCCEEDED(result))
        {
            ++accepted;
        }
        else
        {
            Log::Warning("xr", "bindings for %s refused (%d)", profile.name, static_cast<int>(result));
        }
    }
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &_actionSet;
    if (!Check(_xr.AttachSessionActionSets(_session, &attach), "xrAttachSessionActionSets"))
    {
        return false;
    }
    for (int hand = 0; hand < 2; ++hand)
    {
        XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        spaceInfo.action = _poseAction;
        spaceInfo.subactionPath = _handPaths[hand];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        if (!Check(_xr.CreateActionSpace(_session, &spaceInfo, &_handSpaces[hand]), "xrCreateActionSpace"))
        {
            return false;
        }
        spaceInfo.action = _gripPoseAction;
        if (!Check(_xr.CreateActionSpace(_session, &spaceInfo, &_gripSpaces[hand]), "xrCreateActionSpace (grip)"))
        {
            return false;
        }
    }
    Log::Info(
        "xr",
        "controllers set up: poses, triggers, sticks, buttons (%u of %u controller profiles accepted)",
        accepted,
        static_cast<unsigned>(sizeof(profiles) / sizeof(profiles[0])));
    return true;
}

// Per frame, between xrBeginFrame and xrEndFrame: syncs the actions, locates both controllers in _space and reads
// their triggers, sticks and buttons into _controllerInput.
void LocateHands()
{
    if (!_areHandsReady)
    {
        return;
    }
    XrActiveActionSet active{_actionSet, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    const XrResult synced =
        _xr.SyncActions(_session, &sync); // XR_SESSION_NOT_FOCUSED is normal while a system menu is up
    for (int hand = 0; hand < 2; ++hand)
    {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        const bool wasTracked = _isHandTracked[hand];
        const XrSpaceLocationFlags requiredFlags =
            XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        _isHandTracked[hand] =
            synced == XR_SUCCESS &&
            XR_SUCCEEDED(_xr.LocateSpace(_handSpaces[hand], _space, _frameState.predictedDisplayTime, &location)) &&
            (location.locationFlags & requiredFlags) == requiredFlags;
        if (_isHandTracked[hand])
        {
            _handPoses[hand] = location.pose;
        }
        XrSpaceLocation gripLocation{XR_TYPE_SPACE_LOCATION};
        const bool wasGripTracked = _isGripTracked[hand];
        _isGripTracked[hand] =
            synced == XR_SUCCESS &&
            XR_SUCCEEDED(_xr.LocateSpace(_gripSpaces[hand], _space, _frameState.predictedDisplayTime, &gripLocation)) &&
            (gripLocation.locationFlags & requiredFlags) == requiredFlags;
        if (_isGripTracked[hand])
        {
            _gripPoses[hand] = gripLocation.pose;
        }
        if (_isGripTracked[hand] && !wasGripTracked && _isHandTracked[hand])
        {
            // the aim pose in the grip pose's frame, once: for SIMULATED_GRIP_IN_AIM and the weapons' fine tuning
            const XrQuaternionf& grip = _gripPoses[hand].orientation;
            const XrQuaternionf inverse{-grip.x, -grip.y, -grip.z, grip.w};
            const XrQuaternionf relative = MultiplyQuaternions(inverse, _handPoses[hand].orientation);
            const XrQuaternionf offset{
                _handPoses[hand].position.x - _gripPoses[hand].position.x,
                _handPoses[hand].position.y - _gripPoses[hand].position.y,
                _handPoses[hand].position.z - _gripPoses[hand].position.z,
                0};
            // into the grip's frame: q^-1 v q
            const XrQuaternionf local = MultiplyQuaternions(MultiplyQuaternions(inverse, offset), grip);
            Log::Info(
                "xr",
                "%s aim pose in the grip pose: at %.3f %.3f %.3f m, turned by quaternion %.3f %.3f %.3f %.3f",
                hand == 0 ? "right" : "left",
                static_cast<double>(local.x),
                static_cast<double>(local.y),
                static_cast<double>(local.z),
                static_cast<double>(relative.x),
                static_cast<double>(relative.y),
                static_cast<double>(relative.z),
                static_cast<double>(relative.w));
        }
        if (wasTracked != _isHandTracked[hand])
        {
            Log::Info(
                "xr",
                "%s controller %s",
                hand == 0 ? "right" : "left",
                _isHandTracked[hand] ? "tracked" : "lost");
        }
    }
    ControllerInput input{};
    input.isValid = synced == XR_SUCCESS;
    for (int hand = 0; hand < 2 && input.isValid; ++hand)
    {
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.subactionPath = _handPaths[hand];
        XrActionStateFloat floatState{XR_TYPE_ACTION_STATE_FLOAT};
        get.action = _triggerAction;
        if (XR_SUCCEEDED(_xr.GetActionStateFloat(_session, &get, &floatState)) && floatState.isActive)
        {
            input.trigger[hand] = floatState.currentState;
        }
        XrActionStateVector2f vectorState{XR_TYPE_ACTION_STATE_VECTOR2F};
        get.action = _stickAction;
        if (XR_SUCCEEDED(_xr.GetActionStateVector2f(_session, &get, &vectorState)) && vectorState.isActive)
        {
            input.stickX[hand] = vectorState.currentState.x;
            input.stickY[hand] = vectorState.currentState.y;
        }
        for (int button = 0; button < 2; ++button)
        {
            XrActionStateBoolean buttonState{XR_TYPE_ACTION_STATE_BOOLEAN};
            get.action = _buttonActions[button];
            if (XR_SUCCEEDED(_xr.GetActionStateBoolean(_session, &get, &buttonState)) && buttonState.isActive)
            {
                (button == 0 ? input.isLowerPressed : input.isUpperPressed)[hand] = buttonState.currentState != 0;
            }
        }
        floatState = XrActionStateFloat{XR_TYPE_ACTION_STATE_FLOAT};
        get.action = _gripAction;
        if (XR_SUCCEEDED(_xr.GetActionStateFloat(_session, &get, &floatState)) && floatState.isActive)
        {
            input.grip[hand] = floatState.currentState;
        }
        if (_triggerTouchAction != XR_NULL_HANDLE)
        {
            XrActionStateBoolean touched{XR_TYPE_ACTION_STATE_BOOLEAN};
            get.action = _triggerTouchAction;
            const bool hasTrigger = XR_SUCCEEDED(_xr.GetActionStateBoolean(_session, &get, &touched)) && touched.isActive;
            input.isTriggerTouched[hand] = hasTrigger && touched.currentState != 0;
            touched = XrActionStateBoolean{XR_TYPE_ACTION_STATE_BOOLEAN};
            get.action = _thumbTouchAction;
            const bool hasThumb = XR_SUCCEEDED(_xr.GetActionStateBoolean(_session, &get, &touched)) && touched.isActive;
            input.isThumbTouched[hand] = hasThumb && touched.currentState != 0;
            input.hasTouch[hand] = hasTrigger && hasThumb;
        }
        XrActionStateBoolean pressed{XR_TYPE_ACTION_STATE_BOOLEAN};
        get.action = _stickClickAction;
        if (XR_SUCCEEDED(_xr.GetActionStateBoolean(_session, &get, &pressed)) && pressed.isActive)
        {
            input.isStickPressed[hand] = pressed.currentState != 0;
        }
        if (hand == 1)
        {
            pressed = XrActionStateBoolean{XR_TYPE_ACTION_STATE_BOOLEAN};
            get.action = _menuAction;
            const bool hasState = XR_SUCCEEDED(_xr.GetActionStateBoolean(_session, &get, &pressed));
            if (hasState && pressed.isActive)
            {
                input.isMenuPressed = pressed.currentState != 0;
            }
            // Does the runtime hand us the left menu button at all? (SteamVR opened its own menu on it, 2026-10-07.)
            // Menu state: 0 not given to the game, 1 available, 2 pressed; -1 nothing logged yet.
            static int reportedMenuState = -1;
            const int menuState = hasState && pressed.isActive ? (pressed.currentState ? 2 : 1) : 0;
            if (menuState != reportedMenuState && (menuState == 2 || reportedMenuState != 2 || menuState == 0))
            {
                if (reportedMenuState < 0 || menuState == 0 || reportedMenuState == 0 || menuState == 2)
                {
                    Log::Info(
                        "xr",
                        "left menu button: %s",
                        menuState == 0   ? "not given to the game by the runtime"
                        : menuState == 2 ? "pressed"
                                         : "available");
                }
                reportedMenuState = menuState;
            }
        }
    }
    _controllerInput = input;
}

/// Starts VR: loads the runtime, finds the headset, creates the Direct3D 11 device, the session, the LOCAL space and
/// the controllers, and picks the swapchain format. On failure the caller calls Shutdown.
/// @return false when VR cannot start (logged).
bool Start()
{
    if (!LoadRuntime())
    {
        return false;
    }
    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    const XrResult systemResult = _xr.GetSystem(_instance, &systemInfo, &_systemId);
    if (systemResult == XR_ERROR_FORM_FACTOR_UNAVAILABLE)
    {
        Log::Warning("xr", "no headset is connected (the runtime works, it just has no headset)");
        return false;
    }
    if (!Check(systemResult, "xrGetSystem"))
    {
        return false;
    }
    XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(_xr.GetSystemProperties(_instance, _systemId, &systemProperties)))
    {
        Log::Info("xr", "headset \"%s\"", systemProperties.systemName);
    }
    XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!Check(
            _xr.GetD3D11GraphicsRequirementsKHR(_instance, _systemId, &requirements),
            "xrGetD3D11GraphicsRequirementsKHR"))
    {
        return false;
    }
    if (!CreateD3d11Device(requirements))
    {
        return false;
    }
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = _d3d11Device;
    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = _systemId;
    if (!Check(_xr.CreateSession(_instance, &sessionInfo, &_session), "xrCreateSession"))
    {
        return false;
    }
    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    if (!Check(_xr.CreateReferenceSpace(_session, &spaceInfo, &_space), "xrCreateReferenceSpace"))
    {
        return false;
    }
    _areHandsReady = SetUpHands();
    if (!_areHandsReady)
    {
        Log::Warning("xr", "controller poses are not available; continuing without them");
    }

    uint32_t viewCount = 0;
    XrViewConfigurationView configurationViews[2] = {
        {XR_TYPE_VIEW_CONFIGURATION_VIEW},
        {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    if (XR_SUCCEEDED(_xr.EnumerateViewConfigurationViews(
            _instance,
            _systemId,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            2,
            &viewCount,
            configurationViews)))
    {
        Log::Info(
            "xr",
            "headset recommends %ux%u per eye (max %ux%u)",
            configurationViews[0].recommendedImageRectWidth,
            configurationViews[0].recommendedImageRectHeight,
            configurationViews[0].maxImageRectWidth,
            configurationViews[0].maxImageRectHeight);
    }

    uint32_t formatCount = 0;
    _xr.EnumerateSwapchainFormats(_session, 0, &formatCount, nullptr);
    std::vector<int64_t> formats(formatCount);
    if (!formatCount || !Check(
                            _xr.EnumerateSwapchainFormats(_session, formatCount, &formatCount, formats.data()),
                            "xrEnumerateSwapchainFormats"))
    {
        return false;
    }
    // Preference: BGRA matches the game's back buffer byte for byte; sRGB because the game outputs display-ready
    // colours.
    const int64_t preferredFormats[] = {
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM};
    for (int64_t preferredFormat : preferredFormats)
    {
        for (int64_t offeredFormat : formats)
        {
            if (offeredFormat == preferredFormat)
            {
                _swapchainFormat = preferredFormat;
                break;
            }
        }
        if (_swapchainFormat)
        {
            break;
        }
    }
    if (!_swapchainFormat)
    {
        Log::Info("xr", "the runtime offers no 8-bit RGBA or BGRA swapchain format");
        return false;
    }
    _isBgra = _swapchainFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || _swapchainFormat == DXGI_FORMAT_B8G8R8A8_UNORM;
    Log::Info(
        "xr",
        "session created, swapchain format %d (%s)",
        static_cast<int>(_swapchainFormat),
        _isBgra ? "BGRA, direct copy" : "RGBA, channels swapped on copy");
    return true;
}

/// Handles the runtime's events: begins and ends the session as its state changes, and asks for a stop when the
/// session or the instance is being lost.
void PollEvents()
{
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (_xr.PollEvent(_instance, &event) == XR_SUCCESS)
    {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            Log::Info("xr", "session state %d", static_cast<int>(changed->state));
            if (changed->state == XR_SESSION_STATE_READY)
            {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                _isSessionRunning = Check(_xr.BeginSession(_session, &begin), "xrBeginSession");
            }
            else if (changed->state == XR_SESSION_STATE_STOPPING)
            {
                _xr.EndSession(_session);
                _isSessionRunning = false;
            }
            else if (changed->state == XR_SESSION_STATE_EXITING || changed->state == XR_SESSION_STATE_LOSS_PENDING)
            {
                _isSessionRunning = false;
                _isStopRequested = true;
            }
        }
        else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
        {
            _isSessionRunning = false;
            _isStopRequested = true;
        }
        event = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// Takes the resting pose from the current head pose, on the first tracked frame and whenever a recentre is asked for.
void UpdateOrigin(bool isPositionValid)
{
    if (_hasOrigin && !_isRecenterRequested)
    {
        return;
    }
    if (!isPositionValid)
    {
        return;
    }
    const XrQuaternionf orientation = _views[0].pose.orientation;
    // where the head looks: the horizontal part of its forward (-z) axis
    const float forwardX = -2 * (orientation.x * orientation.z + orientation.w * orientation.y),
                forwardZ = -(1 - 2 * (orientation.x * orientation.x + orientation.y * orientation.y));
    if (forwardX * forwardX + forwardZ * forwardZ < 1e-4f)
    {
        return; // looking straight up or down: wait for a better moment
    }
    _originYawRadians = atan2f(-forwardX, -forwardZ);
    const XrVector3f left = _views[0].pose.position, right = _views[1].pose.position;
    _originPosition = XrVector3f{(left.x + right.x) * 0.5f, (left.y + right.y) * 0.5f, (left.z + right.z) * 0.5f};
    _hasOrigin = true;
    _isRecenterRequested = false;
    Log::Info(
        "xr",
        "view centred: head at %d,%d,%d cm, heading %d degrees",
        static_cast<int>(_originPosition.x * 100),
        static_cast<int>(_originPosition.y * 100),
        static_cast<int>(_originPosition.z * 100),
        static_cast<int>(_originYawRadians * DEGREES_PER_RADIAN));
}

// Asks the headset where both eyes will be when the current frame is shown. Call between xrBeginFrame and xrEndFrame.
bool LocateEyes()
{
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = _frameState.predictedDisplayTime;
    locate.space = _space;
    XrViewState state{XR_TYPE_VIEW_STATE};
    uint32_t viewCount = 0;
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    XrResult result;
    {
        Profiling::Scope profile(Profiling::Stage::Locate);
        result = _xr.LocateViews(_session, &locate, &state, 2, &viewCount, views);
    }
    if (XR_FAILED(result))
    {
        static unsigned loggedFailures = 0;
        if (loggedFailures++ < MAXIMUM_LOGGED_FAILURES)
        {
            Log::Error("xr", "xrLocateViews failed (%d)", static_cast<int>(result));
        }
        return false;
    }
    if (viewCount < 2 || !(state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
    {
        return false;
    }
    const bool isPositionValid = (state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
    if (!isPositionValid)
    {
        views[0].pose.position = views[1].pose.position = XrVector3f{0, 0, 0};
    }
    _views[0] = views[0];
    _views[1] = views[1];
    if (!_areViewsValid)
    {
        const XrFovf& fov = views[0].fov;
        Log::Info(
            "xr",
            "first head pose received; left eye field of view %d/%d/%d/%d degrees (left/right/up/down), position "
            "tracking %s",
            static_cast<int>(-fov.angleLeft * DEGREES_PER_RADIAN),
            static_cast<int>(fov.angleRight * DEGREES_PER_RADIAN),
            static_cast<int>(fov.angleUp * DEGREES_PER_RADIAN),
            static_cast<int>(-fov.angleDown * DEGREES_PER_RADIAN),
            isPositionValid ? "on" : "off");
    }
    _areViewsValid = true;
    UpdateOrigin(isPositionValid);
    {
        Profiling::Scope profile(Profiling::Stage::Locate);
        LocateHands();
    }
    return true;
}

/// Waits for the headset's next frame slot (xrWaitFrame, fills _frameState) and begins the frame.
/// @return false when either call failed; then no frame is open.
bool WaitAndBeginFrame()
{
    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
    _frameState = XrFrameState{XR_TYPE_FRAME_STATE};
    {
        Profiling::Scope profile(Profiling::Stage::Wait);
        if (!Check(_xr.WaitFrame(_session, &wait, &_frameState), "xrWaitFrame"))
        {
            return false;
        }
    }
    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
    Profiling::Scope profile(Profiling::Stage::Begin);
    return Check(_xr.BeginFrame(_session, &begin), "xrBeginFrame");
}

/// Ends the open frame with these composition layers (none: the headset shows nothing new).
void EndFrame(uint32_t layerCount, const XrCompositionLayerBaseHeader* const* layers)
{
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = _frameState.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = layerCount;
    end.layers = layers;
    XrResult result;
    {
        Profiling::Scope profile(Profiling::Stage::End);
        result = _xr.EndFrame(_session, &end);
    }
    if (XR_FAILED(result))
    {
        static unsigned loggedFailures = 0;
        if (loggedFailures++ < MAXIMUM_LOGGED_FAILURES)
        {
            Log::Error("xr", "xrEndFrame failed (%d)", static_cast<int>(result));
        }
    }
    else if (layerCount)
    {
        ++_framesSubmitted;
        if (_framesSubmitted == 1 || _framesSubmitted == 500)
        {
            Log::Info("xr", "%u frames submitted to the headset", _framesSubmitted);
        }
    }
}

/// @return true when a process with this file name (any case) is running.
bool IsProcessRunning(const char* exeName)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    PROCESSENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    for (BOOL hasEntry = Process32First(snapshot, &entry); hasEntry && !found;
         hasEntry = Process32Next(snapshot, &entry))
    {
        found = _stricmp(entry.szExeFile, exeName) == 0;
    }
    CloseHandle(snapshot);
    return found;
}

} // namespace

// ---- simulated headset (no OpenXR): a fixed head pose and a Quest-like field of view, so the whole stereo
// path except the OpenXR calls can be tested on the desktop. Script command "vr sim <yaw deg> <pitch deg>". ----
// The simulated headset's state and functions stay outside the anonymous namespace: they had external linkage
// before the rename, and moving them in lets the compiler inline them, which changes the build's code layout
// (CODE-STYLE section 9). Move them in with the next build that changes behaviour anyway.
bool _isSimulated = false;
float _simulatedYawRadians = 0, _simulatedPitchRadians = 0;
volatile float _simulatedHeightMeters = 0; // test ("vr simheight"): the simulated head above its resting height
volatile float _simulatedSideMeters = 0;   // test ("vr simside"): the simulated head right of its resting place
volatile float _simulatedAheadMeters = 0;  // test ("vr simside <right> <ahead>"): and ahead of it
// period 0 = fixed pose, otherwise the head swings between -pose and +pose
DWORD _simulatedPeriodMilliseconds = 0, _simulatedStartMilliseconds = 0;
// Test clips ("vr glide <ms>", idren2023 2026-11-11: hands and head moved as a person would): a new head pose or hand pose
// is reached over this time, eased in and out; the head's yaw is not wrapped, so 0 -> 360 is one slow full turn.
volatile unsigned _simulatedGlideMilliseconds = 0;
float _simulatedFromYawRadians = 0, _simulatedFromPitchRadians = 0;
// "vr life <0|1>": a slight, slow sway of the head and hands (breathing, a hand never quite still).
volatile bool _isSimulatedLifeOn = false;

/// Smoothstep of the time since `start` over `duration`: 0 -> 1, eased at both ends; 1 when duration is 0.
float GlideFraction(DWORD start, unsigned duration)
{
    if (!duration)
    {
        return 1.0f;
    }
    const DWORD elapsed = GetTickCount() - start;
    if (elapsed >= duration)
    {
        return 1.0f;
    }
    const float t = static_cast<float>(elapsed) / static_cast<float>(duration);
    return t * t * (3.0f - 2.0f * t);
}

/// The life sway: a few slow sines, phase-shifted per channel, in -1..1.
float LifeWave(int channel)
{
    const float seconds = static_cast<float>(GetTickCount() % 600000) * 0.001f;
    const float offset = static_cast<float>(channel) * 1.7f;
    return 0.55f * sinf(seconds * 1.31f + offset) + 0.30f * sinf(seconds * 0.53f + offset * 2.3f) +
           0.15f * sinf(seconds * 2.9f + offset * 0.7f);
}

/// The head's yaw and pitch now: the glide toward the requested pose (or the swing), plus the life sway.
void SimulatedHeadAngles(float* yaw, float* pitch)
{
    const float t = GlideFraction(_simulatedStartMilliseconds, _simulatedPeriodMilliseconds ? 0 : _simulatedGlideMilliseconds);
    *yaw = _simulatedFromYawRadians + (_simulatedYawRadians - _simulatedFromYawRadians) * t;
    *pitch = _simulatedFromPitchRadians + (_simulatedPitchRadians - _simulatedFromPitchRadians) * t;
    if (_isSimulatedLifeOn)
    {
        *yaw += 0.012f * LifeWave(0);
        *pitch += 0.009f * LifeWave(1);
    }
}

/// Fills _views with the simulated head's eyes for this moment.
void SimulateViews()
{
    float yaw = 0, pitch = 0;
    SimulatedHeadAngles(&yaw, &pitch);
    if (_simulatedPeriodMilliseconds)
    {
        const float phase =
            static_cast<float>((GetTickCount() - _simulatedStartMilliseconds) % _simulatedPeriodMilliseconds) /
            static_cast<float>(_simulatedPeriodMilliseconds) * FULL_TURN_RADIANS;
        yaw *= sinf(phase);
        pitch *= sinf(phase * 2.0f);
    }
    const float cosineHalfYaw = cosf(yaw * 0.5f), sineHalfYaw = sinf(yaw * 0.5f), cosineHalfPitch = cosf(pitch * 0.5f),
                sineHalfPitch = sinf(pitch * 0.5f);
    const XrQuaternionf orientation{
        cosineHalfYaw * sineHalfPitch,
        sineHalfYaw * cosineHalfPitch,
        -sineHalfYaw * sineHalfPitch,
        cosineHalfYaw * cosineHalfPitch}; // yaw about +y (left positive), then pitch about the head's +x (up positive)
    const float rightX = 1 - 2 * (orientation.y * orientation.y + orientation.z * orientation.z),
                rightY = 2 * (orientation.x * orientation.y + orientation.w * orientation.z),
                rightZ = 2 * (orientation.x * orientation.z - orientation.w * orientation.y);
    for (int eye = 0; eye < 2; ++eye)
    {
        const float halfEyeDistance =
            eye == 0 ? -SIMULATED_HALF_EYE_DISTANCE_METERS : SIMULATED_HALF_EYE_DISTANCE_METERS;
        _views[eye] = XrView{XR_TYPE_VIEW};
        _views[eye].pose.orientation = orientation;
        _views[eye].pose.position = XrVector3f{
            rightX * halfEyeDistance + _simulatedSideMeters,
            rightY * halfEyeDistance + _simulatedHeightMeters,
            rightZ * halfEyeDistance - _simulatedAheadMeters};
        const float outer = 0.9425f, inner = 0.6981f; // 54 and 40 degrees
        _views[eye].fov = eye == 0 ? XrFovf{-outer, inner, SIMULATED_FOV_UP_RADIANS, -SIMULATED_FOV_DOWN_RADIANS}
                                   : XrFovf{-inner, outer, SIMULATED_FOV_UP_RADIANS, -SIMULATED_FOV_DOWN_RADIANS};
    }
}

void SetSimulatedHeight(float meters)
{
    _simulatedHeightMeters = meters;
}

void SetSimulatedSide(float meters, float aheadMeters)
{
    _simulatedSideMeters = meters;
    _simulatedAheadMeters = aheadMeters;
}

// Requests are picked up by Service, so the state never changes in the middle of a frame.
volatile LONG _simulationRequest = 0; // 0 none, 1 on, 2 off
float _requestedYawDegrees = 0, _requestedPitchDegrees = 0;
unsigned _requestedPeriodMilliseconds = 0;

void SimulateHeadset(bool isOn, float yawDegrees, float pitchDegrees, unsigned periodMilliseconds)
{
    _requestedYawDegrees = yawDegrees;
    _requestedPitchDegrees = pitchDegrees;
    _requestedPeriodMilliseconds = periodMilliseconds;
    // InterlockedExchange: an atomic write (C#'s Interlocked.Exchange), read and cleared in ApplySimulationRequest.
    InterlockedExchange(&_simulationRequest, isOn ? 1 : 2);
}

/// Between frames (Service): takes over a pending SimulateHeadset request.
void ApplySimulationRequest()
{
    const LONG request = InterlockedExchange(&_simulationRequest, 0);
    if (!request)
    {
        return;
    }
    if (request == 1 && !GameBuild::IsCoreAvailable())
    {
        GameBuild::LogCoreUnavailable();
        return;
    }
    float nowYaw = 0, nowPitch = 0;
    SimulatedHeadAngles(&nowYaw, &nowPitch);
    _simulatedFromYawRadians = _isSimulated ? nowYaw : _requestedYawDegrees * RADIANS_PER_DEGREE;
    _simulatedFromPitchRadians = _isSimulated ? nowPitch : _requestedPitchDegrees * RADIANS_PER_DEGREE;
    _simulatedYawRadians = _requestedYawDegrees * RADIANS_PER_DEGREE;
    _simulatedPitchRadians = _requestedPitchDegrees * RADIANS_PER_DEGREE;
    _simulatedPeriodMilliseconds = _requestedPeriodMilliseconds;
    _simulatedStartMilliseconds = GetTickCount();
    _isSimulated = request == 1;
    Log::Info(
        "xr",
        "simulated headset %s, yaw %d pitch %d degrees, %s",
        _isSimulated ? "on" : "off",
        static_cast<int>(_requestedYawDegrees),
        static_cast<int>(_requestedPitchDegrees),
        _simulatedPeriodMilliseconds ? "swinging" : "fixed");
}

// Simulated controllers for desktop tests ("vr hands"): at rest where a standing person's hands sit with the elbows at
// the sides and the forearms level (SimulatedRestPose); moving, the right one swings left/right and up/down there, the
// left one circles. period 0 = both at rest.
volatile bool _areHandsSimulated = false;
volatile unsigned _simulatedHandsPeriodMilliseconds = 0;

void SimulateHands(bool isOn, unsigned periodMilliseconds)
{
    _simulatedHandsPeriodMilliseconds = periodMilliseconds;
    _areHandsSimulated = isOn;
    Log::Info("xr", "simulated controllers %s%s", isOn ? "on" : "off", isOn && periodMilliseconds ? ", moving" : "");
}

// A pulse in one controller. Main thread. With the simulated headset it is only counted in the log.
void Vibrate(int hand, float seconds, float amplitude, float hertz)
{
    if (_isSimulated)
    {
        static unsigned count = 0;
        if (++count <= 30)
        {
            FpGuard guard; // RAII: a clean floating-point state for the logging, the game's restored at the brace
            Log::Info(
                "xr",
                "(simulated) vibration, %s hand, %.0f ms, strength %.2f",
                hand ? "left" : "right",
                static_cast<double>(seconds * 1000),
                static_cast<double>(amplitude));
        }
        return;
    }
    if (!_areHandsReady || _hapticAction == XR_NULL_HANDLE || !_isStarted || !_isSessionRunning)
    {
        return;
    }
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.duration = static_cast<XrDuration>(seconds * NANOSECONDS_PER_SECOND);
    vibration.frequency = hertz > 0 ? hertz : XR_FREQUENCY_UNSPECIFIED;
    vibration.amplitude = amplitude;
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action = _hapticAction;
    info.subactionPath = _handPaths[hand & 1];
    const XrResult result =
        _xr.ApplyHapticFeedback(_session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
    static bool hasLogged = false;
    if (!hasLogged)
    {
        hasLogged = true;
        Log::Info("xr", "first vibration: result %d", static_cast<int>(result));
    }
}

// Test: a simulated controller held at a fixed pose ("vr handpose"). A new pose is reached over
// SIMULATED_HAND_GLIDE_MILLISECONDS from where the hand was (eased), so test moves look like hands moving, not jumping
// (idren2023, 2026-10-13); "off" glides back to the resting pose first.
constexpr DWORD SIMULATED_HAND_GLIDE_MILLISECONDS = 150; // or _simulatedGlideMilliseconds when set

struct SimulatedHandPose
{
    volatile bool isOn;
    bool isEndingAtRest;                        // gliding to the resting pose, then off
    XrQuaternionf fromOrientation, orientation; // start and target of the glide
    XrVector3f fromPosition, position;
    DWORD startTick;
} _simulatedHandPoses[2];

/// The simulated controllers' resting pose (no "vr handpose"), still.
/// A simulated hand at rest, from the eyes of a 6 ft 1 person (anthropometry, idren2023 2026-10-27): the elbows at the
/// sides 57 cm below the eyes and 5 cm behind them, the forearms level and straight ahead, so the hands are 18 cm to
/// the side, 55 cm down and 32 cm ahead, pointing forward.
void SimulatedRestPose(int hand, XrQuaternionf* orientation, XrVector3f* position)
{
    *orientation = XrQuaternionf{0, 0, 0, 1};
    *position = XrVector3f{hand == 0 ? 0.18f : -0.18f, -0.55f, -0.32f};
}

/// Where a simulated hand is in its glide now. Returns false once a glide back to rest has ended (the pose is off).
bool CurrentSimulatedPose(SimulatedHandPose& pose, XrQuaternionf* orientation, XrVector3f* position)
{
    const DWORD elapsed = GetTickCount() - pose.startTick;
    const DWORD glide = _simulatedGlideMilliseconds ? _simulatedGlideMilliseconds : SIMULATED_HAND_GLIDE_MILLISECONDS;
    if (elapsed >= glide)
    {
        *orientation = pose.orientation;
        *position = pose.position;
        if (pose.isEndingAtRest)
        {
            pose.isOn = false;
            return false;
        }
        return true;
    }
    const float linear = static_cast<float>(elapsed) / static_cast<float>(glide);
    const float t = linear * linear * (3.0f - 2.0f * linear); // smoothstep
    *position = XrVector3f{
        pose.fromPosition.x + (pose.position.x - pose.fromPosition.x) * t,
        pose.fromPosition.y + (pose.position.y - pose.fromPosition.y) * t,
        pose.fromPosition.z + (pose.position.z - pose.fromPosition.z) * t};
    // Normalised linear blend of the quaternions, the target turned to the same hemisphere as the start.
    XrQuaternionf to = pose.orientation;
    const XrQuaternionf from = pose.fromOrientation;
    if (from.x * to.x + from.y * to.y + from.z * to.z + from.w * to.w < 0)
    {
        to = XrQuaternionf{-to.x, -to.y, -to.z, -to.w};
    }
    XrQuaternionf blend{
        from.x + (to.x - from.x) * t,
        from.y + (to.y - from.y) * t,
        from.z + (to.z - from.z) * t,
        from.w + (to.w - from.w) * t};
    const float length = sqrtf(blend.x * blend.x + blend.y * blend.y + blend.z * blend.z + blend.w * blend.w);
    if (length > 1e-6f)
    {
        blend = XrQuaternionf{blend.x / length, blend.y / length, blend.z / length, blend.w / length};
    }
    *orientation = blend;
    return true;
}

void SimulateHandPose(
    int hand,
    bool isOn,
    float x,
    float y,
    float z,
    float yawDegrees,
    float pitchDegrees,
    float rollDegrees)
{
    SimulatedHandPose& pose = _simulatedHandPoses[hand & 1];
    XrQuaternionf fromOrientation;
    XrVector3f fromPosition;
    if (!pose.isOn || !CurrentSimulatedPose(pose, &fromOrientation, &fromPosition))
    {
        SimulatedRestPose(hand & 1, &fromOrientation, &fromPosition);
    }
    if (!isOn && !pose.isOn)
    {
        return;
    }
    XrQuaternionf orientation;
    XrVector3f position;
    if (isOn)
    {
        const float halfRadiansPerDegree = 3.14159265f / 360.0f; // half angles in radians
        const XrQuaternionf yaw{0, sinf(yawDegrees * halfRadiansPerDegree), 0, cosf(yawDegrees * halfRadiansPerDegree)},
            pitch{sinf(pitchDegrees * halfRadiansPerDegree), 0, 0, cosf(pitchDegrees * halfRadiansPerDegree)},
            roll{0, 0, sinf(-rollDegrees * halfRadiansPerDegree), cosf(-rollDegrees * halfRadiansPerDegree)};
        orientation = MultiplyQuaternions(MultiplyQuaternions(yaw, pitch), roll);
        position = XrVector3f{x, y, z};
    }
    else
    {
        SimulatedRestPose(hand & 1, &orientation, &position);
    }
    pose.fromOrientation = fromOrientation;
    pose.fromPosition = fromPosition;
    pose.orientation = orientation;
    pose.position = position;
    pose.startTick = GetTickCount();
    pose.isEndingAtRest = !isOn;
    pose.isOn = true;
}

// A pose's 4x4 row matrix in the game's camera convention (rows: right, up, back, position).
void PoseMatrix(const XrQuaternionf& orientation, const XrVector3f& position, float* matrix);

/// The grip pose in the aim pose's frame (rows) for the simulated hands: 3 cm back and 2 cm down, its -z (the handle,
/// toward the thumb) 75 degrees up from where the aim pose points (a pistol-like hold: the handle near upright, 15
/// degrees of rake; a 35 degree guess turned the guns down and back, run434); to be replaced by the "aim pose in the
/// grip pose" a headset run logs.
constexpr float SIMULATED_GRIP_IN_AIM[16] = {
    1, 0, 0, 0, 0, 0.2588f, 0.9659f, 0, 0, -0.9659f, 0.2588f, 0, 0, -0.02f, 0.03f, 1};

// A controller's aim pose as a 4x4 row matrix in the game's camera convention (rows: right, up, back, position),
// measured from the resting pose like the eyes. hand: 0 right, 1 left. False while that controller is not tracked.
bool GetHandPose(int hand, float* matrix)
{
    XrQuaternionf orientation;
    XrVector3f position;
    const bool isPosed = _isSimulated && _areHandsSimulated && _simulatedHandPoses[hand].isOn &&
                         CurrentSimulatedPose(_simulatedHandPoses[hand], &orientation, &position);
    if (isPosed)
    {
        // a "vr handpose" pose, gliding or reached
    }
    else if (_isSimulated)
    {
        if (!_areHandsSimulated)
        {
            return false;
        }
        const unsigned period = _simulatedHandsPeriodMilliseconds;
        const float phase =
            period ? static_cast<float>(GetTickCount() % period) / static_cast<float>(period) * FULL_TURN_RADIANS
                   : 0.0f;
        const float move = period ? 1.0f : 0.0f;
        SimulatedRestPose(hand, &orientation, &position);
        if (hand == 0)
        {
            // swings up to 0.35 rad (20 degrees) left and right and 0.2 rad up and down
            const float yaw = 0.35f * sinf(phase) * move, pitch = 0.2f * sinf(phase * 2.0f) * move;
            const float cosineHalfYaw = cosf(yaw * 0.5f), sineHalfYaw = sinf(yaw * 0.5f),
                        cosineHalfPitch = cosf(pitch * 0.5f), sineHalfPitch = sinf(pitch * 0.5f);
            orientation = XrQuaternionf{
                cosineHalfYaw * sineHalfPitch,
                sineHalfYaw * cosineHalfPitch,
                -sineHalfYaw * sineHalfPitch,
                cosineHalfYaw * cosineHalfPitch};
        }
        else
        {
            // circles with a radius of 10 cm
            position.x += 0.10f * cosf(phase) * move;
            position.y += 0.10f * sinf(phase) * move;
        }
    }
    else
    {
        if (!_isHandTracked[hand] || !_hasOrigin)
        {
            return false;
        }
        orientation = MultiplyQuaternions(YawQuaternion(-_originYawRadians), _handPoses[hand].orientation);
        const XrVector3f world = _handPoses[hand].position;
        position = RotateYaw(
            -_originYawRadians,
            XrVector3f{world.x - _originPosition.x, world.y - _originPosition.y, world.z - _originPosition.z});
    }
    if (_isSimulated && _isSimulatedLifeOn)
    { // a hand held up is never quite still: a few millimetres of slow drift and a degree of wobble
        position.x += 0.004f * LifeWave(2 + 3 * hand);
        position.y += 0.005f * LifeWave(3 + 3 * hand);
        position.z += 0.003f * LifeWave(4 + 3 * hand);
        orientation = MultiplyQuaternions(orientation, YawQuaternion(0.015f * LifeWave(9 + hand)));
    }
    PoseMatrix(orientation, position, matrix);
    return true;
}

void SetSimulatedGlide(unsigned milliseconds)
{
    _simulatedGlideMilliseconds = milliseconds;
}

void SetSimulatedLife(bool isOn)
{
    _isSimulatedLifeOn = isOn;
}

bool GetGripPose(int hand, float* matrix)
{
    if (_isSimulated || !_isGripTracked[hand] || !_hasOrigin)
    {
        // the simulated hands (and a runtime without a grip pose): the aim pose moved by a typical controller's
        // handle offset (SIMULATED_GRIP_IN_AIM, rows)
        float aim[16];
        if (!GetHandPose(hand, aim))
        {
            return false;
        }
        for (int row = 0; row < 4; ++row)
        {
            for (int column = 0; column < 4; ++column)
            {
                float sum = 0;
                for (int inner = 0; inner < 4; ++inner)
                {
                    sum += SIMULATED_GRIP_IN_AIM[row * 4 + inner] * aim[inner * 4 + column];
                }
                matrix[row * 4 + column] = sum;
            }
        }
        return true;
    }
    const XrQuaternionf orientation =
        MultiplyQuaternions(YawQuaternion(-_originYawRadians), _gripPoses[hand].orientation);
    const XrVector3f world = _gripPoses[hand].position;
    const XrVector3f position = RotateYaw(
        -_originYawRadians,
        XrVector3f{world.x - _originPosition.x, world.y - _originPosition.y, world.z - _originPosition.z});
    PoseMatrix(orientation, position, matrix);
    return true;
}

void PoseMatrix(const XrQuaternionf& orientation, const XrVector3f& position, float* matrix)
{
    // Rotation matrix of the quaternion, rows as the game's camera expects.
    const float xx = orientation.x * orientation.x, yy = orientation.y * orientation.y,
                zz = orientation.z * orientation.z, xy = orientation.x * orientation.y,
                xz = orientation.x * orientation.z, yz = orientation.y * orientation.z,
                wx = orientation.w * orientation.x, wy = orientation.w * orientation.y,
                wz = orientation.w * orientation.z;
    matrix[0] = 1 - 2 * (yy + zz);
    matrix[1] = 2 * (xy + wz);
    matrix[2] = 2 * (xz - wy);
    matrix[3] = 0;
    matrix[4] = 2 * (xy - wz);
    matrix[5] = 1 - 2 * (xx + zz);
    matrix[6] = 2 * (yz + wx);
    matrix[7] = 0;
    matrix[8] = 2 * (xz + wy);
    matrix[9] = 2 * (yz - wx);
    matrix[10] = 1 - 2 * (xx + yy);
    matrix[11] = 0;
    matrix[12] = position.x;
    matrix[13] = position.y;
    matrix[14] = position.z;
    matrix[15] = 1;
}

// Simulated buttons and sticks ("vr pad"), used with the simulated headset.
ControllerInput _simulatedInput{};

void SimulateControllerInput(const ControllerInput& input)
{
    _simulatedInput = input;
    _simulatedInput.isValid = true;
}

// Triggers, sticks and buttons of both controllers as of this frame. isValid = false: no controller input now.
void GetControllerInput(ControllerInput* input)
{
    if (_isSimulated)
    {
        *input = _simulatedInput;
        input->isValid = _areHandsSimulated && _simulatedInput.isValid;
        return;
    }
    *input = _controllerInput;
    input->isValid = input->isValid && _isStarted && _isSessionRunning;
}

bool IsSimulated()
{
    return _isSimulated;
}

void Recenter()
{
    _isRecenterRequested = true;
    InterlockedIncrement(&_recenterRequestCount);
}

unsigned RecenterCount()
{
    return static_cast<unsigned>(_recenterRequestCount);
}

void SetGameAdapter(unsigned vendorId, unsigned deviceId)
{
    _gameVendorId = vendorId;
    _gameDeviceId = deviceId;
}

void RequestStart()
{
    _hasFailed = false;
    _isStartRequested = true;
}

void RequestStop()
{
    _isStopRequested = true;
}

bool IsRunning()
{
    return _isSimulated || (_isStarted && _isSessionRunning);
}

bool IsStarted()
{
    return _isSimulated || _isStarted;
}

// Called once per frame on the render thread, between frames: handles start/stop requests and runtime events.
// On the first call it starts VR by itself if SteamVR is already running.
void Service()
{
    ApplySimulationRequest();
    static bool hasCheckedAutoStart = false;
    if (!hasCheckedAutoStart)
    {
        hasCheckedAutoStart = true;
        const bool isSteamVrRunning = IsProcessRunning("vrserver.exe");
#if SWBF_VR_DEV
        const bool isScriptPending = Script::IsAutorunPending();
#else
        const bool isScriptPending = false;
#endif
        Log::Info(
            "xr",
            "SteamVR is %s at game start%s",
            isSteamVrRunning ? "running" : "not running",
            isSteamVrRunning && isScriptPending
                ? ", but a test script runs: staying flat unless the script says otherwise"
            : isSteamVrRunning ? ", starting VR"
                               : ", staying in flat mode (script command \"vr on\" forces VR)");
        if (isSteamVrRunning && !isScriptPending)
        {
            _isStartRequested = true;
        }
    }
    if (_isStopRequested)
    {
        _isStopRequested = false;
        if (_isStarted)
        {
            Shutdown();
            Log::Info("xr", "stopped");
        }
    }
    if (_isStartRequested && !_isStarted && !_hasFailed && !GameBuild::IsCoreAvailable())
    {
        // The game's rendering code was not recognised (GameBuild): VR would crash or show nothing.
        _isStartRequested = false;
        _hasFailed = true;
        GameBuild::LogCoreUnavailable();
    }
    if (_isStartRequested && !_isStarted && !_hasFailed)
    {
        _isStartRequested = false;
        _isStarted = Start();
        if (!_isStarted)
        {
            Shutdown();
            _hasFailed = true;
            Log::Error("xr", "VR start failed, staying in flat mode");
        }
    }
    if (_isStarted)
    {
        PollEvents();
        // The Home key recentres the view. GetAsyncKeyState sets its top bit (0x8000) while the key is down.
        static bool wasHomeDown = false;
        const bool isHomeDown = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
        if (isHomeDown && !wasHomeDown)
        {
            Recenter();
        }
        wasHomeDown = isHomeDown;
    }
}

// Stereo frame, step 1: wait for the headset's frame timing and get both eye poses.
// Returns 0 = VR not running, 1 = VR running but nothing should be drawn this frame, 2 = draw both eyes.
int BeginStereoFrame()
{
    if (_isSimulated)
    {
        SimulateViews();
        return 2;
    }
    if (!IsRunning())
    {
        return 0;
    }
    if (!WaitAndBeginFrame())
    {
        return 0;
    }
    if (!_frameState.shouldRender || !LocateEyes())
    {
        EndFrame(0, nullptr);
        return 1;
    }
    return 2;
}

// Both eye poses as 4x4 row matrices in the game's camera convention (rows: right, up, back, position), measured
// from the resting pose. tanX/tanY receive the symmetric half field of view that covers both eyes.
// Returns false while the headset has not reported a pose yet.
bool GetViews(float* leftEyeMatrix, float* rightEyeMatrix, float* tanX, float* tanY)
{
    if (_isSimulated)
    {
        SimulateViews();
    }
    else if (!_areViewsValid)
    {
        return false;
    }
    const bool isRelative = !_isSimulated && _hasOrigin;
    const XrVector3f left = _views[0].pose.position, right = _views[1].pose.position;
    const XrVector3f centre{(left.x + right.x) * 0.5f, (left.y + right.y) * 0.5f, (left.z + right.z) * 0.5f};
    float* matrices[2] = {leftEyeMatrix, rightEyeMatrix};
    for (int eye = 0; eye < 2; ++eye)
    {
        XrQuaternionf orientation = _views[eye].pose.orientation;
        XrVector3f position = _views[eye].pose.position;
        if (isRelative)
        {
            orientation = MultiplyQuaternions(YawQuaternion(-_originYawRadians), orientation);
            position = RotateYaw(
                -_originYawRadians,
                XrVector3f{
                    position.x - _originPosition.x,
                    position.y - _originPosition.y,
                    position.z - _originPosition.z});
        }
        else if (!_isSimulated)
        {
            position = XrVector3f{
                position.x - centre.x,
                position.y - centre.y,
                position.z - centre.z}; // no resting pose yet: head turns only
        }
        float* matrix = matrices[eye];
        const float xx = orientation.x * orientation.x, yy = orientation.y * orientation.y,
                    zz = orientation.z * orientation.z, xy = orientation.x * orientation.y,
                    xz = orientation.x * orientation.z, yz = orientation.y * orientation.z,
                    wx = orientation.w * orientation.x, wy = orientation.w * orientation.y,
                    wz = orientation.w * orientation.z;
        matrix[0] = 1 - 2 * (yy + zz);
        matrix[1] = 2 * (xy + wz);
        matrix[2] = 2 * (xz - wy);
        matrix[3] = 0;
        matrix[4] = 2 * (xy - wz);
        matrix[5] = 1 - 2 * (xx + zz);
        matrix[6] = 2 * (yz + wx);
        matrix[7] = 0;
        matrix[8] = 2 * (xz + wy);
        matrix[9] = 2 * (yz - wx);
        matrix[10] = 1 - 2 * (xx + yy);
        matrix[11] = 0;
        matrix[12] = position.x;
        matrix[13] = position.y;
        matrix[14] = position.z;
        matrix[15] = 1;
    }
    float largestTanX = 0, largestTanY = 0;
    for (const XrView& view : _views)
    {
        largestTanX = fmaxf(largestTanX, fmaxf(tanf(-view.fov.angleLeft), tanf(view.fov.angleRight)));
        largestTanY = fmaxf(largestTanY, fmaxf(tanf(view.fov.angleUp), tanf(-view.fov.angleDown)));
    }
    if (!(largestTanX > MINIMUM_SENSIBLE_TAN && largestTanX < MAXIMUM_SENSIBLE_TAN &&
          largestTanY > MINIMUM_SENSIBLE_TAN && largestTanY < MAXIMUM_SENSIBLE_TAN))
    {
        return false; // nonsense from the runtime
    }
    *tanX = largestTanX;
    *tanY = largestTanY;
    return true;
}

bool SubmitEye(int eye, const void* bgraPixels, int pitch, int width, int height)
{
    if (_isSimulated)
    {
        return bgraPixels != nullptr;
    }
    return CopyToSwapchain(_eyeSwapchains[eye], bgraPixels, pitch, width, height);
}

bool TakesSharedImages()
{
    if (!GpuTransfer::IsActive())
    {
        return false;
    }
    if (_isSimulated)
    {
        return true;
    }
    if (!_d3d11Device || !_isBgra || _isSharedRefused)
    {
        return false;
    }
    unsigned long low = 0;
    long high = 0;
    GpuTransfer::GetAdapter(&low, &high);
    if (low != _d3d11AdapterLuid.LowPart || high != _d3d11AdapterLuid.HighPart)
    {
        RefuseSharedImages("the headset's adapter is not the game's", E_FAIL);
        return false;
    }
    return true;
}

bool SubmitEyeShared(int eye, const GpuTransfer::SharedImage& image)
{
    if (_isSimulated)
    {
        return true;
    }
    return CopySharedToSwapchain(_eyeSwapchains[eye], image);
}

// Stereo frame, last step: hand both eye images to the headset. The images were drawn with a symmetric
// field of view of +-atan(tanX) by +-atan(tanY).
void EndStereoFrame(float tanX, float tanY, bool hasImages)
{
    if (_isSimulated)
    {
        return;
    }
    if (!hasImages || _eyeSwapchains[0].handle == XR_NULL_HANDLE || _eyeSwapchains[1].handle == XR_NULL_HANDLE)
    {
        EndFrame(0, nullptr);
        return;
    }
    XrCompositionLayerProjectionView views[2] = {
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    const float angleX = atanf(tanX), angleY = atanf(tanY);
    for (int eye = 0; eye < 2; ++eye)
    {
        views[eye].pose = _views[eye].pose;
        views[eye].fov = XrFovf{-angleX, angleX, angleY, -angleY};
        views[eye].subImage.swapchain = _eyeSwapchains[eye].handle;
        views[eye].subImage.imageRect.extent = XrExtent2Di{_eyeSwapchains[eye].width, _eyeSwapchains[eye].height};
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = _space;
    layer.viewCount = 2;
    layer.views = views;
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
    EndFrame(1, layers);
}

namespace
{

/// The flat screen's frame: waits for the headset's frame slot, copies the image with `copyImage` (returns bool) and
/// shows it on a virtual screen 2 m ahead of the resting pose.
/// @return true when copyImage was called.
template <class CopyImage> bool ShowFlatScreen(int width, int height, CopyImage copyImage)
{
    if (!IsRunning() || !WaitAndBeginFrame())
    {
        return false;
    }
    LocateEyes(); // keeps the resting pose and field of view known while only menus are shown
    if (!_frameState.shouldRender)
    {
        EndFrame(0, nullptr);
        return false;
    }
    if (!copyImage())
    {
        EndFrame(0, nullptr);
        return true;
    }
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.space = _space;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = _flatSwapchain.handle;
    quad.subImage.imageRect.extent = XrExtent2Di{width, height};
    quad.pose.orientation.w = 1.0f;
    quad.pose.position = XrVector3f{0.0f, 0.0f, -FLAT_SCREEN_DISTANCE_METERS};
    if (_hasOrigin)
    { // straight ahead of the resting pose
        const XrVector3f ahead = RotateYaw(_originYawRadians, XrVector3f{0.0f, 0.0f, -FLAT_SCREEN_DISTANCE_METERS});
        quad.pose.orientation = YawQuaternion(_originYawRadians);
        quad.pose.position =
            XrVector3f{_originPosition.x + ahead.x, _originPosition.y + ahead.y, _originPosition.z + ahead.z};
    }
    quad.size = XrExtent2Df{
        FLAT_SCREEN_WIDTH_METERS,
        FLAT_SCREEN_WIDTH_METERS * static_cast<float>(height) / static_cast<float>(width)};
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
    EndFrame(1, layers);
    return true;
}

} // namespace

// Menus, loading screens and anything else that is not the 3D scene: show the flat image on a virtual
// screen 2 m in front of the player.
void SubmitFlatFrame(const void* bgraPixels, int pitch, int width, int height)
{
    if (_isSimulated)
    {
        return;
    }
    ShowFlatScreen(
        width,
        height,
        [&]
        {
            return CopyToSwapchain(_flatSwapchain, bgraPixels, pitch, width, height);
        });
}

void SubmitFlatShared(const GpuTransfer::SharedImage& image)
{
    if (_isSimulated)
    {
        return;
    }
    if (!ShowFlatScreen(
            image.width,
            image.height,
            [&]
            {
                return CopySharedToSwapchain(_flatSwapchain, image);
            }))
    {
        SignalSharedRead(image);
    }
}

bool IsSteamVrRunning()
{
    return IsProcessRunning("vrserver.exe");
}

} // namespace Xr

} // namespace SwbfVr
