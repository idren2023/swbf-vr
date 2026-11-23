// GpuTransfer.cpp: the eye images go from the game's Direct3D 9 to the headset on the graphics card (b268), instead of
// through system memory (headset run 16: read-back about 1.9 ms and upload 0.7 ms per eye, with the CPU waiting for
// the card at every read-back).
//
// The game's IDirect3D9 is made by Direct3DCreate9On12 on a D3D12 device of ours. D3D9On12 is Microsoft's Direct3D 9
// on top of D3D12, part of Windows 10 since 1903; the game's own calls stay Direct3D 9. Per image:
//  1. StretchRect the back buffer into our A8R8G8B8 render target (Direct3D 9, on the card).
//  2. UnwrapUnderlyingResource lends that target out as a D3D12 resource; D3D9On12 turns its pending work into a wait
//     on the card in our queue.
//  3. Our queue copies it into a shared texture and signals the shared fence `written`.
//  4. ReturnUnderlyingResource gives the target back with that fence value (Direct3D 9 waits on the card for it).
// Xr opens the shared texture and fences in its Direct3D 11 device, checks `written` on the CPU (capped; since b635 no
// card-side wait: SteamVR syncs to that queue, headset 2026-11-13), copies into the swapchain image and signals `read`. Before the next copy into the same texture we check `read` on the CPU (it is
// normally long reached; a wait would be capped), so a missing signal can never hang the card.
// Checked without the game by tools/gpu-transfer-probe.cpp (2026-10-18: correct pixels in 200 of 200 frames, 0.45 ms
// CPU per image; SteamVR's 32-bit runtime also offers XR_KHR_D3D12_enable, not used).
//
// The Direct3D 12 and DXGI interfaces are used as C++ classes here (no CINTERFACE), like Xr's Direct3D 11.
#include "GpuTransfer.h"

#include "Log.h"
#include "Main.h"
#include "Settings.h"
#include "Xr.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <stdlib.h>
#include <string.h>

