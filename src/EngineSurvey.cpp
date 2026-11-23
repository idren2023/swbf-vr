// EngineSurvey.cpp: engine object survey (diagnostic). Finds live game objects through MSVC RTTI and records them:
//  - transparent thunks on the camera and PlayerController vtables capture `this` pointers ("roots")
//  - every 3 s a walk from the roots follows pointers to other polymorphic objects (edges go to swbf-vr.log)
//  - 4 times a second the raw bytes of the interesting objects go to swbf-vr-snap.bin
//  - twice per session the exe's .data is scanned for static objects and global pointers to objects
// Safety rules for this file: game memory is only read through ReadProcessMemory (a stale pointer cannot
// fault), code that runs inside game code is wrapped in FpGuard, and nothing here uses floating point.
//
// MSVC RTTI in short: the word just before a vtable points at a "complete object locator", whose fields give the
// offset of this vtable's subobject inside the whole object (+4) and a type descriptor (+12) holding the class's
// decorated name (".?AVClassName@@" at +8). tools/rtti.py reads the same structures from the exe file.

#include "EngineSurvey.h"

#include "FpGuard.h"
#include "Log.h"
#include "Main.h"
#include "Xr.h"
#include "engine.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace SwbfVr
{

namespace EngineSurvey
{

namespace
{

// ---- Constants ----

/// Addresses below this are never objects (the first 64 KB of the address space are never mapped).
constexpr uintptr_t MINIMUM_OBJECT_ADDRESS = 0x10000;
/// How many bytes of an object the walk and the snapshots look at. Real sizes are unknown, so these are generous
/// guesses.
constexpr size_t SOLDIER_SPAN_BYTES = 0x1000;
constexpr size_t WEAPON_SPAN_BYTES = 0x800;
constexpr size_t OTHER_SPAN_BYTES = 0x400;
/// Thunks and roots kept at most.
constexpr unsigned MAXIMUM_SLOTS = 64, MAXIMUM_ROOTS = 256;
/// The graph walk: objects visited at most, the depth where it stops, and the depth from which only interesting
/// classes are followed further.
constexpr size_t MAXIMUM_NODES = 400;
constexpr uint32_t WALK_DEPTH_LIMIT = 4;
constexpr uint32_t INTERESTING_ONLY_DEPTH = 2;
/// Most distinct edges logged in one session.
constexpr size_t MAXIMUM_EDGE_LINES = 8000;
/// Objects per snapshot at most, and of those at most this many soldiers and weapons.
constexpr size_t MAXIMUM_SNAPSHOT_OBJECTS = 40;
constexpr unsigned MAXIMUM_SNAPSHOT_SOLDIERS = 4;
constexpr unsigned MAXIMUM_SNAPSHOT_WEAPONS = 12;
/// swbf-vr-snap.bin stops growing at this size.
constexpr unsigned MAXIMUM_SNAPSHOT_FILE_BYTES = 200u * 1024 * 1024;
/// The .data scan: distinct (address, class) pairs remembered, log lines per class and kind, extra roots kept.
constexpr size_t MAXIMUM_GLOBALS_SEEN = 1000000;
constexpr uint16_t MAXIMUM_LINES_PER_CLASS = 4;
constexpr size_t MAXIMUM_EXTRA_ROOTS = 64;
/// Timers of Tick, in GetTickCount milliseconds.
constexpr DWORD GRAPH_INTERVAL_MILLISECONDS = 3000;
constexpr DWORD SNAPSHOT_INTERVAL_MILLISECONDS = 250;
constexpr DWORD CALLS_LOG_INTERVAL_MILLISECONDS = 5000;
/// The two .data scans run this long after the first root shows up.
constexpr DWORD FIRST_SCAN_DELAY_MILLISECONDS = 10000u;
constexpr DWORD SECOND_SCAN_DELAY_MILLISECONDS = 60000u;
/// When the Present hook has not driven the survey for this long, the thunks drive it.
constexpr DWORD PRESENT_QUIET_MILLISECONDS = 5000;
/// FNV-1a, 64 bit: offset basis and prime.
constexpr uint64_t FNV_OFFSET_BASIS = 0xcbf29ce484222325ULL;
constexpr uint64_t FNV_PRIME = 0x100000001b3ULL;

// ---- State ----

// The exe's image and its sections, as addresses: [start, end).
uintptr_t _exeBase, _imageEnd, _textStart, _textEnd, _readOnlyDataStart, _readOnlyDataEnd, _dataStart, _dataEnd;
bool _isReady = false;
// SRWLOCK: a Windows lock (like C#'s lock statement, but taken and released by explicit calls).
SRWLOCK _rootLock = SRWLOCK_INIT, _tickLock = SRWLOCK_INIT;

/// Reads game memory without risking a fault (ReadProcessMemory on our own process fails instead of crashing).
/// @return true if all bytes were read.
bool ReadMemory(uintptr_t address, void* out, size_t byteCount)
{
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), out, byteCount, &bytesRead) &&
           bytesRead == byteCount;
}

// Reads up to byteCount bytes, stopping at the first unreadable 256-byte block. Returns bytes read.
size_t ReadUpTo(uintptr_t address, uint8_t* out, size_t byteCount)
{
    if (ReadMemory(address, out, byteCount))
    {
        return byteCount;
    }
    size_t bytesRead = 0;
    while (bytesRead < byteCount && ReadMemory(address + bytesRead, out + bytesRead, 0x100))
    {
        bytesRead += 0x100;
    }
    return bytesRead;
}

// Set of non-zero 64-bit keys: open addressing, integer arithmetic only.
class KeySet
{
    std::vector<uint64_t> _slots;
    size_t _count = 0;

    // MurmurHash3's 64-bit finalizer: mixes every bit of the key into the low bits used as the slot index.
    static size_t Hash(uint64_t x)
    {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33;
        return static_cast<size_t>(x);
    }

    void Place(uint64_t key)
    {
        const size_t mask = _slots.size() - 1;
        size_t slot = Hash(key) & mask;
        while (_slots[slot])
        {
            slot = (slot + 1) & mask;
        }
        _slots[slot] = key;
    }

  public:
    size_t Size() const
    {
        return _count;
    }

    // Returns true if the key was not present.
    bool Insert(uint64_t key)
    {
        if (!key)
        {
            key = 1;
        }
        if ((_count + 1) * 2 > _slots.size())
        {
            std::vector<uint64_t> oldSlots;
            oldSlots.swap(_slots);
            _slots.assign(oldSlots.empty() ? 1024 : oldSlots.size() * 2, 0);
            for (uint64_t oldKey : oldSlots)
            {
                if (oldKey)
                {
                    Place(oldKey);
                }
            }
        }
        const size_t mask = _slots.size() - 1;
        size_t slot = Hash(key) & mask;
        while (_slots[slot])
        {
            if (_slots[slot] == key)
            {
                return false;
            }
            slot = (slot + 1) & mask;
        }
        _slots[slot] = key;
        ++_count;
        return true;
    }
};

/// FNV-1a hash of a string (for the set of edges already logged).
uint64_t HashString(const char* text)
{
    uint64_t hash = FNV_OFFSET_BASIS;
    for (; *text; ++text)
    {
        hash ^= static_cast<uint8_t>(*text);
        hash *= FNV_PRIME;
    }
    return hash;
}

// ---- class table: every vtable in the exe that has RTTI, built once at startup ----
struct ClassEntry
{
    uint32_t vtable;       // address of the vtable
    uint32_t objectOffset; // offset of this vtable's subobject inside the complete object
    char name[48];
};

std::vector<ClassEntry> _classes; // sorted by vtable; never modified after BuildClassTable

/// Scans .rdata for vtables: a word pointing at a valid complete object locator, followed by a pointer into .text.
void BuildClassTable()
{
    for (uintptr_t address = _readOnlyDataStart; address + 8 <= _readOnlyDataEnd; address += 4)
    {
        const uint32_t locatorAddress = *reinterpret_cast<const uint32_t*>(address);
        if ((locatorAddress & 3) || locatorAddress < _readOnlyDataStart || locatorAddress > _readOnlyDataEnd - 20)
        {
            continue; // written this way so a huge value cannot wrap around
        }
        // The locator's words: signature (0), subobject offset, constructor displacement offset, type descriptor.
        const uint32_t* locator = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(locatorAddress));
        if (locator[0] != 0 || locator[1] >= 0x10000)
        {
            continue;
        }
        const uintptr_t typeDescriptor = locator[3];
        if (typeDescriptor < _exeBase || typeDescriptor > _imageEnd - (8 + sizeof(ClassEntry::name) + 8))
        {
            continue;
        }
        const char* decoratedName = reinterpret_cast<const char*>(typeDescriptor + 8);
        if (decoratedName[0] != '.' || decoratedName[1] != '?' || decoratedName[2] != 'A' ||
            (decoratedName[3] != 'V' && decoratedName[3] != 'U'))
        {
            continue;
        }
        const uint32_t firstMethod = *reinterpret_cast<const uint32_t*>(address + 4);
        if (firstMethod < _textStart || firstMethod >= _textEnd)
        {
            continue;
        }
        ClassEntry entry{};
        entry.vtable = static_cast<uint32_t>(address + 4);
        entry.objectOffset = locator[1];
        decoratedName += 4;
        size_t length = 0;
        for (; length < sizeof(entry.name) - 1 && decoratedName[length] &&
               !(decoratedName[length] == '@' && decoratedName[length + 1] == '@');
             ++length)
        {
            entry.name[length] = decoratedName[length];
        }
        if (length)
        {
            _classes.push_back(entry);
        }
    }
}

