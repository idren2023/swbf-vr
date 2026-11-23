// Limits.cpp: raises fixed limits inside the game's renderer.
//
// The renderer collects everything it is about to draw in fixed-size tables, sized for the game's own narrow
// field of view (about 70 degrees). A headset shows more than five times that area, so more objects are in view
// and the tables fill up. Two of them then write past their end (found in build b17: the game crashed within
// seconds of turning with a wide field of view, see docs/NOTES.md):
//  - the three sorted draw queues accept one entry more than their array holds;
//  - the matrix pool hands out a block that runs past its end when it wraps around.
// The others (draw items, light records, the two list queues) drop or recycle entries, which shows as missing
// or wrongly lit objects. All of them are replaced by larger tables here. Nothing else in the game changes.
//
// Layout facts (RVAs, exe linked 2020-07-31):
//  draw items     4648 x 48 bytes at 0x688010, count at 0x639E10, used only by the submit function 0x27C9A0
//  matrix pool    3062 x 64 bytes at 0x657BF0, count at 0x639E0C, allocators 0x275390 and 0x275410
//  light records  2048 x 56 bytes at 0x63B9D8, count at 0x639E14, allocator 0x2753D0
//  sorted queues  {count, capacity, entries*, owns} at 0x63B9C4 (1800), 0x657AB8 (1024), 0x657AC8 (2312); entry = 8
//  bytes list queues    {nodes*, free*, head*, count, capacity} at 0x657AD8 (800), 0x63B9B0 (512); node = 12 bytes
//  flush          void __cdecl 0x27C1D0(mask): draws and empties the queues

#include "Limits.h"

#include "GameBuild.h"
#include "Log.h"

#include <windows.h>

#include <MinHook.h>

#include <cstdint>
#include <cstdio>

