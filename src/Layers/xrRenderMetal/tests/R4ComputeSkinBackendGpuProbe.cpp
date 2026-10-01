#include "../IR4Backend.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 2 || std::string(argv[1]) != "--self-test")
    {
        std::fprintf(stderr, "usage: R4ComputeSkinBackendGpuProbe --self-test\n");
        return 2;
    }
    std::string error;
    if (!RunMetalR4ComputeSkinBackendGpuProbe(error))
    {
        std::fprintf(stderr, "COMPUTE_SKIN_BACKEND_GPU_PROBE status=FAIL error=%s\n",
            error.c_str());
        return 1;
    }
    std::fprintf(stderr, "COMPUTE_SKIN_BACKEND_GPU_PROBE status=PASS\n");
    return 0;
}
