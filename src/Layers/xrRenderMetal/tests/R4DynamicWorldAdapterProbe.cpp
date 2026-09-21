#include "MetalR4DynamicWorldAdapter.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 2 || std::string(argv[1]) != "--self-test")
    {
        std::fprintf(stderr, "usage: R4DynamicWorldAdapterProbe --self-test\n");
        return 2;
    }
    std::string error;
    if (!RunR4DynamicWorldAdapterSelfTest(error))
    {
        std::fprintf(stderr, "dynamic world adapter self-test failed: %s\n", error.c_str());
        return 1;
    }
    return 0;
}
