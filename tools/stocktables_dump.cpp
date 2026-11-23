// tools/stocktables_dump.cpp: builds src/StockTables from the game's level files and writes every table in the
// format of the Python generators' includes, for tools/inccompare.py (the C++ port checked against Python).
//   build/test/stocktables_dump.exe <_lvl_pc folder with a trailing slash> <out.inc>
#include "../src/StockTables.h"

#include <cstdio>

using namespace SwbfVr;

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("usage: stocktables_dump <lvl folder/> <out.inc>\n");
        return 1;
    }
    std::printf("%s\n", StockTables::Build(argv[1]).c_str());
    FILE* file = std::fopen(argv[2], "w");
    if (!file)
    {
        return 1;
    }
    for (const char* const* name = StockTables::Names(); *name; ++name)
    {
        const StockTables::Table table = StockTables::Get(*name);
        std::fprintf(file, "constexpr int %s_COUNT = %d;\n", *name, table.count);
        std::fprintf(file, "constexpr StockVertex %s[%d] = {\n", *name, table.count > 0 ? table.count : 1);
        for (int index = 0; index < table.count; ++index)
        {
            const StockTables::StockVertex& v = table.vertices[index];
            std::fprintf(
                file, "    {{%.4ff, %.4ff, %.4ff}, {%.4ff, %.4ff, %.4ff}, %.5ff, %.5ff, %d},\n", v.position.x,
                v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z, v.u, v.v, v.texture);
        }
        std::fprintf(file, "};\n");
    }
    std::fclose(file);
    return 0;
}
