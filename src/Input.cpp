// Input.cpp: input injection inside DirectInput. The game reads keyboard and mouse through DirectInput 8, and this
// DLL is the dinput8 proxy, so the device read calls are hooked and synthetic input is merged into what the game
// receives. Used by the test scripts and by the controller mapping (Controls).
// While a script is active the real devices are ignored, so runs are repeatable and need no window focus.
//
// DirectInput is a COM interface: each object starts with a pointer to its table of method pointers (vtable). With
// CINTERFACE, dinput.h declares those tables as plain C structs (IDirectInputDevice8AVtbl with one field per method),
// so the methods can be read as addresses and hooked with MinHook. Only the first device's methods are hooked; the
// keyboard and mouse share one implementation, so that covers both (checked and logged for each later device).

// Both have to be macros: dinput.h reads them with #if while it is being included.
#define CINTERFACE                 // C-style vtable structs instead of C++ classes (see above)
#define DIRECTINPUT_VERSION 0x0800 // NOLINT(cppcoreguidelines-macro-usage): the DirectInput 8 declarations

#include "Input.h"

#include "Keyboard.h"

#include "Log.h"

#include <windows.h>
#include <dinput.h>

#include <MinHook.h>

#include <cstring>

namespace SwbfVr
{

namespace Input
{

namespace
{

/// GUID_SysMouse and GUID_SysKeyboard from dinput.h (written out so dxguid.lib is not needed): the system mouse and
/// keyboard the game asks CreateDevice for.
const GUID SYSTEM_MOUSE_GUID = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID SYSTEM_KEYBOARD_GUID = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

/// Size of a keyboard state (one byte per DIK code, top bit = down).
constexpr DWORD KEYBOARD_STATE_BYTES = 256;

/// The byte value of a pressed key or button in DirectInput state and event data.
constexpr BYTE PRESSED = 0x80;

/// Smallest buffered event entry we fill: DIDEVICEOBJECTDATA's offset, data, time stamp and sequence number.
constexpr DWORD MINIMUM_ENTRY_BYTES = 16;

/// How many devices are tracked, and how many events each queue keeps.
constexpr unsigned MAXIMUM_DEVICE_COUNT = 16;
constexpr unsigned QUEUE_CAPACITY = 1024;

enum class DeviceKind
{
    Unknown,
    Mouse,
    Keyboard,
    Other
};

/// @return the kind's name for the log.
const char* DeviceKindName(DeviceKind kind)
{
    return kind == DeviceKind::Mouse      ? "mouse"
           : kind == DeviceKind::Keyboard ? "keyboard"
           : kind == DeviceKind::Other    ? "other"
                                          : "unknown";
}

/// A DirectInput device the game created or read, with its read counts for the log.
struct Device
{
    void* device;
    DeviceKind kind;
    unsigned stateReadCount, dataReadCount;
};

Device _devices[MAXIMUM_DEVICE_COUNT];
unsigned _deviceCount = 0;
/// Guards _devices, the synthetic key and button state and the event queues (a slim reader/writer lock: shared for
/// reading, exclusive for writing).
SRWLOCK _lock = SRWLOCK_INIT;

/// Looks up a device, optionally adding it. Call with _lock held (shared is enough without adding).
/// @param device the IDirectInputDevice8 object.
/// @param shouldAdd add it when it is not in the list yet (and there is room).
/// @return its entry, or null.
Device* FindDevice(void* device, bool shouldAdd)
{
    for (unsigned index = 0; index < _deviceCount; ++index)
    {
        if (_devices[index].device == device)
        {
            return &_devices[index];
        }
    }
    if (!shouldAdd || _deviceCount >= MAXIMUM_DEVICE_COUNT)
    {
        return nullptr;
    }
    _devices[_deviceCount] = {device, DeviceKind::Unknown, 0, 0};
    return &_devices[_deviceCount++];
}

// ---- synthetic input state ----
/// 1 while a script runs (the real devices are ignored). volatile LONG with Interlocked writes: read and written from
/// several threads without the lock.
volatile LONG _isActive = 0;
volatile LONG _pendingX = 0, _pendingY = 0, _pendingWheel = 0; // pending relative motion for immediate-mode reads
BYTE _mouseButtons[8];
BYTE _keys[KEYBOARD_STATE_BYTES];

// Pending events for buffered-mode reads (GetDeviceData).
struct Event
{
    DWORD offset, data;
};

/// A ring buffer of events; when full, the oldest is dropped.
struct Queue
{
    Event items[QUEUE_CAPACITY];
    unsigned head = 0, count = 0;

