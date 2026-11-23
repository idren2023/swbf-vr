// Function probes (diagnostic): counts calls to a game function and records the call stack of its first calls.
// A probe is an inline hook whose detour is a small generated stub: it saves all registers, reports
// (id, this, stack pointer) to OnProbeHit, restores everything and continues in the original function.
// Script commands: "probe <rva hex> [name]", "probes" (write the report).

#include "Probe.h"

#include "D3dSurvey.h"
#include "FpGuard.h"
#include "Log.h"

#include <windows.h>

#include <MinHook.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace SwbfVr
{

namespace Probe
{

namespace
{

/// How many different `this` values a probe keeps, and how many call stacks it logs.
constexpr unsigned KEPT_THIS_VALUE_COUNT = 4;
constexpr unsigned LOGGED_STACKS_PER_PROBE = 2;

struct InstalledProbe
{
    uintptr_t rva;
    char name[32];
    void* trampoline; // the stub jumps through this
    volatile LONG callCount;
    uint32_t firstFrame, lastFrame;
    uintptr_t thisValues[KEPT_THIS_VALUE_COUNT];
    unsigned thisCount;
    unsigned loggedStackCount;
};

constexpr unsigned MAXIMUM_PROBE_COUNT = 48;
InstalledProbe _probes[MAXIMUM_PROBE_COUNT];
unsigned _probeCount = 0;
/// One executable page holds all stubs (48 of 28 bytes each).
uint8_t* _stubMemory = nullptr;
uint8_t* _nextStub = nullptr;
/// The exe's base and its code section (the first section), found on first use.
uintptr_t _gameBase = 0, _textStart = 0, _textEnd = 0;

/// LogStack reads at most this many stack words and lists at most this many return addresses; the line stops
/// growing at LINE_LIMIT characters.
constexpr unsigned STACK_SCAN_WORDS = 3000;
constexpr unsigned MAXIMUM_STACK_ENTRIES = 48;
constexpr int LINE_LIMIT = 680;
/// Bytes per stack word (32-bit process).
constexpr unsigned STACK_WORD_BYTES = 4;
/// VirtualAlloc size for the stubs: one page.
constexpr SIZE_T STUB_MEMORY_BYTES = 4096;

// True if address is in the game's code right after a call instruction, so it is a return address and not just a
// number that happens to point into code. The forms checked, by their bytes before the address:
// E8 rel32 (call, 5 bytes); FF 15 abs32 (call [address], 6 bytes); FF /2 (call through a register or memory,
// 2, 3 or 6 bytes: the ModRM byte's reg field, mask 0x38, is 2).
bool IsReturnAddress(uintptr_t address)
{
    if (address < _textStart + 8 || address >= _textEnd)
    {
        return false;
    }
    // Pointer arithmetic: code[-5] is the byte 5 before the address.
    const uint8_t* code = reinterpret_cast<const uint8_t*>(address);
    return code[-5] == 0xE8 || (code[-6] == 0xFF && code[-5] == 0x15) ||
           (code[-2] == 0xFF && (code[-1] & 0x38) == 0x10) || (code[-3] == 0xFF && (code[-2] & 0x38) == 0x10) ||
           (code[-6] == 0xFF && (code[-5] & 0x38) == 0x10);
}

} // namespace

// Writes the validated return addresses found above `stackPointer` as one log line.
void LogStack(const char* label, uintptr_t stackPointer)
{
    if (!_gameBase)
    {
        // The PE headers at the exe's base: the DOS header points to the NT headers, after which the section table
        // follows (IMAGE_FIRST_SECTION).
        const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleA(nullptr));
        auto* ntHeaders =
            reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        _gameBase = reinterpret_cast<uintptr_t>(base);
        _textStart = _gameBase + IMAGE_FIRST_SECTION(ntHeaders)[0].VirtualAddress;
        _textEnd = _textStart + IMAGE_FIRST_SECTION(ntHeaders)[0].Misc.VirtualSize;
    }
    char line[700];
    int length = snprintf(line, sizeof(line), "STACK %s:", label);
    unsigned found = 0;
    for (unsigned word = 0; word < STACK_SCAN_WORDS && found < MAXIMUM_STACK_ENTRIES && length < LINE_LIMIT; ++word)
    {
        uint32_t value;
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(
                GetCurrentProcess(),
                reinterpret_cast<const void*>(stackPointer + word * STACK_WORD_BYTES),
                &value,
                STACK_WORD_BYTES,
                &bytesRead) ||
            bytesRead != STACK_WORD_BYTES)
        {
            break;
        }
        if (!IsReturnAddress(value))
        {
            continue;
        }
        length += snprintf(line + length, sizeof(line) - length, " +0x%x", static_cast<unsigned>(value - _gameBase));
        ++found;
    }
    Log::Info("probe", "%s", line);
}