namespace SwbfVr
{

namespace Limits
{

namespace
{

uintptr_t _exeBase = 0;
bool _isApplied = false, _hasFailed = false;
unsigned _attemptCount = 0;

/// The game's sorted draw queue (a binary heap): {count, capacity, entries*, owns}. Entry = {key, item*}, 8 bytes;
/// entry 0 is a guard.
struct SortedQueue
{
    uint32_t count, capacity;
    uint32_t* entries;
    uint8_t ownsEntries; // non-zero: the game frees the entries at exit
};

/// The game's list queue: {nodes*, free*, head*, count, capacity}; node = {item*, key, next*}, 12 bytes.
struct ListQueue
{
    void* nodes;
    void* freeNodes;
    void* head;
    uint32_t count, capacity;
};

// ---- Engine addresses ----
// All from GameBuild (Feature::Limits; layout facts above; docs/offsets.md, "Renderer tables").

using GameBuild::Address;
/// The three sorted draw queues (SortedQueue), capacities 1800, 1024, 2312.
constexpr Address SORTED_QUEUES[3] = {Address::SortedQueue1800, Address::SortedQueue1024, Address::SortedQueue2312};
/// The two list queues (ListQueue), capacities 800, 512.
constexpr Address LIST_QUEUES[2] = {Address::ListQueue800, Address::ListQueue512};

// ---- Constants ----

/// The game's table sizes (entries) and the ones that replace them.
constexpr uint32_t ORIGINAL_ITEM_CAPACITY = 4648, ORIGINAL_MATRIX_CAPACITY = 3062, ORIGINAL_RECORD_CAPACITY = 2048;
constexpr uint32_t NEW_ITEM_CAPACITY = 0x8000, NEW_MATRIX_CAPACITY = 0x8000, NEW_RECORD_CAPACITY = 0x4000;
constexpr uint32_t QUEUE_GROWTH_FACTOR = 8; // queues: times the original capacity
/// Entry sizes in bytes: draw item, matrix, light record, sorted queue entry.
constexpr uint32_t ITEM_BYTES = 48, MATRIX_BYTES = 64, RECORD_BYTES = 56, QUEUE_ENTRY_BYTES = 8;
/// After this many frame starts with a queue not empty, the log says the limits were never raised.
constexpr unsigned ATTEMPTS_BEFORE_GIVING_UP_LOG = 600;
/// Shortest time between two peak-use log lines.
constexpr DWORD PEAK_LOG_INTERVAL_MILLISECONDS = 1000;
/// A table's peak use is logged from this many quarters of its original limit.
constexpr uint32_t LOGGED_QUARTERS_MINIMUM = 3;

/// void __cdecl Flush(uint32_t mask).
using FlushFunction = void(__cdecl*)(uint32_t);
/// The game's Flush behind our hook, called through MinHook's trampoline.
FlushFunction _originalFlush = nullptr;

/// The game's memory at an RVA, as a T (a template: one copy of the function per type T it is used with, like a C#
/// generic method).
template <class T> T& At(Address address)
{
    return *reinterpret_cast<T*>(_exeBase + GameBuild::Rva(address));
}

// One 32-bit constant inside the game's code: where it is, what it must contain now, what it becomes.
struct Constant
{
    Address address;
    uint32_t expected;
    uint32_t replacement;
};

// Peak fill levels, to see how close the game gets to its limits.
const char* const FILL_LEVEL_NAMES[8] =
    {"items", "matrices", "records", "queue1", "queue2", "queue4", "queue8", "queue16"};
const uint32_t ORIGINAL_CAPACITIES[8] =
    {ORIGINAL_ITEM_CAPACITY, ORIGINAL_MATRIX_CAPACITY, ORIGINAL_RECORD_CAPACITY, 1800, 1024, 2312, 800, 512};
uint32_t _peakFillLevels[8], _loggedQuarters[8];
DWORD _lastPeakLogTick = 0;

// general-regs-only: the compiler may not use a floating-point register here, so the game's float state is never
// touched (this runs in front of the game's Flush; see FpGuard for the other hooks).
__attribute__((target("general-regs-only"))) void SampleFillLevels()
{
    const uint32_t now[8] = {
        At<uint32_t>(Address::ItemCount),
        At<uint32_t>(Address::MatrixCount),
        At<uint32_t>(Address::RecordCount),
        At<SortedQueue>(SORTED_QUEUES[0]).count,
        At<SortedQueue>(SORTED_QUEUES[1]).count,
        At<SortedQueue>(SORTED_QUEUES[2]).count,
        At<ListQueue>(LIST_QUEUES[0]).count,
        At<ListQueue>(LIST_QUEUES[1]).count};
    bool isWorthLogging = false;
    for (int table = 0; table < 8; ++table)
    {
        if (now[table] <= _peakFillLevels[table])
        {
            continue;
        }
        _peakFillLevels[table] = now[table];
        const uint32_t quarters = now[table] * 4 / ORIGINAL_CAPACITIES[table]; // quarters of the original limit
        if (quarters >= LOGGED_QUARTERS_MINIMUM && quarters > _loggedQuarters[table])
        {
            _loggedQuarters[table] = quarters;
            isWorthLogging = true;
        }
    }
    if (!isWorthLogging)
    {
        return;
    }
    const DWORD tick = GetTickCount();
    if (tick - _lastPeakLogTick < PEAK_LOG_INTERVAL_MILLISECONDS)
    {
        for (int table = 0; table < 8; ++table)
        {
            if (_loggedQuarters[table])
            {
                --_loggedQuarters[table];
            }
        }
        return;
    } // try again later
    _lastPeakLogTick = tick;
    char text[256];
    int used = 0;
    for (int table = 0; table < 8; ++table)
    {
        used += snprintf(
            text + used,
            sizeof(text) - static_cast<size_t>(used),
            " %s %u/%u",
            FILL_LEVEL_NAMES[table],
            static_cast<unsigned>(_peakFillLevels[table]),
            static_cast<unsigned>(ORIGINAL_CAPACITIES[table]));
    }
    Log::Info("limits", "peak use / original limit:%s", text);
}

/// In front of the game's queue flush: samples the fill levels, then runs the game's Flush.
__attribute__((target("general-regs-only"))) void __cdecl FlushHook(uint32_t mask)
{
    SampleFillLevels();
    _originalFlush(mask);
}

/// Overwrites a 32-bit constant in the game's code (the page is made writable for the moment of the write).
bool WriteConstant(Address address, uint32_t value)
{
    void* const target = reinterpret_cast<void*>(GameBuild::Absolute(address));
    DWORD oldProtection = 0;
    if (!VirtualProtect(target, 4, PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        return false;
    }
    memcpy(target, &value, 4);
    VirtualProtect(target, 4, oldProtection, &oldProtection);
    FlushInstructionCache(GetCurrentProcess(), target, 4);
    return true;
}

} // namespace

void Install()
{
    if (_exeBase)
    {
        return;
    }
    // Checked by GameBuild::Identify: every table, count and code constant used here was found.
    if (!GameBuild::IsAvailable(GameBuild::Feature::Limits))
    {
        static bool hasLogged = false;
        if (!hasLogged)
        {
            hasLogged = true;
            Log::Error(
                "limits",
                "not installed: %s is off for this game build",
                GameBuild::FeatureName(GameBuild::Feature::Limits));
        }
        return;
    }
    _exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    void* const flush = reinterpret_cast<void*>(GameBuild::Absolute(Address::Flush));
    MH_STATUS status =
        MH_CreateHook(flush, reinterpret_cast<void*>(&FlushHook), reinterpret_cast<void**>(&_originalFlush));
    if (status == MH_OK)
    {
        status = MH_EnableHook(flush);
    }
    if (status != MH_OK)
    {
        Log::Warning("limits", "fill-level monitor not installed: %s", MH_StatusToString(status));
    }
}

// Call on the render thread between frames. Does its work once, at a moment when the queues are empty.
void Apply()
{
    if (_isApplied || _hasFailed || !_exeBase)
    {
        return;
    }
    for (const Address queue : SORTED_QUEUES)
    {
        if (At<SortedQueue>(queue).count)
        {
            if (++_attemptCount == ATTEMPTS_BEFORE_GIVING_UP_LOG)
            {
                Log::Info("limits", "the draw queues are never empty between frames, limits not raised");
            }
            return;
        }
    }
    for (const Address queue : LIST_QUEUES)
    {
        if (At<ListQueue>(queue).count || At<ListQueue>(queue).head)
        {
            if (++_attemptCount == ATTEMPTS_BEFORE_GIVING_UP_LOG)
            {
                Log::Info("limits", "the draw queues are never empty between frames, limits not raised");
            }
            return;
        }
    }

    uint8_t* const items = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, NEW_ITEM_CAPACITY * ITEM_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    uint8_t* const matrices = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, (NEW_MATRIX_CAPACITY + 128) * MATRIX_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    uint8_t* const records = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, (NEW_RECORD_CAPACITY + 8) * RECORD_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!items || !matrices || !records)
    {
        _hasFailed = true;
        Log::Info("limits", "out of memory, limits not raised");
        return;
    }
    // The game is a 32-bit program: its code holds addresses as 32-bit numbers.
    const uint32_t base = static_cast<uint32_t>(_exeBase), oldMatrices = base + GameBuild::Rva(Address::Matrices),
                   oldRecords = base + GameBuild::Rva(Address::Records);
    const uint32_t newMatrices = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(matrices)),
                   newRecords = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(records));
    const Constant constants[] = {
        {Address::SubmitItemLimit, ORIGINAL_ITEM_CAPACITY, NEW_ITEM_CAPACITY},
        {Address::SubmitItemTable,
         base + GameBuild::Rva(Address::Items),
         static_cast<uint32_t>(reinterpret_cast<uintptr_t>(items))},
        {Address::MatrixBlockTable, oldMatrices, newMatrices},
        {Address::MatrixBlockLimit, ORIGINAL_MATRIX_CAPACITY, NEW_MATRIX_CAPACITY},
        {Address::MatrixSingleTable, oldMatrices, newMatrices},
        {Address::MatrixSingleLimit, ORIGINAL_MATRIX_CAPACITY, NEW_MATRIX_CAPACITY},
        {Address::MatrixInPoolStart, oldMatrices, newMatrices},
        {Address::MatrixInPoolEnd,
         oldMatrices + ORIGINAL_MATRIX_CAPACITY * MATRIX_BYTES,
         newMatrices + NEW_MATRIX_CAPACITY * MATRIX_BYTES},
        {Address::RecordAllocatorLimit, ORIGINAL_RECORD_CAPACITY, NEW_RECORD_CAPACITY},
        {Address::RecordAllocatorTable, oldRecords, newRecords},
        {Address::RecordInPoolFirstStart, oldRecords, newRecords},
        {Address::RecordInPoolFirstEnd,
         oldRecords + (ORIGINAL_RECORD_CAPACITY + 4) * RECORD_BYTES,
         newRecords + NEW_RECORD_CAPACITY * RECORD_BYTES},
        {Address::RecordInPoolSecondStart, oldRecords, newRecords},
        {Address::RecordInPoolSecondEnd,
         oldRecords + (ORIGINAL_RECORD_CAPACITY + 4) * RECORD_BYTES,
         newRecords + NEW_RECORD_CAPACITY * RECORD_BYTES},
    };
    for (const Constant& constant : constants)
    {
        if (At<uint32_t>(constant.address) != constant.expected)
        {
            _hasFailed = true;
            Log::Warning(
                "limits",
                "unexpected code at +0x%x (found 0x%x, expected 0x%x): this is a different game version, "
                "limits not raised",
                static_cast<unsigned>(GameBuild::Rva(constant.address)),
                static_cast<unsigned>(At<uint32_t>(constant.address)),
                static_cast<unsigned>(constant.expected));
            return;
        }
    }
    for (const Constant& constant : constants)
    {
        if (!WriteConstant(constant.address, constant.replacement))
        {
            _hasFailed = true;
            Log::Warning(
                "limits",
                "could not change code at +0x%x, limits partly raised",
                static_cast<unsigned>(GameBuild::Rva(constant.address)));
            return;
        }
    }
    At<uint32_t>(Address::ItemCount) = 0;
    At<uint32_t>(Address::MatrixCount) = 0;
    At<uint32_t>(Address::RecordCount) = 0;

