// Memory access watches (diagnostic). Uses the CPU's four hardware data breakpoints on the game's main
// thread to record which code writes or reads a given address: the instruction after each access and the
// return addresses above it. This is how the engine's camera-update, render and firing code are located.
// Script commands: "watch <slot> <target> <w|rw> [bytes]", "unwatch". Results go to swbf-vr.log as W lines.

#include "Watch.h"

#include "Log.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace SwbfVr
{

namespace Watch
{

namespace
{

/// The CPU has four data breakpoints (debug registers Dr0 to Dr3).
constexpr unsigned WATCH_SLOT_COUNT = 4;
/// Return addresses kept per code location.
constexpr unsigned CALLER_COUNT = 6;

struct AddressWatch
{
    uintptr_t address;
    char label[48];
    bool isReadWrite;
    unsigned hitCount;
};

AddressWatch _watches[WATCH_SLOT_COUNT];

// One code location that touched a watched address.
struct Site
{
    uint8_t slot;
    uintptr_t codeAddress; // eip after the access: the instruction after the one that touched the address
    unsigned hitCount;
    uintptr_t callers[CALLER_COUNT];
};

constexpr unsigned MAXIMUM_SITE_COUNT = 600;
Site _sites[MAXIMUM_SITE_COUNT];
unsigned _siteCount = 0;
constexpr unsigned MAXIMUM_HIT_COUNT = 15000; // a watch switches itself off after this many accesses

DWORD _mainThreadId = 0;
/// The exe's code section (its first section).
uintptr_t _textStart = 0, _textEnd = 0;
bool _isInstalled = false;

/// Stack words searched for return addresses at each new code location.
constexpr unsigned STACK_SCAN_WORDS = 400;
/// Bytes per stack word (32-bit process).
constexpr unsigned STACK_WORD_BYTES = 4;
/// ClearAndReport waits this long for an exception being handled on another thread to finish.
constexpr DWORD SETTLE_MILLISECONDS = 50;
/// A report line stops growing at this many characters.
constexpr int LINE_LIMIT = 180;

// Reads one 32-bit word; false if the memory cannot be read.
bool ReadWord(uintptr_t address, uint32_t& value)
{
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(
               GetCurrentProcess(),
               reinterpret_cast<const void*>(address),
               &value,
               STACK_WORD_BYTES,
               &bytesRead) &&
           bytesRead == STACK_WORD_BYTES;
}

// True if `address` is inside the exe's code and the bytes before it look like a call instruction.
bool IsReturnAddress(uintptr_t address)
{
    if (address < _textStart + 8 || address >= _textEnd)
    {
        return false;
    }
    // Pointer arithmetic: code[-5] is the byte 5 before the address.
    const uint8_t* code = reinterpret_cast<const uint8_t*>(address);
    if (code[-5] == 0xE8)
    {
        return true; // call rel32
    }
    if (code[-6] == 0xFF && code[-5] == 0x15)
    {
        return true; // call [abs]
    }
    if (code[-2] == 0xFF && (code[-1] & 0x38) == 0x10)
    {
        return true; // call reg / [reg]
    }
    if (code[-3] == 0xFF && (code[-2] & 0x38) == 0x10)
    {
        return true; // call [reg+disp8]
    }
    if (code[-6] == 0xFF && (code[-5] & 0x38) == 0x10)
    {
        return true; // call [reg+disp32]
    }
    return false;
}

// Counts one access by the code at context->Eip; a new location also gets its callers from the stack.
void RecordSite(unsigned slot, const CONTEXT* context)
{
    ++_watches[slot].hitCount;
    for (unsigned index = 0; index < _siteCount; ++index)
    {
        if (_sites[index].codeAddress == context->Eip && _sites[index].slot == slot)
        {
            ++_sites[index].hitCount;
            return;
        }
    }
    if (_siteCount >= MAXIMUM_SITE_COUNT)
    {
        return;
    }
    Site& site = _sites[_siteCount];
    site.slot = static_cast<uint8_t>(slot);
    site.codeAddress = context->Eip;
    site.hitCount = 1;
    memset(site.callers, 0, sizeof(site.callers));
    unsigned found = 0;
    for (unsigned word = 0; word < STACK_SCAN_WORDS && found < CALLER_COUNT; ++word)
    {
        uint32_t value;
        if (!ReadWord(context->Esp + word * STACK_WORD_BYTES, value))
        {
            break;
        }
        if (IsReturnAddress(value))
        {
            site.callers[found++] = value;
        }
    }
    ++_siteCount;
}

// Debug-register enable bits (Dr7) for the watches that are set and still under their hit limit.
DWORD ComputeDr7()
{
    DWORD dr7 = 0;
    for (unsigned slot = 0; slot < WATCH_SLOT_COUNT; ++slot)
    {
        if (!_watches[slot].address || _watches[slot].hitCount >= MAXIMUM_HIT_COUNT)
        {
            continue;
        }
        dr7 |= 1u << (slot * 2);                                          // local enable
        dr7 |= (_watches[slot].isReadWrite ? 3u : 1u) << (16 + slot * 4); // 01 = write, 11 = read or write
        dr7 |= 3u << (18 + slot * 4);                                     // 4 bytes
    }
    return dr7;
}

// Puts the watch table into a thread context's debug registers.
void LoadDebugRegisters(CONTEXT* context)
{
    context->Dr0 = _watches[0].address;
    context->Dr1 = _watches[1].address;
    context->Dr2 = _watches[2].address;
    context->Dr3 = _watches[3].address;
    context->Dr6 = 0;
    context->Dr7 = ComputeDr7();
}

// Vectored exception handler: a data breakpoint raises EXCEPTION_SINGLE_STEP after the access; Dr6's low four bits
// say which breakpoints fired.
LONG CALLBACK OnSingleStep(EXCEPTION_POINTERS* exception)
{
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* context = exception->ContextRecord;
    const DWORD hitBits = context->Dr6 & 0xF;
    if (!hitBits)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    for (unsigned slot = 0; slot < WATCH_SLOT_COUNT; ++slot)
    {
        if ((hitBits & (1u << slot)) && _watches[slot].address)
        {
            RecordSite(slot, context);
        }
    }
    // Continuing restores the debug registers from this context record, which would undo any change another
    // thread made while this exception was being handled. So always continue with the current watch table.
    context->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
    LoadDebugRegisters(context);
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Loads the watch table into the main thread's debug registers.
bool ApplyToMainThread()
{
    if (!_mainThreadId)
    {
        return false;
    }
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, _mainThreadId);
    if (!thread)
    {
        return false;
    }
    bool isApplied = false;
    if (SuspendThread(thread) != static_cast<DWORD>(-1))
    {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        LoadDebugRegisters(&context);
        isApplied = SetThreadContext(thread, &context) != 0;
        ResumeThread(thread);
    }
    CloseHandle(thread);
    return isApplied;
}

} // namespace

void NoteMainThread()
{
    if (!_mainThreadId)
    {
        _mainThreadId = GetCurrentThreadId();
    }
}

bool Set(unsigned slot, uintptr_t address, bool isReadWrite, const char* label)
{
    if (slot >= WATCH_SLOT_COUNT || !address || (address & 3))
    {
        return false;
    }
    if (!_isInstalled)
    {
        // The PE headers at the exe's base: the DOS header points to the NT headers, after which the section table
        // follows (IMAGE_FIRST_SECTION).
        const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleA(nullptr));
        auto* ntHeaders =
            reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(ntHeaders);
        _textStart = reinterpret_cast<uintptr_t>(base) + sections[0].VirtualAddress;
        _textEnd = _textStart + sections[0].Misc.VirtualSize;
        AddVectoredExceptionHandler(1, OnSingleStep);
        _isInstalled = true;
    }
    _watches[slot].address = address;
    _watches[slot].isReadWrite = isReadWrite;
    _watches[slot].hitCount = 0;
    lstrcpynA(_watches[slot].label, label, sizeof(_watches[slot].label));
    const bool isArmed = ApplyToMainThread();
    Log::Info(
        "watch",
        "watch %u: %s at %p (%s) %s",
        slot,
        label,
        reinterpret_cast<void*>(address),
        isReadWrite ? "read/write" : "write",
        isArmed ? "armed" : "FAILED to arm");
    return isArmed;
}

// Disarms all watches and writes what they saw: one line per code location, busiest first.
void ClearAndReport()
{
    bool hasAny = false;
    for (AddressWatch& watch : _watches)
    {
        hasAny = hasAny || watch.address;
    }
    if (!hasAny)
    {
        return;
    }
    uintptr_t addresses[WATCH_SLOT_COUNT];
    for (unsigned slot = 0; slot < WATCH_SLOT_COUNT; ++slot)
    {
        addresses[slot] = _watches[slot].address;
        _watches[slot].address = 0;
    }
    ApplyToMainThread();
    Sleep(SETTLE_MILLISECONDS); // let an in-flight exception finish before reading the tables
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    for (unsigned slot = 0; slot < WATCH_SLOT_COUNT; ++slot)
    {
        if (!addresses[slot])
        {
            continue;
        }
        Log::Info(
            "watch",
            "W%u %s: %u accesses%s",
            slot,
            _watches[slot].label,
            _watches[slot].hitCount,
            _watches[slot].hitCount >= MAXIMUM_HIT_COUNT ? " (stopped at the limit)" : "");
        for (;;)
        { // selection by descending count
            Site* best = nullptr;
            for (unsigned index = 0; index < _siteCount; ++index)
            {
                if (_sites[index].slot == slot && _sites[index].hitCount &&
                    (!best || _sites[index].hitCount > best->hitCount))
                {
                    best = &_sites[index];
                }
            }
            if (!best)
            {
                break;
            }
            char line[200];
            int length = snprintf(
                line,
                sizeof(line),
                "W%u  +0x%x x%u <-",
                slot,
                static_cast<unsigned>(best->codeAddress - base),
                best->hitCount);
            for (unsigned caller = 0; caller < CALLER_COUNT && best->callers[caller] && length < LINE_LIMIT; ++caller)
            {
                length += snprintf(
                    line + length,
                    sizeof(line) - length,
                    " +0x%x",
                    static_cast<unsigned>(best->callers[caller] - base));
            }
            Log::Info("watch", "%s", line);
            best->hitCount = 0;
        }
    }
    _siteCount = 0;
}

} // namespace Watch

} // namespace SwbfVr
