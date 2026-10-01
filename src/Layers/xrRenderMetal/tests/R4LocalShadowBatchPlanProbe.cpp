#include "../R4LocalShadowBatchPlan.h"

#include <cstdio>
#include <cstdlib>
#include <string>

class xrMemory
{
public:
    xrMemory() {}
    void mem_free(void* pointer);
};

xrMemory Memory;

void xrMemory::mem_free(void* pointer)
{
    std::free(pointer);
}

int main()
{
    std::string error;
    if (xray::metal::RunR4LocalShadowBatchPlanSelfTest(error))
        return 0;
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
}
