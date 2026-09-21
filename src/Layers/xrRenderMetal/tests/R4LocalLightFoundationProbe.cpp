#include "R4LocalLightFoundation.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    std::string error;
    if (argc == 2 && std::string(argv[1]) == "--self-test")
    {
        if (xray::metal::RunR4LocalLightFoundationSelfTest(error))
            return 0;
        std::fprintf(stderr, "local-light foundation self-test failed: %s\n", error.c_str());
        return 1;
    }
    if (argc == 3 && std::string(argv[1]) == "--validate-bundle")
    {
        if (xray::metal::ValidateR4LocalLightMscBundle(argv[2], error))
            return 0;
        std::fprintf(stderr, "local-light MSC bundle validation failed: %s\n", error.c_str());
        return 1;
    }
    std::fprintf(stderr,
        "usage: R4LocalLightFoundationProbe --self-test | --validate-bundle <directory>\n");
    return 2;
}
