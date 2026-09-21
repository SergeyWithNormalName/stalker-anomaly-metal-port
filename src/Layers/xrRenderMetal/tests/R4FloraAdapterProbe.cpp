#include "MetalR4FloraAdapter.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 2 || std::string(argv[1]) != "--self-test")
    {
        std::fprintf(stderr, "usage: R4FloraAdapterProbe --self-test\n");
        return 2;
    }
    std::string error;
    if (!RunR4FloraAdapterSelfTest(error))
    {
        std::fprintf(stderr, "METAL_FLORA_ADAPTER_SELF_TEST result=FAIL error=%s\n", error.c_str());
        return 1;
    }
    std::puts("METAL_FLORA_ADAPTER_SELF_TEST result=PASS repack=true xrfs=true traversal_rejected=true auto_selection=true frustum_culling=true");
    return 0;
}