    // The queues' new tables come from the system, not from the game's allocator: the game empties its own heap
    // when a level is loaded (found in build b20, where a queue allocated there lost its guard entry and the
    // renderer looped forever on the first frame of the match).
    for (const Address address : SORTED_QUEUES)
    {
        SortedQueue& queue = At<SortedQueue>(address);
        const uint32_t capacity = queue.capacity * QUEUE_GROWTH_FACTOR;
        uint32_t* const entries = static_cast<uint32_t*>(VirtualAlloc(
            nullptr,
            (capacity + 2) * QUEUE_ENTRY_BYTES,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_READWRITE)); // +2: entry 0 is a guard, and the game fills one past its limit
        if (!entries)
        {
            continue;
        }
        entries[0] = queue.entries[0]; // the guard: key and item
        entries[1] = queue.entries[1];
        queue.entries = entries;
        queue.capacity = capacity;
        queue.ownsEntries =
            0; // the game must not try to free it at exit (its own, smaller table is simply left unused)
    }
    for (const Address address : LIST_QUEUES)
    {
        ListQueue& list = At<ListQueue>(address);
        const uint32_t capacity = list.capacity * QUEUE_GROWTH_FACTOR;

        struct Node
        {
            void* item;
            uint32_t key;
            Node* next;
        };

        Node* const nodes = static_cast<Node*>(
            VirtualAlloc(nullptr, capacity * sizeof(Node), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!nodes)
        {
            continue;
        }
        for (uint32_t node = 0; node + 1 < capacity; ++node)
        {
            nodes[node].next = &nodes[node + 1];
        }
        nodes[capacity - 1].next = nullptr;
        list.freeNodes = nodes; // list.nodes still names the game's own block, which it frees at exit; its nodes are
                                // no longer handed out
        list.capacity = capacity;
    }
    _isApplied = true;
    Log::Info(
        "limits",
        "renderer tables enlarged (items %u, matrices %u, light records %u, queues x%u)",
        static_cast<unsigned>(NEW_ITEM_CAPACITY),
        static_cast<unsigned>(NEW_MATRIX_CAPACITY),
        static_cast<unsigned>(NEW_RECORD_CAPACITY),
        static_cast<unsigned>(QUEUE_GROWTH_FACTOR));
}

} // namespace Limits

} // namespace SwbfVr