namespace SwbfVr
{

namespace GpuTransfer
{

namespace
{

// d3d9on12.h is not in llvm-mingw: these are its declarations from the Windows SDK (the interface id was found in
// SysWOW64\d3d9.dll; the probe's QueryInterface succeeds with it).
struct D3D9On12Arguments // D3D9ON12_ARGS
{
    BOOL Enable9On12;
    IUnknown* pD3D12Device;
    IUnknown* ppD3D12Queues[2];
    UINT NumQueues;
    UINT NodeMask;
};

struct IDirect3DDevice9On12 : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetD3D12Device(REFIID riid, void** device) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnwrapUnderlyingResource(
        IDirect3DResource9* resource,
        ID3D12CommandQueue* queue,
        REFIID riid,
        void** resource12) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReturnUnderlyingResource(
        IDirect3DResource9* resource,
        UINT syncCount,
        UINT64* signalValues,
        ID3D12Fence** fences) = 0;
};

const GUID IID_DIRECT3D_DEVICE_9_ON_12 = {0xe7fda234, 0xb589, 0x4049, {0x94, 0x0d, 0x88, 0x78, 0x97, 0x75, 0x31, 0xc8}};
using Create9On12Function = IDirect3D9*(WINAPI*)(UINT, D3D9On12Arguments*, UINT);

constexpr int SLOT_COUNT = 3;      // left eye, right eye, flat screen
constexpr int ALLOCATOR_COUNT = 4; // command allocators, used in turn; one is reset only after the card is done with it
/// Longest CPU wait for the consumer to finish reading a texture before it is written again (normally no wait at all:
/// it read it a frame ago). Past this the copy goes ahead; at worst one image shows part of the next.
constexpr DWORD READ_WAIT_MILLISECONDS = 20;
/// Each Return with a fence makes the next Returns of that resource slower (D3D9On12 keeps them all: probe 2026-10-18,
/// 0.05 to 0.35 ms over 20000; a new resource starts short again), so a slot's render target is made again after this
/// many transfers (about 28 s per eye at 72 Hz). Return without a fence would need D3D9On12 on our queue, which made
/// the game's own draws slower (runs 295, 296: frames 8.2-9.0 ms against 7.7-7.9 ms on its own queue, run 292).
constexpr unsigned TARGET_RENEW_TRANSFERS = 2000;

struct Slot
{
    ID3D12Resource* shared = nullptr;
    HANDLE sharedHandle = nullptr;
    UINT width = 0, height = 0;
    UINT64 lastFenceValue = 0;           // the value of the last image handed out from this slot
    IDirect3DTexture9* target = nullptr; // our render target on the Direct3D 9 side
    IDirect3DSurface9* targetSurface = nullptr;
    bool isStaged = false; // Stage copied an image into the target, Transfer has not taken it yet
    unsigned useCount = 0; // transfers since the target was made
};

ID3D12Device* _device = nullptr;
ID3D12CommandQueue* _queue = nullptr;
ID3D12CommandAllocator* _allocators[ALLOCATOR_COUNT] = {};
UINT64 _allocatorFenceValues[ALLOCATOR_COUNT] = {};
int _nextAllocator = 0;
ID3D12GraphicsCommandList* _list = nullptr;
ID3D12Fence *_written = nullptr, *_read = nullptr;
HANDLE _writtenHandle = nullptr, _readHandle = nullptr, _waitEvent = nullptr;
UINT64 _fenceValue = 0;
int _eyeLogCount = 0; // the first eye transfers are logged (LogState)
LUID _adapterLuid{};
Slot _slots[SLOT_COUNT];
IDirect3DDevice9* _device9 = nullptr; // the device _device9On12 was queried from
IDirect3DDevice9On12* _device9On12 = nullptr;
bool _isOn12 = false;    // the game's Direct3D 9 runs on D3D9On12 over our device
bool _hasFailed = false; // a step failed: the system-memory path from then on
unsigned _transferCount = 0, _readWaitCount = 0;
/// Milliseconds spent per part of Stage and Transfer since the last report (StretchRect, Unwrap, wait for the consumer,
/// record and execute, Return); logged every PART_REPORT_TRANSFERS transfers.
double _partMilliseconds[5];
constexpr unsigned PART_REPORT_TRANSFERS = 500;

double NowMilliseconds()
{
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return 1000.0 * static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart);
}

template <class T> void Release(T*& object)
{
    if (object)
    {
        object->Release();
        object = nullptr;
    }
}

bool Fail(const char* what, HRESULT result)
{
    if (!_hasFailed)
    {
        Log::Warning(
            "gpu",
            "%s failed (0x%08lx): eye images go through system memory from now on",
            what,
            static_cast<unsigned long>(result));
    }
    _hasFailed = true;
    return false;
}

/// Our D3D12 device on DXGI's first adapter (the one Direct3D 9 calls adapter 0), with a queue, command allocators, a
/// command list and the two shared fences. d3d12.dll and dxgi.dll are loaded at run time, nothing is linked.
bool CreateDevice()
{
    using CreateFactoryFunction = HRESULT(WINAPI*)(REFIID, void**);
    using CreateDeviceFunction = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    HMODULE dxgi = LoadLibraryA("dxgi.dll"), d3d12 = LoadLibraryA("d3d12.dll");
    auto createFactory = dxgi ? reinterpret_cast<CreateFactoryFunction>(
                                    reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")))
                              : nullptr;
    auto createDevice = d3d12 ? reinterpret_cast<CreateDeviceFunction>(
                                    reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice")))
                              : nullptr;
    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    if (!createFactory || !createDevice ||
        FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) ||
        factory->EnumAdapters1(0, &adapter) == DXGI_ERROR_NOT_FOUND)
    {
        Release(factory);
        Log::Info("gpu", "no D3D12 or no adapter: Direct3D 9 as usual");
        return false;
    }
    DXGI_ADAPTER_DESC1 description{};
    adapter->GetDesc1(&description);
    _adapterLuid = description.AdapterLuid;
    HRESULT result =
        createDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&_device));
    adapter->Release();
    factory->Release();
    if (FAILED(result))
    {
        Log::Info("gpu", "D3D12CreateDevice failed (0x%08lx): Direct3D 9 as usual", static_cast<unsigned long>(result));
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    result =
        _device->CreateCommandQueue(&queueDescription, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&_queue));
    for (int index = 0; index < ALLOCATOR_COUNT && SUCCEEDED(result); ++index)
    {
        result = _device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            __uuidof(ID3D12CommandAllocator),
            reinterpret_cast<void**>(&_allocators[index]));
    }
    if (SUCCEEDED(result))
    {
        result = _device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            _allocators[0],
            nullptr,
            __uuidof(ID3D12GraphicsCommandList),
            reinterpret_cast<void**>(&_list));
    }
    if (SUCCEEDED(result))
    {
        _list->Close();
        result = _device->CreateFence(
            0,
            D3D12_FENCE_FLAG_SHARED,
            __uuidof(ID3D12Fence),
            reinterpret_cast<void**>(&_written));
    }
    if (SUCCEEDED(result))
    {
        result =
            _device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&_read));
    }
    if (SUCCEEDED(result))
    {
        result = _device->CreateSharedHandle(_written, nullptr, GENERIC_ALL, nullptr, &_writtenHandle);
    }
    if (SUCCEEDED(result))
    {
        result = _device->CreateSharedHandle(_read, nullptr, GENERIC_ALL, nullptr, &_readHandle);
    }
    _waitEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(result) || !_waitEvent)
    {
        Log::Info(
            "gpu",
            "D3D12 queue, list or fences failed (0x%08lx): Direct3D 9 as usual",
            static_cast<unsigned long>(result));
        Release(_list);
        for (ID3D12CommandAllocator*& allocator : _allocators)
        {
            Release(allocator);
        }
        Release(_written);
        Release(_read);
        Release(_queue);
        Release(_device);
        return false;
    }
    Log::Info("gpu", "D3D12 device on %ls", description.Description);
    return true;
}

