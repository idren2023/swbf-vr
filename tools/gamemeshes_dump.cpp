// tools/gamemeshes_dump.cpp: makes src/GameMeshes from the game's level files and writes each table's eight floats
// per vertex (position, normal, uv) raw to <out dir>/<TABLE>.bin, for checking against the tables compiled before
// (tools/meshpatch.py check=<out dir>).
//   build/test/gamemeshes_dump.exe <_lvl_pc folder with a trailing slash> <out dir> <TABLE>=<count> ...
#include "../src/GameMeshes.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace SwbfVr;

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        return 1;
    }
    std::vector<std::string> names;
    std::vector<std::vector<float>> tables;
    for (int k = 3; k < argc; ++k)
    {
        const std::string arg = argv[k];
        const size_t eq = arg.find('=');
        names.push_back(arg.substr(0, eq));
        tables.emplace_back(static_cast<size_t>(std::atoi(arg.c_str() + eq + 1)) * 8, 0.f);
    }
    for (size_t k = 0; k < names.size(); ++k)
    {
        GameMeshes::Register(
            names[k].c_str(),
            tables[k].data(),
            8 * sizeof(float),
            static_cast<int>(tables[k].size() / 8));
    }
    std::printf("%s\n", GameMeshes::Build(argv[1]).c_str());
    for (size_t k = 0; k < names.size(); ++k)
    {
        FILE* f = std::fopen((std::string(argv[2]) + "/" + names[k] + ".bin").c_str(), "wb");
        std::fwrite(tables[k].data(), sizeof(float), tables[k].size(), f);
        std::fclose(f);
    }
    return 0;
}
