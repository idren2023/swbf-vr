// D3dSurvey.cpp: Direct3D 9 survey. Started as a diagnostic that records how the game drives the device, so the
// stereo renderer could be designed from facts; its device hooks now also drive Stereo and the per-frame subsystems.
// Every 10 s one frame is traced into swbf-vr-trace.txt.
// CINTERFACE gives C-style vtable structs, so hooks are type-checked against the real signatures.
//
// The device's vtable lives inside each device object (Direct3D 9 rewrites it at runtime) and the game
// creates a dozen short-lived devices while probing display modes. So the device methods are hooked
// inline with MinHook, which covers every device, instead of by patching one vtable.
//
// MinHook patches the start of a function with a jump to ours and gives back a pointer ("original", a trampoline)
// through which the real code can still be called. The device methods use WINAPI (__stdcall: arguments on the stack,
// the callee removes them), with the device itself as the first argument, as COM methods do.

// Makes d3d9.h declare the Direct3D interfaces as C structs with an explicit vtable (device->lpVtbl->Method(device,
// ...)). Has to be a macro: d3d9.h reads it.
#define CINTERFACE

#include "D3dSurvey.h"

#include "Aim.h"
#include "Body.h"
#include "Controls.h"
#include "Crash.h"
#include "EngineSurvey.h"
#include "Feedback.h"
#include "Flight.h"
#include "FpGuard.h"
#include "Gesture.h"
#include "GpuTransfer.h"
#include "GunnerSeat.h"
#include "Handlebars.h"
#include "Holster.h"
#include "Keyboard.h"
#include "Levers.h"
#include "Log.h"
#include "Lua.h"
#include "Main.h"
#include "Pointer.h"
#include "Probe.h"
#include "Settings.h"
#include "Squad.h"
#include "Stereo.h"
#include "Textures.h"
#include "Vehicle.h"
#include "WalkerDrive.h"
#include "Watch.h"
#include "Xr.h"

#include <windows.h>
#include <d3d9.h>