/// Binary search of the class table.
/// @return the class whose vtable is at this address, or null.
const ClassEntry* ClassOf(uint32_t vtable)
{
    size_t low = 0, high = _classes.size();
    while (low < high)
    {
        const size_t middle = (low + high) / 2;
        if (_classes[middle].vtable < vtable)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    return low < _classes.size() && _classes[low].vtable == vtable ? &_classes[low] : nullptr;
}

// ---- map of memory that may hold game objects: committed private memory plus the exe's .data ----
struct Region
{
    uintptr_t start, end;
};

std::vector<Region> _regions;

void BuildRegions()
{
    _regions.clear();
    SYSTEM_INFO systemInfo;
    GetSystemInfo(&systemInfo);
    uintptr_t address = reinterpret_cast<uintptr_t>(systemInfo.lpMinimumApplicationAddress);
    const uintptr_t lastAddress = reinterpret_cast<uintptr_t>(systemInfo.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION memoryInfo;
    while (address < lastAddress && VirtualQuery(reinterpret_cast<void*>(address), &memoryInfo, sizeof(memoryInfo)))
    {
        uintptr_t start = reinterpret_cast<uintptr_t>(memoryInfo.BaseAddress), end = start + memoryInfo.RegionSize;
        const uintptr_t next = end;
        const DWORD protection = memoryInfo.Protect & 0xff;
        const bool isReadable =
            memoryInfo.State == MEM_COMMIT && !(memoryInfo.Protect & PAGE_GUARD) &&
            (protection == PAGE_READWRITE || protection == PAGE_READONLY || protection == PAGE_WRITECOPY ||
             protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_READ);
        bool shouldUse = isReadable && memoryInfo.Type == MEM_PRIVATE;
        if (isReadable && memoryInfo.Type == MEM_IMAGE && end > _dataStart && start < _dataEnd)
        {
            if (start < _dataStart)
            {
                start = _dataStart;
            }
            if (end > _dataEnd)
            {
                end = _dataEnd;
            }
            shouldUse = true;
        }
        if (shouldUse)
        {
            if (!_regions.empty() && _regions.back().end == start)
            {
                _regions.back().end = end;
            }
            else
            {
                _regions.push_back({start, end});
            }
        }
        if (next <= address)
        {
            break;
        }
        address = next;
    }
}

/// @return true if the 4 bytes at this address lie in one of the regions of BuildRegions.
bool IsMapped(uintptr_t address)
{
    size_t low = 0, high = _regions.size();
    while (low < high)
    {
        const size_t middle = (low + high) / 2;
        if (address < _regions[middle].start)
        {
            high = middle;
        }
        else if (address >= _regions[middle].end)
        {
            low = middle + 1;
        }
        else
        {
            return address + 4 <= _regions[middle].end;
        }
    }
    return false;
}

// If address points at a polymorphic object (or at one of its base subobjects), returns its class and start.
const ClassEntry* ObjectAt(uintptr_t address, uintptr_t& start)
{
    if ((address & 3) || address < MINIMUM_OBJECT_ADDRESS || !IsMapped(address))
    {
        return nullptr;
    }
    uint32_t vtable;
    if (!ReadMemory(address, &vtable, 4))
    {
        return nullptr;
    }
    const ClassEntry* entry = ClassOf(vtable);
    if (!entry || entry->objectOffset > address)
    {
        return nullptr;
    }
    start = address - entry->objectOffset;
    return entry;
}

bool StartsWith(const char* text, const char* prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

bool EndsWith(const char* text, const char* suffix)
{
    const size_t textLength = strlen(text), suffixLength = strlen(suffix);
    return textLength >= suffixLength && strcmp(text + textLength - suffixLength, suffix) == 0;
}

bool IsSoldier(const char* className)
{
    return StartsWith(className, "EntitySoldier") && !EndsWith(className, "Class");
}

bool IsWeapon(const char* className)
{
    return StartsWith(className, "Weapon") && !EndsWith(className, "Class");
}

bool IsCameraOrController(const char* className)
{
    return strstr(className, "Camera") || strcmp(className, "PlayerController") == 0;
}

bool IsInteresting(const char* className)
{
    return IsCameraOrController(className) || IsSoldier(className) || IsWeapon(className) ||
           strstr(className, "Aimer") || strcmp(className, "Controllable") == 0 ||
           strcmp(className, "FirstPersonRenderable") == 0;
}

// How many bytes of an object to look at. Real sizes are unknown, so these are generous guesses.
size_t SpanOf(const char* className)
{
    return IsSoldier(className) ? SOLDIER_SPAN_BYTES : IsWeapon(className) ? WEAPON_SPAN_BYTES : OTHER_SPAN_BYTES;
}

// ---- vtable thunks ----
/// A vtable whose first methods get a thunk.
struct VtableHook
{
    const char* className;
    uint32_t vtableRva;
    uint32_t slotCount;
};

// From tools/rtti.py on the Steam build (see docs/offsets.md). Each is verified against RTTI before use.
const VtableHook VTABLE_HOOKS[] = {
    {"GameCamera", 0x41811c, 6},
    {"ChaseCamera", 0x418140, 6},
    {"DeathCamera", 0x418164, 6},
    {"FreeCamera", 0x418188, 6},
    {"CameraManager", 0x410bf8, 5},
    {"CameraInterpolator", 0x410bc8, 5},
    {"PlayerController", 0x422b6c, 12},
};

/// The method behind each thunk, by thunk id (for the log).
struct HookedSlot
{
    const char* className;
    uint32_t slot;
} _hookedSlots[MAXIMUM_SLOTS];

unsigned _hookedSlotCount = 0;
LONG _callCounts[MAXIMUM_SLOTS];
LONG _previousCallCounts[MAXIMUM_SLOTS];
uintptr_t _roots[MAXIMUM_ROOTS];
volatile LONG _rootCount = 0;
DWORD _lastPresentTick = 0, _firstRootTick = 0;

void Tick(uint32_t frame, bool isForced);

// Called by every thunk with the hooked method's `this`. The thunk saves the integer registers and flags;
// FpGuard saves the floating-point state, so the game sees no change at all.
// force_align_arg_pointer: the game's code may call with the stack not 16-byte aligned; this realigns it on entry.
__attribute__((force_align_arg_pointer)) void __cdecl RecordRoot(uint32_t slotId, uintptr_t self)
{
    FpGuard fp;
    InterlockedIncrement(&_callCounts[slotId]);
    const LONG rootCount = _rootCount;
    bool isKnown = false;
    for (LONG root = 0; root < rootCount; ++root)
    {
        if (_roots[root] == self)
        {
            isKnown = true;
            break;
        }
    }
    if (!isKnown)
    {
        AcquireSRWLockExclusive(&_rootLock);
        const LONG lockedRootCount = _rootCount;
        for (LONG root = 0; root < lockedRootCount; ++root)
        {
            if (_roots[root] == self)
            {
                isKnown = true;
            }
        }
        if (!isKnown && lockedRootCount < static_cast<LONG>(MAXIMUM_ROOTS))
        {
            _roots[lockedRootCount] = self;
            _rootCount = lockedRootCount + 1;
        }
        ReleaseSRWLockExclusive(&_rootLock);
        if (!isKnown)
        {
            if (!_firstRootTick)
            {
                _firstRootTick = GetTickCount();
            }
            Log::Info(
                "survey",
                "root %p first seen in %s[%u]",
                reinterpret_cast<void*>(self),
                _hookedSlots[slotId].className,
                _hookedSlots[slotId].slot);
        }
    }
    // Normally the Direct3D Present hook drives the survey. If it has gone quiet, drive it from here.
    if (GetTickCount() - _lastPresentTick > PRESENT_QUIET_MILLISECONDS)
    {
        Tick(0, false);
    }
}

// pushad; pushfd; push ecx; push id; mov eax, RecordRoot; call eax; add esp, 8; popfd; popad; jmp original
/// Writes one thunk as x86 machine code: the hooked method's `this` is in ECX (__thiscall), so the thunk passes ECX and
/// its id to RecordRoot, restores every register and jumps on to the original method.
/// @param cursor where to write; executable memory.
/// @return the address after the thunk.
uint8_t* EmitThunk(uint8_t* cursor, uint32_t slotId, uintptr_t original)
{
    // A lambda capturing `cursor` by reference ([&cursor]): it writes through and advances the caller's variable.
    auto put32 = [&cursor](uint32_t value)
    {
        memcpy(cursor, &value, 4);
        cursor += 4;
    };
    *cursor++ = 0x60; // pushad
    *cursor++ = 0x9C; // pushfd
    *cursor++ = 0x51; // push ecx
    *cursor++ = 0x68; // push imm32
    put32(slotId);
    *cursor++ = 0xB8; // mov eax, imm32
    put32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&RecordRoot)));
    *cursor++ = 0xFF; // call eax
    *cursor++ = 0xD0;
    *cursor++ = 0x83; // add esp, 8
    *cursor++ = 0xC4;
    *cursor++ = 0x08;
    *cursor++ = 0x9D; // popfd
    *cursor++ = 0x61; // popad
    *cursor++ = 0xE9; // jmp rel32 (relative to the end of the instruction)
    put32(static_cast<uint32_t>(original - (reinterpret_cast<uintptr_t>(cursor) + 4)));
    return cursor;
}

