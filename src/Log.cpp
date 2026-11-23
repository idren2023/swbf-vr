// The mod's log file, swbf-vr.log, and the header written at its start (CODE-STYLE section 6).

#include "Log.h"

#include "Main.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace SwbfVr
{

namespace Log
{

namespace
{

/// swbf-vr.log, opened by the first line written.
FILE* _file = nullptr;
/// Serialises writers: lines come from the render thread, the game's main thread and the crash handler.
SRWLOCK _lock = SRWLOCK_INIT;
bool _isDebugEnabled = false;

/// Old logs kept in swbf-vr-logs: 40 runs, two files (log and crash log) each.
constexpr unsigned KEPT_LOG_FILE_COUNT = 80;
/// At most this many file names are read from swbf-vr-logs, each shorter than MAXIMUM_LOG_NAME_LENGTH.
constexpr unsigned MAXIMUM_LISTED_LOG_COUNT = 256;
constexpr unsigned MAXIMUM_LOG_NAME_LENGTH = 48;

// Before this run's log replaces the last one: the previous run's log and crash log move to
// GameData\swbf-vr-logs\<time the old log was last written>-swbf-vr.log / -crash.log. The newest 40 runs are kept.
void KeepOldLogs()
{
    const char* const directory = Main::ModDirectory();
    char logPath[MAX_PATH], crashPath[MAX_PATH], archiveDirectory[MAX_PATH], path[MAX_PATH];
    snprintf(logPath, sizeof(logPath), "%sswbf-vr.log", directory);
    snprintf(crashPath, sizeof(crashPath), "%sswbf-vr-crash.log", directory);
    snprintf(archiveDirectory, sizeof(archiveDirectory), "%sswbf-vr-logs", directory);
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExA(logPath, GetFileExInfoStandard, &attributes) ||
        (attributes.nFileSizeLow == 0 && attributes.nFileSizeHigh == 0))
    {
        return;
    }
    FILETIME localFileTime{};
    SYSTEMTIME time{};
    FileTimeToLocalFileTime(&attributes.ftLastWriteTime, &localFileTime);
    FileTimeToSystemTime(&localFileTime, &time);
    CreateDirectoryA(archiveDirectory, nullptr);
    char stamp[32];
    snprintf(
        stamp,
        sizeof(stamp),
        "%04u%02u%02u-%02u%02u%02u",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond);
    snprintf(path, sizeof(path), "%s\\%s-swbf-vr.log", archiveDirectory, stamp);
    MoveFileExA(logPath, path, MOVEFILE_REPLACE_EXISTING);
    snprintf(path, sizeof(path), "%s\\%s-crash.log", archiveDirectory, stamp);
    MoveFileExA(crashPath, path, MOVEFILE_REPLACE_EXISTING);
    // Names sort by time. Delete the oldest files beyond 40 runs (80 files).
    static char names[MAXIMUM_LISTED_LOG_COUNT][MAXIMUM_LOG_NAME_LENGTH];
    unsigned count = 0;
    snprintf(path, sizeof(path), "%s\\*.log", archiveDirectory);
    WIN32_FIND_DATAA found{};
    HANDLE search = FindFirstFileA(path, &found);
    if (search == INVALID_HANDLE_VALUE)
    {
        return;
    }
    do
    {
        if (count < MAXIMUM_LISTED_LOG_COUNT && strlen(found.cFileName) < MAXIMUM_LOG_NAME_LENGTH)
        {
            strcpy_s(names[count++], found.cFileName);
        }
    } while (FindNextFileA(search, &found));
    FindClose(search);
    while (count > KEPT_LOG_FILE_COUNT)
    {
        unsigned oldest = 0;
        for (unsigned index = 1; index < count; ++index)
        {
            if (strcmp(names[index], names[oldest]) < 0)
            {
                oldest = index;
            }
        }
        snprintf(path, sizeof(path), "%s\\%s", archiveDirectory, names[oldest]);
        DeleteFileA(path);
        memcpy(names[oldest], names[--count], sizeof(names[0]));
    }
}

// One line: tick count, level padded to 5 characters, subsystem, text. The file is flushed after every line so a
// crash loses nothing.
void Write(const char* level, const char* subsystem, const char* format, va_list arguments)
{
    AcquireSRWLockExclusive(&_lock);
    if (!_file)
    {
        KeepOldLogs();
        _file = Main::OpenModFile("swbf-vr.log", "w");
    }
    if (_file)
    {
        fprintf(_file, "%lu %-5s %s: ", GetTickCount(), level, subsystem);
        vfprintf(_file, format, arguments);
        fputc('\n', _file);
        fflush(_file);
    }
    ReleaseSRWLockExclusive(&_lock);
}

} // namespace

// Each function below gathers its "..." arguments into a va_list (C's way of passing on a variable argument list,
// like C# params object[]) and hands them to Write.
void Debug(const char* subsystem, const char* format, ...)
{
    if (!_isDebugEnabled)
    {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    Write("DEBUG", subsystem, format, arguments);
    va_end(arguments);
}

void Info(const char* subsystem, const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    Write("INFO", subsystem, format, arguments);
    va_end(arguments);
}

void Warning(const char* subsystem, const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    Write("WARN", subsystem, format, arguments);
    va_end(arguments);
}

void Error(const char* subsystem, const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    Write("ERROR", subsystem, format, arguments);
    va_end(arguments);
}

void SetDebugEnabled(bool isEnabled)
{
    if (isEnabled != _isDebugEnabled)
    {
        Info("log", "debug lines %s", isEnabled ? "on" : "off");
    }
    _isDebugEnabled = isEnabled;
}

void WriteStartupHeader(const char* buildId)
{
    Info("init", "swbf-vr build %s", buildId);

    // RtlGetVersion gives the real Windows version; GetVersionEx answers what the game's manifest claims.
    struct VersionInfo
    {
        ULONG size, major, minor, build, platform;
        WCHAR servicePack[128];
    } version{sizeof(VersionInfo), 0, 0, 0, 0, {}};

    using RtlGetVersionFunction = LONG(WINAPI*)(VersionInfo*);
    const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFunction>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion")));
    if (rtlGetVersion && rtlGetVersion(&version) == 0)
    {
        Info("init", "windows: %lu.%lu build %lu", version.major, version.minor, version.build);
    }
}

} // namespace Log

} // namespace SwbfVr
