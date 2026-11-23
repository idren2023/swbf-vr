// Net: what the mod knows about a network game (Net.h, docs/research/netcode.md).

#include "Net.h"

#include "GameBuild.h"
#include "Log.h"

#include <MinHook.h>
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace SwbfVr
{

namespace Net
{

namespace
{
// Steam build (RVAs), from the static traces of 2026-11-10 (docs/research/net/).
/// Byte: a network game is running.
constexpr uintptr_t NET_GAME_FLAG = 0x506BF2;
/// Byte: the client world is the active one.
constexpr uintptr_t CLIENT_WORLD_FLAG = 0x506F60;
/// Byte: this machine is the host.
constexpr uintptr_t HOST_FLAG = 0x503A10;
/// Byte: the turn being simulated is simulated for the first time (not a replay after a host update).
constexpr uintptr_t FIRST_SIMULATION_FLAG = 0x506F61;
/// int[2]: the network player id of local player 0 and 1 (-1: none).
constexpr uintptr_t LOCAL_PLAYER_IDS = 0x505620;
/// TeamManager* of the host world (the only one in single player) and of the client world (null outside a net game).
constexpr uintptr_t HOST_TEAM_MANAGER = 0x62EA54;
constexpr uintptr_t CLIENT_TEAM_MANAGER = 0x62EA58;
/// TeamManager: Character count and array; Character size, its player id and its unit.
constexpr uintptr_t TEAM_MANAGER_CHARACTER_COUNT = 0x38;
constexpr uintptr_t TEAM_MANAGER_CHARACTERS = 0x3C;
constexpr uintptr_t CHARACTER_SIZE = 0xE8;
constexpr uintptr_t CHARACTER_UNIT = 0xB4;
/// EntitySoldier: main vtable, the Controllable base, world transform (rows left, up, forward, position), pitch.
constexpr uintptr_t SOLDIER_VTABLE = 0x416454;
constexpr uintptr_t SOLDIER_CONTROLLABLE = 0x250;
constexpr uintptr_t SOLDIER_TRANSFORM = 0x1B0;
constexpr uintptr_t SOLDIER_PITCH = 0x8D8;
/// EntitySoldier: its embedded Aimer; Aimer: fire origin and direction (3 floats each, Aimer::Set writes them).
constexpr uintptr_t SOLDIER_AIMER = 0x8F0;
constexpr uintptr_t AIMER_ORIGIN = 0x7C;
constexpr uintptr_t AIMER_DIRECTION = 0x48;
/// Controllable: strafe, move, turn, pitch as PlayerController set them.
constexpr uintptr_t CONTROLLABLE_AXES = 0x54;
/// Controllable: the point its aim ray ends at (3 floats).
constexpr uintptr_t CONTROLLABLE_TARGET = 0x10;

/// void __cdecl CopyControlState(ControlState* destination, const ControlState* source): 20 bytes copied. Three
/// callers: AdvanceTurn 0x1AF5E0 for local player 0 (the call at 0x1AF751: from the input mapper's state at
/// 0x4E64F4 into this turn's slot of the move ring 0x507440, only in a network game) and for local player 1
/// (0x1AF7A3, split screen), and the host's turn advance 0x1A6F20 (0x1A6F5C, only in single player).
constexpr uintptr_t COPY_CONTROL_STATE = 0x1AF320;
/// The instruction after AdvanceTurn's call for local player 0.
constexpr uintptr_t COPY_LOCAL_PLAYER_RETURN = 0x1AF756;
/// The function's bytes, checked before hooking.
constexpr uint8_t COPY_CONTROL_STATE_CODE[] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x0C, 0x8B, 0x4D, 0x08, 0x0F, 0x10, 0x00,
                                               0x0F, 0x11, 0x01, 0x8B, 0x40, 0x10, 0x89, 0x41, 0x10, 0x5D, 0xC3};

using CopyControlStateFunction = void(__cdecl*)(ControlState* destination, const ControlState* source);
/// The game's copy behind our hook (MinHook's trampoline).
CopyControlStateFunction _originalCopyControlState = nullptr;
ControlStateWriter _controlWriter = nullptr;
bool _isTraceOn = false;
/// Buttons to press in the next turn (PressButton), and those pressed in the last one (a press needs a turn without
/// the bit before it: the game acts on the rising edge).
uint32_t _pendingButtons = 0, _lastPressedButtons = 0;
/// float: seconds per game turn of the network game (1 / turns per second).
constexpr uintptr_t TURN_SECONDS = 0x507064;
// The world ray cast (docs/offsets.md) and the collision mask a soldier's aim ray uses.
constexpr uintptr_t WORLD_RAY_CAST = 0xA40C0;
constexpr unsigned AIM_RAY_MASK = 0x4E;
bool _wasControlHookTried = false;

uintptr_t Base()
{
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
}

bool IsReadable(uintptr_t address, size_t size)
{
    return address > 0x10000 && !IsBadReadPtr(reinterpret_cast<const void*>(address), size);
}

uint8_t FlagAt(uintptr_t rva)
{
    return GameBuild::IsKnownBuild() ? *reinterpret_cast<const uint8_t*>(Base() + rva) : 0;
}

/// The local player's unit (what its Character controls) in the world of that TeamManager, or 0.
uintptr_t FindLocalUnit(uintptr_t teamManagerRva)
{
    const int* playerIds = reinterpret_cast<const int*>(Base() + LOCAL_PLAYER_IDS);
    const int playerId = playerIds[0] >= 0 ? playerIds[0] : 0;
    const uintptr_t teamManager = *reinterpret_cast<const uintptr_t*>(Base() + teamManagerRva);
    if (!IsReadable(teamManager, TEAM_MANAGER_CHARACTERS + 4))
    {
        return 0;
    }
    const int count = *reinterpret_cast<const int*>(teamManager + TEAM_MANAGER_CHARACTER_COUNT);
    const uintptr_t characters = *reinterpret_cast<const uintptr_t*>(teamManager + TEAM_MANAGER_CHARACTERS);
    if (playerId >= count || !IsReadable(characters + playerId * CHARACTER_SIZE, CHARACTER_SIZE))
    {
        return 0;
    }
    return *reinterpret_cast<const uintptr_t*>(characters + playerId * CHARACTER_SIZE + CHARACTER_UNIT);
}

/// The local player's soldier in the world of that TeamManager, or 0 (no character, no unit, not a soldier).
uintptr_t FindLocalSoldier(uintptr_t teamManagerRva)
{
    const int* playerIds = reinterpret_cast<const int*>(Base() + LOCAL_PLAYER_IDS);
    const int playerId = playerIds[0] >= 0 ? playerIds[0] : 0;
    const uintptr_t teamManager = *reinterpret_cast<const uintptr_t*>(Base() + teamManagerRva);
    if (!IsReadable(teamManager, TEAM_MANAGER_CHARACTERS + 4))
    {
        return 0;
    }
    const int count = *reinterpret_cast<const int*>(teamManager + TEAM_MANAGER_CHARACTER_COUNT);
    const uintptr_t characters = *reinterpret_cast<const uintptr_t*>(teamManager + TEAM_MANAGER_CHARACTERS);
    if (playerId >= count || !IsReadable(characters + playerId * CHARACTER_SIZE, CHARACTER_SIZE))
    {
        return 0;
    }
    const uintptr_t unit =
        *reinterpret_cast<const uintptr_t*>(characters + playerId * CHARACTER_SIZE + CHARACTER_UNIT);
    // The unit pointer may be the entity or its Controllable base: take the one that is a soldier.
    for (const uintptr_t candidate : {unit, unit - SOLDIER_CONTROLLABLE})
    {
        if (IsReadable(candidate, 0xB50) && *reinterpret_cast<const uintptr_t*>(candidate) == Base() + SOLDIER_VTABLE)
        {
            return candidate;
        }
    }
    return 0;
}

/// Logs the local player's soldier of one world.
void LogWorld(const char* name, uintptr_t teamManagerRva, int playerId)
{
    const uintptr_t soldier = FindLocalSoldier(teamManagerRva);
    if (!soldier)
    {
        // In a seat: the unit is the seat's Controllable (aim direction at +0xEC) or its entity. Both readings are
        // logged; the one that is no position shows as nonsense. For comparing the two worlds in a test.
        const uintptr_t unit = FindLocalUnit(teamManagerRva);
        if (!IsReadable(unit, 0x250))
        {
            Log::Info("net", "%s world: player %d has no unit", name, playerId);
            return;
        }
        const float* aim = reinterpret_cast<const float*>(unit + 0xEC);
        const float* inputs = reinterpret_cast<const float*>(unit + CONTROLLABLE_AXES);
        const float* asEntity = reinterpret_cast<const float*>(unit + SOLDIER_TRANSFORM + 0x30);
        const float* asControllable = reinterpret_cast<const float*>(unit - SOLDIER_CONTROLLABLE + SOLDIER_TRANSFORM + 0x30);
        const bool hasBefore = IsReadable(unit - SOLDIER_CONTROLLABLE, 0x250);
        Log::Info(
            "net",
            "%s world: unit %p (vtable rva 0x%X) aim heading %.1f pitch %.1f inputs %.2f %.2f %.2f %.2f, position if "
            "entity %.2f %.2f %.2f, if Controllable of a vehicle %.2f %.2f %.2f",
            name, reinterpret_cast<void*>(unit), static_cast<unsigned>(*reinterpret_cast<const uintptr_t*>(unit) - Base()),
            std::atan2(aim[0], aim[2]) * 57.29578f,
            std::asin(aim[1] > 1 ? 1 : aim[1] < -1 ? -1 : aim[1]) * 57.29578f, inputs[0], inputs[1], inputs[2],
            inputs[3], asEntity[0], asEntity[1], asEntity[2], hasBefore ? asControllable[0] : 0.0f,
            hasBefore ? asControllable[1] : 0.0f, hasBefore ? asControllable[2] : 0.0f);
        return;
    }
    const float* transform = reinterpret_cast<const float*>(soldier + SOLDIER_TRANSFORM);
    const float* axes = reinterpret_cast<const float*>(soldier + SOLDIER_CONTROLLABLE + CONTROLLABLE_AXES);
    // Heading of the soldier's forward (row 2; row 0 is its left: runs 682, 683) around y, in degrees: 0 = +z, 90 = +x.
    const float heading = std::atan2(transform[8], transform[10]) * 57.29578f;
    Log::Info(
        "net", "%s world: soldier %p at %.2f %.2f %.2f heading %.1f pitch %.1f axes %.2f %.2f %.2f %.2f", name,
        reinterpret_cast<void*>(soldier), transform[12], transform[13], transform[14], heading,
        *reinterpret_cast<const float*>(soldier + SOLDIER_PITCH) * 57.29578f, axes[0], axes[1], axes[2], axes[3]);
    // Where this world's soldier would fire: its Aimer's origin and direction (the host fires from the host world's).
    const float* aimOrigin = reinterpret_cast<const float*>(soldier + SOLDIER_AIMER + AIMER_ORIGIN);
    const float* aimDirection = reinterpret_cast<const float*>(soldier + SOLDIER_AIMER + AIMER_DIRECTION);
    Log::Info(
        "net", "%s world: aim from %.3f %.3f %.3f heading %.2f pitch %.2f direction %.5f %.5f %.5f", name,
        aimOrigin[0], aimOrigin[1], aimOrigin[2], std::atan2(aimDirection[0], aimDirection[2]) * 57.29578f,
        std::asin(aimDirection[1] > 1 ? 1 : aimDirection[1] < -1 ? -1 : aimDirection[1]) * 57.29578f,
        aimDirection[0], aimDirection[1], aimDirection[2]);
}

/// Hook on the game's control state copy. The copy AdvanceTurn makes for local player 0 is the move of this turn:
/// what the client world predicts with and what is sent to the host (after quantising, which the game does right
/// after this call). The writer changes it there. __builtin_return_address(0) is where the call came from.
void __cdecl CopyControlStateHook(ControlState* destination, const ControlState* source)
{
    _originalCopyControlState(destination, source);
    const uintptr_t from = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    if (from == Base() + COPY_LOCAL_PLAYER_RETURN && _controlWriter && IsNetGame())
    {
        _controlWriter(*destination);
        const uint32_t pressed = _pendingButtons & ~_lastPressedButtons;
        destination->buttons |= pressed;
        _pendingButtons &= ~pressed;
        _lastPressedButtons = pressed;
        if (_isTraceOn)
        {
            float host[16] = {}, client[16] = {}, hostPitch = 0, clientPitch = 0;
            const bool hasHost = GetLocalSoldier(false, host, &hostPitch);
            const bool hasClient = GetLocalSoldier(true, client, &clientPitch);
            Log::Info(
                "net", "NT %.3f %.3f %.3f %.3f | %d %.3f %.3f %.3f %.2f %.2f | %d %.3f %.3f %.3f %.2f %.2f",
                destination->strafe, destination->move, destination->turnRate, destination->pitchRate, hasHost,
                host[12], host[13], host[14], std::atan2(host[8], host[10]) * 57.29578f, hostPitch * 57.29578f,
                hasClient, client[12], client[13], client[14], std::atan2(client[8], client[10]) * 57.29578f,
                clientPitch * 57.29578f);
        }
    }
}

void InstallControlHook()
{
    if (_wasControlHookTried)
    {
        return;
    }
    _wasControlHookTried = true;
    if (!GameBuild::IsKnownBuild())
    {
        Log::Warning("net", "unknown build: the mod's controls do not reach a network game");
        return;
    }
    void* const target = reinterpret_cast<void*>(Base() + COPY_CONTROL_STATE);
    if (memcmp(target, COPY_CONTROL_STATE_CODE, sizeof(COPY_CONTROL_STATE_CODE)) != 0)
    {
        Log::Warning("net", "control state copy not as expected: the mod's controls do not reach a network game");
        return;
    }
    MH_Initialize();
    MH_STATUS status = MH_CreateHook(
        target, reinterpret_cast<void*>(&CopyControlStateHook), reinterpret_cast<void**>(&_originalCopyControlState));
    if (status == MH_OK)
    {
        status = MH_EnableHook(target);
    }
    Log::Info("net", "control state hook: %s", MH_StatusToString(status));
}
} // namespace

void SetLocalControlWriter(ControlStateWriter writer)
{
    _controlWriter = writer;
    InstallControlHook();
}

void PressButton(int bit)
{
    if (bit >= 0 && bit < 30)
    {
        _pendingButtons |= 1u << bit;
    }
}

bool GetLocalSoldier(bool isClientWorld, float* transform, float* pitchRadians)
{
    if (!IsNetGame())
    {
        return false;
    }
    const uintptr_t soldier = FindLocalSoldier(isClientWorld ? CLIENT_TEAM_MANAGER : HOST_TEAM_MANAGER);
    if (!soldier)
    {
        return false;
    }
    memcpy(transform, reinterpret_cast<const void*>(soldier + SOLDIER_TRANSFORM), 16 * sizeof(float));
    *pitchRadians = *reinterpret_cast<const float*>(soldier + SOLDIER_PITCH);
    return true;
}

bool GetLocalSoldierTarget(float* point)
{
    if (!IsNetGame())
    {
        return false;
    }
    const uintptr_t soldier = FindLocalSoldier(CLIENT_TEAM_MANAGER);
    if (!soldier)
    {
        return false;
    }
    memcpy(point, reinterpret_cast<const void*>(soldier + SOLDIER_CONTROLLABLE + CONTROLLABLE_TARGET), 3 * sizeof(float));
    return true;
}

bool CastAimRay(const float* from, const float* direction, float rangeMeters, float* distanceMeters)
{
    *distanceMeters = rangeMeters;
    if (!IsNetGame())
    {
        return false;
    }
    // float ray(start, direction, range, out hit object, 0, entities to leave out, their count, mask, 1): returns the
    // part of the range that is free (1 = nothing hit). Arguments as the soldier's aim function passes them (0x11D553).
    using RayCast = float(__cdecl*)(const float*, const float*, float, void**, int, void**, int, unsigned, int);
    void* hit = nullptr;
    void* own = reinterpret_cast<void*>(FindLocalSoldier(IsClientWorldActive() ? CLIENT_TEAM_MANAGER : HOST_TEAM_MANAGER));
    const float freePart = reinterpret_cast<RayCast>(Base() + WORLD_RAY_CAST)(
        from, direction, rangeMeters, &hit, 0, &own, own ? 1 : 0, AIM_RAY_MASK, 1);
    if (!(freePart >= 0.0f && freePart < 1.0f)) // (also false for a value that is not a number)
    {
        return false;
    }
    *distanceMeters = freePart * rangeMeters;
    return true;
}

float GetTurnSeconds()
{
    const float seconds = GameBuild::IsKnownBuild() ? *reinterpret_cast<const float*>(Base() + TURN_SECONDS) : 0;
    return seconds > 0.005f && seconds < 0.1f ? seconds : 1.0f / 60.0f;
}

void SetTrace(bool isOn)
{
    _isTraceOn = isOn;
    Log::Info("net", "trace %s", isOn ? "on" : "off");
}

bool IsNetGame()
{
    return FlagAt(NET_GAME_FLAG) != 0;
}

bool IsHost()
{
    return FlagAt(HOST_FLAG) != 0;
}

bool IsLocalPlayerStep()
{
    return !IsNetGame() || (FlagAt(CLIENT_WORLD_FLAG) != 0 && FlagAt(FIRST_SIMULATION_FLAG) != 0);
}

bool IsClientWorldActive()
{
    return IsNetGame() && FlagAt(CLIENT_WORLD_FLAG) != 0;
}

void LogState()
{
    if (!GameBuild::IsKnownBuild())
    {
        Log::Info("net", "unknown build: no network addresses");
        return;
    }
    const int* playerIds = reinterpret_cast<const int*>(Base() + LOCAL_PLAYER_IDS);
    Log::Info(
        "net", "net game %d, host %d, client world active %d, local player ids %d %d", IsNetGame(), IsHost(),
        IsClientWorldActive(), playerIds[0], playerIds[1]);
    const int playerId = playerIds[0] >= 0 ? playerIds[0] : 0;
    LogWorld("host", HOST_TEAM_MANAGER, playerId);
    LogWorld("client", CLIENT_TEAM_MANAGER, playerId);
}

} // namespace Net
} // namespace SwbfVr