// ---- object graph walk ----
struct Node
{
    uintptr_t start;
    uint32_t depth;
    uintptr_t from;
    uint32_t fromOffset;
    const ClassEntry* entry;
};

std::vector<Node> _snapshotList;
std::vector<uintptr_t> _extraRoots;
KeySet _edgesSeen;

/// Logs an edge of the graph once per session: B base subobject, E embedded member object, P pointer.
void LogEdge(const Node& from, size_t offset, char kind, const ClassEntry* to)
{
    char line[160];
    snprintf(
        line,
        sizeof(line),
        "G d%u %s+0x%x %c %s+0x%x",
        from.depth,
        from.entry->name,
        static_cast<unsigned>(offset),
        kind,
        to->name,
        to->objectOffset);
    if (_edgesSeen.Size() < MAXIMUM_EDGE_LINES && _edgesSeen.Insert(HashString(line)))
    {
        Log::Info("survey", "%s", line);
    }
}

/// Walks pointers from the roots to other polymorphic objects, logs new edges and picks the objects to snapshot.
void WalkGraph()
{
    BuildRegions();
    std::vector<Node> nodes;
    KeySet seen;
    // [&]: the lambda captures the variables it uses (nodes, seen) by reference.
    auto addRoot = [&](uintptr_t address)
    {
        uintptr_t start;
        const ClassEntry* entry = ObjectAt(address, start);
        if (entry && seen.Insert(start))
        {
            nodes.push_back({start, 0, 0, 0, entry});
        }
    };
    const LONG rootCount = _rootCount;
    for (LONG root = 0; root < rootCount; ++root)
    {
        addRoot(_roots[root]);
    }
    for (uintptr_t address : _extraRoots)
    {
        addRoot(address);
    }

    std::vector<uint8_t> buffer(0x1000);
    for (size_t nodeIndex = 0; nodeIndex < nodes.size(); ++nodeIndex)
    {
        const Node current = nodes[nodeIndex]; // copy: the vector grows below
        if (current.depth >= WALK_DEPTH_LIMIT ||
            (current.depth >= INTERESTING_ONLY_DEPTH && !IsInteresting(current.entry->name)))
        {
            continue;
        }
        const size_t bytesRead = ReadUpTo(current.start, buffer.data(), SpanOf(current.entry->name));
        for (size_t offset = 4; offset + 4 <= bytesRead; offset += 4)
        {
            uint32_t value;
            memcpy(&value, &buffer[offset], 4);
            if (value >= _readOnlyDataStart && value < _readOnlyDataEnd)
            {
                // A vtable pointer inside the object: a base subobject (B) or an embedded member object (E).
                if (const ClassEntry* entry = ClassOf(value))
                {
                    LogEdge(
                        current,
                        offset,
                        entry->objectOffset == offset && strcmp(entry->name, current.entry->name) == 0 ? 'B' : 'E',
                        entry);
                }
                continue;
            }
            uintptr_t start;
            const ClassEntry* entry = ObjectAt(value, start);
            if (!entry)
            {
                continue;
            }
            LogEdge(current, offset, 'P', entry);
            if (nodes.size() < MAXIMUM_NODES && seen.Insert(start))
            {
                nodes.push_back({start, current.depth + 1, current.start, static_cast<uint32_t>(offset), entry});
            }
        }
    }

    _snapshotList.clear();
    unsigned soldierCount = 0, weaponCount = 0;
    for (const Node& node : nodes)
    {
        if (_snapshotList.size() >= MAXIMUM_SNAPSHOT_OBJECTS)
        {
            break;
        }
        if (node.depth > 0 && !IsInteresting(node.entry->name))
        {
            continue;
        }
        if (IsSoldier(node.entry->name) && ++soldierCount > MAXIMUM_SNAPSHOT_SOLDIERS)
        {
            continue;
        }
        if (IsWeapon(node.entry->name) && ++weaponCount > MAXIMUM_SNAPSHOT_WEAPONS)
        {
            continue;
        }
        _snapshotList.push_back(node);
    }
}

