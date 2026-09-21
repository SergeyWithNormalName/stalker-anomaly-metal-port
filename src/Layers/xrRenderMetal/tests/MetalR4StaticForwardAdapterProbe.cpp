#include "MetalR4StaticForwardAdapter.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    std::string error;
    if (argc == 2 && std::string(argv[1]) == "--self-test")
        return RunMetalR4StaticForwardAdapterSelfTest(error) ? 0 :
            (std::fprintf(stderr, "%s\n", error.c_str()), 1);
    std::fprintf(stderr, "usage: MetalR4StaticForwardAdapterProbe --self-test\n");
    return 2;
}
