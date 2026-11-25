#pragma once

/// GpuTransfer: the eye images go from the game's Direct3D 9 to the headset on the graphics card (GpuTransfer.cpp,
/// b268), instead of through system memory. The game's Direct3D 9 runs on D3D9On12 over a D3D12 device of ours; each
/// image is copied on the card into a shared texture that Xr's Direct3D 11 device opens. Render thread only.

#include <d3d9.h>

namespace SwbfVr
{

namespace GpuTransfer
{

/// An image copied on the graphics card into a shared texture (B8G8R8A8, the back buffer's size). The consumer (Xr)
/// waits on the card for `written` to reach `fenceValue` before reading the texture, and signals `read` with the same
/// value when done (also when it did not read it), so the slot may be written again.
struct SharedImage
{
    int slot;      // 0 left eye, 1 right eye, 2 flat screen
    void* texture; // shared NT handle of the D3D12 texture
    void* written; // shared NT handle of the fence that says the texture is written; nullptr on 9Ex (texture is then a
                   // legacy share handle, opened with OpenSharedResource, and there are no fences)
    void* read;    // shared NT handle of the fence that says the consumer is done with it (nullptr on 9Ex)
    unsigned long long fenceValue;
    int width, height;
    unsigned long adapterLow; // LUID of the adapter the texture lives on
    long adapterHigh;
};

/// Called by the Direct3DCreate9 hook. When the transfer is wanted (swbf-vr.ini `gpu_transfer`: 0 off, 1 when SteamVR
/// is running at game start (default), 2 always), creates our D3D12 device and the game's IDirect3D9 through
/// Direct3DCreate9On12 on it.
/// @param sdkVersion the game's argument to Direct3DCreate9.
/// @return the new IDirect3D9, or nullptr: then the caller makes the usual one.
IDirect3D9* CreateDirect3D(UINT sdkVersion);

/// The game runs on Direct3D 9Ex (D3dSurvey, ini `shared_transfer`, M2 route 1, b674): then each image is copied by
/// Direct3D 9Ex itself into a render target made with a shared handle (a legacy share handle, no fences), which Xr's
/// Direct3D 11 device opens with OpenSharedResource. Ordering between the two devices is left to the graphics kernel's
/// implicit synchronisation of shared allocations; Transfer flushes Direct3D 9Ex so the copy is on its way.
/// @param direct3dEx the game's IDirect3D9Ex (for the adapter LUID).
void UseDirect3D9Ex(void* direct3dEx);

/// @return true while images can be copied on the card (D3D9On12 on our device, or 9Ex; no failure so far).
bool IsActive();

/// LUID of the adapter our D3D12 device is on (valid while IsActive).
void GetAdapter(unsigned long* low, long* high);

/// First half of a transfer: copies `source` on the card into our render target of `slot` (Direct3D 9, no flush).
/// @param device the game's device (D3D9On12).
/// @param source the image (the back buffer), any size, A8R8G8B8 or X8R8G8B8.
/// @param slot 0 left eye, 1 right eye, 2 flat screen.
/// @return false when it could not be made (then the transfer stays off, logged once, and the caller uses the
///         system-memory path).
bool Stage(IDirect3DDevice9* device, IDirect3DSurface9* source, int slot);

/// Second half: the staged slots' targets go into their shared textures, in one command list on our queue (one flush
/// of the game's pending work for all of them, no CPU wait for the card).
/// @param slots the staged slots.
/// @param count how many (1 to 3).
/// @param hasConsumer false when no one will read the images (simulated headset): the read fence is signalled here.
/// @param images set to the shared images (one per slot, same order) when the result is true.
/// @return false when the transfer failed (then it stays off, logged once).
bool Transfer(const int* slots, int count, bool hasConsumer, SharedImage* images);

/// Logs where the fences stand (which image our queue has written, which the consumer has read, whether the D3D12
/// device was removed). Safe from any thread; called by the hang watchdog (Crash.cpp) and for the first eye images.
void LogState(const char* reason);

/// Device reset: releases our render targets in the default pool (made again at the next copy).
void OnReset();

} // namespace GpuTransfer

} // namespace SwbfVr
