// Foliage: the prop generator's cluster pool always at its cap (Foliage.h).
#include "Foliage.h"

#include "GameBuild.h"
#include "Log.h"

#include <windows.h>

#include <cstdint>

namespace SwbfVr
{

namespace Foliage
{

namespace
{

/// The jump past the cap (0x201B52, docs/offsets.md "Prop generator"): jle rel8 (7E 0F) keeps a pool size of at most
/// 254; two NOPs set 254 always.
constexpr uint8_t CAP_JUMP[2] = {0x7E, 0x0F};
constexpr uint8_t NO_OPERATIONS[2] = {0x90, 0x90};

bool _isDone = false;

} // namespace

void BeforeFrame()
{
    if (_isDone)
    {
        return;
    }
    _isDone = true;
    if (!GameBuild::IsAvailable(GameBuild::Feature::Foliage))
    {
        return;
    }
    uint8_t* const site = reinterpret_cast<uint8_t*>(GameBuild::Absolute(GameBuild::Address::PropPoolCapJump));
    if (site[0] != CAP_JUMP[0] || site[1] != CAP_JUMP[1])
    {
        Log::Info("foliage", "prop pool cap jump holds %02x %02x, not patched", site[0], site[1]);
        return;
    }
    DWORD oldProtection = 0;
    if (!VirtualProtect(site, sizeof(NO_OPERATIONS), PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        return;
    }
    memcpy(site, NO_OPERATIONS, sizeof(NO_OPERATIONS));
    VirtualProtect(site, sizeof(NO_OPERATIONS), oldProtection, &oldProtection);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(NO_OPERATIONS));
    Log::Info("foliage", "prop cluster pool: 254 (the game's 66)");
}

} // namespace Foliage

} // namespace SwbfVr
