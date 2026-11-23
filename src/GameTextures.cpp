// GameTextures: our arm and body textures from the player's game textures (GameTextures.h). The base arithmetic is
// integer-only and the same as tools/texpatch.py (lvlgeo.resize_bilinear), so the difference gives our texture exactly.
#include "GameTextures.h"

#include "LvlFile.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace SwbfVr
{

namespace GameTextures
{

namespace
{

/// A region of a texture's base: from a source (0 none, 1 a game texture, 2 a table made before) cropped to `from`
/// (source pixels x0, y0, x1, y1), resized to `to` (target pixels), then op (0 copy, 1 luma, 2 fill, 3 remap).
struct PatchRegion
{
    int sourceKind;
    const char* lvl;
    const char* texture;
    const char* table;
    unsigned hash; // the game texture the difference was made against
    int from[4];
    int to[4];
    int op;
    int params[6];
    const short* map; // remap: per target texel the source pixel (x, y), -1 none
};

struct TexturePatch
{
    const char* table;
    int width, height;
    const PatchRegion* regions;
    int regionCount;
    const unsigned* delta; // per channel, modulo 256; null: the base is ours unchanged
};

#if __has_include("TexturePatches.inc")
#include "TexturePatches.inc"
#else
constexpr PatchRegion PATCH_REGIONS_NONE[1] = {};
constexpr TexturePatch TEXTURE_PATCHES[1] = {{"", 0, 0, PATCH_REGIONS_NONE, 0, nullptr}};
#endif

struct Made
{
    int width, height;
    std::vector<unsigned> pixels;
};

std::map<std::string, Made> _made;

std::vector<unsigned> Crop(const std::vector<unsigned>& pixels, int width, const int* box)
{
    std::vector<unsigned> out;
    for (int y = box[1]; y < box[3]; ++y)
    {
        for (int x = box[0]; x < box[2]; ++x)
        {
            out.push_back(pixels[static_cast<size_t>(y) * width + x]);
        }
    }
    return out;
}

/// One axis of the integer bilinear resampling: per output pixel the two source pixels and the second's weight /256.
struct Tap
{
    int first, second, weight;
};

std::vector<Tap> Axis(int outSize, int size)
{
    std::vector<Tap> taps;
    for (int x = 0; x < outSize; ++x)
    {
        const long long f = (static_cast<long long>(2 * x + 1) * size * 256) / (2LL * outSize) - 128;
        if (f < 0)
        {
            taps.push_back({0, 0, 0});
            continue;
        }
        const int first = static_cast<int>(f >> 8);
        const int weight = static_cast<int>(f & 255);
        if (first >= size - 1)
        {
            taps.push_back({size - 1, size - 1, 0});
        }
        else
        {
            taps.push_back({first, first + 1, weight});
        }
    }
    return taps;
}

/// Integer bilinear (pixel centres aligned, edges clamped): tools/lvlgeo.py resize_bilinear.
std::vector<unsigned> Resize(const std::vector<unsigned>& pixels, int width, int height, int outWidth, int outHeight)
{
    const std::vector<Tap> xs = Axis(outWidth, width), ys = Axis(outHeight, height);
    std::vector<unsigned> out;
    out.reserve(static_cast<size_t>(outWidth) * outHeight);
    for (const Tap& ty : ys)
    {
        const size_t row0 = static_cast<size_t>(ty.first) * width, row1 = static_cast<size_t>(ty.second) * width;
        for (const Tap& tx : xs)
        {
            const unsigned p00 = pixels[row0 + tx.first], p10 = pixels[row0 + tx.second];
            const unsigned p01 = pixels[row1 + tx.first], p11 = pixels[row1 + tx.second];
            unsigned value = 0;
            for (int shift = 24; shift >= 0; shift -= 8)
            {
                const long long c =
                    (static_cast<long long>((p00 >> shift) & 255) * (256 - tx.weight) +
                     static_cast<long long>((p10 >> shift) & 255) * tx.weight) *
                        (256 - ty.weight) +
                    (static_cast<long long>((p01 >> shift) & 255) * (256 - tx.weight) +
                     static_cast<long long>((p11 >> shift) & 255) * tx.weight) *
                        ty.weight;
                value |= static_cast<unsigned>((c + 32768) >> 16) << shift;
            }
            out.push_back(value);
        }
    }
    return out;
}

unsigned Luminance(unsigned c)
{
    return (77 * ((c >> 16) & 255) + 150 * ((c >> 8) & 255) + 29 * (c & 255)) >> 8;
}

/// Python's floor division by 256 (>> on a negative int floors in C++20 as in Python).
int ShiftDown(int value)
{
    return value >> 8;
}

void ApplyOp(std::vector<unsigned>& pixels, const PatchRegion& region)
{
    if (region.op == 2) // fill
    {
        const unsigned colour = static_cast<unsigned>(region.params[0]) << 24 |
                                static_cast<unsigned>(region.params[1]) << 16 |
                                static_cast<unsigned>(region.params[2]) << 8 | static_cast<unsigned>(region.params[3]);
        std::fill(pixels.begin(), pixels.end(), colour);
    }
    else if (region.op == 1) // luma: per channel (k * luminance >> 8) + m, alpha kept
    {
        for (unsigned& c : pixels)
        {
            const int lum = static_cast<int>(Luminance(c));
            unsigned value = c & 0xFF000000u;
            const int shifts[3] = {16, 8, 0};
            for (int channel = 0; channel < 3; ++channel)
            {
                const int v = ShiftDown(region.params[2 * channel] * lum) + region.params[2 * channel + 1];
                value |= static_cast<unsigned>(std::clamp(v, 0, 255)) << shifts[channel];
            }
            c = value;
        }
    }
}

/// The base of one patch; false when a game texture it needs is missing. `isExact` turns false when one is not the
/// texture the difference was made against.
bool MakeBase(const std::string& lvlFolder, const TexturePatch& patch, std::vector<unsigned>& base, bool& isExact)
{
    base.assign(static_cast<size_t>(patch.width) * patch.height, 0);
    for (int index = 0; index < patch.regionCount; ++index)
    {
        const PatchRegion& region = patch.regions[index];
        const int* to = region.to;
        const int regionWidth = to[2] - to[0], regionHeight = to[3] - to[1];
        std::vector<unsigned> pixels;
        if (region.sourceKind == 1)
        {
            LvlFile::Texture texture;
            if (!LvlFile::LoadTexture(lvlFolder + region.lvl, region.texture, texture))
            {
                return false;
            }
            isExact = isExact && texture.hash == region.hash;
            if (region.op == 3) // remap: each texel a pixel of the source, as our bake took it
            {
                pixels.assign(static_cast<size_t>(regionWidth) * regionHeight, 0);
                for (size_t texel = 0; texel < pixels.size(); ++texel)
                {
                    const int x = region.map[2 * texel], y = region.map[2 * texel + 1];
                    if (x >= 0 && y >= 0 && x < texture.width && y < texture.height)
                    {
                        pixels[texel] = texture.pixels[static_cast<size_t>(y) * texture.width + x];
                    }
                }
            }
            else
            {
                const int* from = region.from;
                pixels = Resize(
                    Crop(texture.pixels, texture.width, from), from[2] - from[0], from[3] - from[1], regionWidth,
                    regionHeight);
            }
        }
        else if (region.sourceKind == 2)
        {
            auto found = _made.find(region.table);
            if (found == _made.end())
            {
                return false;
            }
            const int* from = region.from;
            pixels = Resize(
                Crop(found->second.pixels, found->second.width, from), from[2] - from[0], from[3] - from[1],
                regionWidth, regionHeight);
        }
        else
        {
            pixels.assign(static_cast<size_t>(regionWidth) * regionHeight, 0);
        }
        ApplyOp(pixels, region);
        for (int y = 0; y < regionHeight; ++y)
        {
            std::memcpy(
                &base[static_cast<size_t>(to[1] + y) * patch.width + to[0]],
                &pixels[static_cast<size_t>(y) * regionWidth], static_cast<size_t>(regionWidth) * 4);
        }
    }
    return true;
}

} // namespace

std::string Build(const std::string& lvlFolder)
{
    _made.clear();
    int made = 0, exact = 0, missing = 0;
    for (const TexturePatch& patch : TEXTURE_PATCHES)
    {
        if (!patch.table[0])
        {
            continue;
        }
        std::vector<unsigned> base;
        bool isExact = true;
        if (!MakeBase(lvlFolder, patch, base, isExact))
        {
            ++missing;
            continue;
        }
        if (patch.delta && isExact)
        {
            for (size_t texel = 0; texel < base.size(); ++texel)
            {
                unsigned value = 0;
                for (int shift = 24; shift >= 0; shift -= 8)
                {
                    value |= (((base[texel] >> shift) + (patch.delta[texel] >> shift)) & 255) << shift;
                }
                base[texel] = value;
            }
        }
        exact += isExact ? 1 : 0;
        ++made;
        _made[patch.table] = Made{patch.width, patch.height, std::move(base)};
    }
    char summary[160];
    std::snprintf(
        summary, sizeof(summary),
        "%d textures made from the game's (%d exactly ours, %d from a different game texture: base only), %d missing",
        made, exact, made - exact, missing);
    return summary;
}

const unsigned* Pixels(const char* table, int& width, int& height)
{
    auto found = _made.find(table);
    if (found == _made.end())
    {
        width = height = 0;
        return nullptr;
    }
    width = found->second.width;
    height = found->second.height;
    return found->second.pixels.data();
}

} // namespace GameTextures

} // namespace SwbfVr
