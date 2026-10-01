#include "MetalR4DetailGrassAdapter.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

class xrMemory
{
public:
    xrMemory() {}
    void mem_free(void* p);
};
xrMemory Memory;
void xrMemory::mem_free(void* p)
{
    std::free(p);
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--self-test")
    {
        std::string error;
        if (!RunR4DetailGrassAdapterSelfTest(error))
        {
            std::fprintf(stderr, "METAL_DETAIL_GRASS_ADAPTER_SELF_TEST result=FAIL error=%s\n", error.c_str());
            return 1;
        }
        std::puts("METAL_DETAIL_GRASS_ADAPTER_SELF_TEST result=PASS parser=true v3_slots=true v4_slots=true repack_61=true validation=true");
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--level-details")
    {
        std::ifstream input(argv[2], std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>());
        R4DetailGrassSnapshot snapshot;
        std::string error;
        if (!input || !LoadR4DetailGrassSnapshot(bytes, snapshot, error))
        {
            std::fprintf(stderr, "METAL_DETAIL_GRASS_LEVEL_DETAILS result=FAIL error=%s\n",
                error.empty() ? "cannot read level.details" : error.c_str());
            return 1;
        }
        std::printf("METAL_DETAIL_GRASS_LEVEL_DETAILS result=PASS version=%u models=%zu slots=%zu grid=%ux%u\n",
            snapshot.version, snapshot.models.size(), snapshot.slots.size(), snapshot.sizeX, snapshot.sizeZ);
        return 0;
    }
    std::fprintf(stderr, "usage: R4DetailGrassAdapterProbe --self-test | --level-details <path>\n");
    return 2;
}