namespace
{

// Called by every probe's stub, on whatever thread called the probed function. force_align_arg_pointer: the stub
// does not keep the stack 16-byte aligned, so the compiler realigns it here (SSE code below needs it).
__attribute__((force_align_arg_pointer)) void __cdecl OnProbeHit(uint32_t id, uintptr_t self, uintptr_t stackPointer)
{
    // FpGuard: an RAII guard (FpGuard.h); the game's floating-point state is restored at the closing brace.
    FpGuard guard;
    InstalledProbe& probe = _probes[id];
    const LONG callCount = InterlockedIncrement(&probe.callCount);
    const uint32_t frame = D3dSurvey::FrameCount();
    if (callCount == 1)
    {
        probe.firstFrame = frame;
    }
    probe.lastFrame = frame;
    bool isKnown = false;
    for (unsigned index = 0; index < probe.thisCount; ++index)
    {
        isKnown = isKnown || probe.thisValues[index] == self;
    }
    if (!isKnown && probe.thisCount < KEPT_THIS_VALUE_COUNT)
    {
        probe.thisValues[probe.thisCount++] = self;
    }
    if (probe.loggedStackCount < LOGGED_STACKS_PER_PROBE)
    {
        ++probe.loggedStackCount;
        // [0] is the return address, [1..4] the first four stack arguments.
        uint32_t stackWords[5] = {};
        SIZE_T bytesRead = 0;
        ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<const void*>(stackPointer),
            stackWords,
            sizeof(stackWords),
            &bytesRead);
        char label[96];
        snprintf(
            label,
            sizeof(label),
            "%s +0x%x this=%08x args=%08x %08x %08x %08x frame %u",
            probe.name,
            static_cast<unsigned>(probe.rva),
            static_cast<unsigned>(self),
            stackWords[1],
            stackWords[2],
            stackWords[3],
            stackWords[4],
            frame);
        LogStack(label, stackPointer);
    }
}

// pushad; pushfd; lea eax,[esp+36]; push eax; push ecx; push id; mov eax,OnProbeHit; call eax; add esp,12;
// popfd; popad; jmp [trampoline]
uint8_t* EmitStub(uint8_t* code, uint32_t id, void** trampolineSlot)
{
    // A lambda (like a C# local function); [&code] captures code by reference, so the writes move the caller's
    // position forward.
    auto putWord = [&code](uint32_t value)
    {
        memcpy(code, &value, 4);
        code += 4;
    };
    *code++ = 0x60;
    *code++ = 0x9C;
    *code++ = 0x8D;
    *code++ = 0x44;
    *code++ = 0x24;
    *code++ = 0x24;
    *code++ = 0x50;
    *code++ = 0x51;
    *code++ = 0x68;
    putWord(id);
    *code++ = 0xB8;
    putWord(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&OnProbeHit)));
    *code++ = 0xFF;
    *code++ = 0xD0;
    *code++ = 0x83;
    *code++ = 0xC4;
    *code++ = 0x0C;
    *code++ = 0x9D;
    *code++ = 0x61;
    *code++ = 0xFF;
    *code++ = 0x25;
    putWord(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(trampolineSlot)));
    return code;
}

} // namespace

bool Add(uintptr_t rva, const char* name)
{
    if (!_gameBase)
    {
        LogStack("probe setup", 0); // initialises the section bounds
    }
    for (unsigned index = 0; index < _probeCount; ++index)
    {
        if (_probes[index].rva == rva)
        {
            return true;
        }
    }
    const uintptr_t target = _gameBase + rva;
    if (_probeCount >= MAXIMUM_PROBE_COUNT || target < _textStart || target >= _textEnd)
    {
        Log::Info("probe", "+0x%x rejected", static_cast<unsigned>(rva));
        return false;
    }
    if (!_stubMemory)
    {
        _stubMemory = _nextStub = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, STUB_MEMORY_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!_stubMemory)
        {
            return false;
        }
        const MH_STATUS initializeStatus = MH_Initialize();
        if (initializeStatus != MH_OK && initializeStatus != MH_ERROR_ALREADY_INITIALIZED)
        {
            return false;
        }
    }
    const unsigned id = _probeCount;
    InstalledProbe& probe = _probes[id];
    memset(&probe, 0, sizeof(probe));
    probe.rva = rva;
    lstrcpynA(probe.name, name && *name ? name : "fn", sizeof(probe.name));
    uint8_t* stub = _nextStub;
    uint8_t* end = EmitStub(stub, id, &probe.trampoline);
    FlushInstructionCache(GetCurrentProcess(), stub, end - stub);
    MH_STATUS status = MH_CreateHook(reinterpret_cast<void*>(target), stub, &probe.trampoline);
    if (status == MH_OK)
    {
        status = MH_EnableHook(reinterpret_cast<void*>(target));
    }
    if (status != MH_OK)
    {
        Log::Error("probe", "+0x%x (%s) failed: %s", static_cast<unsigned>(rva), probe.name, MH_StatusToString(status));
        return false;
    }
    _nextStub = end;
    ++_probeCount;
    Log::Info("probe", "+0x%x (%s) installed", static_cast<unsigned>(rva), probe.name);
    return true;
}

void Report()
{
    for (unsigned index = 0; index < _probeCount; ++index)
    {
        InstalledProbe& probe = _probes[index];
        const LONG callCount = probe.callCount;
        const uint32_t frames = probe.lastFrame > probe.firstFrame ? probe.lastFrame - probe.firstFrame : 1;
        Log::Info(
            "probe",
            "PROBE %s +0x%x: %ld calls over %u frames (%ld.%02ld per frame), this: %08x %08x %08x %08x%s",
            probe.name,
            static_cast<unsigned>(probe.rva),
            callCount,
            frames,
            callCount / static_cast<LONG>(frames),
            (callCount * 100 / static_cast<LONG>(frames)) % 100,
            static_cast<unsigned>(probe.thisValues[0]),
            static_cast<unsigned>(probe.thisValues[1]),
            static_cast<unsigned>(probe.thisValues[2]),
            static_cast<unsigned>(probe.thisValues[3]),
            probe.thisCount >= KEPT_THIS_VALUE_COUNT ? " ..." : "");
        InterlockedExchange(&probe.callCount, 0);
        probe.firstFrame = probe.lastFrame = 0;
    }
}

} // namespace Probe

} // namespace SwbfVr