// ---- snapshots: 'SNP1', tick, frame, count, then per object: start, depth, from, fromOff, len, name[48], bytes ----
// (read by tools/snap.py)
FILE* _snapshotFile = nullptr;
uint64_t _snapshotBytes = 0;

void WriteSnapshot(uint32_t frame)
{
    if (!_snapshotFile || _snapshotBytes > MAXIMUM_SNAPSHOT_FILE_BYTES || _snapshotList.empty())
    {
        return;
    }
    std::vector<uint8_t> out;
    auto put = [&out](const void* data, size_t byteCount)
    {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        out.insert(out.end(), bytes, bytes + byteCount);
    };
    auto put32 = [&put](uint32_t value)
    {
        put(&value, 4);
    };
    put("SNP1", 4);
    put32(GetTickCount());
    put32(frame);
    put32(0); // object count, patched below
    uint32_t count = 0;
    std::vector<uint8_t> buffer(0x1000);
    for (const Node& node : _snapshotList)
    {
        uint32_t vtable;
        if (!ReadMemory(node.start, &vtable, 4))
        {
            continue;
        }
        // Compare by name: node.entry may be the entry for a secondary vtable of the same class.
        const ClassEntry* now = ClassOf(vtable);
        if (!now || strcmp(now->name, node.entry->name) != 0)
        {
            continue; // freed or reused
        }
        const size_t bytesRead = ReadUpTo(node.start, buffer.data(), SpanOf(node.entry->name));
        if (!bytesRead)
        {
            continue;
        }
        put32(static_cast<uint32_t>(node.start));
        put32(node.depth);
        put32(static_cast<uint32_t>(node.from));
        put32(node.fromOffset);
        put32(static_cast<uint32_t>(bytesRead));
        put(node.entry->name, sizeof(node.entry->name));
        put(buffer.data(), bytesRead);
        ++count;
    }
    memcpy(&out[12], &count, 4);
    fwrite(out.data(), 1, out.size(), _snapshotFile);
    fflush(_snapshotFile);
    _snapshotBytes += out.size();
}

