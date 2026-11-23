// leverpreview.cpp: a starfighter's flight handles (LeverModel's JetStick, Throttle and the take-off lever's Tank grip,
// placed as Flight places them) as an OBJ in a cockpit shell's space, for tools/meshview.py and tools/shelltour.py
// beside the shell's own OBJ (tools/fightertour.sh builds and runs it).
//
//     leverpreview.exe <out.obj> [eyeX eyeY eyeZ] [landed|flying] [notch -1|0|1] [highlight]
//
// Positions: Flight's STICK_PIVOT, THROTTLE_PIVOT and LEVER_PIVOT (resting-pose space; copied below, keep in step) +
// the shell's eye (default 0, 1.5, 1.5; ShellDesigns' design eye). `highlight` colours each handle in one loud colour
// (the stick red, the throttle yellow, the take-off lever cyan) so a render shows at once what of it the pilot sees.

#include "../src/LeverModel.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace SwbfVr
{
namespace Log
{
void Info(const char*, const char*, ...) {}
void Warning(const char*, const char*, ...) {}
} // namespace Log
namespace Textures
{
void* FindByName(const char*)
{
    return nullptr;
}
} // namespace Textures
} // namespace SwbfVr

using namespace SwbfVr;

namespace
{
// Flight.cpp's constants (kept in step by hand: Flight's are in an anonymous namespace).
constexpr float RADIANS_PER_DEGREE = 3.14159265f / 180.0f;
constexpr float STICK_PIVOT[3] = {0.0f, -0.86f, -0.34f};
constexpr float THROTTLE_PIVOT[3] = {-0.25f, -0.64f, -0.27f};
constexpr float LEVER_PIVOT[3] = {-0.36f, -0.62f, -0.10f};
constexpr float LEVER_DOWN_RADIANS = 5.0f * RADIANS_PER_DEGREE, LEVER_UP_RADIANS = 45.0f * RADIANS_PER_DEGREE;
constexpr float NOTCH_SPACING_METERS = 0.08f;
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: leverpreview <out.obj> [eyeX eyeY eyeZ] [landed|flying] [notch] [highlight]\n");
        return 1;
    }
    float eye[3] = {0.0f, 1.5f, 1.5f};
    if (argc >= 5)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            eye[axis] = static_cast<float>(std::atof(argv[2 + axis]));
        }
    }
    const bool isFlying = argc >= 6 && !std::strcmp(argv[5], "flying");
    const int notch = argc >= 7 ? std::atoi(argv[6]) : 0;
    const bool isHighlit = argc >= 8 && !std::strcmp(argv[7], "highlight");
    FILE* file = std::fopen(argv[1], "w");
    if (!file)
    {
        return 1;
    }
    // metal, polished, rubber, button (LeverModel's neutral style)
    static const unsigned COLORS[4] = {0x6E7076, 0x9A9CA2, 0x2A2A2C, 0xE03020};
    static const unsigned LOUD[3] = {0xFF2020, 0xFFE000, 0x00E0FF};
    struct Placed
    {
        LeverModel::Model model;
        const float* pivot;
        float forward; // the grip's lean ahead (radians), as Flight::Placement
    };
    const float leverAngle = isFlying ? LEVER_DOWN_RADIANS : LEVER_UP_RADIANS;
    const Placed placed[3] = {
        {LeverModel::Model::JetStick, STICK_PIVOT, 0.0f},
        {LeverModel::Model::Throttle, THROTTLE_PIVOT, notch * NOTCH_SPACING_METERS / LeverModel::THROTTLE_GRIP_METERS},
        {LeverModel::Model::Tank, LEVER_PIVOT, 90.0f * RADIANS_PER_DEGREE - leverAngle}};
    static float positions[36000][3];
    static int kinds[36000];
    int base = 1;
    for (int handle = 0; handle < 3; ++handle)
    {
        const Placed& at = placed[handle];
        const int count = LeverModel::PreviewMesh(at.model, positions, kinds, 36000);
        // the grip's axes leaning ahead (Flight::GripAxes with no side lean): up (0, cos, -sin), back (0, sin, cos)
        const float c = std::cos(at.forward), s = std::sin(at.forward);
        int lastKind = -1;
        for (int i = 0; i < count; ++i)
        {
            float p[3] = {positions[i][0], positions[i][1], positions[i][2]};
            if (kinds[i] / 4 >= 1) // the grip and trigger lean; the carriage stays on the hull's axes
            {
                const float y = p[1], z = p[2];
                p[1] = y * c + z * s;
                p[2] = -y * s + z * c;
            }
            std::fprintf(file, "v %.4f %.4f %.4f\nvt 0 0\n", p[0] + at.pivot[0] + eye[0], p[1] + at.pivot[1] + eye[1],
                         p[2] + at.pivot[2] + eye[2]);
        }
        for (int i = 0; i + 2 < count; i += 3)
        {
            if (kinds[i] != lastKind)
            {
                lastKind = kinds[i];
                std::fprintf(file, "usemtl #flat\n# color %06X\n", isHighlit ? LOUD[handle] : COLORS[lastKind & 3]);
            }
            std::fprintf(file, "f %d/%d %d/%d %d/%d\n", base + i, base + i, base + i + 1, base + i + 1, base + i + 2,
                         base + i + 2);
        }
        base += count;
        std::printf("model %d: %d triangles\n", static_cast<int>(at.model), count / 3);
    }
    std::fclose(file);
    return 0;
}
