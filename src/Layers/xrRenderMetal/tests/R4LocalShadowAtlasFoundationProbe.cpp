#include "R4LocalShadowAtlasFoundation.h"
#include <cstdio>
#include <cstdlib>

class xrMemory
{
public:
    xrMemory() {}
    void mem_free(void* p);
};
xrMemory Memory;
void xrMemory::mem_free(void* p)
{
    std::free(p);
}

int main(){std::string e; if(xray::metal::RunR4LocalShadowAtlasFoundationSelfTest(e))return 0;std::fprintf(stderr,"%s\n",e.c_str());return 1;}