// ---- scan of the exe's .data for static objects (S) and global pointers to objects (P) ----
KeySet _globalsSeen;
std::vector<uint16_t> _linesPerClass; // [class index * 2 + kind]

void ScanGlobals()
{
    const DWORD startTick = GetTickCount();
    BuildRegions();
    if (_linesPerClass.empty())
    {
        _linesPerClass.assign(_classes.size() * 2, 0);
    }
    uint32_t staticCount = 0, pointerCount = 0, newCount = 0;
    uintptr_t coveredBytes = 0;
    std::vector<uint32_t> buffer(0x4000);
    const std::vector<Region> regions = _regions; // .data is read in 64 KB blocks through ReadProcessMemory
    for (const Region& region : regions)
    {
        if (region.start < _dataStart || region.end > _dataEnd)
        {
            continue;
        }
        for (uintptr_t block = region.start; block < region.end; block += 0x10000)
        {
            const size_t byteCount =
                (region.end - block < 0x10000 ? region.end - block : 0x10000) & ~static_cast<size_t>(3);
            if (!byteCount || !ReadMemory(block, buffer.data(), byteCount))
            {
                continue;
            }
            coveredBytes += byteCount;
            for (size_t word = 0; word < byteCount / 4; ++word)
            {
                const uint32_t value = buffer[word];
                if (value < MINIMUM_OBJECT_ADDRESS || (value & 3))
                {
                    continue;
                }
                const uintptr_t address = block + word * 4;
                const ClassEntry* entry;
                char kind;
                if (value >= _readOnlyDataStart && value < _readOnlyDataEnd)
                {
                    entry = ClassOf(value);
                    if (!entry)
                    {
                        continue;
                    }
                    kind = 'S';
                    ++staticCount;
                }
                else
                {
                    uintptr_t start;
                    entry = ObjectAt(value, start);
                    if (!entry)
                    {
                        continue;
                    }
                    kind = 'P';
                    ++pointerCount;
                }
                if (IsCameraOrController(entry->name))
                {
                    const uintptr_t root = kind == 'S' ? address : value;
                    bool isKnown = false;
                    for (uintptr_t extraRoot : _extraRoots)
                    {
                        if (extraRoot == root)
                        {
                            isKnown = true;
                        }
                    }
                    if (!isKnown && _extraRoots.size() < MAXIMUM_EXTRA_ROOTS)
                    {
                        _extraRoots.push_back(root);
                    }
                }
                if (_globalsSeen.Size() > MAXIMUM_GLOBALS_SEEN ||
                    !_globalsSeen.Insert((static_cast<uint64_t>(address) << 32) | entry->vtable))
                {
                    continue;
                }
                ++newCount;
                // At most 4 lines per class and kind: pools and entity lists would flood the log otherwise.
                uint16_t& lines =
                    _linesPerClass[static_cast<size_t>(entry - _classes.data()) * 2 + (kind == 'S' ? 0 : 1)];
                if (lines >= MAXIMUM_LINES_PER_CLASS)
                {
                    continue;
                }
                ++lines;
                if (kind == 'S')
                {
                    Log::Info(
                        "survey",
                        "S rva=0x%x %s+0x%x",
                        static_cast<unsigned>(address - _exeBase),
                        entry->name,
                        entry->objectOffset);
                }
                else
                {
                    Log::Info(
                        "survey",
                        "P rva=0x%x -> %p %s+0x%x",
                        static_cast<unsigned>(address - _exeBase),
                        reinterpret_cast<void*>(static_cast<uintptr_t>(value)),
                        entry->name,
                        entry->objectOffset);
                }
            }
        }
    }
    Log::Info(
        "globals",
        "statics=%u pointers=%u new=%u, read 0x%x of 0x%x bytes of .data in %lu ms",
        staticCount,
        pointerCount,
        newCount,
        static_cast<unsigned>(coveredBytes),
        static_cast<unsigned>(_dataEnd - _dataStart),
        GetTickCount() - startTick);
}

