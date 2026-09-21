#include "R4DefObjectsLodFoundation.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 2 || std::string(argv[1]) != "--self-test")
    {
        std::fprintf(stderr, "usage: R4DefObjectsLodFoundationProbe --self-test\n");
        return 2;
    }
    std::string error;
    if (!xray::metal::RunR4DefObjectsLodFoundationSelfTest(error))
    {
        std::fprintf(stderr, "def_objects_lod foundation self-test failed: %s\n", error.c_str());
        return 1;
    }
    return 0;
}
