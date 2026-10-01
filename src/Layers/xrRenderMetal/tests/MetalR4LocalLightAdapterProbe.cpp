#include "MetalR4LocalLightAdapter.h"

#include <cstdio>
#include <cstdlib>
#include <string>

class xrMemory
{
public:
    xrMemory() {}
    void mem_free(void* p);
    void* mem_alloc(std::size_t size, bool);
};
xrMemory Memory;
void xrMemory::mem_free(void* p)
{
    std::free(p);
}
void* xrMemory::mem_alloc(std::size_t size, bool)
{
    return std::malloc(size);
}

int main(int argc, char** argv)
{
    if (argc != 2 || std::string(argv[1]) != "--self-test")
    {
        std::fprintf(stderr, "usage: MetalR4LocalLightAdapterProbe --self-test\n");
        return 2;
    }
    std::string error;
    if (RunMetalR4LocalLightAdapterSelfTest(error))
        return 0;
    std::fprintf(stderr, "local-light adapter self-test failed: %s\n", error.c_str());
    return 1;
}
