// Records fatal exceptions to swbf-vr-crash.log: code, faulting module+offset, registers, and the
// code addresses found on the stack. Uses only Win32 calls and no locks, so it works mid-crash.

#include "Crash.h"

#include "GpuTransfer.h"

#include "Main.h"

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstring>

namespace SwbfVr
{

namespace Crash
{

// `extern "C"` declares a symbol by its plain C name (no namespace in it): the linker defines __ImageBase.
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming): the linker's name
extern "C" IMAGE_DOS_HEADER __ImageBase; // start of this DLL, provided by the linker

namespace
{

HANDLE _file = INVALID_HANDLE_VALUE;
// First-chance exceptions are reported once per faulting address, so a benign repeating one cannot use up the budget.
constexpr LONG MAXIMUM_REPORTED_SITES = 40;
uintptr_t _reportedSites[MAXIMUM_REPORTED_SITES];
// volatile: changed by several threads at once (through InterlockedIncrement), so every read goes to memory.
volatile LONG _reportedSiteCount = 0;

/// wvsprintfA writes at most 1024 characters.
constexpr int LINE_BUFFER_BYTES = 1100;
/// Stack words copied for a backtrace (1600 bytes), and at most this many code addresses printed from them.
constexpr unsigned STACK_COPY_WORDS = 400;
constexpr unsigned MAXIMUM_BACKTRACE_LINES = 28;
/// Bytes per stack word (32-bit process).
constexpr unsigned STACK_WORD_BYTES = 4;
/// Values below this are small numbers, never code addresses (the lowest 64 KB are never mapped).
constexpr uint32_t LOWEST_CODE_ADDRESS = 0x10000;

/// Exception codes without a name in the Windows headers we include.
constexpr DWORD CPP_EXCEPTION_CODE = 0xE06D7363;
constexpr DWORD HEAP_CORRUPTION_CODE = 0xC0000374;
constexpr DWORD FAST_FAIL_CODE = 0xC0000409;
/// The floating-point exceptions, EXCEPTION_FLT_DENORMAL_OPERAND to EXCEPTION_FLT_UNDERFLOW.
constexpr DWORD FIRST_FLOAT_EXCEPTION_CODE = 0xC000008C;
constexpr DWORD LAST_FLOAT_EXCEPTION_CODE = 0xC0000093;

/// The no-frame watcher: checks twice a second, reports after 4 s without a frame, at most 4 reports per run,
/// three samples 150 ms apart per report.
constexpr DWORD WATCH_INTERVAL_MILLISECONDS = 500;
constexpr DWORD HANG_MILLISECONDS = 4000;
constexpr unsigned MAXIMUM_HANG_REPORTS = 4;
constexpr int HANG_SAMPLE_COUNT = 3;
constexpr DWORD SAMPLE_INTERVAL_MILLISECONDS = 150;

// printf-style write to the crash log (wvsprintfA: no floating point, but no C runtime either).
void Write(const char* format, ...)
{
    char buffer[LINE_BUFFER_BYTES];
    va_list arguments;
    va_start(arguments, format);
    const int length = wvsprintfA(buffer, format, arguments);
    va_end(arguments);
    DWORD written = 0;
    if (length > 0)
    {
        WriteFile(_file, buffer, static_cast<DWORD>(length), &written, nullptr);
    }
}

// If address is inside a loaded module, returns its file name and base.
bool FindModule(uintptr_t address, char* name, size_t nameSize, uintptr_t& base, bool& isExecutable)
{
    MEMORY_BASIC_INFORMATION memoryInfo;
    if (!VirtualQuery(reinterpret_cast<void*>(address), &memoryInfo, sizeof(memoryInfo)) ||
        memoryInfo.Type != MEM_IMAGE || memoryInfo.State != MEM_COMMIT)
    {
        return false;
    }
    char path[MAX_PATH];
    if (!GetModuleFileNameA(static_cast<HMODULE>(memoryInfo.AllocationBase), path, MAX_PATH))
    {
        return false;
    }
    const char* slash = strrchr(path, '\\');
    lstrcpynA(name, slash ? slash + 1 : path, static_cast<int>(nameSize));
    base = reinterpret_cast<uintptr_t>(memoryInfo.AllocationBase);
    isExecutable = (memoryInfo.Protect &
                    (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
    return true;
}

// One line: the label, the address and, inside a module, module+offset.
void Describe(const char* label, uintptr_t address)
{
    char name[64];
    uintptr_t base = 0;
    bool isExecutable = false;
    if (FindModule(address, name, sizeof(name), base, isExecutable))
    {
        Write(
            "%s %08lX %s+0x%lX\r\n",
            label,
            static_cast<unsigned long>(address),
            name,
            static_cast<unsigned long>(address - base));
    }
    else
    {
        Write("%s %08lX\r\n", label, static_cast<unsigned long>(address));
    }
}

// Copies up to maximumCount stack words from stackPointer on, stopping at the first unreadable one.
unsigned CopyStack(uintptr_t stackPointer, uint32_t* words, unsigned maximumCount)
{
    unsigned count = 0;
    for (; count < maximumCount; ++count)
    {
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(
                GetCurrentProcess(),
                reinterpret_cast<const void*>(stackPointer + count * STACK_WORD_BYTES),
                &words[count],
                STACK_WORD_BYTES,
                &bytesRead) ||
            bytesRead != STACK_WORD_BYTES)
        {
            break;
        }
    }
    return count;
}

// Rough backtrace: every stack word that points into executable module code.
void Backtrace(const uint32_t* words, unsigned count)
{
    unsigned found = 0;
    for (unsigned word = 0; word < count && found < MAXIMUM_BACKTRACE_LINES; ++word)
    {
        char name[64];
        uintptr_t base = 0;
        bool isExecutable = false;
        if (words[word] < LOWEST_CODE_ADDRESS || !FindModule(words[word], name, sizeof(name), base, isExecutable) ||
            !isExecutable)
        {
            continue;
        }
        Write(
            "  [esp+0x%03X] %08lX %s+0x%lX\r\n",
            word * STACK_WORD_BYTES,
            static_cast<unsigned long>(words[word]),
            name,
            static_cast<unsigned long>(words[word] - base));
        ++found;
    }
}

bool IsFatal(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_GUARD_PAGE:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
    case CPP_EXCEPTION_CODE:   // C++ exception
    case HEAP_CORRUPTION_CODE: // heap corruption
    case FAST_FAIL_CODE:       // stack buffer overrun / fast fail
        return true;
    default:
        break;
    }
    return code >= FIRST_FLOAT_EXCEPTION_CODE && code <= LAST_FLOAT_EXCEPTION_CODE; // floating-point exceptions
}

// The exception's code, address, registers and backtrace.
void Report(const char* kind, EXCEPTION_POINTERS* exception)
{
    const EXCEPTION_RECORD* record = exception->ExceptionRecord;
    const CONTEXT* context = exception->ContextRecord;
    Write(
        "\r\n%s exception %08lX tick %lu thread %lu\r\n",
        kind,
        record->ExceptionCode,
        GetTickCount(),
        GetCurrentThreadId());
    Describe(" at", reinterpret_cast<uintptr_t>(record->ExceptionAddress));
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
    {
        // ExceptionInformation[0]: 0 read, 1 write, 8 execute (DEP); [1]: the address that was accessed.
        Write(
            " %s address %08lX\r\n",
            record->ExceptionInformation[0] == 1   ? "writing"
            : record->ExceptionInformation[0] == 8 ? "executing"
                                                   : "reading",
            static_cast<unsigned long>(record->ExceptionInformation[1]));
    }
    Write(
        " eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX eip=%08lX\r\n",
        context->Eax,
        context->Ebx,
        context->Ecx,
        context->Edx,
        context->Esi,
        context->Edi,
        context->Ebp,
        context->Esp,
        context->Eip);
    if (record->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
    {
        return; // almost no stack left: stop here
    }
    uint32_t words[STACK_COPY_WORDS];
    Backtrace(words, CopyStack(context->Esp, words, STACK_COPY_WORDS));
}

// A second kind of failure: the game stops producing frames without crashing. A watcher thread notices that
// the render thread's heartbeat (one per presented frame) has stopped and records where that thread is.
volatile LONG _heartbeatCount = 0;
volatile DWORD _heartbeatThreadId = 0;

// The watcher thread's function (WINAPI: the stdcall convention CreateThread expects).
DWORD WINAPI WatchForHang(void*)
{
    LONG lastHeartbeat = 0;
    DWORD unchangedSinceTick = GetTickCount();
    bool isReported = false;
    unsigned reportCount = 0;
    for (;;)
    {
        Sleep(WATCH_INTERVAL_MILLISECONDS);
        const LONG heartbeat = _heartbeatCount;
        if (heartbeat != lastHeartbeat)
        {
            lastHeartbeat = heartbeat;
            unchangedSinceTick = GetTickCount();
            isReported = false;
            continue;
        }
        if (!heartbeat || isReported || reportCount >= MAXIMUM_HANG_REPORTS ||
            GetTickCount() - unchangedSinceTick < HANG_MILLISECONDS)
        {
            continue;
        }
        isReported = true;
        ++reportCount;
        HANDLE thread = OpenThread(
            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
            FALSE,
            _heartbeatThreadId);
        if (!thread)
        {
            continue;
        }
        Write("\r\nNO FRAME for 4 s (tick %lu): where the render thread is, three samples\r\n", GetTickCount());
        GpuTransfer::LogState("no frame for 4 s");
        for (int sample = 0; sample < HANG_SAMPLE_COUNT; ++sample)
        { // several samples tell a loop from a wait
            static uint32_t words[STACK_COPY_WORDS];
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            unsigned count = 0;
            bool isCaptured = false;
            if (SuspendThread(thread) != static_cast<DWORD>(-1))
            {
                // Only copy while it is suspended (it might hold a lock that describing addresses needs).
                isCaptured = GetThreadContext(thread, &context) != 0;
                if (isCaptured)
                {
                    count = CopyStack(context.Esp, words, STACK_COPY_WORDS);
                }
                ResumeThread(thread);
            }
            if (isCaptured)
            {
                Describe(" at", context.Eip);
                Write(
                    " eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX\r\n",
                    context.Eax,
                    context.Ebx,
                    context.Ecx,
                    context.Edx,
                    context.Esi,
                    context.Edi,
                    context.Ebp,
                    context.Esp);
                Backtrace(words, count);
            }
            Sleep(SAMPLE_INTERVAL_MILLISECONDS);
        }
        CloseHandle(thread);
    }
}

// Vectored handler: sees every exception first, before the game's own handlers (which may swallow it). Fatal kinds
// are reported once per faulting address; the search always continues, so behaviour is unchanged.
LONG CALLBACK OnFirstChance(EXCEPTION_POINTERS* exception)
{
    if (!IsFatal(exception->ExceptionRecord->ExceptionCode))
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const uintptr_t site = reinterpret_cast<uintptr_t>(exception->ExceptionRecord->ExceptionAddress);
    const LONG knownCount = _reportedSiteCount < MAXIMUM_REPORTED_SITES ? _reportedSiteCount : MAXIMUM_REPORTED_SITES;
    for (LONG index = 0; index < knownCount; ++index)
    {
        if (_reportedSites[index] == site)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    const LONG slot = InterlockedIncrement(&_reportedSiteCount) - 1;
    if (slot >= MAXIMUM_REPORTED_SITES)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    _reportedSites[slot] = site;
    Report("first-chance", exception);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Last-chance filter: the exception reached the top without a handler; the process ends after this.
LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* exception)
{
    Report("UNHANDLED", exception);
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void Install(const char* buildId)
{
    char path[MAX_PATH];
    wsprintfA(path, "%sswbf-vr-crash.log", Main::ModDirectory());
    _file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (_file == INVALID_HANDLE_VALUE)
    {
        return;
    }
    Write(
        "swbf-vr build %s, exe base %08lX, dinput8 proxy base %08lX\r\n",
        buildId,
        static_cast<unsigned long>(reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr))),
        static_cast<unsigned long>(reinterpret_cast<uintptr_t>(&__ImageBase)));
    AddVectoredExceptionHandler(1, OnFirstChance);
    SetUnhandledExceptionFilter(OnUnhandled);
    if (HANDLE watcher = CreateThread(nullptr, 0, WatchForHang, nullptr, 0, nullptr))
    {
        CloseHandle(watcher);
    }
}

void Heartbeat()
{
    _heartbeatThreadId = GetCurrentThreadId();
    InterlockedIncrement(&_heartbeatCount);
}

} // namespace Crash

} // namespace SwbfVr
