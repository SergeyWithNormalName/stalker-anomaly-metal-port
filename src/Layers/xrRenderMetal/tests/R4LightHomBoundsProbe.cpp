#include "../../xrRender/HOMDiagnosticBounds.h"
#include <cstdio>
#include <limits>

int main()
{
    const std::array<float,3> lo{.2f,.2f,.2f}, hi{.3f,.3f,.3f}, eye{0,0,0};
    std::array<float,16> matrix{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const auto test=[&](const std::array<float,3>& a, const std::array<float,3>& b,
        const std::array<float,3>& camera, unsigned frame, unsigned rendered) {
        return HomDiagnosticBoxCanBeTested(a,b,camera,matrix,frame,rendered);
    };
    bool ok=test(lo,hi,eye,9,9);
    ok &= !test(lo,hi,eye,9,8); // Old camera results cannot be reused.
    ok &= !test(lo,hi,eye,UINT32_MAX,UINT32_MAX);
    ok &= !test(lo,hi,{.25f,.25f,.25f},9,9);
    ok &= !test({.2f,.2f,-.1f},hi,eye,9,9); // Near-plane crossing.
    ok &= !test(lo,{.3f,.3f,1.1f},eye,9,9); // Far-plane crossing.
    ok &= !test(hi,lo,eye,9,9);
    matrix[0]=std::numeric_limits<float>::quiet_NaN();
    ok &= !test(lo,hi,eye,9,9);
    matrix[0]=std::numeric_limits<float>::max();
    ok &= !test({2,2,.2f},{3,3,.3f},eye,9,9); // Projection overflow.
    matrix[0]=1; matrix[15]=0;
    ok &= !test(lo,hi,eye,9,9); // Zero/behind-camera w.
    if (!ok) std::fprintf(stderr,"HOM diagnostic projection admission failed\n");
    return ok?0:1;
}
