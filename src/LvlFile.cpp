// LvlFile: models from the game's level files (LvlFile.h). Replayed in Python first: tools/lvlgeo.py.
#include "LvlFile.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace SwbfVr
{

namespace LvlFile
{

namespace
{

/// The file mapped read-only into our address space (pages are read only where we look: the side files are up to
/// 110 MB, of which a model is a few hundred kB).
class MappedFile
{
  public:
    explicit MappedFile(const std::string& path)
    {
        _file = CreateFileA(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (_file == INVALID_HANDLE_VALUE)
        {
            return;
        }
        LARGE_INTEGER size = {};
        GetFileSizeEx(_file, &size);
        _mapping = CreateFileMappingA(_file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (_mapping)
        {
            _data = static_cast<const uint8_t*>(MapViewOfFile(_mapping, FILE_MAP_READ, 0, 0, 0));
            _size = _data ? static_cast<size_t>(size.QuadPart) : 0;
        }
    }

    ~MappedFile()
    {
        if (_data)
        {
            UnmapViewOfFile(_data);
        }
        if (_mapping)
        {
            CloseHandle(_mapping);
        }
        if (_file != INVALID_HANDLE_VALUE)
        {
            CloseHandle(_file);
        }
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    const uint8_t* Data() const
    {
        return _data;
    }

    size_t Size() const
    {
        return _size;
    }

  private:
    HANDLE _file = INVALID_HANDLE_VALUE;
    HANDLE _mapping = nullptr;
    const uint8_t* _data = nullptr;
    size_t _size = 0;
};

/// A chunk's body: [start, start + size) in the file.
struct Chunk
{
    char tag[4];
    size_t start;
    size_t size;
};

uint32_t ReadUint32(const uint8_t* data, size_t offset)
{
    uint32_t value = 0;
    std::memcpy(&value, data + offset, 4);
    return value;
}

float ReadFloat(const uint8_t* data, size_t offset)
{
    float value = 0;
    std::memcpy(&value, data + offset, 4);
    return value;
}

/// The chunks laid end to end in [start, end).
std::vector<Chunk> Children(const MappedFile& file, size_t start, size_t end)
{
    std::vector<Chunk> chunks;
    size_t offset = start;
    while (offset + 8 <= end)
    {
        Chunk chunk = {};
        std::memcpy(chunk.tag, file.Data() + offset, 4);
        chunk.size = ReadUint32(file.Data(), offset + 4);
        chunk.start = offset + 8;
        if (chunk.start + chunk.size > end)
        {
            break;
        }
        chunks.push_back(chunk);
        offset = chunk.start + chunk.size + ((4 - chunk.size % 4) % 4);
    }
    return chunks;
}

bool IsTag(const Chunk& chunk, const char* tag)
{
    return std::memcmp(chunk.tag, tag, 4) == 0;
}

/// A zero-terminated string at the start of [start, start + size).
std::string StringAt(const MappedFile& file, size_t start, size_t size)
{
    const char* text = reinterpret_cast<const char*>(file.Data() + start);
    return std::string(text, strnlen(text, size));
}

/// The chunk's name: its first child, NAME (modl) or INFO (skel: the name leads it).
std::string NameOf(const MappedFile& file, const Chunk& chunk)
{
    for (const Chunk& child : Children(file, chunk.start, chunk.start + chunk.size))
    {
        if (IsTag(child, "NAME") || IsTag(child, "INFO"))
        {
            return StringAt(file, child.start, child.size);
        }
        break;
    }
    return std::string();
}

bool SameName(const std::string& a, const char* b)
{
    return _stricmp(a.c_str(), b) == 0;
}

/// The first chunk of this tag and name, walking ucfb and the lvl_ sub-levels (hash and size before their children).
bool FindChunk(const MappedFile& file, size_t start, size_t end, const char* tag, const char* name, Chunk& found)
{
    for (const Chunk& chunk : Children(file, start, end))
    {
        if (IsTag(chunk, "ucfb"))
        {
            if (FindChunk(file, chunk.start, chunk.start + chunk.size, tag, name, found))
            {
                return true;
            }
        }
        else if (IsTag(chunk, "lvl_") && chunk.size >= 8)
        {
            const size_t innerSize = ReadUint32(file.Data(), chunk.start + 4);
            const size_t innerEnd = chunk.start + 8 + innerSize;
            if (innerEnd <= chunk.start + chunk.size && FindChunk(file, chunk.start + 8, innerEnd, tag, name, found))
            {
                return true;
            }
        }
        else if (IsTag(chunk, tag) && SameName(NameOf(file, chunk), name))
        {
            found = chunk;
            return true;
        }
    }
    return false;
}

/// A bone's frame: rows of a 3x3 rotation (row vectors: p' = p * rows + translation) and a translation.
struct Frame
{
    double rows[3][3];
    Point translation;
};

const Frame IDENTITY = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {0, 0, 0}};

Frame Compose(const Frame& local, const Frame& parent)
{
    Frame result = {};
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            double sum = 0;
            for (int k = 0; k < 3; ++k)
            {
                sum += local.rows[row][k] * parent.rows[k][column];
            }
            result.rows[row][column] = sum;
        }
    }
    const double* t = &local.translation.x;
    double* out = &result.translation.x;
    for (int column = 0; column < 3; ++column)
    {
        double sum = 0;
        for (int k = 0; k < 3; ++k)
        {
            sum += t[k] * parent.rows[k][column];
        }
        out[column] = sum + (&parent.translation.x)[column];
    }
    return result;
}

Point Apply(const Frame& frame, const Point& point, bool isDirection)
{
    const double* p = &point.x;
    Point result = {};
    double* out = &result.x;
    for (int column = 0; column < 3; ++column)
    {
        double sum = isDirection ? 0 : (&frame.translation.x)[column];
        for (int k = 0; k < 3; ++k)
        {
            sum += p[k] * frame.rows[k][column];
        }
        out[column] = sum;
    }
    return result;
}

/// Zero-terminated names laid end to end.
std::vector<std::string> NameList(const MappedFile& file, const Chunk& chunk)
{
    std::vector<std::string> names;
    size_t offset = chunk.start;
    const size_t end = chunk.start + chunk.size;
    while (offset < end)
    {
        std::string name = StringAt(file, offset, end - offset);
        offset += name.size() + 1;
        if (offset > end)
        {
            break;
        }
        names.push_back(name);
    }
    return names;
}

/// The model's bones in the skeleton's order, with their frames in model space.
void LoadBones(
    const MappedFile& file,
    const char* modelName,
    std::vector<std::string>& names,
    std::vector<Frame>& frames)
{
    Chunk skeleton = {};
    if (!FindChunk(file, 0, file.Size(), "skel", modelName, skeleton))
    {
        return;
    }
    std::vector<std::string> parents;
    std::vector<Frame> locals;
    for (const Chunk& child : Children(file, skeleton.start, skeleton.start + skeleton.size))
    {
        if (IsTag(child, "NAME"))
        {
            names = NameList(file, child);
        }
        else if (IsTag(child, "PRNT"))
        {
            parents = NameList(file, child);
        }
        else if (IsTag(child, "XFRM"))
        {
            for (size_t offset = child.start; offset + 48 <= child.start + child.size; offset += 48)
            {
                Frame local = {};
                for (int k = 0; k < 9; ++k)
                {
                    local.rows[k / 3][k % 3] = ReadFloat(file.Data(), offset + 4 * k);
                }
                local.translation = {
                    ReadFloat(file.Data(), offset + 36),
                    ReadFloat(file.Data(), offset + 40),
                    ReadFloat(file.Data(), offset + 44)};
                locals.push_back(local);
            }
        }
    }
    frames.assign(names.size(), IDENTITY);
    std::vector<int> state(names.size(), 0); // 0 not yet, 1 in progress, 2 done
    // a bone's world frame is its local frame then its parent's (the parent found by name)
    auto resolve = [&](auto& self, size_t index) -> void
    {
        if (state[index] != 0)
        {
            return;
        }
        state[index] = 1;
        const Frame local = index < locals.size() ? locals[index] : IDENTITY;
        frames[index] = local;
        if (index < parents.size() && !parents[index].empty() && parents[index] != names[index])
        {
            for (size_t parent = 0; parent < names.size(); ++parent)
            {
                if (names[parent] == parents[index])
                {
                    self(self, parent);
                    frames[index] = Compose(local, frames[parent]);
                    break;
                }
            }
        }
        state[index] = 2;
    };
    for (size_t index = 0; index < names.size(); ++index)
    {
        resolve(resolve, index);
    }
}

} // namespace

bool LoadModel(const std::string& path, const char* modelName, Model& model)
{
    model.segments.clear();
    MappedFile file(path);
    if (!file.Data())
    {
        return false;
    }
    Chunk modelChunk = {};
    if (!FindChunk(file, 0, file.Size(), "modl", modelName, modelChunk))
    {
        return false;
    }
    model.hash = 0x811C9DC5u; // FNV-1a of the modl chunk's bytes (tells the player's copy from another)
    for (size_t index = 0; index < modelChunk.size; ++index)
    {
        model.hash = (model.hash ^ file.Data()[modelChunk.start + index]) * 0x01000193u;
    }
    std::vector<std::string> boneNames;
    std::vector<Frame> boneFrames;
    LoadBones(file, modelName, boneNames, boneFrames);
    auto frameOf = [&](const std::string& bone) -> Frame
    {
        for (size_t index = 0; index < boneNames.size(); ++index)
        {
            if (boneNames[index] == bone)
            {
                return boneFrames[index];
            }
        }
        return IDENTITY;
    };
    for (const Chunk& segmentChunk : Children(file, modelChunk.start, modelChunk.start + modelChunk.size))
    {
        if (!IsTag(segmentChunk, "segm"))
        {
            continue;
        }
        Segment segment;
        std::vector<int> strip;
        std::string bone;
        size_t bufferStart = 0;
        uint32_t bufferCount = 0, bufferStride = 0, bufferFlags = 0;
        const uint8_t* skin = nullptr;
        uint32_t skinCount = 0;
        const uint8_t* boneMap = nullptr;
        uint32_t boneMapCount = 0;
        for (const Chunk& child : Children(file, segmentChunk.start, segmentChunk.start + segmentChunk.size))
        {
            if (IsTag(child, "TNAM") && child.size > 4 && ReadUint32(file.Data(), child.start) == 0)
            {
                segment.texture = StringAt(file, child.start + 4, child.size - 4); // slot 0 the base texture, 1 bump
            }
            else if (IsTag(child, "IBUF") && child.size >= 4)
            {
                const uint32_t count = ReadUint32(file.Data(), child.start);
                for (uint32_t index = 0; index < count && 4 + 2 * index + 2 <= child.size; ++index)
                {
                    uint16_t value = 0;
                    std::memcpy(&value, file.Data() + child.start + 4 + 2 * index, 2);
                    strip.push_back(value);
                }
            }
            else if (IsTag(child, "VBUF") && child.size >= 12)
            {
                // the uncompressed buffer with the most fields (16-byte buffers are a compressed copy)
                const uint32_t stride = ReadUint32(file.Data(), child.start + 4);
                if (stride > bufferStride)
                {
                    bufferCount = ReadUint32(file.Data(), child.start);
                    bufferStride = stride;
                    bufferFlags = ReadUint32(file.Data(), child.start + 8);
                    bufferStart = child.start + 12;
                    if (12 + static_cast<size_t>(bufferCount) * stride > child.size)
                    {
                        bufferCount = 0;
                    }
                }
            }
            else if (IsTag(child, "BNAM"))
            {
                bone = StringAt(file, child.start, child.size);
            }
            else if (IsTag(child, "SKIN") && child.size >= 8)
            {
                // a count, the weights per vertex (1: hard skinned), then one BMAP index per vertex
                skinCount = ReadUint32(file.Data(), child.start);
                skin = file.Data() + child.start + 8;
                if (8 + static_cast<size_t>(skinCount) > child.size)
                {
                    skin = nullptr;
                }
            }
            else if (IsTag(child, "BMAP") && child.size >= 4)
            {
                boneMapCount = ReadUint32(file.Data(), child.start); // BMAP index -> skeleton bone index
                boneMap = file.Data() + child.start + 4;
                if (4 + static_cast<size_t>(boneMapCount) > child.size)
                {
                    boneMap = nullptr;
                }
            }
        }
        // flags: 0x2 position, 0x4 four bytes after it (bone index or colour), 0x20 normal, 0x40 tangent and
        // binormal, 0x200 texture coordinates, in that order; the texture coordinates are the last 8 bytes
        const size_t normalOffset = (bufferFlags & 0x4) ? 16 : 12;
        const Frame segmentFrame = frameOf(bone);
        const bool isSkinned = skin && boneMap && skinCount >= bufferCount;
        for (uint32_t index = 0; index < bufferCount; ++index)
        {
            const size_t vertex = bufferStart + static_cast<size_t>(index) * bufferStride;
            const Point position = {
                ReadFloat(file.Data(), vertex),
                ReadFloat(file.Data(), vertex + 4),
                ReadFloat(file.Data(), vertex + 8)};
            const Point normal = {
                ReadFloat(file.Data(), vertex + normalOffset),
                ReadFloat(file.Data(), vertex + normalOffset + 4),
                ReadFloat(file.Data(), vertex + normalOffset + 8)};
            Frame frame = segmentFrame;
            if (isSkinned)
            {
                const uint8_t mapIndex = skin[index];
                frame = IDENTITY;
                if (mapIndex < boneMapCount && boneMap[mapIndex] < boneFrames.size())
                {
                    frame = boneFrames[boneMap[mapIndex]];
                }
            }
            segment.positions.push_back(Apply(frame, position, false));
            segment.normals.push_back(Apply(frame, normal, true));
            segment.u.push_back(ReadFloat(file.Data(), vertex + bufferStride - 8));
            segment.v.push_back(ReadFloat(file.Data(), vertex + bufferStride - 4));
        }
        // one triangle strip joined by degenerate triangles; every other triangle wound the other way
        for (size_t k = 0; k + 2 < strip.size(); ++k)
        {
            const int a = strip[k], b = strip[k + 1], c = strip[k + 2];
            if (a == b || b == c || a == c || a >= static_cast<int>(bufferCount) ||
                b >= static_cast<int>(bufferCount) || c >= static_cast<int>(bufferCount))
            {
                continue;
            }
            if (k % 2 == 0)
            {
                segment.triangles.insert(segment.triangles.end(), {a, b, c});
            }
            else
            {
                segment.triangles.insert(segment.triangles.end(), {b, a, c});
            }
        }
        model.segments.push_back(std::move(segment));
    }
    return true;
}

bool LoadTexture(const std::string& path, const char* textureName, Texture& texture)
{
    texture = Texture();
    MappedFile file(path);
    if (!file.Data())
    {
        return false;
    }
    Chunk textureChunk = {};
    if (!FindChunk(file, 0, file.Size(), "tex_", textureName, textureChunk))
    {
        return false;
    }
    // the formats in order of preference (tools/lvlgeo.py TEXTURE_FORMATS)
    constexpr uint32_t FORMATS[] = {21, 23, 26};
    int bestRank = 99;
    uint32_t bestFormat = 0;
    int bestWidth = 0, bestHeight = 0;
    size_t bodyStart = 0, bodySize = 0;
    for (const Chunk& format : Children(file, textureChunk.start, textureChunk.start + textureChunk.size))
    {
        if (!IsTag(format, "FMT_"))
        {
            continue;
        }
        uint32_t code = 0;
        int width = 0, height = 0;
        size_t start = 0, size = 0;
        bool hasBody = false;
        for (const Chunk& child : Children(file, format.start, format.start + format.size))
        {
            if (IsTag(child, "INFO") && child.size >= 8)
            {
                code = ReadUint32(file.Data(), child.start);
                uint16_t w = 0, h = 0;
                std::memcpy(&w, file.Data() + child.start + 4, 2);
                std::memcpy(&h, file.Data() + child.start + 6, 2);
                width = w;
                height = h;
            }
            else if (IsTag(child, "FACE"))
            {
                for (const Chunk& level : Children(file, child.start, child.start + child.size))
                {
                    if (!IsTag(level, "LVL_") || hasBody)
                    {
                        continue;
                    }
                    for (const Chunk& body : Children(file, level.start, level.start + level.size))
                    {
                        if (IsTag(body, "BODY"))
                        {
                            start = body.start;
                            size = body.size;
                            hasBody = true;
                        }
                    }
                }
            }
        }
        for (int rank = 0; rank < 3; ++rank)
        {
            if (hasBody && code == FORMATS[rank] && rank < bestRank)
            {
                bestRank = rank;
                bestFormat = code;
                bestWidth = width;
                bestHeight = height;
                bodyStart = start;
                bodySize = size;
            }
        }
    }
    const size_t count = static_cast<size_t>(bestWidth) * bestHeight;
    const size_t bytesPerPixel = bestFormat == 21 ? 4 : 2;
    if (bestRank == 99 || count == 0 || bodySize < count * bytesPerPixel)
    {
        return false;
    }
    const uint8_t* body = file.Data() + bodyStart;
    unsigned hash = 0x811C9DC5u;
    for (size_t index = 0; index < bodySize; ++index)
    {
        hash = (hash ^ body[index]) * 0x01000193u;
    }
    texture.width = bestWidth;
    texture.height = bestHeight;
    texture.hash = hash;
    texture.pixels.resize(count);
    for (size_t index = 0; index < count; ++index)
    {
        if (bestFormat == 21)
        {
            texture.pixels[index] = ReadUint32(body, 4 * index);
            continue;
        }
        uint16_t v = 0;
        std::memcpy(&v, body + 2 * index, 2);
        if (bestFormat == 23)
        {
            const unsigned r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
            texture.pixels[index] =
                0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
        }
        else
        {
            const unsigned a = (v >> 12) & 15, r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;
            texture.pixels[index] = (a * 17) << 24 | (r * 17) << 16 | (g * 17) << 8 | b * 17;
        }
    }
    return true;
}

} // namespace LvlFile

} // namespace SwbfVr
