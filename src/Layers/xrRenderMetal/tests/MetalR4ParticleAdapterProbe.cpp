#include "MetalR4ParticleAdapter.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 2 || std::string(argv[1]) != "--self-test")
    {
        std::fprintf(stderr, "usage: MetalR4ParticleAdapterProbe --self-test\n");
        return 2;
    }
    std::string error;
    if (RunMetalR4ParticleAdapterSelfTest(error))
        return 0;
    std::fprintf(stderr, "particle adapter self-test failed: %s\n", error.c_str());
    return 1;
}