/// The IDirect3DDevice9On12 of the device the game presents with; checks that it runs on our D3D12 device (D3D9On12
/// makes a device of its own for an adapter other than ours, and our queue cannot copy its resources).
bool EnsureDevice9On12(IDirect3DDevice9* device)
{
    if (device == _device9 && _device9On12)
    {
        return true;
    }
    Release(_device9On12);
    OnReset(); // targets of another device
    _device9 = device;
    HRESULT result = device->QueryInterface(IID_DIRECT3D_DEVICE_9_ON_12, reinterpret_cast<void**>(&_device9On12));
    if (FAILED(result))
    {
        return Fail("QueryInterface IDirect3DDevice9On12", result);
    }
    ID3D12Device* device12 = nullptr;
    result = _device9On12->GetD3D12Device(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device12));
    const bool isOurs = SUCCEEDED(result) && device12 == _device;
    Release(device12);
    if (!isOurs)
    {
        return Fail("the game's device is not on our D3D12 device:", SUCCEEDED(result) ? E_FAIL : result);
    }
    return true;
}

/// The slot's Direct3D 9 render target and its shared D3D12 texture, at this size.
bool EnsureSlot(IDirect3DDevice9* device, Slot& slot, UINT width, UINT height)
{
    if (slot.width != width || slot.height != height)
    {
        Release(slot.targetSurface);
        Release(slot.target);
        Release(slot.shared);
        if (slot.sharedHandle)
        {
            CloseHandle(slot.sharedHandle);
            slot.sharedHandle = nullptr;
        }
        slot.width = slot.height = 0;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = width;
        description.Height = height;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // what D3D9On12 makes of D3DFMT_A8R8G8B8 (probe: format 87)
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        HRESULT result = _device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_SHARED,
            &description,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            __uuidof(ID3D12Resource),
            reinterpret_cast<void**>(&slot.shared));
        if (SUCCEEDED(result))
        {
            result = _device->CreateSharedHandle(slot.shared, nullptr, GENERIC_ALL, nullptr, &slot.sharedHandle);
        }
        if (FAILED(result))
        {
            return Fail("shared texture", result);
        }
        slot.width = width;
        slot.height = height;
    }
    if (!slot.target)
    {
        const HRESULT result = device->CreateTexture(
            width,
            height,
            1,
            D3DUSAGE_RENDERTARGET,
            D3DFMT_A8R8G8B8,
            D3DPOOL_DEFAULT,
            &slot.target,
            nullptr);
        if (FAILED(result) || FAILED(slot.target->GetSurfaceLevel(0, &slot.targetSurface)))
        {
            Release(slot.target);
            return Fail("render target", result);
        }
    }
    return true;
}

