#pragma once

/// The DLL's entry points and shared services (Main.cpp): files next to the DLL, memory patching (Main), and the
/// per-stage frame timer (Profiling). DllMain and the DirectInput8Create export live in Main.cpp under their global
/// names.

#include <windows.h>

#include <cstdio>

namespace SwbfVr
{

namespace Main
{

/// Opens a file in the folder that contains the DLL (the game's GameData folder), shared so that other programs can
/// read it while the game runs.
/// @param name the file name, without a folder.
/// @param mode the fopen mode ("r", "w", "wb", ...).
/// @return the open file, null if it cannot be opened. The caller closes it with fclose.
FILE* OpenModFile(const char* name, const char* mode);

/// The folder that contains the DLL.
/// @return the folder's path, ending with a backslash (empty if it could not be found).
const char* ModDirectory();

/// Overwrites one pointer in read-only memory (vtable or import table entry).
/// @param slot the pointer to overwrite.
/// @param value the new value.
/// @return false when the memory's protection cannot be changed.
bool PatchPointer(void** slot, void* value);

} // namespace Main

namespace Profiling
{

/// Time per stage of a frame. Reported every 2 s as a "time:" log line while VR runs.
enum class Stage
{
    Wait,
    Begin,
    Locate,
    Readback,
    Acquire,
    Upload,
    Release,
    End,
    RenderLeft,
    RenderRight,
    RenderFlat,
    RenderScope,
    Present,
    GpuCopy, // GpuTransfer::Stage and Transfer: our copies of the images on the card (CPU time to issue it)
    Count
};

/// Adds one measured call to a stage.
/// @param stage the stage.
/// @param milliseconds how long the call took.
void Add(Stage stage, double milliseconds);

/// Logs the "time:" line (average milliseconds per frame and longest call per stage, which window is in front) and
/// starts the sums again. Nothing is logged when no stage was measured.
/// @param frames the frames presented since the last report.
void Report(unsigned frames);

/// RAII timer (an object that does its work in its destructor): starts when created and adds the time to its stage
/// when it goes out of scope, at the closing brace of the block that holds it.
struct Scope
{
    Stage stage;
    LARGE_INTEGER start;

    /// @param measuredStage the stage the time is added to.
    explicit Scope(Stage measuredStage) : stage(measuredStage)
    {
        QueryPerformanceCounter(&start);
    }

    ~Scope()
    {
        static LARGE_INTEGER frequency;
        if (!frequency.QuadPart)
        {
            QueryPerformanceFrequency(&frequency);
        }
        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);
        Add(stage,
            static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart));
    }
};

} // namespace Profiling

} // namespace SwbfVr
