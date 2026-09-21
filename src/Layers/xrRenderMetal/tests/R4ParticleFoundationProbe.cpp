#include "R4ParticleFoundation.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    std::string error;
    if (argc == 2 && std::string(argv[1]) == "--self-test")
    {
        if (xray::metal::RunR4ParticleFoundationSelfTest(error))
            return 0;
        std::fprintf(stderr, "particle foundation self-test failed: %s\n", error.c_str());
        return 1;
    }
    if (argc == 3 && std::string(argv[1]) == "--validate-bundle")
    {
        if (xray::metal::ValidateR4ParticleMscBundle(argv[2], error))
            return 0;
        std::fprintf(stderr, "particle MSC bundle validation failed: %s\n", error.c_str());
        return 1;
    }
    std::fprintf(stderr,
        "usage: R4ParticleFoundationProbe --self-test | --validate-bundle <directory>\n");
    return 2;
}