DWORD _lastGraphTick = 0, _lastSnapshotTick = 0, _lastCallsTick = 0;
unsigned _scanCount = 0;

/// Runs whatever survey work is due (graph walk, .data scan, snapshot, call counts). Only one thread at a time; a
/// call while another runs returns at once.
void Tick(uint32_t frame, bool isForced)
{
    if (!_isReady || !TryAcquireSRWLockExclusive(&_tickLock))
    {
        return;
    }
    const DWORD now = GetTickCount();
    // The survey work causes small hitches, which are unpleasant in a headset: pause it while VR is on.
    if (Xr::IsStarted())
    {
        ReleaseSRWLockExclusive(&_tickLock);
        return;
    }
    if (now - _lastGraphTick >= GRAPH_INTERVAL_MILLISECONDS)
    {
        WalkGraph();
        _lastGraphTick = now;
    }
    // Two scans per session: 10 s and 60 s after the first camera or controller object shows up (i.e. in a match).
    if (_firstRootTick && _scanCount < 2 &&
        now - _firstRootTick >= (_scanCount ? SECOND_SCAN_DELAY_MILLISECONDS : FIRST_SCAN_DELAY_MILLISECONDS))
    {
        ++_scanCount;
        Log::Info("globals", "scan %u starting", _scanCount);
        ScanGlobals();
    }
    if (isForced || now - _lastSnapshotTick >= SNAPSHOT_INTERVAL_MILLISECONDS)
    {
        WriteSnapshot(frame);
        _lastSnapshotTick = now;
    }
    if (now - _lastCallsTick >= CALLS_LOG_INTERVAL_MILLISECONDS)
    {
        _lastCallsTick = now;
        std::string line = "calls";
        for (unsigned slotId = 0; slotId < _hookedSlotCount; ++slotId)
        {
            const LONG callCount = _callCounts[slotId];
            if (callCount == _previousCallCounts[slotId])
            {
                continue;
            }
            char item[80];
            snprintf(
                item,
                sizeof(item),
                " %s[%u]=%ld",
                _hookedSlots[slotId].className,
                _hookedSlots[slotId].slot,
                callCount - _previousCallCounts[slotId]);
            line += item;
            _previousCallCounts[slotId] = callCount;
        }
        Log::Info(
            "survey",
            "%s | frame %u roots %ld snapshotting %u objects",
            line.c_str(),
            frame,
            _rootCount,
            static_cast<unsigned>(_snapshotList.size()));
    }
    ReleaseSRWLockExclusive(&_tickLock);
}

