// Textures.cpp: the game's texture table, read for diagnostics (docs/offsets.md "Zoom", TextureByHash).
//
// The game keeps every loaded texture in one table keyed by name hash (0x6E2EF8 on Steam: +0 count, +4 0x400 hash
// keys, then 0x400 texture pointers; read from 0x300890). TextureByHash 0x74290 looks a texture up; the HUD's bitmap
// elements call it on every draw. The table's address is the `mov ecx, imm32` inside TextureByHash. Entries live on
// the game's heap and are cleared at level load (0x282DC0), so nothing here is kept between frames.

#include "Textures.h"

// Direct3D 9 through its C interface, as in Hud.cpp.
#define CINTERFACE

#include "GameBuild.h"
#include "Log.h"

#include <windows.h>
#include <d3d9.h>

#include <MinHook.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace SwbfVr
{

namespace Textures
{

namespace
{

/// TextureByHash's first bytes up to its table address: push ebp; mov ebp, esp; push [ebp+8]; mov ecx, imm32.
constexpr uint8_t LOOKUP_PROLOGUE[7] = {0x55, 0x8B, 0xEC, 0xFF, 0x75, 0x08, 0xB9};
/// Table: count, then the hash keys, then the texture pointers.
constexpr int TABLE_SLOT_COUNT = 0x400;
constexpr uintptr_t OFFSET_tableKeys = 0x4;
constexpr uintptr_t OFFSET_tableValues = 0x4 + 4 * TABLE_SLOT_COUNT;
/// Texture object (0x3C bytes, constructor 0x282B80).
constexpr uintptr_t OFFSET_textureHash = 0x00;
constexpr uintptr_t OFFSET_textureHeight = 0x04;
constexpr uintptr_t OFFSET_textureWidth = 0x08;
constexpr uintptr_t OFFSET_textureLevels = 0x10;
constexpr uintptr_t OFFSET_textureFormat = 0x14;
constexpr uintptr_t OFFSET_textureKind = 0x18;
constexpr uintptr_t OFFSET_textureD3d = 0x1C;
constexpr uintptr_t OFFSET_texturePool = 0x20;
constexpr int TEXTURE_KIND_2D = 1;

/// Distinct (caller, hash) pairs kept while recording one frame.
constexpr int LOOKUP_CAPACITY = 512;
/// Textures waiting to be saved.
constexpr int SAVE_CAPACITY = 32;

using LookupFunction = uintptr_t(__cdecl*)(unsigned);
LookupFunction _originalLookup = nullptr;
uintptr_t _table = 0;

struct Lookup
{
    uintptr_t caller;
    unsigned hash;
    unsigned count;
    bool isFound;
};
Lookup _lookups[LOOKUP_CAPACITY];
int _lookupCount = 0;
bool _isRecording = false;

volatile LONG _isDumpRequested = 0;
volatile LONG _saveCount = 0;
volatile unsigned _saveHashes[SAVE_CAPACITY];

template <typename T> T Read(uintptr_t base, uintptr_t offset)
{
    T value;
    memcpy(&value, reinterpret_cast<const void*>(base + offset), sizeof(T));
    return value;
}

uintptr_t __cdecl LookupHook(unsigned hash)
{
    const uintptr_t texture = _originalLookup(hash);
    if (_isRecording)
    {
        const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
        int index = 0;
        while (index < _lookupCount && (_lookups[index].caller != caller || _lookups[index].hash != hash))
        {
            ++index;
        }
        if (index < _lookupCount)
        {
            ++_lookups[index].count;
        }
        else if (_lookupCount < LOOKUP_CAPACITY)
        {
            _lookups[_lookupCount++] = Lookup{caller, hash, 1, texture != 0};
        }
    }
    return texture;
}

/// Four characters of a compressed format, or the number.
void FormatName(unsigned format, char* out, size_t size)
{
    if (format > 0xFFFF)
    {
        snprintf(
            out, size, "%c%c%c%c", static_cast<char>(format), static_cast<char>(format >> 8),
            static_cast<char>(format >> 16), static_cast<char>(format >> 24));
    }
    else
    {
        snprintf(out, size, "%u", format);
    }
}

void DumpTable()
{
    const unsigned count = Read<unsigned>(_table, 0);
    int used = 0, mismatched = 0;
    for (int slot = 0; slot < TABLE_SLOT_COUNT; ++slot)
    {
        const unsigned key = Read<unsigned>(_table, OFFSET_tableKeys + 4 * slot);
        const uintptr_t texture = Read<uintptr_t>(_table, OFFSET_tableValues + 4 * slot);
        if (!texture)
        {
            continue;
        }
        ++used;
        if (Read<unsigned>(texture, OFFSET_textureHash) != key)
        {
            ++mismatched;
        }
        char format[16];
        FormatName(Read<unsigned>(texture, OFFSET_textureFormat), format, sizeof(format));
        Log::Info(
            "textures",
            "entry 0x%08x %ux%u %s levels %u kind %d pool %u",
            key,
            Read<unsigned>(texture, OFFSET_textureWidth),
            Read<unsigned>(texture, OFFSET_textureHeight),
            format,
            Read<unsigned>(texture, OFFSET_textureLevels),
            Read<int>(texture, OFFSET_textureKind),
            Read<unsigned>(texture, OFFSET_texturePool));
    }
    Log::Info("textures", "table: count %u, %d slots used, %d with a hash not matching their key", count, used, mismatched);
}

void ReportLookups()
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    for (int index = 0; index < _lookupCount; ++index)
    {
        const Lookup& lookup = _lookups[index];
        Log::Info(
            "textures",
            "lookup from 0x%x: 0x%08x x%u %s",
            static_cast<unsigned>(lookup.caller - base),
            lookup.hash,
            lookup.count,
            lookup.isFound ? "found" : "not loaded");
    }
    Log::Info("textures", "%d distinct lookups in one frame%s", _lookupCount, _lookupCount == LOOKUP_CAPACITY ? " (full)" : "");
}

void Save(unsigned hash)
{
    const uintptr_t texture = _originalLookup(hash);
    if (!texture || Read<int>(texture, OFFSET_textureKind) != TEXTURE_KIND_2D)
    {
        Log::Warning("textures", "save 0x%08x: %s", hash, texture ? "not 2D" : "not loaded");
        return;
    }
    IDirect3DTexture9* const d3dTexture = Read<IDirect3DTexture9*>(texture, OFFSET_textureD3d);
    D3DSURFACE_DESC description{};
    D3DLOCKED_RECT locked{};
    if (!d3dTexture || FAILED(d3dTexture->lpVtbl->GetLevelDesc(d3dTexture, 0, &description)) ||
        FAILED(d3dTexture->lpVtbl->LockRect(d3dTexture, 0, &locked, nullptr, D3DLOCK_READONLY)))
    {
        Log::Warning("textures", "save 0x%08x: cannot lock (pool %d)", hash, static_cast<int>(description.Pool));
        return;
    }
    const bool isBlock = description.Format == D3DFMT_DXT1 || description.Format == D3DFMT_DXT2 ||
                         description.Format == D3DFMT_DXT3 || description.Format == D3DFMT_DXT4 ||
                         description.Format == D3DFMT_DXT5;
    const UINT rows = isBlock ? (description.Height + 3) / 4 : description.Height;
    char name[48];
    snprintf(name, sizeof(name), "swbf-vr-tex-%08x.bin", hash);
    FILE* file = fopen(name, "wb");
    if (file)
    {
        const uint32_t header[5] = {
            0x30584554, description.Width, description.Height, static_cast<uint32_t>(description.Format),
            static_cast<uint32_t>(locked.Pitch)};
        fwrite(header, sizeof(header), 1, file);
        fwrite(locked.pBits, static_cast<size_t>(locked.Pitch), rows, file);
        fclose(file);
    }
    d3dTexture->lpVtbl->UnlockRect(d3dTexture, 0);
    char format[16];
    FormatName(static_cast<unsigned>(description.Format), format, sizeof(format));
    Log::Info("textures", "saved 0x%08x %ux%u %s to %s", hash, description.Width, description.Height, format, file ? name : "(open failed)");
}

} // namespace

void Install()
{
    if (!GameBuild::IsAvailable(GameBuild::Feature::Zoom))
    {
        return;
    }
    uint8_t* const lookup = reinterpret_cast<uint8_t*>(GameBuild::Absolute(GameBuild::Address::TextureByHash));
    if (memcmp(lookup, LOOKUP_PROLOGUE, sizeof(LOOKUP_PROLOGUE)) != 0)
    {
        Log::Warning("textures", "TextureByHash not as expected: no texture table");
        return;
    }
    memcpy(&_table, lookup + sizeof(LOOKUP_PROLOGUE), sizeof(_table));
    MH_Initialize();
    MH_STATUS status =
        MH_CreateHook(lookup, reinterpret_cast<void*>(&LookupHook), reinterpret_cast<void**>(&_originalLookup));
    if (status == MH_OK)
    {
        status = MH_EnableHook(lookup);
    }
    if (status != MH_OK)
    {
        Log::Error("textures", "lookup hook failed: %s", MH_StatusToString(status));
        _originalLookup = nullptr;
        return;
    }
    Log::Info("textures", "table at 0x%x", static_cast<unsigned>(_table - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));
}

void* FindByName(const char* name)
{
    if (!name || !GameBuild::IsAvailable(GameBuild::Feature::Zoom))
    {
        return nullptr;
    }
    unsigned hash = 0x811C9DC5u; // FNV-1a of the lower-cased name: the table's key (0x3008F0)
    for (const char* character = name; *character; ++character)
    {
        hash = (hash ^ (static_cast<unsigned char>(*character) | 0x20u)) * 0x01000193u;
    }
    using LookupFunction = uintptr_t(__cdecl*)(unsigned);
    const auto lookup = reinterpret_cast<LookupFunction>(GameBuild::Absolute(GameBuild::Address::TextureByHash));
    const uintptr_t texture = lookup(hash);
    if (!texture || IsBadReadPtr(reinterpret_cast<void*>(texture), OFFSET_textureD3d + 4) ||
        Read<int>(texture, OFFSET_textureKind) != 1)
    {
        return nullptr;
    }
    return Read<void*>(texture, OFFSET_textureD3d);
}

void* MakeOwn(void* d3dDevice, const unsigned* pixels, int width, int height)
{
    auto* const device = static_cast<IDirect3DDevice9*>(d3dDevice);
    IDirect3DTexture9* texture = nullptr;
    if (!device || !pixels || width <= 0 || height <= 0 ||
        FAILED(device->lpVtbl->CreateTexture(
            device, width, height, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)))
    {
        return nullptr;
    }
    std::vector<unsigned> level(pixels, pixels + width * height), next;
    const DWORD levels = texture->lpVtbl->GetLevelCount(texture);
    for (DWORD mip = 0; mip < levels; ++mip)
    {
        D3DLOCKED_RECT locked;
        if (SUCCEEDED(texture->lpVtbl->LockRect(texture, mip, &locked, nullptr, 0)))
        {
            for (int row = 0; row < height; ++row)
            {
                memcpy(static_cast<char*>(locked.pBits) + row * locked.Pitch, level.data() + row * width, width * 4);
            }
            texture->lpVtbl->UnlockRect(texture, mip);
        }
        // the next level: each texel the mean of four, per channel
        const int nextWidth = width > 1 ? width / 2 : 1, nextHeight = height > 1 ? height / 2 : 1;
        next.assign(nextWidth * nextHeight, 0);
        for (int row = 0; row < nextHeight; ++row)
        {
            for (int column = 0; column < nextWidth; ++column)
            {
                unsigned sum[4] = {};
                for (int dy = 0; dy < 2; ++dy)
                {
                    for (int dx = 0; dx < 2; ++dx)
                    {
                        const int y = row * 2 + dy < height ? row * 2 + dy : height - 1;
                        const int x = column * 2 + dx < width ? column * 2 + dx : width - 1;
                        const unsigned texel = level[y * width + x];
                        for (int channel = 0; channel < 4; ++channel)
                        {
                            sum[channel] += (texel >> (channel * 8)) & 0xFF;
                        }
                    }
                }
                unsigned texel = 0;
                for (int channel = 0; channel < 4; ++channel)
                {
                    texel |= ((sum[channel] + 2) / 4) << (channel * 8);
                }
                next[row * nextWidth + column] = texel;
            }
        }
        level.swap(next);
        width = nextWidth;
        height = nextHeight;
    }
    return texture;
}

void BeforeFrame()
{
    if (!_originalLookup)
    {
        return;
    }
    if (_isRecording)
    {
        _isRecording = false;
        ReportLookups();
    }
    if (InterlockedExchange(&_isDumpRequested, 0))
    {
        DumpTable();
        _lookupCount = 0;
        _isRecording = true;
    }
    const LONG saveCount = InterlockedExchange(&_saveCount, 0);
    for (LONG index = 0; index < saveCount && index < SAVE_CAPACITY; ++index)
    {
        Save(_saveHashes[index]);
    }
}

void RequestDump()
{
    InterlockedExchange(&_isDumpRequested, 1);
}

void RequestSave(unsigned hash)
{
    const LONG index = _saveCount;
    if (index < SAVE_CAPACITY)
    {
        _saveHashes[index] = hash;
        InterlockedExchange(&_saveCount, index + 1);
    }
}

} // namespace Textures

} // namespace SwbfVr
