#include "R4DefVertexFoundation.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--self-test")
    {
        std::string error;
        if (!xray::metal::RunR4DefVertexFoundationSelfTest(error))
        {
            std::fprintf(stderr, "def_vertex foundation self-test failed: %s\n", error.c_str());
            return 1;
        }
        return 0;
    }

    if (argc == 3 && std::string(argv[1]) == "--validate-bundle")
    {
        std::string error;
        if (!xray::metal::ValidateR4DefVertexFlatMscBundle(argv[2], error))
        {
            std::fprintf(stderr, "def_vertex MSC bundle validation failed: %s\n", error.c_str());
            return 1;
        }
        return 0;
    }

    std::fprintf(stderr,
        "usage: R4DefVertexFoundationProbe --self-test | --validate-bundle <directory>\n");
    return 2;
}