/// Waits (capped) until the consumer has signalled `read` for `value`.
void WaitUntilRead(UINT64 value)
{
    if (!value || _read->GetCompletedValue() >= value)
    {
        return;
    }
    if (++_readWaitCount <= 5)
    {
        Log::Info(
            "gpu",
            "the consumer has not read image %llu yet: waiting up to %lu ms",
            value,
            READ_WAIT_MILLISECONDS);
    }
    if (SUCCEEDED(_read->SetEventOnCompletion(value, _waitEvent)))
    {
        WaitForSingleObject(_waitEvent, READ_WAIT_MILLISECONDS);
    }
}

/// The next command allocator, once the card is done with what it recorded last time.
ID3D12CommandAllocator* NextAllocator(int* index)
{
    *index = _nextAllocator;
    _nextAllocator = (_nextAllocator + 1) % ALLOCATOR_COUNT;
    const UINT64 value = _allocatorFenceValues[*index];
    if (value && _written->GetCompletedValue() < value && SUCCEEDED(_written->SetEventOnCompletion(value, _waitEvent)))
    {
        WaitForSingleObject(_waitEvent, INFINITE);
    }
    return _allocators[*index];
}

/// Settings value of `gpu_transfer`: default 0 since b338 (idren2023's headset test 2026-10-19 on b337: the game hung in
/// Present at the spawn, on D3D9On12, as the real runtime's second swapchain came; the system-memory path is the one
/// headset runs 1-16 used). 1 or 2 in the ini still turn it on.
int Mode()
{
    char value[64];
    Settings::ReadEarly("gpu_transfer", value);
    return *value ? atoi(value) : 0;
}

} // namespace

IDirect3D9* CreateDirect3D(UINT sdkVersion)
{
    const int mode = Mode();
    if (mode == 0)
    {
        Log::Info("gpu", "gpu_transfer = 0: Direct3D 9 as usual, eye images through system memory");
        return nullptr;
    }
    if (mode == 1 && !Xr::IsSteamVrRunning())
    {
        Log::Info("gpu", "SteamVR not running at game start: Direct3D 9 as usual, eye images through system memory");
        return nullptr;
    }
    auto create9On12 = reinterpret_cast<Create9On12Function>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("d3d9.dll"), "Direct3DCreate9On12")));
    if (!create9On12)
    {
        Log::Info("gpu", "no Direct3DCreate9On12 (Windows before 10 1903): Direct3D 9 as usual");
        return nullptr;
    }
    if (!_device && !CreateDevice())
    {
        return nullptr;
    }
    D3D9On12Arguments arguments{};
    arguments.Enable9On12 = TRUE;
    arguments.pD3D12Device = _device;
    IDirect3D9* direct3d = create9On12(sdkVersion, &arguments, 1);
    if (!direct3d)
    {
        Log::Warning("gpu", "Direct3DCreate9On12 failed: Direct3D 9 as usual");
        return nullptr;
    }
    _isOn12 = true;
    Log::Info("gpu", "the game's Direct3D 9 runs on D3D9On12 over our D3D12 device (gpu_transfer = %d)", mode);
    return direct3d;
}

bool IsActive()
{
    return _isOn12 && !_hasFailed;
}