#include <MinHook.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace SwbfVr
{

namespace D3dSurvey
{

namespace
{

// ---- Constants ----

/// Traces taken at most in one session (automatic ones and those the script asks for).
constexpr uint32_t MAXIMUM_TRACES = 400;
/// Traces taken automatically at startup (outside VR), and the time between them.
constexpr uint32_t AUTOMATIC_TRACES = 6;
constexpr DWORD AUTOMATIC_TRACE_INTERVAL_MILLISECONDS = 10000;
/// Most render target and depth surface creations logged in one session.
constexpr uint32_t MAXIMUM_CREATE_LOGS = 400;
/// Vertex shader constant registers the tracer keeps (c0..c255, the vs_2_0 limit).
constexpr unsigned VERTEX_SHADER_REGISTERS = 256;
/// A register holds 4 floats; the tracer keeps at most 4 registers (one matrix) per write.
constexpr uint32_t FLOATS_PER_REGISTER = 4;
constexpr uint32_t REGISTERS_PER_MATRIX = 4;
constexpr uint32_t FLOATS_PER_MATRIX = 16;
/// Kinds of draw call counted per pass ("drawlog"): DrawPrimitive, DrawIndexedPrimitive, and the two UP variants
/// (vertices from a user pointer) together.
constexpr int DRAW_PLAIN = 0;
constexpr int DRAW_INDEXED = 1;
constexpr int DRAW_USER_POINTER = 2;
constexpr int DRAW_KIND_COUNT = 3;

// ---- State ----

/// IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion), the d3d9.dll export the game imports.
using Direct3DCreate9Function = IDirect3D9*(WINAPI*)(UINT);
Direct3DCreate9Function _originalDirect3DCreate9 = nullptr;
// decltype(x) is "the type of x": here the type of IDirect3D9's CreateDevice method pointer.
decltype(IDirect3D9Vtbl::CreateDevice) _originalCreateDevice = nullptr;
/// The game's Direct3D runs on Direct3D 9Ex (ini `shared_transfer`, M2 route 1, b666): Ex devices can share render
/// targets with Direct3D 11 (the game's ordinary device refuses, run 889: D3DERR_INVALIDCALL) but have no managed
/// pool, so every D3DPOOL_MANAGED resource is made in the default pool (textures dynamic, so they stay lockable).
bool _isEx = false;
unsigned _poolChangeLogs = 8, _poolFailureLogs = 8;

/// Running counts on 9Ex, logged every EX_COUNT_PRESENTS presents while they change.
struct ExCounts
{
    unsigned managedTextures, managedMipTextures, managedBuffers, dynamicLocks, dynamicLockFailures, unlocks;
    unsigned created[4]
                    [2]; // CreateTexture by asked pool (default, managed, systemmem, scratch) and with / without mips
    unsigned bound[4];   // SetTexture of a 2D texture, by its pool
    unsigned updateTextures, updateTextureFailures, updateSurfaces, updateSurfaceFailures;
};

unsigned _boundLogs = 16, _updateLogs = 12, _mipLogs = 4;
void* _boundLogged[16];
ExCounts _exCounts{}, _exCountsLogged{};
IDirect3DDevice9Vtbl _original; // trampolines to the original code of the hooked methods (other entries stay null)
IDirect3DDevice9Vtbl
    _firstDeviceMethods; // the first device's method addresses, to detect devices that use different code
bool _isHooked = false;

FILE* _traceFile = nullptr;
bool _isTracing = false;
bool _isVertexShaderBound = false;
uint32_t _frameCount = 0, _traceCount = 0;
DWORD _lastTraceTick = 0;
uint32_t _createLogCount = 0;
volatile bool _isTraceRequested = false;
volatile bool _isStackRequested = false;
uint32_t _segmentShaderDraws = 0, _segmentFixedDraws = 0; // draw calls since the last traced event

/// Calls of the traced frame, by kind.
struct Totals
{
    uint32_t beginScene, clear, setRenderTarget, setViewport, view, projection, world, otherTransform, setVertexShader,
        setPixelShader, setFvf, vertexShaderConstants, shaderDraws, fixedDraws, stretchRect;
} _totals;

/// One vertex shader constant register during the traced frame: how often it was written and changed, and its first
/// and last values.
struct ConstantRegister
{
    uint32_t writes, changes, firstCount, lastCount;
    float first[16], last[16];
} _registers[VERTEX_SHADER_REGISTERS];

/// Writes the draw calls counted since the last traced event, if any.
void FlushDraws()
{
    if (_segmentShaderDraws || _segmentFixedDraws)
    {
        fprintf(_traceFile, "  draws shader=%u fixed=%u\n", _segmentShaderDraws, _segmentFixedDraws);
    }
    _segmentShaderDraws = _segmentFixedDraws = 0;
}

void PrintFloats(const float* values, unsigned count)
{
    for (unsigned index = 0; index < count; ++index)
    {
        fprintf(_traceFile, " %.6g", static_cast<double>(values[index]));
    }
}

/// Counts a draw call of the traced frame, as shader or fixed function (by whether a vertex shader is bound).
void CountDraw()
{
    if (!_isTracing)
    {
        return;
    }
    if (_isVertexShaderBound)
    {
        ++_segmentShaderDraws;
        ++_totals.shaderDraws;
    }
    else
    {
        ++_segmentFixedDraws;
        ++_totals.fixedDraws;
    }
}

void LogPresentParameters(const char* what, const D3DPRESENT_PARAMETERS* parameters)
{
    if (!parameters)
    {
        return;
    }
    Log::Info(
        "d3d",
        "%s %ux%u fmt=%d buffers=%u msaa=%d swap=%d windowed=%d autoDS=%d dsFmt=%d flags=0x%lx hz=%u "
        "interval=0x%x",
        what,
        parameters->BackBufferWidth,
        parameters->BackBufferHeight,
        static_cast<int>(parameters->BackBufferFormat),
        parameters->BackBufferCount,
        static_cast<int>(parameters->MultiSampleType),
        static_cast<int>(parameters->SwapEffect),
        parameters->Windowed,
        parameters->EnableAutoDepthStencil,
        static_cast<int>(parameters->AutoDepthStencilFormat),
        parameters->Flags,
        parameters->FullScreen_RefreshRateInHz,
        parameters->PresentationInterval);
}

void BeginTrace(IDirect3DDevice9* device, DWORD now)
{
    // FpGuard: an RAII guard (its constructor saves the floating-point state, its destructor restores it at the end
    // of the scope), so the game's FPU settings are the same after our code ran.
    FpGuard fp;
    _lastTraceTick = now;
    if (!_traceFile)
    {
        _traceFile = Main::OpenModFile("swbf-vr-trace.txt", "w");
    }
    if (!_traceFile)
    {
        return;
    }
    ++_traceCount;
    memset(&_totals, 0, sizeof(_totals));
    memset(_registers, 0, sizeof(_registers));
    _segmentShaderDraws = _segmentFixedDraws = 0;
    IDirect3DSurface9* backBuffer = nullptr;
    D3DVIEWPORT9 viewport{};
    device->lpVtbl->GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    device->lpVtbl->GetViewport(device, &viewport);
    fprintf(
        _traceFile,
        "=== frame %u tick %lu backbuffer %p viewport %lu,%lu %lux%lu\n",
        _frameCount,
        now,
        static_cast<void*>(backBuffer),
        viewport.X,
        viewport.Y,
        viewport.Width,
        viewport.Height);
    if (backBuffer)
    {
        backBuffer->lpVtbl->Release(backBuffer);
    }
    _isTracing = true;
}

void EndTrace()
{
    FpGuard fp;
    _isTracing = false;
    FlushDraws();
    fprintf(
        _traceFile,
        " totals beginScene=%u clear=%u setRT=%u setVP=%u view=%u proj=%u world=%u otherXf=%u setVS=%u setPS=%u "
        "setFVF=%u vsConst=%u drawShader=%u drawFixed=%u stretch=%u\n",
        _totals.beginScene,
        _totals.clear,
        _totals.setRenderTarget,
        _totals.setViewport,
        _totals.view,
        _totals.projection,
        _totals.world,
        _totals.otherTransform,
        _totals.setVertexShader,
        _totals.setPixelShader,
        _totals.setFvf,
        _totals.vertexShaderConstants,
        _totals.shaderDraws,
        _totals.fixedDraws,
        _totals.stretchRect);
    for (unsigned registerIndex = 0; registerIndex < VERTEX_SHADER_REGISTERS; ++registerIndex)
    {
        const ConstantRegister& constant = _registers[registerIndex];
        if (!constant.writes)
        {
            continue;
        }
        fprintf(_traceFile, " c%u writes=%u changes=%u first:", registerIndex, constant.writes, constant.changes);
        PrintFloats(constant.first, constant.firstCount);
        if (constant.changes)
        {
            fprintf(_traceFile, " last:");
            PrintFloats(constant.last, constant.lastCount);
        }
        fputc('\n', _traceFile);
    }
    fflush(_traceFile);
}

IDirect3DDevice9* _presentingDevice = nullptr;
// Test ("drawlog <n>"): draw calls and primitives of each of the next n passes (one per Present call), by kind.
volatile LONG _drawLogPassesLeft = 0;
unsigned _passDrawCalls[DRAW_KIND_COUNT], _passPrimitives[DRAW_KIND_COUNT]; // by DRAW_PLAIN, ...

// ---- Device hooks (each replaces the device method of the same name; WINAPI, render thread) ----

/// Present: the end of every pass. Stereo decides first whether this Present is real (the left-eye pass is not);
/// then the per-frame subsystems run, the tracer finishes or starts a trace, and the frame is presented.
// ---- The game's model lighting (PLAN 57 l, runs 356-357) ----
// The game's lit model shaders get, per draw, the vertex shader constants c11-c21 in one write: c15-c17 the model's
// matrix, c18 a colour scale (0.549 for the cockpits on Naboo), c19 the ambient colour, c20 the sun's colour, c21 the
// direction the sunlight travels, in the world (the same at any head yaw and hull heading); an unlit draw writes
// c11-c19 only (c19 white). Kept for the draws with a watched stage-0 texture (WatchLightOf).
float _modelBlock[11 * 4];
bool _isModelBlockLit = false;
void* _watchedTexture = nullptr;
float _watchedLight[16];
DWORD _watchedLightTick = 0;
bool _hasWatchedLight = false;

// ---- Shader dump (script `shaderdump`, PLAN 57 l: how the game lights its first-person models) ----
// From the first call in the next first-person pass to the Present: every vertex / pixel shader constant write (up to
// four registers each), every texture set on stage 0, every vertex shader set and every draw, in the log (at most
// SHADER_DUMP_LINES lines).
constexpr int SHADER_DUMP_LINES = 3000;
volatile LONG _isShaderDumpRequested = 0;
bool _isShaderDumping = false;
int _shaderDumpLines = 0;
void* _stageTexture = nullptr;
void* _boundVertexShader = nullptr;

bool IsShaderDumping()
{
    if (!_isShaderDumping && _isShaderDumpRequested && Stereo::IsFirstPersonPass())
    {
        _isShaderDumping = true;
        _shaderDumpLines = 0;
        Log::Info(
            "shaderdump",
            "start (first-person pass, eye %d); speeder cockpit texture %p, window %p",
            Stereo::CurrentEye(),
            Textures::FindByName("all_1st_cockpit_combatspeeder"),
            Textures::FindByName("all_cockpit_xwingwin"));
    }
    if (_isShaderDumping && _shaderDumpLines >= SHADER_DUMP_LINES)
    {
        return false;
    }
    if (_isShaderDumping)
    {
        ++_shaderDumpLines;
    }
    return _isShaderDumping;
}

void DumpConstants(const char* kind, UINT start, const float* data, UINT count)
{
    // Every register up to 48 (the big blocks: their first 48), four a line.
    const UINT shown = count < 48 ? count : 48;
    for (UINT first = 0; first < shown; first += 4)
    {
        char line[400];
        int length = snprintf(line, sizeof(line), "%s c%u (of c%u x%u):", kind, start + first, start, count);
        for (UINT r = first; r < shown && r < first + 4 && length < 360; ++r)
        {
            length += snprintf(
                line + length,
                sizeof(line) - length,
                " [%.4g %.4g %.4g %.4g]",
                static_cast<double>(data[r * 4]),
                static_cast<double>(data[r * 4 + 1]),
                static_cast<double>(data[r * 4 + 2]),
                static_cast<double>(data[r * 4 + 3]));
        }
        Log::Info("shaderdump", "%s", line);
    }
}

void FitWindow(HWND window); // below, with ApplyRenderSize
void LogExCounts(const char* when);
extern bool _isEx;
constexpr unsigned EX_COUNT_PRESENTS = 600;
extern HWND _gameWindow;
/// How often Present checks the game's window size (FitWindow).
constexpr unsigned FIT_CHECK_PRESENTS = 30;

HRESULT WINAPI PresentHook(
    IDirect3DDevice9* device,
    const RECT* sourceRect,
    const RECT* destinationRect,
    HWND window,
    const RGNDATA* dirtyRegion)
{
    Crash::Heartbeat();
    static unsigned presentsToExCount = 0;
    if (_isEx && ++presentsToExCount >= EX_COUNT_PRESENTS)
    {
        presentsToExCount = 0;
        LogExCounts("present");
    }
    static unsigned presentsToFitCheck = 0;
    if (_gameWindow && ++presentsToFitCheck >= FIT_CHECK_PRESENTS)
    {
        presentsToFitCheck = 0;
        FitWindow(_gameWindow);
    }
    if (_isShaderDumping)
    {
        _isShaderDumping = false;
        InterlockedExchange(&_isShaderDumpRequested, 0);
        Log::Info("shaderdump", "end at Present (%d lines)", _shaderDumpLines);
    }
    if (_drawLogPassesLeft > 0)
    {
        InterlockedDecrement(&_drawLogPassesLeft);
        Log::Info(
            "draws",
            "eye %d: plain %u/%u indexed %u/%u up %u/%u",
            Stereo::CurrentEye(),
            _passDrawCalls[DRAW_PLAIN],
            _passPrimitives[DRAW_PLAIN],
            _passDrawCalls[DRAW_INDEXED],
            _passPrimitives[DRAW_INDEXED],
            _passDrawCalls[DRAW_USER_POINTER],
            _passPrimitives[DRAW_USER_POINTER]);
    }
    for (int kind = 0; kind < DRAW_KIND_COUNT; ++kind)
    {
        _passDrawCalls[kind] = _passPrimitives[kind] = 0;
    }
    if (Stereo::OnPresent(device))
    {
        return D3D_OK;
    }
    Watch::NoteMainThread();
    Lua::OnPresent();
    Settings::OnPresent();
    Aim::OnPresent();
    Gesture::OnPresent();
    Holster::OnPresent();
    Squad::OnPresent();
    Vehicle::OnPresent();
    Levers::OnPresent();
    Flight::OnPresent();
    WalkerDrive::OnPresent();
    Handlebars::OnPresent();
    GunnerSeat::OnPresent();
    Body::OnPresent();
    Feedback::OnPresent();
    Pointer::OnPresent();
    Keyboard::OnPresent();
    Controls::OnPresent();
    if (_isStackRequested)
    {
        _isStackRequested = false;
        // __builtin_frame_address(0): the compiler's address of this function's stack frame, where the walk starts.
        Probe::LogStack("Present", reinterpret_cast<uintptr_t>(__builtin_frame_address(0)));
    }
    // A static local is created once and keeps its value between calls, like a static field.
    static IDirect3DDevice9* lastPresentingDevice = nullptr;
    _presentingDevice = device;
    if (device != lastPresentingDevice)
    {
        lastPresentingDevice = device;
        Log::Info("d3d", "device %p is presenting (frame %u)", static_cast<void*>(device), _frameCount);
    }
    const bool wasTraced = _isTracing;
    if (wasTraced)
    {
        EndTrace();
    }
    Stereo::GrabPresented(device);
    Stereo::PaceBeforePresent();
    HRESULT result;
    {
        Profiling::Scope profile(Profiling::Stage::Present);
        result = _original.Present(device, sourceRect, destinationRect, window, dirtyRegion);
    }
    EngineSurvey::OnPresent(_frameCount, wasTraced);
    ++_frameCount;
    const DWORD now = GetTickCount();
    // Six automatic traces 10 s apart, then only on request (script command "trace").
    if (_traceCount < MAXIMUM_TRACES &&
        (_isTraceRequested || (_traceCount < AUTOMATIC_TRACES &&
                               now - _lastTraceTick >= AUTOMATIC_TRACE_INTERVAL_MILLISECONDS && !Xr::IsStarted())))
    {
        _isTraceRequested = false;
        BeginTrace(device, now);
    }
    return result;
}

/// Reset (window size or mode change): the default-pool render targets must be released first (Stereo::OnReset).
HRESULT WINAPI ResetHook(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters)
{
    LogPresentParameters("Reset", parameters);
    LogExCounts("reset");
    Stereo::OnReset();
    const HRESULT result = _original.Reset(device, parameters);
    Log::Info("d3d", "Reset -> 0x%lx", result);
    return result;
}

HRESULT WINAPI SetRenderTargetHook(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9* surface)
{
    if (_isTracing)
    {
        FpGuard fp;
        ++_totals.setRenderTarget;
        FlushDraws();
        D3DSURFACE_DESC description{};
        if (surface)
        {
            surface->lpVtbl->GetDesc(surface, &description);
        }
        fprintf(
            _traceFile,
            " RT%lu %p %ux%u fmt=%d usage=0x%lx\n",
            index,
            static_cast<void*>(surface),
            description.Width,
            description.Height,
            static_cast<int>(description.Format),
            description.Usage);
    }
    return _original.SetRenderTarget(device, index, surface);
}

HRESULT WINAPI SetDepthStencilSurfaceHook(IDirect3DDevice9* device, IDirect3DSurface9* surface)
{
    if (_isTracing)
    {
        FpGuard fp;
        FlushDraws();
        D3DSURFACE_DESC description{};
        if (surface)
        {
            surface->lpVtbl->GetDesc(surface, &description);
        }
        fprintf(
            _traceFile,
            " DS %p %ux%u fmt=%d\n",
            static_cast<void*>(surface),
            description.Width,
            description.Height,
            static_cast<int>(description.Format));
    }
    return _original.SetDepthStencilSurface(device, surface);
}

HRESULT WINAPI BeginSceneHook(IDirect3DDevice9* device)
{
    if (_isTracing)
    {
        FpGuard fp;
        ++_totals.beginScene;
        FlushDraws();
        fprintf(_traceFile, " BeginScene\n");
    }
    return _original.BeginScene(device);
}

HRESULT WINAPI EndSceneHook(IDirect3DDevice9* device)
{
    if (_isTracing)
    {
        FpGuard fp;
        FlushDraws();
        fprintf(_traceFile, " EndScene\n");
    }
    return _original.EndScene(device);
}

HRESULT WINAPI ClearHook(
    IDirect3DDevice9* device,
    DWORD count,
    const D3DRECT* rects,
    DWORD flags,
    D3DCOLOR color,
    float z,
    DWORD stencil)
{
    if (_isTracing)
    {
        FpGuard fp;
        ++_totals.clear;
        FlushDraws();
        fprintf(_traceFile, " Clear flags=0x%lx rects=%lu color=0x%08lx\n", flags, count, color);
    }
    return _original.Clear(device, count, rects, flags, color, z, stencil);
}

/// SetTransform: traced, and every projection the game sets goes to Stereo (it tells the 3D scene from the HUD pass).
HRESULT WINAPI SetTransformHook(IDirect3DDevice9* device, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix)
{
    if (_isTracing && matrix)
    {
        FpGuard fp;
        if (state == D3DTS_VIEW || state == D3DTS_PROJECTION)
        {
            if (state == D3DTS_VIEW)
            {
                ++_totals.view;
            }
            else
            {
                ++_totals.projection;
            }
            FlushDraws();
            fprintf(_traceFile, " %s", state == D3DTS_VIEW ? "VIEW" : "PROJ");
            PrintFloats(&matrix->m[0][0], FLOATS_PER_MATRIX);
            fputc('\n', _traceFile);
        }
        else if (static_cast<int>(state) >= 256) // D3DTS_WORLDMATRIX(n) is 256 + n
        {
            ++_totals.world;
        }
        else
        {
            ++_totals.otherTransform;
        }
    }
    const HRESULT result = _original.SetTransform(device, state, matrix);
    if (state == D3DTS_PROJECTION && matrix)
    {
        Stereo::OnProjection(device, &matrix->m[0][0]);
    }
    return result;
}

HRESULT WINAPI SetViewportHook(IDirect3DDevice9* device, const D3DVIEWPORT9* viewport)
{
    if (_isTracing && viewport)
    {
        FpGuard fp;
        ++_totals.setViewport;
        FlushDraws();
        fprintf(
            _traceFile,
            " VP %lu,%lu %lux%lu z=%g..%g\n",
            viewport->X,
            viewport->Y,
            viewport->Width,
            viewport->Height,
            static_cast<double>(viewport->MinZ),
            static_cast<double>(viewport->MaxZ));
    }
    return _original.SetViewport(device, viewport);
}

HRESULT WINAPI StretchRectHook(
    IDirect3DDevice9* device,
    IDirect3DSurface9* source,
    const RECT* sourceRect,
    IDirect3DSurface9* destination,
    const RECT* destinationRect,
    D3DTEXTUREFILTERTYPE filter)
{
    if (_isTracing)
    {
        FpGuard fp;
        ++_totals.stretchRect;
        FlushDraws();
        fprintf(_traceFile, " StretchRect %p -> %p\n", static_cast<void*>(source), static_cast<void*>(destination));
    }
    return _original.StretchRect(device, source, sourceRect, destination, destinationRect, filter);
}

HRESULT WINAPI DrawPrimitiveHook(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT start, UINT count)
{
    CountDraw();
    ++_passDrawCalls[DRAW_PLAIN];
    _passPrimitives[DRAW_PLAIN] += count;
    return _original.DrawPrimitive(device, type, start, count);
}

HRESULT WINAPI DrawIndexedPrimitiveHook(
    IDirect3DDevice9* device,
    D3DPRIMITIVETYPE type,
    INT baseVertex,
    UINT minimumIndex,
    UINT vertexCount,
    UINT startIndex,
    UINT primitiveCount)
{
    CountDraw();
    if (_watchedTexture && _stageTexture == _watchedTexture && _isModelBlockLit)
    {
        memcpy(_watchedLight, _modelBlock + 7 * 4, sizeof(_watchedLight)); // c18-c21
        _watchedLightTick = GetTickCount();
        _hasWatchedLight = true;
    }
    if (_isShaderDumpRequested && IsShaderDumping())
    {
        Log::Info(
            "shaderdump",
            "draw indexed %u triangles, %u vertices, texture %p, vertex shader %p",
            primitiveCount,
            vertexCount,
            _stageTexture,
            _boundVertexShader);
    }
    ++_passDrawCalls[DRAW_INDEXED];
    _passPrimitives[DRAW_INDEXED] += primitiveCount;
    return _original
        .DrawIndexedPrimitive(device, type, baseVertex, minimumIndex, vertexCount, startIndex, primitiveCount);
}

HRESULT WINAPI DrawPrimitiveUPHook(
    IDirect3DDevice9* device,
    D3DPRIMITIVETYPE type,
    UINT primitiveCount,
    const void* vertices,
    UINT stride)
{
    CountDraw();
    ++_passDrawCalls[DRAW_USER_POINTER];
    _passPrimitives[DRAW_USER_POINTER] += primitiveCount;
    return _original.DrawPrimitiveUP(device, type, primitiveCount, vertices, stride);
}

HRESULT WINAPI DrawIndexedPrimitiveUPHook(
    IDirect3DDevice9* device,
    D3DPRIMITIVETYPE type,
    UINT minimumIndex,
    UINT vertexCount,
    UINT primitiveCount,
    const void* indices,
    D3DFORMAT indexFormat,
    const void* vertices,
    UINT stride)
{
    CountDraw();
    ++_passDrawCalls[DRAW_USER_POINTER];
    _passPrimitives[DRAW_USER_POINTER] += primitiveCount;
    return _original.DrawIndexedPrimitiveUP(
        device,
        type,
        minimumIndex,
        vertexCount,
        primitiveCount,
        indices,
        indexFormat,
        vertices,
        stride);
}

// A mirrored first-person model (the super battle droid's left arm, Aim) has its triangles turned inside out: while
// one is drawn, the first-person pass draws without back-face culling.
HRESULT WINAPI SetRenderStateHook(IDirect3DDevice9* device, D3DRENDERSTATETYPE state, DWORD value)
{
    if (state == D3DRS_CULLMODE && value != D3DCULL_NONE && Aim::IsMirrorDrawn() && Stereo::IsFirstPersonPass())
    {
        value = D3DCULL_NONE;
    }
    return _original.SetRenderState(device, state, value);
}

HRESULT WINAPI SetFVFHook(IDirect3DDevice9* device, DWORD fvf)
{
    if (_isTracing)
    {
        ++_totals.setFvf;
    }
    return _original.SetFVF(device, fvf);
}

HRESULT WINAPI SetVertexShaderHook(IDirect3DDevice9* device, IDirect3DVertexShader9* shader)
{
    _isVertexShaderBound = shader != nullptr;
    _boundVertexShader = shader;
    if (_isShaderDumpRequested && IsShaderDumping())
    {
        Log::Info("shaderdump", "vertex shader %p", static_cast<void*>(shader));
    }
    if (_isTracing)
    {
        ++_totals.setVertexShader;
    }
    return _original.SetVertexShader(device, shader);
}

HRESULT WINAPI SetPixelShaderHook(IDirect3DDevice9* device, IDirect3DPixelShader9* shader)
{
    if (_isTracing)
    {
        ++_totals.setPixelShader;
    }
    return _original.SetPixelShader(device, shader);
}

HRESULT WINAPI SetTextureHook(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* texture)
{
    if (stage == 0)
    {
        _stageTexture = texture;
        if (_isShaderDumpRequested && IsShaderDumping())
        {
            Log::Info("shaderdump", "texture %p", static_cast<void*>(texture));
        }
    }
    return _original.SetTexture(device, stage, texture);
}

HRESULT WINAPI SetPixelShaderConstantFHook(IDirect3DDevice9* device, UINT start, const float* data, UINT count)
{
    if (_isShaderDumpRequested && data && IsShaderDumping())
    {
        DumpConstants("ps", start, data, count);
    }
    return _original.SetPixelShaderConstantF(device, start, data, count);
}

HRESULT WINAPI SetVertexShaderConstantFHook(IDirect3DDevice9* device, UINT start, const float* data, UINT count)
{
    if (start == 11 && data)
    {
        _isModelBlockLit = count >= 11;
        if (_isModelBlockLit)
        {
            memcpy(_modelBlock, data, sizeof(_modelBlock));
        }
    }
    if (_isShaderDumpRequested && data && IsShaderDumping())
    {
        DumpConstants("vs", start, data, count);
    }
    if (_isTracing && data && count && start < VERTEX_SHADER_REGISTERS)
    {
        ++_totals.vertexShaderConstants;
        ConstantRegister& constant = _registers[start];
        const uint32_t floatCount = count >= REGISTERS_PER_MATRIX ? FLOATS_PER_MATRIX : count * FLOATS_PER_REGISTER;
        if (!constant.writes)
        {
            memcpy(constant.first, data, floatCount * sizeof(float));
            constant.firstCount = floatCount;
        }
        else if (constant.lastCount != floatCount || memcmp(constant.last, data, floatCount * sizeof(float)) != 0)
        {
            ++constant.changes;
        }
        memcpy(constant.last, data, floatCount * sizeof(float));
        constant.lastCount = floatCount;
        ++constant.writes;
    }
    return _original.SetVertexShaderConstantF(device, start, data, count);
}

/// On Direct3D 9Ex: a managed resource is made in the default pool; textures (not targets) also dynamic, so the game's
/// and the mod's LockRect keep working.
void MapPool(const char* what, D3DPOOL* pool, DWORD* usage, bool isTexture)
{
    if (!_isEx || *pool != D3DPOOL_MANAGED)
    {
        return;
    }
    *pool = D3DPOOL_DEFAULT;
    isTexture ? ++_exCounts.managedTextures : ++_exCounts.managedBuffers;
    if (isTexture && !(*usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)))
    {
        *usage |= D3DUSAGE_DYNAMIC;
    }
    if (_poolChangeLogs)
    {
        --_poolChangeLogs;
        Log::Info("d3d", "Ex: %s managed -> default pool, usage 0x%lx", what, *usage);
    }
}

/// Logs the first failures of a creation on Direct3D 9Ex.
HRESULT CheckPoolResult(const char* what, HRESULT result, unsigned a, unsigned b, DWORD usage, int format)
{
    if (_isEx && FAILED(result) && _poolFailureLogs)
    {
        --_poolFailureLogs;
        Log::Warning("d3d", "Ex: %s %ux%u usage 0x%lx fmt %d failed: 0x%lx", what, a, b, usage, format, result);
    }
    return result;
}

HRESULT WINAPI CreateCubeTextureHook(
    IDirect3DDevice9* device,
    UINT edge,
    UINT levels,
    DWORD usage,
    D3DFORMAT format,
    D3DPOOL pool,
    IDirect3DCubeTexture9** texture,
    HANDLE* sharedHandle)
{
    MapPool("cube texture", &pool, &usage, true);
    return CheckPoolResult(
        "cube texture",
        _original.CreateCubeTexture(device, edge, levels, usage, format, pool, texture, sharedHandle),
        edge,
        edge,
        usage,
        static_cast<int>(format));
}

HRESULT WINAPI CreateVolumeTextureHook(
    IDirect3DDevice9* device,
    UINT width,
    UINT height,
    UINT depth,
    UINT levels,
    DWORD usage,
    D3DFORMAT format,
    D3DPOOL pool,
    IDirect3DVolumeTexture9** texture,
    HANDLE* sharedHandle)
{
    MapPool("volume texture", &pool, &usage, true);
    return CheckPoolResult(
        "volume texture",
        _original.CreateVolumeTexture(device, width, height, depth, levels, usage, format, pool, texture, sharedHandle),
        width,
        height,
        usage,
        static_cast<int>(format));
}

HRESULT WINAPI CreateVertexBufferHook(
    IDirect3DDevice9* device,
    UINT length,
    DWORD usage,
    DWORD fvf,
    D3DPOOL pool,
    IDirect3DVertexBuffer9** buffer,
    HANDLE* sharedHandle)
{
    MapPool("vertex buffer", &pool, &usage, false);
    return CheckPoolResult(
        "vertex buffer",
        _original.CreateVertexBuffer(device, length, usage, fvf, pool, buffer, sharedHandle),
        length,
        1,
        usage,
        static_cast<int>(fvf));
}

HRESULT WINAPI CreateIndexBufferHook(
    IDirect3DDevice9* device,
    UINT length,
    DWORD usage,
    D3DFORMAT format,
    D3DPOOL pool,
    IDirect3DIndexBuffer9** buffer,
    HANDLE* sharedHandle)
{
    MapPool("index buffer", &pool, &usage, false);
    return CheckPoolResult(
        "index buffer",
        _original.CreateIndexBuffer(device, length, usage, format, pool, buffer, sharedHandle),
        length,
        1,
        usage,
        static_cast<int>(format));
}

/// Ex diagnostics (b667, run 890: most textures white on 9Ex): the texture lock methods, hooked on the first texture
/// made on 9Ex; the first locks and every failure (up to a limit) are logged.
using TextureLockRectFunction = HRESULT(WINAPI*)(IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
using TextureUnlockRectFunction = HRESULT(WINAPI*)(IDirect3DTexture9*, UINT);
TextureLockRectFunction _originalTextureLockRect = nullptr;
TextureUnlockRectFunction _originalTextureUnlockRect = nullptr;
unsigned _lockLogs = 24, _lockFailureLogs = 24, _textureLogs = 24;

void LogExCounts(const char* when)
{
    if (!_isEx || memcmp(&_exCounts, &_exCountsLogged, sizeof(_exCounts)) == 0)
    {
        return;
    }
    _exCountsLogged = _exCounts;
    Log::Info(
        "d3d",
        "Ex counts (%s): managed textures %u (with mips %u), buffers %u; locks on them %u, failed %u; unlocks %u",
        when,
        _exCounts.managedTextures,
        _exCounts.managedMipTextures,
        _exCounts.managedBuffers,
        _exCounts.dynamicLocks,
        _exCounts.dynamicLockFailures,
        _exCounts.unlocks);
    const ExCounts& c = _exCounts;
    Log::Info(
        "d3d",
        "Ex counts (%s): created by pool (no mips / mips) default %u/%u managed %u/%u systemmem %u/%u scratch %u/%u; "
        "bound by pool default %u managed %u systemmem %u scratch %u; UpdateTexture %u failed %u; UpdateSurface %u "
        "failed %u",
        when,
        c.created[0][0],
        c.created[0][1],
        c.created[1][0],
        c.created[1][1],
        c.created[2][0],
        c.created[2][1],
        c.created[3][0],
        c.created[3][1],
        c.bound[0],
        c.bound[1],
        c.bound[2],
        c.bound[3],
        c.updateTextures,
        c.updateTextureFailures,
        c.updateSurfaces,
        c.updateSurfaceFailures);
}

/// Ex diagnostics (b669, not called since b674: ~1500 calls a frame): what the draws bind. Counts 2D textures by pool;
/// logs the first distinct ones.
[[maybe_unused]] void CountBoundTexture(DWORD stage, IDirect3DBaseTexture9* texture)
{
    if (texture->lpVtbl->GetType(texture) != D3DRTYPE_TEXTURE)
    {
        return;
    }
    auto* const texture2d = reinterpret_cast<IDirect3DTexture9*>(texture);
    D3DSURFACE_DESC desc{};
    if (FAILED(texture2d->lpVtbl->GetLevelDesc(texture2d, 0, &desc)) || static_cast<unsigned>(desc.Pool) > 3)
    {
        return;
    }
    ++_exCounts.bound[desc.Pool];
    if (!_boundLogs)
    {
        return;
    }
    for (unsigned i = 0; i < 16 - _boundLogs; ++i)
    {
        if (_boundLogged[i] == texture)
        {
            return;
        }
    }
    _boundLogged[16 - _boundLogs] = texture;
    --_boundLogs;
    Log::Info(
        "d3d",
        "Ex: bound stage %lu %p %ux%u levels %lu fmt %d pool %d usage 0x%lx",
        stage,
        static_cast<void*>(texture),
        desc.Width,
        desc.Height,
        texture->lpVtbl->GetLevelCount(texture),
        static_cast<int>(desc.Format),
        static_cast<int>(desc.Pool),
        desc.Usage);
}

HRESULT WINAPI
UpdateTextureHook(IDirect3DDevice9* device, IDirect3DBaseTexture9* source, IDirect3DBaseTexture9* destination)
{
    const HRESULT result = _original.UpdateTexture(device, source, destination);
    if (_isEx)
    {
        ++_exCounts.updateTextures;
        if (FAILED(result))
        {
            ++_exCounts.updateTextureFailures;
        }
        if (_updateLogs || FAILED(result))
        {
            _updateLogs ? --_updateLogs : 0;
            Log::Info(
                "d3d",
                "Ex: UpdateTexture %p (levels %lu) -> %p (levels %lu): 0x%lx",
                static_cast<void*>(source),
                source ? source->lpVtbl->GetLevelCount(source) : 0,
                static_cast<void*>(destination),
                destination ? destination->lpVtbl->GetLevelCount(destination) : 0,
                result);
        }
    }
    return result;
}

HRESULT WINAPI UpdateSurfaceHook(
    IDirect3DDevice9* device,
    IDirect3DSurface9* source,
    const RECT* sourceRect,
    IDirect3DSurface9* destination,
    const POINT* destinationPoint)
{
    const HRESULT result = _original.UpdateSurface(device, source, sourceRect, destination, destinationPoint);
    if (_isEx)
    {
        ++_exCounts.updateSurfaces;
        if (FAILED(result))
        {
            ++_exCounts.updateSurfaceFailures;
        }
        if (_updateLogs || FAILED(result))
        {
            _updateLogs ? --_updateLogs : 0;
            D3DSURFACE_DESC from{}, to{};
            source->lpVtbl->GetDesc(source, &from);
            destination->lpVtbl->GetDesc(destination, &to);
            Log::Info(
                "d3d",
                "Ex: UpdateSurface %ux%u fmt %d pool %d -> %ux%u fmt %d pool %d usage 0x%lx: 0x%lx",
                from.Width,
                from.Height,
                static_cast<int>(from.Format),
                static_cast<int>(from.Pool),
                to.Width,
                to.Height,
                static_cast<int>(to.Format),
                static_cast<int>(to.Pool),
                to.Usage,
                result);
        }
    }
    return result;
}

HRESULT WINAPI
TextureLockRectHook(IDirect3DTexture9* texture, UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags)
{
    const HRESULT result = _originalTextureLockRect(texture, level, locked, rect, flags);
    if (_isEx)
    {
        D3DSURFACE_DESC levelDesc{};
        texture->lpVtbl->GetLevelDesc(texture, level, &levelDesc);
        if (levelDesc.Usage & D3DUSAGE_DYNAMIC)
        {
            ++_exCounts.dynamicLocks;
            if (FAILED(result))
            {
                ++_exCounts.dynamicLockFailures;
            }
        }
    }
    if ((FAILED(result) && _lockFailureLogs) || _lockLogs)
    {
        FAILED(result) ? --_lockFailureLogs : --_lockLogs;
        D3DSURFACE_DESC desc{};
        texture->lpVtbl->GetLevelDesc(texture, level, &desc);
        Log::Info(
            "d3d",
            "Ex: LockRect %p level %u %ux%u fmt %d pool %d usage 0x%lx rect %s flags 0x%lx -> 0x%lx",
            static_cast<void*>(texture),
            level,
            desc.Width,
            desc.Height,
            static_cast<int>(desc.Format),
            static_cast<int>(desc.Pool),
            desc.Usage,
            rect ? "part" : "all",
            flags,
            result);
    }
    return result;
}

HRESULT WINAPI TextureUnlockRectHook(IDirect3DTexture9* texture, UINT level)
{
    ++_exCounts.unlocks;
    return _originalTextureUnlockRect(texture, level);
}

void HookTextureLocks(IDirect3DTexture9* texture)
{
    if (_originalTextureLockRect || !texture)
    {
        return;
    }
    void* const lockRect = reinterpret_cast<void*>(texture->lpVtbl->LockRect);
    void* const unlockRect = reinterpret_cast<void*>(texture->lpVtbl->UnlockRect);
    TextureLockRectFunction lockHook = TextureLockRectHook;
    TextureUnlockRectFunction unlockHook = TextureUnlockRectHook;
    const MH_STATUS lockStatus =
        MH_CreateHook(lockRect, reinterpret_cast<void*>(lockHook), reinterpret_cast<void**>(&_originalTextureLockRect));
    const MH_STATUS unlockStatus = MH_CreateHook(
        unlockRect,
        reinterpret_cast<void*>(unlockHook),
        reinterpret_cast<void**>(&_originalTextureUnlockRect));
    MH_EnableHook(lockRect);
    MH_EnableHook(unlockRect);
    Log::Info(
        "d3d",
        "Ex: texture LockRect / UnlockRect hooked: %s / %s",
        MH_StatusToString(lockStatus),
        MH_StatusToString(unlockStatus));
}

HRESULT WINAPI CreateTextureHook(
    IDirect3DDevice9* device,
    UINT width,
    UINT height,
    UINT levels,
    DWORD usage,
    D3DFORMAT format,
    D3DPOOL pool,
    IDirect3DTexture9** texture,
    HANDLE* sharedHandle)
{
    const D3DPOOL askedPool = pool;
    if (levels != 1 && _mipLogs)
    {
        --_mipLogs;
        Log::Info(
            "d3d",
            "mip texture %ux%u levels %u fmt %d pool %d usage 0x%lx from %p",
            width,
            height,
            levels,
            static_cast<int>(format),
            static_cast<int>(pool),
            usage,
            __builtin_return_address(0));
    }
    if (_isEx && static_cast<unsigned>(pool) <= 3)
    {
        ++_exCounts.created[pool][levels != 1];
    }
    if (_isEx && pool == D3DPOOL_MANAGED && levels != 1)
    {
        ++_exCounts.managedMipTextures;
    }
    MapPool("texture", &pool, &usage, true);
    const HRESULT result = CheckPoolResult(
        "texture",
        _original.CreateTexture(device, width, height, levels, usage, format, pool, texture, sharedHandle),
        width,
        height,
        usage,
        static_cast<int>(format));
    if (_isEx && SUCCEEDED(result) && texture && *texture)
    {
        HookTextureLocks(*texture);
        if (_textureLogs)
        {
            --_textureLogs;
            Log::Info(
                "d3d",
                "Ex: texture %ux%u levels %u fmt %d asked pool %d usage 0x%lx -> %p",
                width,
                height,
                levels,
                static_cast<int>(format),
                static_cast<int>(askedPool),
                usage,
                static_cast<void*>(*texture));
        }
    }
    if ((usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) && _createLogCount++ < MAXIMUM_CREATE_LOGS)
    {
        Log::Info(
            "d3d",
            "CreateTexture target %ux%u usage=0x%lx fmt=%d -> %p",
            width,
            height,
            usage,
            static_cast<int>(format),
            texture ? static_cast<void*>(*texture) : nullptr);
    }
    return result;
}

HRESULT WINAPI CreateRenderTargetHook(
    IDirect3DDevice9* device,
    UINT width,
    UINT height,
    D3DFORMAT format,
    D3DMULTISAMPLE_TYPE multiSample,
    DWORD quality,
    BOOL isLockable,
    IDirect3DSurface9** surface,
    HANDLE* sharedHandle)
{
    const HRESULT result =
        _original
            .CreateRenderTarget(device, width, height, format, multiSample, quality, isLockable, surface, sharedHandle);
    if (_createLogCount++ < MAXIMUM_CREATE_LOGS)
    {
        Log::Info(
            "d3d",
            "CreateRenderTarget %ux%u fmt=%d msaa=%d -> %p",
            width,
            height,
            static_cast<int>(format),
            static_cast<int>(multiSample),
            surface ? static_cast<void*>(*surface) : nullptr);
    }
    return result;
}

HRESULT WINAPI CreateDepthStencilSurfaceHook(
    IDirect3DDevice9* device,
    UINT width,
    UINT height,
    D3DFORMAT format,
    D3DMULTISAMPLE_TYPE multiSample,
    DWORD quality,
    BOOL shouldDiscard,
    IDirect3DSurface9** surface,
    HANDLE* sharedHandle)
{
    const HRESULT result = _original.CreateDepthStencilSurface(
        device,
        width,
        height,
        format,
        multiSample,
        quality,
        shouldDiscard,
        surface,
        sharedHandle);
    if (_createLogCount++ < MAXIMUM_CREATE_LOGS)
    {
        Log::Info(
            "d3d",
            "CreateDepthStencilSurface %ux%u fmt=%d msaa=%d -> %p",
            width,
            height,
            static_cast<int>(format),
            static_cast<int>(multiSample),
            surface ? static_cast<void*>(*surface) : nullptr);
    }
    return result;
}

// The device methods we hook, each with a `<Method>Hook` above. An "X-macro": the list is written once, and each use
// defines X to expand every entry differently (in HookDevice one comparison per entry, then one MinHook call per
// entry). It has to be a macro: C++ has no other way to turn one list of method names into field accesses, function
// names and string literals.
// clang-format off
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): an X-macro, see above
#define DEVICE_HOOKS(X)                                                                                                \
    X(Present)                                                                                                         \
    X(Reset)                                                                                                           \
    X(SetRenderTarget)                                                                                                 \
    X(SetDepthStencilSurface)                                                                                          \
    X(BeginScene)                                                                                                      \
    X(EndScene)                                                                                                        \
    X(Clear)                                                                                                           \
    X(SetTransform)                                                                                                    \
    X(SetViewport)                                                                                                     \
    X(StretchRect)                                                                                                     \
    X(DrawPrimitive)                                                                                                   \
    X(DrawIndexedPrimitive)                                                                                            \
    X(DrawPrimitiveUP)                                                                                                 \
    X(DrawIndexedPrimitiveUP)                                                                                          \
    X(SetFVF)                                                                                                          \
    X(SetRenderState)                                                                                                  \
    X(SetVertexShader)                                                                                                 \
    X(SetPixelShader)                                                                                                  \
    X(SetVertexShaderConstantF)                                                                                        \
    X(SetPixelShaderConstantF)                                                                                         \
    X(SetTexture)                                                                                                      \
    X(UpdateTexture)                                                                                                   \
    X(UpdateSurface)                                                                                                   \
    X(CreateTexture)                                                                                                   \
    X(CreateCubeTexture)                                                                                               \
    X(CreateVolumeTexture)                                                                                             \
    X(CreateVertexBuffer)                                                                                              \
    X(CreateIndexBuffer)                                                                                               \
    X(CreateRenderTarget)                                                                                              \
    X(CreateDepthStencilSurface)
// clang-format on

/// Hooks the device methods in DEVICE_HOOKS. The first device's methods are hooked inline (MinHook patches the code in
/// d3d9.dll, so every later device that uses the same code is covered); later devices are only checked for different
/// code.
void HookDevice(IDirect3DDevice9* device)
{
    IDirect3DDevice9Vtbl* methods = device->lpVtbl;
    if (_isHooked)
    {
        // Same d3d9 code serves every device, so nothing to do unless this one uses different functions.
        // (#method turns the macro argument into a string literal: "Present", ...)
#define X(method)                                                                                                      \
    if (methods->method != _firstDeviceMethods.method)                                                                 \
        Log::Info(                                                                                                     \
            "d3d",                                                                                                     \
            "device %p uses different code for %s (%p, hooked %p)",                                                    \
            static_cast<void*>(device),                                                                                \
            #method,                                                                                                   \
            reinterpret_cast<void*>(methods->method),                                                                  \
            reinterpret_cast<void*>(_firstDeviceMethods.method));
        DEVICE_HOOKS(X)
#undef X
        return;
    }
    _isHooked = true;
    _firstDeviceMethods = *methods;
    const MH_STATUS initializeStatus = MH_Initialize();
    if (initializeStatus != MH_OK && initializeStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        Log::Error("d3d", "MinHook init failed: %s", MH_StatusToString(initializeStatus));
        return;
    }
    unsigned hookedCount = 0, failedCount = 0;
// The typed local makes the compiler verify each hook against the real method signature (method##Hook pastes the
// two into one name: PresentHook, ...).
#define X(method)                                                                                                      \
    {                                                                                                                  \
        decltype(methods->method) hook = method##Hook;                                                                 \
        const MH_STATUS status = MH_CreateHook(                                                                        \
            reinterpret_cast<void*>(methods->method),                                                                  \
            reinterpret_cast<void*>(hook),                                                                             \
            reinterpret_cast<void**>(&_original.method));                                                              \
        if (status == MH_OK)                                                                                           \
            ++hookedCount;                                                                                             \
        else                                                                                                           \
        {                                                                                                              \
            ++failedCount;                                                                                             \
            _original.method = nullptr;                                                                                \
            Log::Error("d3d", "hook %s failed: %s", #method, MH_StatusToString(status));                               \
        }                                                                                                              \
    }
    DEVICE_HOOKS(X)
#undef X
    const MH_STATUS enableStatus = MH_EnableHook(MH_ALL_HOOKS);
    Log::Info(
        "d3d",
        "device %p, %u methods hooked, %u failed, enable: %s",
        static_cast<void*>(device),
        hookedCount,
        failedCount,
        MH_StatusToString(enableStatus));
}

/// IDirect3D9::CreateDevice, hooked by patching the IDirect3D9 vtable: logs the device parameters and hooks the new
/// device's methods.
HRESULT WINAPI CreateDeviceHook(
    IDirect3D9* self,
    UINT adapter,
    D3DDEVTYPE type,
    HWND window,
    DWORD flags,
    D3DPRESENT_PARAMETERS* parameters,
    IDirect3DDevice9** device)
{
    Log::Info(
        "d3d",
        "CreateDevice adapter=%u type=%d behavior=0x%lx hwnd=%p",
        adapter,
        static_cast<int>(type),
        flags,
        static_cast<void*>(window));
    LogPresentParameters("CreateDevice asks", parameters);
    const HRESULT result = _originalCreateDevice(self, adapter, type, window, flags, parameters, device);
    Log::Info("d3d", "CreateDevice -> 0x%lx", result);
    if (SUCCEEDED(result))
    {
        LogPresentParameters("CreateDevice got", parameters);
        FitWindow(parameters->hDeviceWindow ? parameters->hDeviceWindow : window);
        if (device && *device)
        {
            HookDevice(*device);
            Log::Info(
                "d3d",
                "available texture memory %u (Ex %d)",
                (*device)->lpVtbl->GetAvailableTextureMem(*device),
                _isEx ? 1 : 0);
        }
    }
    return result;
}

/// The game's forced window size: its command line "/win /resolution W H" sets a flag and two integers, and its
/// video set-up (battlefront.exe+0x1f8d20) then makes a windowed mode of exactly that size instead of the profile's
/// (read in the code 2026-11-10). A window's back buffer may be larger than the monitor, so this is how the eye image
/// gets more pixels than the monitor's mode: ini `render_size = <width>x<height>` (PLAN M2 "Render size").
constexpr uintptr_t RVA_forcedWidth = 0x581ebc, RVA_forcedHeight = 0x581ec0, RVA_isWindowForced = 0x581ec4;
/// The command-line parser's store of the width: `mov [forcedWidth], eax` (A3 and the absolute address).
constexpr uintptr_t RVA_forcedWidthStore = 0xb92a5;

/// The size ApplyRenderSize forced, 0 = none: the game's window then takes the back buffer's size, which may not fit
/// the monitor (1920x1792 on a 1920x1200 screen hides the menus' lower buttons; captures and clips read the back
/// buffer, so tests see all of it). With ini `fit_window = 1` FitWindow shrinks the window (run 881: frame 13.2 ms
/// against 11.8 without, cause not separated from run-to-run spread); the back buffer keeps its size (Present scales it
/// into the window; the menu cursor counts mouse movement, not window pixels).
int _forcedWindowWidth = 0, _forcedWindowHeight = 0;
HWND _gameWindow = nullptr; // the device's window, checked again every FIT_CHECK_PRESENTS presents
unsigned _fitLogsLeft = 4;
bool _isWindowFitOn = false; // ini fit_window = 1 (off by default: idren2023 2026-11-24, no cost for the player)

/// Shrinks the game's window to the monitor's work area, keeping the picture's shape, and centres it. The game sizes
/// its window again after creating the device (run 880), so Present calls this every FIT_CHECK_PRESENTS frames; it
/// does nothing while the window already has the fitted size.
void FitWindow(HWND window)
{
    if (!_isWindowFitOn || !window || !_forcedWindowWidth || !_forcedWindowHeight)
    {
        return;
    }
    _gameWindow = window;
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoA(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
    {
        return;
    }
    const DWORD style = static_cast<DWORD>(GetWindowLongA(window, GWL_STYLE));
    const DWORD extendedStyle = static_cast<DWORD>(GetWindowLongA(window, GWL_EXSTYLE));
    RECT frame{0, 0, 0, 0};
    AdjustWindowRectEx(&frame, style, FALSE, extendedStyle);
    const int frameWidth = frame.right - frame.left, frameHeight = frame.bottom - frame.top;
    const RECT& area = monitor.rcWork;
    const int areaWidth = area.right - area.left, areaHeight = area.bottom - area.top;
    const float scale = std::min(
        1.0f,
        std::min(
            static_cast<float>(areaWidth - frameWidth) / static_cast<float>(_forcedWindowWidth),
            static_cast<float>(areaHeight - frameHeight) / static_cast<float>(_forcedWindowHeight)));
    const int clientWidth = static_cast<int>(static_cast<float>(_forcedWindowWidth) * scale);
    const int clientHeight = static_cast<int>(static_cast<float>(_forcedWindowHeight) * scale);
    RECT client{0, 0, 0, 0};
    GetClientRect(window, &client);
    if (client.right - client.left == clientWidth && client.bottom - client.top == clientHeight)
    {
        return;
    }
    const int windowWidth = clientWidth + frameWidth, windowHeight = clientHeight + frameHeight;
    SetWindowPos(
        window,
        nullptr,
        area.left + (areaWidth - windowWidth) / 2,
        area.top + (areaHeight - windowHeight) / 2,
        windowWidth,
        windowHeight,
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    if (!_fitLogsLeft)
    {
        return;
    }
    _fitLogsLeft--;
    Log::Info(
        "d3d",
        "window fitted to the monitor: %dx%d shown for the %dx%d picture",
        clientWidth,
        clientHeight,
        _forcedWindowWidth,
        _forcedWindowHeight);
}

/// Eye image sizes for ini `eye_resolution` (VR Settings HUD tab), all of the shape 15:14. That shape is close to the
/// Quest 3's view (tan 1.376 x 1.280 half angles on SteamVR, 1.376 x 1.428 on Virtual Desktop's runtime), and the menu
/// cursor's counts are known for it (run 739 at 2400x2240: about 1 015 across, 890 down; they follow the shape, not the
/// size). The readback through system memory costs about 1.6 ms per million pixels of both eyes (first tester's log,
/// 3840x2160: 13 ms), so "medium" keeps the frame inside 72 Hz on both test machines.
struct EyeResolution
{
    const char* name;
    int width, height;
};

constexpr EyeResolution EYE_RESOLUTIONS[] =
    {{"low", 1440, 1344}, {"medium", 1920, 1792}, {"high", 2400, 2240}, {"max", 2880, 2688}};
constexpr float EYE_SHAPE_MENU_COUNTS_X = 1015.0f, EYE_SHAPE_MENU_COUNTS_Y = 890.0f;

/// Sets the forced window size, once, before the game creates its Direct3D: ini `render_size` if set, else the
/// `eye_resolution` preset when VR will start by itself (SteamVR running at game start, as in Xr::Service).
void ApplyRenderSize()
{
    static bool isDone = false;
    if (isDone)
    {
        return;
    }
    isDone = true;
    char value[64];
    Settings::ReadEarly("render_size", value);
    int width = 0, height = 0;
    bool isPreset = false;
    if (sscanf(value, "%dx%d", &width, &height) != 2)
    {
        if (!Xr::IsSteamVrRunning())
        {
            return;
        }
        Settings::ReadEarly("eye_resolution", value);
        const char* const name = value[0] ? value : "medium";
        for (const EyeResolution& preset : EYE_RESOLUTIONS)
        {
            if (strcmp(preset.name, name) == 0)
            {
                width = preset.width;
                height = preset.height;
                isPreset = true;
            }
        }
        if (!isPreset)
        {
            Log::Info("d3d", "eye_resolution %s: the game keeps its own display mode", name);
            return;
        }
    }
    if (width < 800 || height < 600 || width > 8192 || height > 8192)
    {
        Log::Warning("d3d", "render_size %dx%d is out of range (800x600 to 8192x8192), not used", width, height);
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    const uint8_t* const store = reinterpret_cast<const uint8_t*>(base + RVA_forcedWidthStore);
    uintptr_t storedAddress = 0;
    memcpy(&storedAddress, store + 1, sizeof(storedAddress));
    if (store[0] != 0xA3 || storedAddress != base + RVA_forcedWidth)
    {
        Log::Warning("d3d", "render_size: this exe does not keep its forced window size where expected, not used");
        return;
    }
    *reinterpret_cast<int*>(base + RVA_forcedWidth) = width;
    *reinterpret_cast<int*>(base + RVA_forcedHeight) = height;
    *reinterpret_cast<uint8_t*>(base + RVA_isWindowForced) = 1;
    _forcedWindowWidth = width;
    _forcedWindowHeight = height;
    Settings::ReadEarly("fit_window", value);
    _isWindowFitOn = atoi(value) != 0;
    if (width * 14 == height * 15)
    {
        // The presets' shape, also when render_size has it. ini menu_counts_x / _y, applied later by Settings, win.
        Pointer::SetScreenCounts(EYE_SHAPE_MENU_COUNTS_X, EYE_SHAPE_MENU_COUNTS_Y);
    }
    Log::Info(
        "d3d",
        "%s: the game is told to run in a window of %dx%d",
        isPreset ? "eye_resolution" : "render_size",
        width,
        height);
}

/// Direct3DCreate9, hooked through the exe's import table: patches the new IDirect3D9's CreateDevice.
/// Direct3D 9Ex has no managed pool, and the game picks each level texture's format with
/// D3DXCheckTextureRequirements(..., D3DPOOL_MANAGED) (battlefront.exe+0x2832c0, the tex_ chunk's format list): on an
/// Ex device that check fails, no format is chosen and the texture is never made (runs 890-895: world textures
/// white). On 9Ex the check is asked for the default pool, where CreateTexture puts those textures anyway (MapPool).
void** FindImportSlot(HMODULE exe, const char* dllName, const char* functionName); // below, with Install
using CheckTextureRequirementsFunction =
    HRESULT(WINAPI*)(IDirect3DDevice9*, UINT*, UINT*, UINT*, DWORD, D3DFORMAT*, D3DPOOL);
CheckTextureRequirementsFunction _originalCheckTextureRequirements = nullptr;
unsigned _checkFailureLogs = 8;

HRESULT WINAPI CheckTextureRequirementsHook(
    IDirect3DDevice9* device,
    UINT* width,
    UINT* height,
    UINT* levels,
    DWORD usage,
    D3DFORMAT* format,
    D3DPOOL pool)
{
    const D3DFORMAT asked = format ? *format : D3DFMT_UNKNOWN;
    if (_isEx && pool == D3DPOOL_MANAGED)
    {
        pool = D3DPOOL_DEFAULT;
    }
    const HRESULT result = _originalCheckTextureRequirements(device, width, height, levels, usage, format, pool);
    if (FAILED(result) && _checkFailureLogs)
    {
        --_checkFailureLogs;
        Log::Warning(
            "d3d",
            "D3DXCheckTextureRequirements fmt %d pool %d failed: 0x%lx",
            static_cast<int>(asked),
            static_cast<int>(pool),
            result);
    }
    return result;
}

void HookCheckTextureRequirements()
{
    void** slot = FindImportSlot(GetModuleHandleA(nullptr), "d3dx9_43.dll", "D3DXCheckTextureRequirements");
    if (!slot || !*slot || _originalCheckTextureRequirements)
    {
        Log::Warning("d3d", "Ex: D3DXCheckTextureRequirements import not found: level textures will be missing");
        return;
    }
    _originalCheckTextureRequirements = reinterpret_cast<CheckTextureRequirementsFunction>(*slot);
    CheckTextureRequirementsFunction hook = CheckTextureRequirementsHook;
    Main::PatchPointer(slot, reinterpret_cast<void*>(hook));
    Log::Info("d3d", "Ex: D3DXCheckTextureRequirements import hooked (managed pool asked as default)");
}

/// Direct3D 9Ex in place of the game's Direct3D 9 when ini `shared_transfer` asks for it (1: when SteamVR runs at game
/// start, the default since b681 (idren2023 2026-11-24): eye images through shared textures, ~5 ms a frame against ~11;
/// a failure falls back to the usual Direct3D 9 or to the system-memory copy; 2: always; 0: never).
/// @return the IDirect3D9Ex (its vtable starts with IDirect3D9's), or nullptr for the usual one.
IDirect3D9* CreateDirect3D9Ex(UINT sdkVersion)
{
    char value[64];
    Settings::ReadEarly("shared_transfer", value);
    const int mode = value[0] ? atoi(value) : 1;
    if (mode == 0 || (mode == 1 && !Xr::IsSteamVrRunning()))
    {
        return nullptr;
    }
    using Direct3DCreate9ExFunction = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
    HMODULE d3d9 = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(_originalDirect3DCreate9),
        &d3d9);
    const auto create =
        d3d9 ? reinterpret_cast<Direct3DCreate9ExFunction>(GetProcAddress(d3d9, "Direct3DCreate9Ex")) : nullptr;
    IDirect3D9Ex* direct3dEx = nullptr;
    const HRESULT result = create ? create(sdkVersion, &direct3dEx) : E_NOTIMPL;
    if (FAILED(result) || !direct3dEx)
    {
        Log::Warning("d3d", "Direct3DCreate9Ex failed (0x%lx): the usual Direct3D 9", result);
        return nullptr;
    }
    _isEx = true;
    GpuTransfer::UseDirect3D9Ex(direct3dEx);
    HookCheckTextureRequirements();
    Log::Info("d3d", "the game runs on Direct3D 9Ex (shared_transfer = %d)", mode);
    return reinterpret_cast<IDirect3D9*>(direct3dEx);
}

IDirect3D9* WINAPI Direct3DCreate9Hook(UINT sdkVersion)
{
    ApplyRenderSize();
    // On D3D9On12 when the eye images are to go to the headset on the graphics card (GpuTransfer), else as usual.
    IDirect3D9* direct3d = GpuTransfer::CreateDirect3D(sdkVersion);
    if (!direct3d)
    {
        direct3d = CreateDirect3D9Ex(sdkVersion);
    }
    if (!direct3d)
    {
        direct3d = _originalDirect3DCreate9(sdkVersion);
    }
    Log::Info("d3d", "Direct3DCreate9(%u) -> %p", sdkVersion, static_cast<void*>(direct3d));
    if (direct3d && direct3d->lpVtbl->CreateDevice != CreateDeviceHook)
    {
        _originalCreateDevice = direct3d->lpVtbl->CreateDevice;
        decltype(direct3d->lpVtbl->CreateDevice) hook = CreateDeviceHook;
        Main::PatchPointer(reinterpret_cast<void**>(&direct3d->lpVtbl->CreateDevice), reinterpret_cast<void*>(hook));
    }
    return direct3d;
}

/// Finds the exe's import-table entry for dll!func.
/// @param exe the exe's module (its base address).
/// @param dllName the imported DLL, compared without case ("d3d9.dll").
/// @param functionName the imported function.
/// @return the address of the import slot the exe calls through, or null when not found.
void** FindImportSlot(HMODULE exe, const char* dllName, const char* functionName)
{
    // Walks the exe's PE headers in memory: pointer arithmetic from the module base by the RVAs they hold.
    BYTE* base = reinterpret_cast<BYTE*>(exe);
    auto* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dosHeader->e_lfanew);
    const IMAGE_DATA_DIRECTORY& directory = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress)
    {
        return nullptr;
    }
    HMODULE module = GetModuleHandleA(dllName);
    void* resolved = module ? reinterpret_cast<void*>(GetProcAddress(module, functionName)) : nullptr;
    for (auto* import = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress); import->Name;
         ++import)
    {
        if (_stricmp(reinterpret_cast<const char*>(base + import->Name), dllName) != 0)
        {
            continue;
        }
        auto* addressTable = reinterpret_cast<IMAGE_THUNK_DATA*>(base + import->FirstThunk);
        auto* names = import->OriginalFirstThunk
                          ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + import->OriginalFirstThunk)
                          : nullptr;
        for (unsigned slot = 0; addressTable[slot].u1.Function; ++slot)
        {
            if (names)
            {
                if (!names[slot].u1.AddressOfData)
                {
                    break;
                }
                if (IMAGE_SNAP_BY_ORDINAL(names[slot].u1.Ordinal))
                {
                    continue;
                }
                auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names[slot].u1.AddressOfData);
                if (strcmp(reinterpret_cast<const char*>(byName->Name), functionName) == 0)
                {
                    return reinterpret_cast<void**>(&addressTable[slot].u1.Function);
                }
            }
            else if (resolved && reinterpret_cast<void*>(addressTable[slot].u1.Function) == resolved)
            {
                return reinterpret_cast<void**>(&addressTable[slot].u1.Function);
            }
        }
    }
    return nullptr;
}

} // namespace

