// Feedback.cpp: what the player feels and sees when a shot leaves the gun and when it hits.
//
// Shot: the game's weapon fire function (0x2461A0, reached from WeaponCannon's vtable slot 28) is hooked with a
// small assembly stub that keeps every register, so its calling convention does not matter. A shot of the
// player's own weapon gives a short pulse in the right controller and a kick of the gun model (Aim.cpp).
//
// Hit: the game marks a hit with a small circle in the middle of the reticle (ReticuleDisplay+0x700, texture
// "hit_indicator"). Its strength comes from three fading values, one per kind of thing hit, kept in an object of
// the player at +0xD4 (kind -1, shown red: enemy), +0xD8 (kind 0, blue: neither, such as empty vehicles and
// droids) and +0xDC (kind 1, green: friend). ReticuleDisplay's update reads them with 0x9B710(kind); the one
// function that sets them is 0x9BBE0(target), called once for every hit from the damage code at 0xC765E: it works
// out the kind and stores 0.5 there. That function is hooked, so every single hit is felt (the circle itself only
// shows a fresh hit when the last one has faded a little: idren2023, headset run 8, "not registering every hit").
// Whose hits: the object the reticule reads from is remembered from the reader 0x9B710 (hooked too, calls from
// the reticule's update only), and only hits registered on that object count.
// Feedback: a short sharp pulse in the right controller, the aiming line flashes in the kind's colour, a thud.

#include "Feedback.h"

#include "Aim.h"
#include "FpGuard.h"
#include "GameBuild.h"
#include "Hud.h"
#include "LockOn.h"
#include "Log.h"
#include "Sight.h"
#include "Xr.h"
#include "engine.h"

#include <windows.h>
#include <mmsystem.h>