/// Finds the exe's base, image end and its .text, .rdata and .data sections from the PE headers in memory.
/// @return false if a section is missing or lies outside the image.
bool FindSections()
{
    _exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(_exeBase);
    auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(_exeBase + dosHeader->e_lfanew);
    _imageEnd = _exeBase + ntHeaders->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(ntHeaders);
    for (unsigned section = 0; section < ntHeaders->FileHeader.NumberOfSections; ++section)
    {
        const uintptr_t start = _exeBase + sections[section].VirtualAddress,
                        end = start + sections[section].Misc.VirtualSize;
        if (memcmp(sections[section].Name, ".text\0\0\0", 8) == 0)
        {
            _textStart = start;
            _textEnd = end;
        }
        else if (memcmp(sections[section].Name, ".rdata\0\0", 8) == 0)
        {
            _readOnlyDataStart = start;
            _readOnlyDataEnd = end;
        }
        else if (memcmp(sections[section].Name, ".data\0\0\0", 8) == 0)
        {
            _dataStart = start;
            _dataEnd = end & ~static_cast<uintptr_t>(3);
        }
    }
    return _textStart && _readOnlyDataStart && _dataStart && _textEnd <= _imageEnd && _readOnlyDataEnd <= _imageEnd &&
           _dataEnd <= _imageEnd;
}

// Newest live root object of the given class, or 0.
uintptr_t RootOfClass(const char* className)
{
    for (LONG root = _rootCount - 1; root >= 0; --root)
    {
        uint32_t vtable;
        if (!ReadMemory(_roots[root], &vtable, 4))
        {
            continue;
        }
        const ClassEntry* entry = ClassOf(vtable);
        if (entry && entry->objectOffset == 0 && strcmp(entry->name, className) == 0)
        {
            return _roots[root];
        }
    }
    return 0;
}

} // namespace

