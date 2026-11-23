// shellpreview: writes the cockpit shells (src/ShellDesigns.cpp) as OBJ files for tools/meshview.py and
// tools/shellcheck.py. Built and run by tools/shellpreview.sh; not part of the DLL.
//
//     shellpreview <out dir> <odf name> ...

#include "../src/ShellDesigns.h"
#include "../src/StockTables.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace SwbfVr;

static ShellKit::Vertex vertices[225000];

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("usage: shellpreview <out dir> <odf name> ...\n");
        return 1;
    }
    // the stock pieces from the game's own level files, as the DLL builds them at start (SWBF_LVL overrides)
    const char* lvlFolder = std::getenv("SWBF_LVL");
    std::puts(
        StockTables::Build(
            lvlFolder ? lvlFolder
                      : "C:/Program Files (x86)/Steam/steamapps/common/Star Wars Battlefront (Classic 2004)/GameData/"
                        "Data/_lvl_pc/")
            .c_str());
    for (int index = 2; index < argc; ++index)
    {
        ShellDesigns::Shell shell = {};
        if (!ShellDesigns::Build(argv[index], vertices, 225000, &shell))
        {
            std::printf("%s: no shell\n", argv[index]);
            continue;
        }
        char path[512];
        std::snprintf(path, sizeof(path), "%s/%s.obj", argv[1], argv[index]);
        FILE* file = std::fopen(path, "w");
        if (!file)
        {
            return 1;
        }
        // the vehicle's own exterior texture (tools/uvaudit.py leaves the game's hull out of its count)
        std::fprintf(file, "# exterior %s\n", shell.exteriorName ? shell.exteriorName : "-");
        // whether the game also draws its own first-person model round the shell (tools/allpositions.py loads it)
        std::fprintf(file, "# whole %d\n", ShellDesigns::IsWholeCockpit(argv[index]) ? 1 : 0);
        float eye[3];
        if (ShellDesigns::DesignEye(argv[index], eye))
        {
            std::fprintf(file, "# eye %.3f %.3f %.3f\n", eye[0], eye[1], eye[2]);
        }
        for (int i = 0; i < shell.vertexCount; ++i)
        {
            const ShellKit::Vertex& v = vertices[i];
            std::fprintf(
                file,
                "v %.4f %.4f %.4f\nvt %.4f %.4f\n",
                v.position.x,
                v.position.y,
                v.position.z,
                v.u,
                1.0f - v.v);
        }
        int last = -1, lastPart = 0;
        for (int i = 0; i + 2 < shell.vertexCount; i += 3)
        {
            const int finish = vertices[i].finish;
            if (vertices[i].part != lastPart)
            {
                lastPart = vertices[i].part;
                std::fprintf(file, "# part %d\n", lastPart); // for posing tools (meshview ignores it)
            }
            if (finish != last)
            {
                last = finish;
                const ShellKit::Finish& f = shell.finishes[finish];
                const char* texture = f.texture == 1   ? shell.textureName
                                      : f.texture == 2 ? shell.effectsName
                                      : f.texture == 3 ? shell.exteriorName
                                      : f.texture == 4 ? shell.glassName
                                                       : nullptr;
                const char* name = f.opacity < 1.0f ? "#glass" : texture ? texture : f.glow > 0 ? "#glow" : "#flat";
                if (f.opacity < 1.0f && texture)
                {
                    std::fprintf(file, "usemtl #glass=%s\n", texture); // textured glass (the preview blends it)
                }
                else
                {
                    std::fprintf(file, "usemtl %s\n", name);
                }
                std::fprintf(file, "# color %06X\n# finish %d\n", static_cast<unsigned>(f.color), finish);
            }
            std::fprintf(file, "f %d/%d %d/%d %d/%d\n", i + 1, i + 1, i + 2, i + 2, i + 3, i + 3);
        }
        // The HUD displays as flat glowing rectangles (their room on the screen faces), coloured by group.
        static const unsigned GROUP_COLORS[5] = {0x40C040, 0xFFC040, 0xFF4040, 0x4080FF, 0x30A0A0};
        for (int d = 0; d < shell.displayCount; ++d)
        {
            const ShellDesigns::Display& display = shell.displays[d];
            const ShellKit::Vec3 halfX = display.right * (display.width * 0.5f);
            const ShellKit::Vec3 halfY = display.up * (display.height * 0.5f);
            const ShellKit::Vec3 corners[4] = {
                display.center - halfX - halfY,
                display.center + halfX - halfY,
                display.center + halfX + halfY,
                display.center - halfX + halfY};
            std::fprintf(file, "usemtl #glow\n# color %06X\n", GROUP_COLORS[display.group % 5]);
            for (const ShellKit::Vec3& corner : corners)
            {
                std::fprintf(file, "v %.4f %.4f %.4f\nvt 0 0\n", corner.x, corner.y, corner.z);
            }
            const int base = shell.vertexCount + 4 * d + 1;
            std::fprintf(file, "f %d/%d %d/%d %d/%d\n", base, base, base + 1, base + 1, base + 2, base + 2);
            std::fprintf(file, "f %d/%d %d/%d %d/%d\n", base, base, base + 2, base + 2, base + 3, base + 3);
        }
        for (int k = 0; k + 2 < shell.maskVertexCount; k += 3)
        {
            std::fprintf(file, "# cutout");
            for (int corner = 0; corner < 3; ++corner)
            {
                const ShellKit::Vec3& p = shell.maskVertices[k + corner];
                std::fprintf(file, " %.4f %.4f %.4f", p.x, p.y, p.z);
            }
            std::fprintf(file, "\n");
        }
        std::fclose(file);
        std::printf(
            "%s: %d triangles%s, %d displays -> %s\n",
            argv[index],
            shell.vertexCount / 3,
            shell.isFull ? " (FULL)" : "",
            shell.displayCount,
            path);
    }
    return 0;
}
