#include "../IR4Backend.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 2 || std::string(argv[1]) != "--self-test")
    {
        std::fprintf(stderr, "usage: R4WholeLightVisibilityBackendGpuProbe --self-test\n");
        return 2;
    }
    std::string error;
    if (!RunMetalR4WholeLightVisibilityGpuProbe(error))
    {
        std::fprintf(stderr, "WHOLE_LIGHT_VISIBILITY_BACKEND_GPU status=FAIL error=%s\n",
            error.c_str());
        return 1;
    }
    std::fprintf(stderr, "WHOLE_LIGHT_VISIBILITY_BACKEND_GPU status=PASS\n");
    return 0;
}