void GetAdapter(unsigned long* low, long* high)
{
    *low = _adapterLuid.LowPart;
    *high = _adapterLuid.HighPart;
}

bool Stage(IDirect3DDevice9* device, IDirect3DSurface9* source, int slotIndex)
{
    if (!IsActive() || slotIndex < 0 || slotIndex >= SLOT_COUNT || !EnsureDevice9On12(device))
    {
        return false;
    }
    Profiling::Scope profile(Profiling::Stage::GpuCopy);
    const double start = NowMilliseconds();
    D3DSURFACE_DESC sourceDescription{};
    source->GetDesc(&sourceDescription);
    Slot& slot = _slots[slotIndex];
    slot.isStaged = false;
    if (slot.useCount >= TARGET_RENEW_TRANSFERS)
    {
        Release(slot.targetSurface);
        Release(slot.target);
        slot.useCount = 0;
    }
    if (!EnsureSlot(device, slot, sourceDescription.Width, sourceDescription.Height))
    {
        return false;
    }
    const HRESULT result = device->StretchRect(source, nullptr, slot.targetSurface, nullptr, D3DTEXF_NONE);
    _partMilliseconds[0] += NowMilliseconds() - start;
    if (FAILED(result))
    {
        return Fail("StretchRect into our render target", result);
    }
    slot.isStaged = true;
    return true;
}