uintptr_t Resolve(const char* name)
{
    if (!_isReady)
    {
        return 0;
    }
    const uintptr_t cameraManager = RootOfClass("CameraManager");
    if (!strcmp(name, "cam"))
    {
        return cameraManager;
    }
    if (!strcmp(name, "red"))
    {
        return cameraManager ? cameraManager + OFFSET_cameraManagerRedCamera : 0;
    }
    if (!strcmp(name, "chase"))
    {
        return cameraManager ? cameraManager + OFFSET_cameraManagerChaseCamera : 0;
    }
    const uintptr_t playerController = RootOfClass("PlayerController");
    if (!strcmp(name, "pc"))
    {
        return playerController;
    }
    uint32_t controllable = 0;
    if (!playerController || !ReadMemory(playerController + OFFSET_playerControllerControllable, &controllable, 4) ||
        !controllable)
    {
        return 0;
    }
    // The Controllable is a subobject inside the soldier: step back by its offset to the soldier's start.
    const uintptr_t soldier = controllable - OFFSET_soldierControllable;
    if (!strcmp(name, "soldier"))
    {
        return soldier;
    }
    if (!strcmp(name, "aimer"))
    {
        return soldier + OFFSET_soldierAimer;
    }
    if (!strcmp(name, "weapon"))
    {
        uint32_t weapon = 0;
        return ReadMemory(soldier + OFFSET_soldierAimer + OFFSET_aimerWeapon, &weapon, 4) ? weapon : 0;
    }
    if (!strcmp(name, "unit"))
    {
        // The player's unit as the killer line reads a killer's (offsets "Killer"): [[soldier+0x28]+0xB4].
        uint32_t character = 0, unit = 0;
        return ReadMemory(soldier + 0x28, &character, 4) && character && ReadMemory(character + 0xB4, &unit, 4) ? unit
                                                                                                                : 0;
    }
    return 0;
}

void OnPresent(uint32_t frame, bool forceSnapshot)
{
    FpGuard fp;
    _lastPresentTick = GetTickCount();
    Tick(frame, forceSnapshot);
}

void Install()
{
    if (!FindSections())
    {
        Log::Warning("survey", "exe sections not found, engine survey disabled");
        return;
    }
    BuildClassTable();
    Log::Info(
        "survey",
        "base %p text +0x%x..0x%x rdata +0x%x..0x%x data +0x%x..0x%x, %u vtables with RTTI",
        reinterpret_cast<void*>(_exeBase),
        static_cast<unsigned>(_textStart - _exeBase),
        static_cast<unsigned>(_textEnd - _exeBase),
        static_cast<unsigned>(_readOnlyDataStart - _exeBase),
        static_cast<unsigned>(_readOnlyDataEnd - _exeBase),
        static_cast<unsigned>(_dataStart - _exeBase),
        static_cast<unsigned>(_dataEnd - _exeBase),
        static_cast<unsigned>(_classes.size()));
    // One page of executable memory for the thunks (each is 25 bytes).
    uint8_t* thunkMemory =
        static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!thunkMemory)
    {
        Log::Error("survey", "VirtualAlloc failed");
        return;
    }
    uint8_t* cursor = thunkMemory;
    for (const VtableHook& hook : VTABLE_HOOKS)
    {
        const uintptr_t vtableAddress = _exeBase + hook.vtableRva;
        const ClassEntry* entry = ClassOf(static_cast<uint32_t>(vtableAddress));
        if (!entry || strcmp(entry->name, hook.className) != 0 || entry->objectOffset != 0)
        {
            Log::Info("survey", "vtable for %s not at +0x%x in this exe, skipped", hook.className, hook.vtableRva);
            continue;
        }
        void** vtable = reinterpret_cast<void**>(vtableAddress);
        unsigned hookedCount = 0;
        for (uint32_t slot = 0; slot < hook.slotCount && _hookedSlotCount < MAXIMUM_SLOTS; ++slot)
        {
            const uintptr_t original = reinterpret_cast<uintptr_t>(vtable[slot]);
            if (original < _textStart || original >= _textEnd)
            {
                break;
            }
            const unsigned slotId = _hookedSlotCount++;
            _hookedSlots[slotId] = {hook.className, slot};
            uint8_t* thunk = cursor;
            cursor = EmitThunk(cursor, slotId, original);
            if (Main::PatchPointer(&vtable[slot], thunk))
            {
                ++hookedCount;
            }
        }
        Log::Info("survey", "%s hooked %u/%u slots", hook.className, hookedCount, hook.slotCount);
    }
    FlushInstructionCache(GetCurrentProcess(), thunkMemory, 4096);
    _snapshotFile = Main::OpenModFile("swbf-vr-snap.bin", "wb");
    const DWORD now = GetTickCount();
    _lastGraphTick = _lastSnapshotTick = _lastCallsTick = _lastPresentTick = now;
    _isReady = true;
}

} // namespace EngineSurvey

} // namespace SwbfVr
