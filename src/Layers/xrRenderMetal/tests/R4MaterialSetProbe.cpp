#include "R4MaterialSet.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 2 || std::string(argv[1]) != "--self-test")
    {
        std::fprintf(stderr, "usage: R4MaterialSetProbe --self-test\n");
        return 2;
    }
    std::string error;
    if (!xray::metal::RunR4ControlledMaterialSetSelfTest(error))
    {
        std::fprintf(stderr, "R4 controlled material-set self-test failed: %s\n", error.c_str());
        return 1;
    }
    return 0;
}