// Redirects the game's Direct3DCreate9 import so the device is seen as it is created.
// Called from DllMain and again at first DirectInput8Create in case the first attempt was too early.
void Install(const char* when)
{
    if (_originalDirect3DCreate9)
    {
        return;
    }
    HMODULE exe = GetModuleHandleA(nullptr);
    void** slot = FindImportSlot(exe, "d3d9.dll", "Direct3DCreate9");
    if (!slot)
    {
        Log::Warning("d3d", "import slot for Direct3DCreate9 not found (%s)", when);
        return;
    }
    void* current = *slot;
    // Only accept a slot the loader has already resolved: it must point at executable code in another module.
    MEMORY_BASIC_INFORMATION memoryInfo{};
    const DWORD executableProtections =
        PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!VirtualQuery(current, &memoryInfo, sizeof(memoryInfo)) || memoryInfo.State != MEM_COMMIT ||
        memoryInfo.AllocationBase == static_cast<void*>(exe) || !(memoryInfo.Protect & executableProtections))
    {
        Log::Info("d3d", "Direct3DCreate9 import not resolved yet (%s), value %p", when, current);
        return;
    }
    _originalDirect3DCreate9 = reinterpret_cast<Direct3DCreate9Function>(current);
    Direct3DCreate9Function hook = Direct3DCreate9Hook;
    Main::PatchPointer(slot, reinterpret_cast<void*>(hook));
    Log::Info("d3d", "Direct3DCreate9 import hooked (%s), original %p", when, current);
}

