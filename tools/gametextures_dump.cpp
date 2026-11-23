// tools/gametextures_dump.cpp: makes src/GameTextures from the game's level files and writes each texture raw
// (0xAARRGGBB little-endian) to <out dir>/<TABLE>.bin, for checking against the textures compiled before.
//   build/test/gametextures_dump.exe <_lvl_pc folder with a trailing slash> <out dir>
#include "../src/GameTextures.h"

#include <cstdio>
#include <string>

using namespace SwbfVr;

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        return 1;
    }
    std::printf("%s\n", GameTextures::Build(argv[1]).c_str());
    const char* names[] = {"BODY_ARM_TEXTURE_0", "BODY_ARM_TEXTURE_1", "BODY_ARM_TEXTURE_2", "BODY_ARM_TEXTURE_3",
                           "PLAYER_BODY_TEXELS_0", "PLAYER_BODY_TEXELS_1", "PLAYER_BODY_TEXELS_2"};
    for (const char* name : names)
    {
        int width = 0, height = 0;
        const unsigned* pixels = GameTextures::Pixels(name, width, height);
        if (!pixels)
        {
            std::printf("%s: none\n", name);
            continue;
        }
        FILE* file = std::fopen((std::string(argv[2]) + "/" + name + ".bin").c_str(), "wb");
        std::fwrite(pixels, 4, static_cast<size_t>(width) * height, file);
        std::fclose(file);
    }
    return 0;
}
