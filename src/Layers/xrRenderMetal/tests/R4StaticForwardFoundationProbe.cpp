#include "R4StaticForwardFoundation.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    std::string error;
    if (argc == 2 && std::string(argv[1]) == "--self-test")
        return xray::metal::RunR4StaticForwardFoundationSelfTest(error) ? 0 :
            (std::fprintf(stderr, "%s\n", error.c_str()), 1);
    if (argc == 3 && std::string(argv[1]) == "--validate-bundle")
        return xray::metal::ValidateR4StaticForwardMscBundle(argv[2], error) ? 0 :
            (std::fprintf(stderr, "%s\n", error.c_str()), 1);
    std::fprintf(stderr, "usage: R4StaticForwardFoundationProbe --self-test | --validate-bundle DIR\n");
    return 2;
}
