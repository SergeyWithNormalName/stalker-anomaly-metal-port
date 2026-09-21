#include "MetalR4ScopeAdapter.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 2 || std::string(argv[1]) != "--self-test")
    {
        std::fprintf(stderr, "usage: MetalR4ScopeAdapterProbe --self-test\n");
        return 2;
    }
    std::string error;
    if (!RunMetalR4ScopeAdapterSelfTest(error))
    {
        std::fprintf(stderr, "3DSS scope adapter self-test failed: %s\n", error.c_str());
        return 1;
    }
    return 0;
}