    void Push(DWORD offset, DWORD data)
    {
        if (count == QUEUE_CAPACITY)
        {
            head = (head + 1) % QUEUE_CAPACITY;
            --count;
        } // drop the oldest
        items[(head + count++) % QUEUE_CAPACITY] = {offset, data};
    }

    bool Pop(Event& event)
    {
        if (!count)
        {
            return false;
        }
        event = items[head];
        head = (head + 1) % QUEUE_CAPACITY;
        --count;
        return true;
    }
};

Queue _mouseQueue, _keyQueue;
/// Sequence numbers for injected events, started high (inferred to stay clear of the real devices' numbers).
DWORD _sequenceNumber = 0x10000;

// The original methods, called by the hooks. decltype(...) takes the type of the vtable field, a function pointer
// with the method's exact signature.
decltype(IDirectInput8AVtbl::CreateDevice) _originalCreateDevice = nullptr;
decltype(IDirectInputDevice8AVtbl::GetDeviceState) _originalGetDeviceState = nullptr;
decltype(IDirectInputDevice8AVtbl::GetDeviceData) _originalGetDeviceData = nullptr;
decltype(IDirectInputDevice8AVtbl::SetDataFormat) _originalSetDataFormat = nullptr;
decltype(IDirectInputDevice8AVtbl::SetCooperativeLevel) _originalSetCooperativeLevel = nullptr;
bool _areDeviceMethodsHooked = false;
/// A copy of the first device's vtable, to tell whether a later device uses the same (hooked) methods.
IDirectInputDevice8AVtbl _firstDeviceVtable;

/// The device's kind: as recorded, or else guessed from the size of the state the game reads.
/// @param device the IDirectInputDevice8 object.
/// @param stateSize bytes the game reads with GetDeviceState; 0 when unknown.
DeviceKind FindDeviceKind(void* device, DWORD stateSize)
{
    AcquireSRWLockShared(&_lock);
    Device* entry = FindDevice(device, false);
    DeviceKind kind = entry ? entry->kind : DeviceKind::Unknown;
    ReleaseSRWLockShared(&_lock);
    if (kind == DeviceKind::Unknown)
    {
        kind = stateSize == KEYBOARD_STATE_BYTES                                           ? DeviceKind::Keyboard
               : (stateSize == sizeof(DIMOUSESTATE) || stateSize == sizeof(DIMOUSESTATE2)) ? DeviceKind::Mouse
                                                                                           : DeviceKind::Unknown;
    }
    return kind;
}

/// Hook of IDirectInputDevice8::GetDeviceState (immediate mode: the device's whole current state), called by the game
/// on its main thread; WINAPI is the stdcall convention of COM methods, and the object comes first. Adds the pending
/// mouse motion and ORs in held buttons and keys; while a script runs the real state is replaced by zeros first.
HRESULT WINAPI GetDeviceStateHook(IDirectInputDevice8A* device, DWORD size, LPVOID data)
{
    HRESULT result = _originalGetDeviceState(device, size, data);
    const DeviceKind kind = FindDeviceKind(device, size);
    AcquireSRWLockExclusive(&_lock);
    if (Device* entry = FindDevice(device, true))
    {
        if (entry->kind == DeviceKind::Unknown)
        {
            entry->kind = kind;
        }
        if (!entry->stateReadCount++)
        {
            Log::Info(
                "input",
                "%s device %p read with GetDeviceState, %lu bytes (first call -> 0x%lx)",
                DeviceKindName(kind),
                static_cast<void*>(device),
                size,
                result);
        }
    }
    ReleaseSRWLockExclusive(&_lock);
    if (!data || (kind != DeviceKind::Mouse && kind != DeviceKind::Keyboard))
    {
        return result;
    }
    if (_isActive)
    {
        // Ignore the real device, and keep the game reading even if it lost the device (no focus).
        memset(data, 0, size);
        result = DI_OK;
    }
    if (FAILED(result))
    {
        return result;
    }
    if (kind == DeviceKind::Mouse && size >= sizeof(DIMOUSESTATE))
    {
        DIMOUSESTATE* mouse = static_cast<DIMOUSESTATE*>(data);
        mouse->lX += InterlockedExchange(&_pendingX, 0);
        mouse->lY += InterlockedExchange(&_pendingY, 0);
        mouse->lZ += InterlockedExchange(&_pendingWheel, 0);
        const unsigned buttonCount = size >= sizeof(DIMOUSESTATE2) ? 8 : 4; // DIMOUSESTATE2 has 8 buttons
        for (unsigned button = 0; button < buttonCount; ++button)
        {
            mouse->rgbButtons[button] |= _mouseButtons[button];
        }
    }
    else if (kind == DeviceKind::Keyboard && size == KEYBOARD_STATE_BYTES)
    {
        BYTE* keys = static_cast<BYTE*>(data);
        for (unsigned key = 0; key < KEYBOARD_STATE_BYTES; ++key)
        {
            keys[key] |= _keys[key];
        }
    }
    return result;
}

/// Hook of IDirectInputDevice8::GetDeviceData (buffered mode: the events since the last read), called by the game on
/// its main thread. Appends the queued synthetic events after the real ones (or instead of them while a script runs),
/// as long as the game's buffer has room. With DIGDD_PEEK the queue is left alone.
HRESULT WINAPI GetDeviceDataHook(
    IDirectInputDevice8A* device,
    DWORD entrySize,
    LPDIDEVICEOBJECTDATA entries,
    LPDWORD inOut,
    DWORD flags)
{
    const DWORD capacity = inOut ? *inOut : 0;
    HRESULT result = _originalGetDeviceData(device, entrySize, entries, inOut, flags);
    const DeviceKind kind = FindDeviceKind(device, 0);
    AcquireSRWLockExclusive(&_lock);
    if (Device* entry = FindDevice(device, true))
    {
        if (!entry->dataReadCount++)
        {
            Log::Info(
                "input",
                "%s device %p read with GetDeviceData, entry %lu bytes, room for %lu (first call -> 0x%lx)",
                DeviceKindName(kind),
                static_cast<void*>(device),
                entrySize,
                capacity,
                result);
        }
    }
    Queue* queue = kind == DeviceKind::Mouse ? &_mouseQueue : kind == DeviceKind::Keyboard ? &_keyQueue : nullptr;
    if (queue && inOut && entrySize >= MINIMUM_ENTRY_BYTES)
    {
        if (_isActive)
        {
            *inOut = 0;
            result = DI_OK;
        } // drop the real events
        if (SUCCEEDED(result) && entries && !(flags & DIGDD_PEEK))
        {
            DWORD entryCount = *inOut;
            Event event;
            while (entryCount < capacity && queue->Pop(event))
            {
                // The game's entry size may be larger than DIDEVICEOBJECTDATA, so the entry is found by byte
                // arithmetic and filled as four DWORDs: offset, data, time stamp, sequence number.
                DWORD* out = reinterpret_cast<DWORD*>(reinterpret_cast<BYTE*>(entries) + entryCount * entrySize);
                memset(out, 0, entrySize);
                out[0] = event.offset;
                out[1] = event.data;
                out[2] = GetTickCount();
                out[3] = ++_sequenceNumber;
                ++entryCount;
            }
            *inOut = entryCount;
        }
    }
    ReleaseSRWLockExclusive(&_lock);
    return result;
}

/// Hook of IDirectInputDevice8::SetDataFormat: records the device's kind from the size of its state and logs the
/// format.
HRESULT WINAPI SetDataFormatHook(IDirectInputDevice8A* device, LPCDIDATAFORMAT format)
{
    const HRESULT result = _originalSetDataFormat(device, format);
    if (format)
    {
        AcquireSRWLockExclusive(&_lock);
        Device* entry = FindDevice(device, true);
        if (entry && entry->kind == DeviceKind::Unknown)
        {
            entry->kind = format->dwDataSize == KEYBOARD_STATE_BYTES ? DeviceKind::Keyboard
                          : (format->dwDataSize == sizeof(DIMOUSESTATE) || format->dwDataSize == sizeof(DIMOUSESTATE2))
                              ? DeviceKind::Mouse
                              : DeviceKind::Other;
        }
        Log::Info(
            "input",
            "device %p (%s) data format: %lu bytes, %lu objects, flags 0x%lx",
            static_cast<void*>(device),
            entry ? DeviceKindName(entry->kind) : "?",
            format->dwDataSize,
            format->dwNumObjs,
            format->dwFlags);
        ReleaseSRWLockExclusive(&_lock);
    }
    return result;
}

/// Hook of IDirectInputDevice8::SetCooperativeLevel: only logs it (exclusive or not, foreground or background).
HRESULT WINAPI SetCooperativeLevelHook(IDirectInputDevice8A* device, HWND window, DWORD flags)
{
    const HRESULT result = _originalSetCooperativeLevel(device, window, flags);
    Log::Info(
        "input",
        "device %p cooperative level 0x%lx (%s, %s) -> 0x%lx",
        static_cast<void*>(device),
        flags,
        (flags & DISCL_EXCLUSIVE) ? "exclusive" : "non-exclusive",
        (flags & DISCL_BACKGROUND) ? "background" : "foreground",
        result);
    return result;
}

/// Creates a MinHook hook (enabled later with MH_EnableHook). A template so it takes any function pointer type and
/// keeps target, detour and original the same type.
/// @param target the method to hook.
/// @param detour our replacement.
/// @param original receives the trampoline that calls the original method; null on failure.
/// @param name the method's name for the log.
/// @return true if the hook was created.
template <class Function> bool CreateHook(Function target, Function detour, Function* original, const char* name)
{
    const MH_STATUS status = MH_CreateHook(
        reinterpret_cast<void*>(target),
        reinterpret_cast<void*>(detour),
        reinterpret_cast<void**>(original));
    if (status != MH_OK)
    {
        Log::Error("input", "hook %s failed: %s", name, MH_StatusToString(status));
        *original = nullptr;
        return false;
    }
    return true;
}

/// Hooks the read and setup methods of the first device the game creates; for later devices only checks that they
/// share the hooked methods.
void HookDeviceMethods(IDirectInputDevice8A* device)
{
    IDirectInputDevice8AVtbl* vtable = device->lpVtbl;
    if (_areDeviceMethodsHooked)
    {
        if (vtable->GetDeviceState != _firstDeviceVtable.GetDeviceState ||
            vtable->GetDeviceData != _firstDeviceVtable.GetDeviceData)
        {
            Log::Info(
                "input",
                "device %p uses different read functions than the hooked ones (not covered)",
                static_cast<void*>(device));
        }
        return;
    }
    _areDeviceMethodsHooked = true;
    _firstDeviceVtable = *vtable;
    unsigned hookedCount = 0;
    hookedCount += CreateHook(vtable->GetDeviceState, &GetDeviceStateHook, &_originalGetDeviceState, "GetDeviceState");
    hookedCount += CreateHook(vtable->GetDeviceData, &GetDeviceDataHook, &_originalGetDeviceData, "GetDeviceData");
    hookedCount += CreateHook(vtable->SetDataFormat, &SetDataFormatHook, &_originalSetDataFormat, "SetDataFormat");
    hookedCount += CreateHook(
        vtable->SetCooperativeLevel,
        &SetCooperativeLevelHook,
        &_originalSetCooperativeLevel,
        "SetCooperativeLevel");
    const MH_STATUS enableStatus = MH_EnableHook(MH_ALL_HOOKS);
    Log::Info("input", "%u/4 device methods hooked, enable: %s", hookedCount, MH_StatusToString(enableStatus));
}

/// Hook of IDirectInput8::CreateDevice: records the new device's kind from the GUID the game asked for (system mouse
/// or keyboard) and hooks the device methods.
HRESULT WINAPI CreateDeviceHook(IDirectInput8A* self, REFGUID guid, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN outer)
{
    const HRESULT result = _originalCreateDevice(self, guid, out, outer);
    if (SUCCEEDED(result) && out && *out)
    {
        const DeviceKind kind = memcmp(&guid, &SYSTEM_MOUSE_GUID, sizeof(GUID)) == 0      ? DeviceKind::Mouse
                                : memcmp(&guid, &SYSTEM_KEYBOARD_GUID, sizeof(GUID)) == 0 ? DeviceKind::Keyboard
                                                                                          : DeviceKind::Unknown;
        AcquireSRWLockExclusive(&_lock);
        if (Device* entry = FindDevice(*out, true))
        {
            entry->kind = kind;
            entry->stateReadCount = entry->dataReadCount = 0;
        }
        ReleaseSRWLockExclusive(&_lock);
        Log::Info(
            "input",
            "CreateDevice {%08lX-...} -> %p (%s)",
            guid.Data1,
            static_cast<void*>(*out),
            DeviceKindName(kind));
        HookDeviceMethods(*out);
    }
    return result;
}

} // namespace

// Called with the IDirectInput8 object the real dinput8 handed to the game.
void OnDirectInputCreated(void* directInput)
{
    if (_originalCreateDevice || !directInput)
    {
        return;
    }
    const MH_STATUS initializeStatus = MH_Initialize();
    if (initializeStatus != MH_OK && initializeStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        Log::Error("input", "MinHook init failed: %s", MH_StatusToString(initializeStatus));
        return;
    }
    IDirectInput8A* directInput8 = static_cast<IDirectInput8A*>(directInput);
    if (CreateHook(
            directInput8->lpVtbl->CreateDevice,
            &CreateDeviceHook,
            &_originalCreateDevice,
            "IDirectInput8::CreateDevice"))
    {
        const MH_STATUS enableStatus = MH_EnableHook(MH_ALL_HOOKS);
        Log::Info("input", "IDirectInput8 %p CreateDevice hooked: %s", directInput, MH_StatusToString(enableStatus));
    }
}

bool IsReady()
{
    return _originalGetDeviceState != nullptr || _originalGetDeviceData != nullptr;
}

void SetActive(bool isActive)
{
    InterlockedExchange(&_isActive, isActive ? 1 : 0);
    if (!isActive)
    {
        AcquireSRWLockExclusive(&_lock);
        memset(_keys, 0, sizeof(_keys));
        memset(_mouseButtons, 0, sizeof(_mouseButtons));
        _mouseQueue = Queue();
        _keyQueue = Queue();
        ReleaseSRWLockExclusive(&_lock);
        InterlockedExchange(&_pendingX, 0);
        InterlockedExchange(&_pendingY, 0);
        InterlockedExchange(&_pendingWheel, 0);
    }
}

void Key(unsigned keyCode, bool isDown)
{
    if (keyCode >= KEYBOARD_STATE_BYTES)
    {
        return;
    }
    AcquireSRWLockExclusive(&_lock);
    _keys[keyCode] = isDown ? PRESSED : 0;
    _keyQueue.Push(keyCode, isDown ? PRESSED : 0);
    ReleaseSRWLockExclusive(&_lock);
}

void MouseMove(long countsX, long countsY)
{
    InterlockedExchangeAdd(&_pendingX, countsX);
    InterlockedExchangeAdd(&_pendingY, countsY);
    AcquireSRWLockExclusive(&_lock);
    if (countsX)
    {
        _mouseQueue.Push(DIMOFS_X, static_cast<DWORD>(countsX));
    }
    if (countsY)
    {
        _mouseQueue.Push(DIMOFS_Y, static_cast<DWORD>(countsY));
    }
    ReleaseSRWLockExclusive(&_lock);
}

void MouseButton(unsigned buttonIndex, bool isDown)
{
    if (buttonIndex >= 8)
    {
        return;
    }
    if (buttonIndex == 0 && Keyboard::OnMouseButton(isDown))
    {
        return; // a click on the virtual keyboard: not for the menu behind it
    }
    AcquireSRWLockExclusive(&_lock);
    _mouseButtons[buttonIndex] = isDown ? PRESSED : 0;
    _mouseQueue.Push(DIMOFS_BUTTON0 + buttonIndex, isDown ? PRESSED : 0);
    ReleaseSRWLockExclusive(&_lock);
}

void MouseWheel(long delta)
{
    InterlockedExchangeAdd(&_pendingWheel, delta);
    AcquireSRWLockExclusive(&_lock);
    _mouseQueue.Push(DIMOFS_Z, static_cast<DWORD>(delta));
    ReleaseSRWLockExclusive(&_lock);
}

void LogStats()
{
    AcquireSRWLockShared(&_lock);
    for (unsigned index = 0; index < _deviceCount; ++index)
    {
        Log::Info(
            "input",
            "device %p %s: %u GetDeviceState calls, %u GetDeviceData calls",
            _devices[index].device,
            DeviceKindName(_devices[index].kind),
            _devices[index].stateReadCount,
            _devices[index].dataReadCount);
    }
    ReleaseSRWLockShared(&_lock);
}

} // namespace Input

} // namespace SwbfVr
