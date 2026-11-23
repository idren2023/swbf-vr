// gpu-transfer-probe.cpp: desktop check of the GPU-side eye transfer (performance step 2) without the game.
// 1. Lists the instance extensions SteamVR's 32-bit OpenXR runtime offers (is XR_KHR_D3D12_enable among them?).
// 2. Creates a Direct3D 9 device through D3D9On12 on our own D3D12 device, clears its back buffer to a known colour,
//    copies it on the card into an A8R8G8B8 render target, unwraps that as a D3D12 resource, copies it into a shared
//    D3D12 texture, opens that in a D3D11 device with a shared fence, and checks the pixels there. Times each step and
//    the old way (GetRenderTargetData + LockRect) for comparison.
// Build: sh tools/gpu-transfer-probe.sh (32-bit, like the mod). Run: re/gpu-transfer-probe.exe
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3d9.h>
#include <dxgi1_4.h>

#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include <cstdio>
#include <string>
#include <vector>

// d3d9on12.h is not in llvm-mingw: the declarations from the Windows SDK (IID checked in SysWOW64\d3d9.dll).
struct D3D9ON12_ARGS
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
        IDirect3DResource9* resource, ID3D12CommandQueue* queue, REFIID riid, void** resource12) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReturnUnderlyingResource(
        IDirect3DResource9* resource, UINT syncCount, UINT64* signalValues, ID3D12Fence** fences) = 0;
};
const GUID IID_Device9On12 = {0xe7fda234, 0xb589, 0x4049, {0x94, 0x0d, 0x88, 0x78, 0x97, 0x75, 0x31, 0xc8}};
using Create9On12Function = IDirect3D9*(WINAPI*)(UINT, D3D9ON12_ARGS*, UINT);

double Now()
{
    static LARGE_INTEGER frequency{};
    if (!frequency.QuadPart)
    {
        QueryPerformanceFrequency(&frequency);
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return 1000.0 * static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart);
}

bool Fail(const char* what, HRESULT result)
{
    printf("FAILED: %s (0x%08lx)\n", what, static_cast<unsigned long>(result));
    return false;
}

// ---- 1. OpenXR extensions ----

std::string ReadFile(const std::string& path)
{
    std::string text;
    if (FILE* file = fopen(path.c_str(), "rb"))
    {
        char buffer[4096];
        size_t count;
        while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0)
        {
            text.append(buffer, count);
        }
        fclose(file);
    }
    return text;
}