bool Transfer(const int* slotIndices, int count, bool hasConsumer, SharedImage* images)
{
    if (!IsActive() || count < 1 || count > SLOT_COUNT)
    {
        return false;
    }
    for (int index = 0; index < count; ++index)
    {
        if (slotIndices[index] < 0 || slotIndices[index] >= SLOT_COUNT || !_slots[slotIndices[index]].isStaged)
        {
            return false;
        }
    }
    Profiling::Scope profile(Profiling::Stage::GpuCopy);
    double time = NowMilliseconds(), previous = time;
    const auto part = [&](int index)
    {
        time = NowMilliseconds();
        _partMilliseconds[index] += time - previous;
        previous = time;
    };
    // Lend the targets out. Unwrap submits D3D9On12's pending work (the eye's draws) and makes our queue wait on the
    // card for it.
    ID3D12Resource* unwrapped[SLOT_COUNT] = {};
    // Give them back; with our fence value once our copy is queued (Direct3D 9 waits on the card for it before using
    // the target again), without one when nothing of ours was queued.
    const auto giveBack = [&](int unwrappedCount, UINT64 value)
    {
        for (int index = 0; index < unwrappedCount; ++index)
        {
            Slot& slot = _slots[slotIndices[index]];
            slot.isStaged = false;
            ++slot.useCount;
            const HRESULT result = value ? _device9On12->ReturnUnderlyingResource(slot.target, 1, &value, &_written)
                                         : _device9On12->ReturnUnderlyingResource(slot.target, 0, nullptr, nullptr);
            unwrapped[index]->Release();
            if (FAILED(result))
            {
                Fail("ReturnUnderlyingResource", result);
            }
        }
    };
    for (int index = 0; index < count; ++index)
    {
        const HRESULT result = _device9On12->UnwrapUnderlyingResource(
            _slots[slotIndices[index]].target,
            _queue,
            __uuidof(ID3D12Resource),
            reinterpret_cast<void**>(&unwrapped[index]));
        if (FAILED(result))
        {
            giveBack(index, 0);
            return Fail("UnwrapUnderlyingResource", result);
        }
    }
    part(1);
    for (int index = 0; index < count; ++index)
    {
        WaitUntilRead(_slots[slotIndices[index]].lastFenceValue);
    }
    part(2);
    int allocatorIndex = 0;
    ID3D12CommandAllocator* allocator = NextAllocator(&allocatorIndex);
    allocator->Reset();
    _list->Reset(allocator, nullptr);
    // Each unwrapped target is in the common state and is promoted to copy source by the copy itself (and decays back
    // at the end of the list); the shared textures are moved to copy destination and back explicitly, so that
    // Direct3D 11 always finds them in the common state.
    D3D12_RESOURCE_BARRIER barriers[SLOT_COUNT]{};
    for (int index = 0; index < count; ++index)
    {
        barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[index].Transition.pResource = _slots[slotIndices[index]].shared;
        barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[index].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barriers[index].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    _list->ResourceBarrier(static_cast<UINT>(count), barriers);
    for (int index = 0; index < count; ++index)
    {
        _list->CopyResource(_slots[slotIndices[index]].shared, unwrapped[index]);
        barriers[index].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[index].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    }
    _list->ResourceBarrier(static_cast<UINT>(count), barriers);
    HRESULT result = _list->Close();
    if (FAILED(result))
    {
        giveBack(count, 0);
        return Fail("command list", result);
    }
    ID3D12CommandList* lists[] = {_list};
    _queue->ExecuteCommandLists(1, lists);
    const UINT64 value = ++_fenceValue;
    _queue->Signal(_written, value);
    _allocatorFenceValues[allocatorIndex] = value;
    if (!hasConsumer)
    {
        _queue->Signal(_read, value);
    }
    part(3);
    giveBack(count, value);
    part(4);
    if (_hasFailed)
    {
        return false;
    }
    for (int index = 0; index < count; ++index)
    {
        Slot& slot = _slots[slotIndices[index]];
        slot.lastFenceValue = value;
        SharedImage& image = images[index];
        image.slot = slotIndices[index];
        image.texture = slot.sharedHandle;
        image.written = _writtenHandle;
        image.read = _readHandle;
        image.fenceValue = value;
        image.width = static_cast<int>(slot.width);
        image.height = static_cast<int>(slot.height);
        GetAdapter(&image.adapterLow, &image.adapterHigh);
    }
    if (slotIndices[0] < 2 && _eyeLogCount < 6)
    {
        ++_eyeLogCount;
        LogState(slotIndices[0] == 0 ? "left eye transferred" : "right eye transferred");
    }
    if (++_transferCount % PART_REPORT_TRANSFERS == 0)
    {
        Log::Info(
            "gpu",
            "ms per transfer (%d images): StretchRect %.3f, Unwrap %.3f, wait for the consumer %.3f, record and "
            "execute "
            "%.3f, Return %.3f",
            count,
            _partMilliseconds[0] / PART_REPORT_TRANSFERS,
            _partMilliseconds[1] / PART_REPORT_TRANSFERS,
            _partMilliseconds[2] / PART_REPORT_TRANSFERS,
            _partMilliseconds[3] / PART_REPORT_TRANSFERS,
            _partMilliseconds[4] / PART_REPORT_TRANSFERS);
        memset(_partMilliseconds, 0, sizeof(_partMilliseconds));
    }
    if (_transferCount == 1)
    {
        Log::Info(
            "gpu",
            "first transfer on the card (%d image(s) of %dx%d%s)",
            count,
            images[0].width,
            images[0].height,
            hasConsumer ? "" : ", no consumer: simulated headset");
    }
    return true;
}

void LogState(const char* reason)
{
    if (!_device || !_written || !_read)
    {
        return;
    }
    Log::Info(
        "gpu",
        "state (%s): last image %llu, written %llu, read %llu, transfers %u, device removed reason 0x%08lx, slots "
        "last %llu %llu %llu",
        reason,
        _fenceValue,
        _written->GetCompletedValue(),
        _read->GetCompletedValue(),
        _transferCount,
        static_cast<unsigned long>(_device->GetDeviceRemovedReason()),
        _slots[0].lastFenceValue,
        _slots[1].lastFenceValue,
        _slots[2].lastFenceValue);
}

void OnReset()
{
    for (Slot& slot : _slots)
    {
        Release(slot.targetSurface);
        Release(slot.target);
        slot.isStaged = false;
        slot.useCount = 0;
    }
}

} // namespace GpuTransfer

} // namespace SwbfVr
