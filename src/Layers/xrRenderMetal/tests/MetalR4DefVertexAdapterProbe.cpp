#include "MetalR4DefVertexAdapter.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 2 || std::string(argv[1]) != "--self-test")
    {
        std::fprintf(stderr, "usage: MetalR4DefVertexAdapterProbe --self-test\n");
        return 2;
    }
    std::string error;
    if (!RunMetalR4DefVertexAdapterSelfTest(error))
    {
        std::fprintf(stderr, "def_vertex adapter self-test failed: %s\n", error.c_str());
        return 1;
    }
    return 0;
}