#include <MinHook.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace SwbfVr
{

namespace Feedback
{

// The assembly stub in front of the weapon fire function. The three names below have C linkage (extern "C": no C++
// name decoration, so the assembly can refer to them as _Name); such names are global, whatever namespace they are
// declared in, and keep their Feedback_ / g_ form for that reason.
// NOLINTBEGIN(readability-identifier-naming): C-linkage names the assembly refers to
extern "C"
{
    void* g_feedbackFireOriginal = nullptr;
    void __cdecl Feedback_FireNotify(void* self);
    void Feedback_FireStub();
}
// NOLINTEND(readability-identifier-naming)
// Keeps all registers and flags, tells Feedback_FireNotify which object fires (ecx), then runs the game's function.
// (GNU assembler, AT&T syntax: pushal/popal save and restore all general registers, pushfl/popfl the flags; the
// final jump goes through MinHook's trampoline to the game's own code.)
__asm__(".text\n"
        ".globl _Feedback_FireStub\n"
        "_Feedback_FireStub:\n"
        "    pushal\n"
        "    pushfl\n"
        "    pushl %ecx\n"
        "    call _Feedback_FireNotify\n"
        "    addl $4, %esp\n"
        "    popfl\n"
        "    popal\n"
        "    jmp *_g_feedbackFireOriginal\n");

namespace
{

// ---- Engine addresses (GameBuild, Feature::Feedback; docs/offsets.md, "Reticule and hit indicator"; shared
// layouts in engine.h) ----
// WeaponFire: the weapon fire function, reached from WeaponCannon's vtable slot 28; `this` (ecx) is the weapon. Game
// thread. Observed with the hook log (run77).
// RegisterHit(target), thiscall on the player's hit-value object: works out the kind of thing hit and sets its value
// to 0.5. Called once per hit from the damage code at 0xC765E. Observed (b83-b85).
// float HitValue(int kind), thiscall: the reader ReticuleDisplay's update uses for the three hit values.
// ReticuleUpdate to ReticuleUpdateEnd: ReticuleDisplay's update; calls of HitValue from there tell which object the
// reticule reads from. Kept as absolute addresses, set before the hooks are enabled.
uintptr_t _reticuleUpdateStart = 0, _reticuleUpdateEnd = 0;
/// Hit-value object: three fading floats, enemy (+0xD4), neither (+0xD8), friend (+0xDC). Observed (b83).
constexpr uintptr_t OFFSET_hitValues = 0xD4;

// ---- Constants ----

/// Written into the three hit values before RegisterHit runs, as raw bits (-1.0f, a value the game never stores
/// there). The value that no longer holds it afterwards tells the kind of the hit.
constexpr uint32_t HIT_VALUE_MARKER = 0xBF800000;

/// Colours of the aiming line's flash per kind of hit (0xRRGGBB): enemy red, friend green, neither blue.
constexpr unsigned FLASH_COLOR_ENEMY = 0xFF2818;
constexpr unsigned FLASH_COLOR_FRIEND = 0x30FF40;
constexpr unsigned FLASH_COLOR_NEITHER = 0x3C8CFF;

/// Hit pulse in the right controller: short and hard for an enemy, softer for the rest.
constexpr float HIT_PULSE_ENEMY_SECONDS = 0.028f;
constexpr float HIT_PULSE_OTHER_SECONDS = 0.022f;
constexpr float HIT_PULSE_ENEMY_AMPLITUDE = 1.0f;
constexpr float HIT_PULSE_OTHER_AMPLITUDE = 0.5f;
constexpr float HIT_PULSE_HERTZ = 320.0f;

/// Shot pulse in the right controller (at the runtime's default frequency).
constexpr float SHOT_PULSE_SECONDS = 0.035f;
constexpr float SHOT_PULSE_AMPLITUDE = 0.5f;

/// How many shots, hits and hits on other objects are logged.
constexpr unsigned MAXIMUM_LOGGED_SHOTS = 12;
constexpr unsigned MAXIMUM_LOGGED_HITS = 60;
constexpr unsigned MAXIMUM_LOGGED_OTHER_HITS = 4;

/// The thud: sample rate, length, and the samples over which its end fades out.
constexpr int THUD_SAMPLE_RATE = 22050;
constexpr int THUD_LENGTH_MILLISECONDS = 260;
constexpr int THUD_FADE_SAMPLES = 660;
/// The thud's room: each delay line feeds back this much, loses this much per pass, and is mixed in at this level.
constexpr float THUD_ECHO_FEEDBACK = 0.45f;
constexpr float THUD_ECHO_DAMPING = 0.8f;
constexpr float THUD_ECHO_LEVEL = 0.12f;

// ---- State ----

bool _isInstalled = false, _hasTried = false;
bool _isHitFeedbackOn = true, _isHapticsOn = true, _isRecoilOn = true, _isHitSoundOn = true;
/// void __thiscall RegisterHit(void* target). The hooks are declared __fastcall with an unused second parameter (EDX)
/// so that they receive the object in ECX, as the game's __thiscall passes it.
using RegisterHitFunction = void(__fastcall*)(void* self, void* edx, void* target);
/// float __thiscall HitValue(int kind).
using HitValueFunction = float(__fastcall*)(void* self, void* edx, int kind);
// The game's functions behind our hooks, called through MinHook's trampolines.
RegisterHitFunction _originalRegisterHit = nullptr;
HitValueFunction _originalHitValue = nullptr;
void* _reticuleObject = nullptr; // the object whose hit values the player's reticule shows
unsigned _shotCount = 0, _hitCount = 0, _otherHitCount = 0;
bool _shouldDumpHitKinds = false; // test ("hud hitdump 1")

// ---- Hit sound: a short low thud, made here as a WAV file in memory and played through the Windows default output

unsigned char* _thudSound = nullptr;
DWORD _thudByteCount = 0;

void MakeThudSound()
{
    const int rate = THUD_SAMPLE_RATE, samples = rate * THUD_LENGTH_MILLISECONDS / 1000;
    _thudByteCount = 44 + static_cast<DWORD>(samples) * 2;
    _thudSound = static_cast<unsigned char*>(malloc(_thudByteCount));
    if (!_thudSound)
    {
        return;
    }
    const DWORD dataBytes = static_cast<DWORD>(samples) * 2, riff = 36 + dataBytes, fmt = 16,
                byteRate = static_cast<DWORD>(rate) * 2;
    const WORD pcm = 1, channels = 1, align = 2, bits = 16;
    unsigned char* writePosition = _thudSound;
    // A lambda (like a C# local function); [&writePosition] captures that variable by reference, so each call moves
    // it on.
    const auto put = [&writePosition](const void* value, size_t byteCount)
    {
        memcpy(writePosition, value, byteCount);
        writePosition += byteCount;
    };
    const DWORD rateValue = static_cast<DWORD>(rate);
    put("RIFF", 4);
    put(&riff, 4);
    put("WAVEfmt ", 8);
    put(&fmt, 4);
    put(&pcm, 2);
    put(&channels, 2);
    put(&rateValue, 4);
    put(&byteRate, 4);
    put(&align, 2);
    put(&bits, 2);
    put("data", 4);
    put(&dataBytes, 4);
    // b124, third version (idren2023 after headset run 10: a crack, a little reverb; a distant shot, "KRRrrhh /
    // Doomff", not a UI click; then: a mix of this and b93's): b93's body (now falling from 125 to 45 Hz) that rings for a
    // quarter of a second and its dull slap of low-passed noise, plus a crack of high-passed noise over the first
    // 6 ms and a rumble of low-passed noise fading over about 90 ms, then three feedback delays (31, 37 and 43 ms) as
    // a small room, and soft clipping so a headset's small speakers carry the low part. Then muted and deeper (idren2023): crack at a sixth, the whole low-passed at about 750 Hz. Preview: re/thud-v6.wav.
    static float dry[THUD_SAMPLE_RATE * THUD_LENGTH_MILLISECONDS / 1000];
    unsigned noise = 12345;
    float phase = 0, low = 0, lower = 0, previousNoise = 0;
    for (int sampleIndex = 0; sampleIndex < samples; ++sampleIndex)
    {
        const float time = static_cast<float>(sampleIndex) / static_cast<float>(rate);
        noise = noise * 1664525u + 1013904223u; // a linear congruential random number generator
        const float white = static_cast<float>(noise >> 16) / 32768.0f - 1.0f;
        const float high = white - previousNoise; // a first difference: noise without its low part
        previousNoise = white;
        low += (white - low) * 0.10f;     // about 370 Hz low-pass
        lower += (white - lower) * 0.06f; // about 220 Hz
        phase += 6.2831853f * (45.0f + 80.0f * expf(-time * 28.0f)) / static_cast<float>(rate);
        const float body = (sinf(phase) + 0.35f * sinf(phase * 2.0f)) * expf(-time * 13.0f) *
                           (time < 0.002f ? time / 0.002f : 1.0f);
        const float slap = low * expf(-time * 55.0f) * 2.2f;
        const float crack = high * expf(-time * 700.0f) * 0.15f;
        const float rumble = lower * expf(-time * 24.0f) * 1.6f;
        dry[sampleIndex] = body * 1.3f + slap + crack + rumble;
    }
    const int delays[3] = {rate * 31 / 1000, rate * 37 / 1000, rate * 43 / 1000};
    static float echo[3][THUD_SAMPLE_RATE * THUD_LENGTH_MILLISECONDS / 1000];
    float muted = 0;
    for (int sampleIndex = 0; sampleIndex < samples; ++sampleIndex)
    {
        float wet = 0;
        for (int line = 0; line < 3; ++line)
        {
            const int from = sampleIndex - delays[line];
            echo[line][sampleIndex] =
                from >= 0 ? (dry[from] + echo[line][from] * THUD_ECHO_FEEDBACK) * THUD_ECHO_DAMPING : 0.0f;
            wet += echo[line][sampleIndex];
        }
        const float tail = sampleIndex > samples - THUD_FADE_SAMPLES
                               ? static_cast<float>(samples - sampleIndex) / static_cast<float>(THUD_FADE_SAMPLES)
                               : 1.0f;
        const float value = tanhf((dry[sampleIndex] + wet * THUD_ECHO_LEVEL) * 1.3f) * tail;
        muted += (value - muted) * 0.18f; // about 750 Hz low-pass: muted and deep (idren2023)
        const short sample = static_cast<short>(muted * 0.9f * 27000.0f);
        put(&sample, 2);
    }
}

// The player's current weapon: FirstPersonRenderable -> soldier's Controllable part (soldier + 0x250) -> soldier
// + 0x8F0 Aimer -> +0x128 current weapon (the chain Aim.cpp uses for the muzzle).
void* FindPlayerWeapon()
{
    const uintptr_t renderables = GameBuild::Absolute(GameBuild::Address::FirstPersonRenderables);
    void* firstPersonRenderable = renderables ? *reinterpret_cast<void**>(renderables) : nullptr;
    if (!firstPersonRenderable || IsBadReadPtr(firstPersonRenderable, 0x1650))
    {
        return nullptr;
    }
    void* controllable =
        *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(firstPersonRenderable) + OFFSET_firstPersonControllable);
    if (!controllable || IsBadReadPtr(controllable, 0x800))
    {
        return nullptr;
    }
    const uintptr_t aimer =
        reinterpret_cast<uintptr_t>(controllable) - OFFSET_soldierControllable + OFFSET_soldierAimer;
    if (IsBadReadPtr(reinterpret_cast<void*>(aimer), 0x130))
    {
        return nullptr;
    }
    return *reinterpret_cast<void**>(aimer + OFFSET_aimerWeapon);
}

// The reader of the hit values: remembers whose values the reticule's update asks for. Nothing else is touched;
// the float result passes straight through.
float __fastcall HitValueHook(void* self, void* edx, int kind)
{
    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    if (caller >= _reticuleUpdateStart && caller < _reticuleUpdateEnd)
    {
        _reticuleObject = self;
    }
    return _originalHitValue(self, edx, kind);
}

// Every hit. Which kind it is shows in which of the three values the game sets: they are filled with a marker
// first (as raw bits: no floating-point register is used before the guard), and put back afterwards.
void __fastcall RegisterHitHook(void* self, void* edx, void* target)
{
    uint32_t* values =
        reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(self) + OFFSET_hitValues); // enemy, neither, friend
    const uint32_t marker = HIT_VALUE_MARKER;
    uint32_t before[3];
    memcpy(before, values, sizeof(before));
    values[0] = values[1] = values[2] = marker;
    _originalRegisterHit(self, edx, target);
    int kind = -1;
    for (int valueIndex = 0; valueIndex < 3; ++valueIndex)
    {
        if (values[valueIndex] != marker)
        {
            kind = valueIndex;
        }
        else
        {
            values[valueIndex] = before[valueIndex];
        }
    }
    FpGuard guard;
    const bool isPlayers = self == _reticuleObject;
    static const char* const HIT_KIND_NAMES[3] = {"enemy", "neither", "friend"};
    if (!isPlayers)
    {
        // Other soldiers' hits come through here too. The first few are logged, before and after the player's object is
        // known.
        static unsigned earlyCount = 0, lateCount = 0;
        ++_otherHitCount;
        if ((_reticuleObject ? ++lateCount : ++earlyCount) <= MAXIMUM_LOGGED_OTHER_HITS)
        {
            Log::Info(
                "feedback",
                "a hit registered on another object (%p, the reticule reads %p): ignored",
                self,
                _reticuleObject);
        }
        return;
    }
    ++_hitCount;
    if (_hitCount <= MAXIMUM_LOGGED_HITS)
    {
        Log::Info(
            "feedback",
            "hit %u: %s (target %p)",
            _hitCount,
            kind >= 0 ? HIT_KIND_NAMES[kind] : "kind not found",
            target);
    }
    if (_shouldDumpHitKinds)
    { // test: the first hit of each kind writes the HUD texture
        static bool isDumped[3];
        if (kind >= 0 && !isDumped[kind])
        {
            isDumped[kind] = true;
            Hud::DumpNext(1);
            Log::Info("feedback", "HUD texture requested for a %s hit", HIT_KIND_NAMES[kind]);
        }
    }
    if (kind < 0 || !Xr::IsRunning())
    {
        return;
    }
    if (_isHitFeedbackOn)
    {
        Sight::Flash(kind == 0 ? FLASH_COLOR_ENEMY : kind == 2 ? FLASH_COLOR_FRIEND : FLASH_COLOR_NEITHER);
        // Short and hard for an enemy; softer for the rest.
        if (_isHapticsOn)
        {
            Xr::Vibrate(
                0,
                kind == 0 ? HIT_PULSE_ENEMY_SECONDS : HIT_PULSE_OTHER_SECONDS,
                kind == 0 ? HIT_PULSE_ENEMY_AMPLITUDE : HIT_PULSE_OTHER_AMPLITUDE,
                HIT_PULSE_HERTZ);
        }
    }
    if (_isHitSoundOn && _thudSound)
    {
        PlaySoundA(reinterpret_cast<const char*>(_thudSound), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }
}

} // namespace

// Called by the assembly stub for every weapon that fires (any soldier's): only the player's own weapon counts.
extern "C" void __cdecl Feedback_FireNotify(void* self)
{
    FpGuard guard;
    void* weapon = FindPlayerWeapon();
    ++_shotCount;
    if (_shotCount <= MAXIMUM_LOGGED_SHOTS)
    {
        Log::Info("feedback", "fire %u: object %p, the player's weapon %p", _shotCount, self, weapon);
    }
    if (!weapon || self != weapon || !Xr::IsRunning())
    {
        return;
    }
    LockOn::OnShot();
    if (_isRecoilOn)
    {
        Aim::Kick();
    }
    if (_isHapticsOn)
    {
        Xr::Vibrate(0, SHOT_PULSE_SECONDS, SHOT_PULSE_AMPLITUDE, 0.0f);
    }
}

void SetOptions(bool isHitFeedbackOn, bool isHapticsOn, bool isRecoilOn, bool isHitSoundOn)
{
    _isHitFeedbackOn = isHitFeedbackOn;
    _isHapticsOn = isHapticsOn;
    _isRecoilOn = isRecoilOn;
    _isHitSoundOn = isHitSoundOn;
    Log::Info(
        "feedback",
        "hit feedback %s, hit sound %s, haptics %s, recoil kick %s",
        isHitFeedbackOn ? "on" : "off",
        isHitSoundOn ? "on" : "off",
        isHapticsOn ? "on" : "off",
        isRecoilOn ? "on" : "off");
}

// Other parts of the mod: a pulse if vibration is switched on.
void Pulse(int hand, float seconds, float amplitude)
{
    if (_isHapticsOn)
    {
        Xr::Vibrate(hand, seconds, amplitude, 0.0f);
    }
}

void SetDumpHitKinds(bool isOn)
{
    _shouldDumpHitKinds = isOn;
}

// Main thread, once per presented frame: installs the two hooks when VR first runs.
int AimedRelation()
{
    // _reticuleObject is the player's 0xE8-byte object from the reticule's update; the Controllable it controls is
    // the first non-zero of +0xBC, +0xB8, +0xB4 (0x8C320); +0x30 holds the target relation the reticle's colour comes
    // from (0x208444: below 0 red, above 0 green). Agent finding 2026-10-12, b124.
    static int lastRelation = 0;
    int relation = 0;
    const auto readPointer = [](uintptr_t address) -> uintptr_t
    { return IsBadReadPtr(reinterpret_cast<void*>(address), 4) ? 0 : *reinterpret_cast<uintptr_t*>(address); };
    const uintptr_t player = reinterpret_cast<uintptr_t>(_reticuleObject);
    if (player)
    {
        uintptr_t controllable = readPointer(player + 0xBC);
        controllable = controllable ? controllable : readPointer(player + 0xB8);
        controllable = controllable ? controllable : readPointer(player + 0xB4);
        if (controllable && !IsBadReadPtr(reinterpret_cast<void*>(controllable + 0x30), 4))
        {
            const int value = *reinterpret_cast<int*>(controllable + 0x30);
            relation = value < 0 ? -1 : value > 0 ? 1 : 0;
        }
    }
    if (relation != lastRelation)
    {
        lastRelation = relation;
        Log::Info("feedback", "aimed at %s", relation < 0 ? "an enemy" : relation > 0 ? "a friend" : "nothing");
    }
    return relation;
}

void OnPresent()
{
    if (_hasTried || !Xr::IsRunning())
    {
        return;
    }
    _hasTried = true;
    if (!GameBuild::IsAvailable(GameBuild::Feature::Feedback))
    {
        Log::Warning(
            "feedback",
            "fire or hit functions not found (%s is off for this game build): no shot and hit feedback",
            GameBuild::FeatureName(GameBuild::Feature::Feedback));
        return;
    }
    _reticuleUpdateStart = GameBuild::Absolute(GameBuild::Address::ReticuleUpdate);
    _reticuleUpdateEnd = GameBuild::Absolute(GameBuild::Address::ReticuleUpdateEnd);
    void* const weaponFire = reinterpret_cast<void*>(GameBuild::Absolute(GameBuild::Address::WeaponFire));
    void* const registerHit = reinterpret_cast<void*>(GameBuild::Absolute(GameBuild::Address::RegisterHit));
    void* const hitValue = reinterpret_cast<void*>(GameBuild::Absolute(GameBuild::Address::HitValue));
    MakeThudSound();
    MH_Initialize();
    MH_STATUS fireStatus =
        MH_CreateHook(weaponFire, reinterpret_cast<void*>(&Feedback_FireStub), &g_feedbackFireOriginal);
    if (fireStatus == MH_OK)
    {
        fireStatus = MH_EnableHook(weaponFire);
    }
    MH_STATUS hitStatus = MH_CreateHook(
        registerHit,
        reinterpret_cast<void*>(&RegisterHitHook),
        reinterpret_cast<void**>(&_originalRegisterHit));
    if (hitStatus == MH_OK)
    {
        hitStatus = MH_EnableHook(registerHit);
    }
    MH_STATUS hitValueStatus =
        MH_CreateHook(hitValue, reinterpret_cast<void*>(&HitValueHook), reinterpret_cast<void**>(&_originalHitValue));
    if (hitValueStatus == MH_OK)
    {
        hitValueStatus = MH_EnableHook(hitValue);
    }
    if (hitStatus == MH_OK && hitValueStatus != MH_OK)
    {
        hitStatus = hitValueStatus;
    }
    _isInstalled = fireStatus == MH_OK && hitStatus == MH_OK;
    Log::Info(
        "feedback",
        "hooks: fire %s, hit indicator %s",
        MH_StatusToString(fireStatus),
        MH_StatusToString(hitStatus));
}

} // namespace Feedback

} // namespace SwbfVr