void RequestTrace()
{
    _isTraceRequested = true;
}

void WatchLightOf(void* texture)
{
    if (texture != _watchedTexture)
    {
        _watchedTexture = texture;
        _hasWatchedLight = false;
    }
}

bool GetWatchedLight(float* scale, float* ambient, float* sunColour, float* sunDirection)
{
    constexpr DWORD HOLD_MILLISECONDS = 1000;
    if (!_hasWatchedLight || GetTickCount() - _watchedLightTick > HOLD_MILLISECONDS)
    {
        return false;
    }
    memcpy(scale, _watchedLight, 3 * sizeof(float));
    memcpy(ambient, _watchedLight + 4, 3 * sizeof(float));
    memcpy(sunColour, _watchedLight + 8, 3 * sizeof(float));
    memcpy(sunDirection, _watchedLight + 12, 3 * sizeof(float));
    return true;
}

void RequestShaderDump()
{
    InterlockedExchange(&_isShaderDumpRequested, 1);
}

uint32_t FrameCount()
{
    return _frameCount;
}

void* PresentingDevice()
{
    return _presentingDevice;
}

void StartDrawLog(unsigned count)
{
    InterlockedExchange(&_drawLogPassesLeft, static_cast<LONG>(count));
}

void RequestPresentStack()
{
    _isStackRequested = true;
}

} // namespace D3dSurvey

} // namespace SwbfVr