void ProbeOpenXr()
{
    char path[MAX_PATH] = "";
    DWORD size = sizeof(path);
    if (RegGetValueA(
            HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Khronos\\OpenXR\\1",
            "ActiveRuntime",
            RRF_RT_REG_SZ | 0x00010000 /* RRF_SUBKEY_WOW6464KEY */,
            nullptr,
            path,
            &size) != ERROR_SUCCESS)
    {
        printf("openxr: no active runtime in the registry\n");
        return;
    }
    std::string manifest = path;
    manifest = manifest.substr(0, manifest.find_last_of("\\/") + 1) + "steamxr_win32.json";
    const std::string json = ReadFile(manifest);
    const size_t key = json.find("\"library_path\"");
    const size_t open = json.find('"', json.find(':', key) + 1), close = json.find('"', open + 1);
    if (key == std::string::npos || open == std::string::npos || close == std::string::npos)
    {
        printf("openxr: cannot read %s\n", manifest.c_str());
        return;
    }
    std::string library = json.substr(open + 1, close - open - 1);
    for (char& character : library)
    {
        character = character == '/' ? '\\' : character;
    }
    if (library.size() < 2 || library[1] != ':')
    {
        library = manifest.substr(0, manifest.find_last_of("\\/") + 1) + library;
    }
    HMODULE runtime = LoadLibraryExA(library.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    auto negotiate = runtime ? reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(reinterpret_cast<void*>(
                                   GetProcAddress(runtime, "xrNegotiateLoaderRuntimeInterface")))
                             : nullptr;
    if (!negotiate)
    {
        printf("openxr: cannot load %s (%lu)\n", library.c_str(), GetLastError());
        return;
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
    PFN_xrEnumerateInstanceExtensionProperties enumerate = nullptr;
    if (XR_FAILED(negotiate(&loader, &request)) || !request.getInstanceProcAddr ||
        XR_FAILED(request.getInstanceProcAddr(
            XR_NULL_HANDLE,
            "xrEnumerateInstanceExtensionProperties",
            reinterpret_cast<PFN_xrVoidFunction*>(&enumerate))))
    {
        printf("openxr: negotiation failed\n");
        return;
    }
    uint32_t count = 0;
    enumerate(nullptr, 0, &count, nullptr);
    std::vector<XrExtensionProperties> extensions(count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    enumerate(nullptr, count, &count, extensions.data());
    printf("openxr: %s offers %u extensions; graphics:", library.c_str(), count);
    for (const auto& extension : extensions)
    {
        if (strstr(extension.extensionName, "_enable") || strstr(extension.extensionName, "refresh"))
        {
            printf(" %s", extension.extensionName);
        }
    }
    printf("\n");
}

// ---- 2. D3D9On12 -> D3D12 -> shared texture -> D3D11 ----

int ProbeTransfer(bool isOn12, bool isSharedQueue = false)
{
    constexpr UINT WIDTH = 1920, HEIGHT = 1200;
    printf(
        "---- %s\n",
        !isOn12         ? "plain D3D9 (old path only)"
        : isSharedQueue ? "D3D9On12 on our queue, Return without fence"
                        : "D3D9On12 on its own queue, Return with fence");
    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapters1(0, &adapter)))
    {
        return Fail("DXGI adapter 0", E_FAIL);
    }
    DXGI_ADAPTER_DESC1 adapterDescription{};
    adapter->GetDesc1(&adapterDescription);
    printf("adapter 0: %ls\n", adapterDescription.Description);

    ID3D12Device* device12 = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence12 = nullptr;
    IDirect3D9* direct3d = nullptr;
    HRESULT result;
    if (isOn12)
    {
        if (FAILED(result = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12))))
        {
            return Fail("D3D12CreateDevice", result);
        }
        D3D12_COMMAND_QUEUE_DESC queueDescription{};
        queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(result = device12->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue))) ||
            FAILED(result = device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(
                result = device12->CreateCommandList(
                    0,
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    allocator,
                    nullptr,
                    IID_PPV_ARGS(&list))) ||
            FAILED(result = device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence12))))
        {
            return Fail("D3D12 queue, list or fence", result);
        }
        list->Close();
        auto create9On12 = reinterpret_cast<Create9On12Function>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("d3d9.dll"), "Direct3DCreate9On12")));
        D3D9ON12_ARGS arguments{};
        arguments.Enable9On12 = TRUE;
        arguments.pD3D12Device = device12;
        if (isSharedQueue)
        {
            arguments.ppD3D12Queues[0] = queue;
            arguments.NumQueues = 1;
        }
        direct3d = create9On12 ? create9On12(D3D_SDK_VERSION, &arguments, 1) : nullptr;
    }
    else
    {
        direct3d = Direct3DCreate9(D3D_SDK_VERSION);
    }
    if (!direct3d)
    {
        return Fail("Direct3DCreate9(On12)", E_FAIL);
    }
    HWND window = CreateWindowA("STATIC", "probe", WS_POPUP, 0, 0, WIDTH, HEIGHT, nullptr, nullptr, nullptr, nullptr);
    D3DPRESENT_PARAMETERS parameters{};
    parameters.BackBufferWidth = WIDTH;
    parameters.BackBufferHeight = HEIGHT;
    parameters.BackBufferFormat = D3DFMT_X8R8G8B8;
    parameters.BackBufferCount = 1;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = window;
    parameters.Windowed = TRUE;
    parameters.EnableAutoDepthStencil = TRUE;
    parameters.AutoDepthStencilFormat = D3DFMT_D24S8;
    IDirect3DDevice9* device9 = nullptr;
    if (FAILED(
            result = direct3d->CreateDevice(
                D3DADAPTER_DEFAULT,
                D3DDEVTYPE_HAL,
                window,
                D3DCREATE_HARDWARE_VERTEXPROCESSING,
                &parameters,
                &device9)))
    {
        return Fail("CreateDevice", result);
    }
    IDirect3DDevice9On12* device9On12 = nullptr;
    if (isOn12 && FAILED(result = device9->QueryInterface(IID_Device9On12, reinterpret_cast<void**>(&device9On12))))
    {
        return Fail("QueryInterface IDirect3DDevice9On12", result);
    }
    IDirect3DTexture9* eyeTexture = nullptr;
    IDirect3DSurface9 *eyeSurface = nullptr, *backBuffer = nullptr, *systemCopy = nullptr;
    if (FAILED(
            result = device9->CreateTexture(
                WIDTH,
                HEIGHT,
                1,
                D3DUSAGE_RENDERTARGET,
                D3DFMT_A8R8G8B8,
                D3DPOOL_DEFAULT,
                &eyeTexture,
                nullptr)) ||
        FAILED(result = eyeTexture->GetSurfaceLevel(0, &eyeSurface)) ||
        FAILED(result = device9->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) ||
        FAILED(
            result = device9->CreateOffscreenPlainSurface(
                WIDTH,
                HEIGHT,
                D3DFMT_A8R8G8B8,
                D3DPOOL_SYSTEMMEM,
                &systemCopy,
                nullptr)))
    {
        return Fail("D3D9 surfaces", result);
    }

    // The old way, for comparison: read back to system memory.
    double readback = 0;
    int readbackErrors = 0;
    for (int frame = 0; frame < 60; ++frame)
    {
        const D3DCOLOR colour = D3DCOLOR_XRGB(frame * 7 & 255, frame * 13 & 255, frame * 29 & 255);
        device9->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, colour, 1.0f, 0);
        device9->StretchRect(backBuffer, nullptr, eyeSurface, nullptr, D3DTEXF_NONE);
        const double start = Now();
        D3DLOCKED_RECT lock{};
        if (FAILED(device9->GetRenderTargetData(eyeSurface, systemCopy)) ||
            FAILED(systemCopy->LockRect(&lock, nullptr, D3DLOCK_READONLY)))
        {
            return Fail("read-back", E_FAIL);
        }
        readback += Now() - start;
        const DWORD pixel = static_cast<const DWORD*>(lock.pBits)[WIDTH * (HEIGHT / 2) + WIDTH / 2] & 0xFFFFFF;
        readbackErrors += pixel != (colour & 0xFFFFFF);
        systemCopy->UnlockRect();
        device9->Present(nullptr, nullptr, nullptr, nullptr);
    }
    printf("old path: read-back %.2f ms per eye, %d wrong pixels in 60 frames\n", readback / 60, readbackErrors);
    if (!isOn12)
    {
        return 0;
    }

    // The new way. Unwrap once to learn the resource's description, then the shared texture with the same layout.
    ID3D12Resource* unwrapped = nullptr;
    if (FAILED(result = device9On12->UnwrapUnderlyingResource(eyeTexture, queue, IID_PPV_ARGS(&unwrapped))))
    {
        return Fail("UnwrapUnderlyingResource", result);
    }
    D3D12_RESOURCE_DESC description = unwrapped->GetDesc();
    printf(
        "unwrapped: %llux%u, format %d, flags 0x%x, mips %u\n",
        static_cast<unsigned long long>(description.Width),
        description.Height,
        static_cast<int>(description.Format),
        static_cast<unsigned>(description.Flags),
        description.MipLevels);
    UINT64 fenceValue = 0;
    {
        ++fenceValue;
        queue->Signal(fence12, fenceValue);
        if (FAILED(result = device9On12->ReturnUnderlyingResource(eyeTexture, 1, &fenceValue, &fence12)))
        {
            return Fail("ReturnUnderlyingResource", result);
        }
        unwrapped->Release();
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC sharedDescription = description;
    sharedDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    sharedDescription.Alignment = 0;
    ID3D12Resource* shared12 = nullptr;
    HANDLE sharedHandle = nullptr, fenceHandle = nullptr;
    if (FAILED(
            result = device12->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_SHARED,
                &sharedDescription,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                IID_PPV_ARGS(&shared12))) ||
        FAILED(result = device12->CreateSharedHandle(shared12, nullptr, GENERIC_ALL, nullptr, &sharedHandle)) ||
        FAILED(result = device12->CreateSharedHandle(fence12, nullptr, GENERIC_ALL, nullptr, &fenceHandle)))
    {
        return Fail("shared D3D12 texture or fence handle", result);
    }

    ID3D11Device* device11 = nullptr;
    ID3D11DeviceContext* context11 = nullptr;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(
            result = D3D11CreateDevice(
                adapter,
                D3D_DRIVER_TYPE_UNKNOWN,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                &level,
                1,
                D3D11_SDK_VERSION,
                &device11,
                nullptr,
                &context11)))
    {
        return Fail("D3D11CreateDevice", result);
    }
    ID3D11Device5* device11_5 = nullptr;
    ID3D11DeviceContext4* context11_4 = nullptr;
    ID3D11Texture2D *shared11 = nullptr, *staging11 = nullptr;
    ID3D11Fence* fence11 = nullptr;
    if (FAILED(result = device11->QueryInterface(IID_PPV_ARGS(&device11_5))) ||
        FAILED(result = context11->QueryInterface(IID_PPV_ARGS(&context11_4))) ||
        FAILED(result = device11_5->OpenSharedResource1(sharedHandle, IID_PPV_ARGS(&shared11))) ||
        FAILED(result = device11_5->OpenSharedFence(fenceHandle, IID_PPV_ARGS(&fence11))))
    {
        return Fail("D3D11 side (Device5, OpenSharedResource1, OpenSharedFence)", result);
    }
    D3D11_TEXTURE2D_DESC description11{};
    shared11->GetDesc(&description11);
    printf("opened in D3D11: %ux%u format %d\n", description11.Width, description11.Height, description11.Format);
    D3D11_TEXTURE2D_DESC stagingDescription = description11;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.BindFlags = 0;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDescription.MiscFlags = 0;
    if (FAILED(result = device11->CreateTexture2D(&stagingDescription, nullptr, &staging11)))
    {
        return Fail("staging texture", result);
    }

    double unwrapTime = 0, copyTime = 0, returnTime = 0, waitTime = 0;
    int wrongPixels = 0;
    constexpr int FRAMES = 20000;
    double windowUnwrap = 0, windowReturn = 0;
    for (int frame = 0; frame < FRAMES; ++frame)
    {
        const D3DCOLOR colour = D3DCOLOR_XRGB(frame * 7 & 255, frame * 13 & 255, frame * 29 & 255);
        device9->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, colour, 1.0f, 0);
        device9->StretchRect(backBuffer, nullptr, eyeSurface, nullptr, D3DTEXF_NONE);
        double start = Now();
        if (FAILED(result = device9On12->UnwrapUnderlyingResource(eyeTexture, queue, IID_PPV_ARGS(&unwrapped))))
        {
            return Fail("UnwrapUnderlyingResource (loop)", result);
        }
        unwrapTime += Now() - start;
        start = Now();
        allocator->Reset();
        list->Reset(allocator, nullptr);
        D3D12_RESOURCE_BARRIER barriers[2]{};
        for (D3D12_RESOURCE_BARRIER& barrier : barriers)
        {
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        barriers[0].Transition.pResource = shared12;
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &barriers[0]);
        list->CopyResource(shared12, unwrapped);
        barriers[1].Transition.pResource = shared12;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        list->ResourceBarrier(1, &barriers[1]);
        list->Close();
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        ++fenceValue;
        queue->Signal(fence12, fenceValue);
        copyTime += Now() - start;
        start = Now();
        result = isSharedQueue ? device9On12->ReturnUnderlyingResource(eyeTexture, 0, nullptr, nullptr)
                               : device9On12->ReturnUnderlyingResource(eyeTexture, 1, &fenceValue, &fence12);
        if (FAILED(result))
        {
            return Fail("ReturnUnderlyingResource (loop)", result);
        }
        unwrapped->Release();
        returnTime += Now() - start;
        windowReturn += Now() - start;
        if ((frame + 1) % 4000 == 0)
        {
            printf("  frames %d-%d: unwrap %.3f ms, return %.3f ms\n", frame - 3999, frame, (unwrapTime - windowUnwrap) / 4000, windowReturn / 4000);
            windowUnwrap = unwrapTime;
            windowReturn = 0;
            if (!isSharedQueue && frame + 1 == 8000)
            { // does a new render target start with a short list again?
                eyeSurface->Release();
                eyeTexture->Release();
                if (FAILED(device9->CreateTexture(
                        WIDTH,
                        HEIGHT,
                        1,
                        D3DUSAGE_RENDERTARGET,
                        D3DFMT_A8R8G8B8,
                        D3DPOOL_DEFAULT,
                        &eyeTexture,
                        nullptr)) ||
                    FAILED(eyeTexture->GetSurfaceLevel(0, &eyeSurface)))
                {
                    return Fail("new render target", E_FAIL);
                }
                printf("  (render target made again)\n");
            }
        }
        start = Now();
        context11_4->Wait(fence11, fenceValue);
        context11->CopyResource(staging11, shared11);
        context11->Flush();
        waitTime += Now() - start;
        // Check (not part of the real path: the swapchain image would be the copy's target, no Map).
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context11->Map(staging11, 0, D3D11_MAP_READ, 0, &mapped)))
        {
            const DWORD pixel = *reinterpret_cast<const DWORD*>(
                                    static_cast<const BYTE*>(mapped.pData) + mapped.RowPitch * (HEIGHT / 2) +
                                    (WIDTH / 2) * 4) &
                                0xFFFFFF;
            if (pixel != (colour & 0xFFFFFF) && wrongPixels++ < 3)
            {
                printf("frame %d: pixel 0x%06lx, expected 0x%06lx\n", frame, pixel, colour & 0xFFFFFF);
            }
            context11->Unmap(staging11, 0);
        }
        device9->Present(nullptr, nullptr, nullptr, nullptr);
    }
    printf(
        "new path per eye: unwrap %.3f ms, D3D12 copy submit %.3f ms, return %.3f ms, D3D11 wait+copy submit %.3f "
        "ms; %d wrong pixels in %d frames\n",
        unwrapTime / FRAMES,
        copyTime / FRAMES,
        returnTime / FRAMES,
        waitTime / FRAMES,
        wrongPixels,
        FRAMES);
    return 0;
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    ProbeOpenXr();
    ProbeTransfer(true, false);
    ProbeTransfer(true, true);
    return 0;
}
