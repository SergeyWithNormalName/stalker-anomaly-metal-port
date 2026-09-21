#include "R4LocalShadowAtlasFoundation.h"
#include <cstdio>
int main(){std::string e; if(xray::metal::RunR4LocalShadowAtlasFoundationSelfTest(e))return 0;std::fprintf(stderr,"%s\n",e.c_str());return 1;}
