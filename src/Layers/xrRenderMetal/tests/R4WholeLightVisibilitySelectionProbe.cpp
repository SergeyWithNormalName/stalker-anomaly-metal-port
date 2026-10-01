#include "../R4WholeLightVisibilitySelection.h"

#include <cstdio>
#include <string>

int main()
{
    std::string error;
    if (!xray::metal::RunR4WholeLightVisibilitySelectionSelfTest(error))
    {
        std::fprintf(stderr, "WHOLE_LIGHT_SELECTION FAIL %s\n", error.c_str());
        return 1;
    }
    std::fprintf(stderr, "WHOLE_LIGHT_SELECTION PASS complete-groups batch-coverage exclusion-rotation frame-cap\n");
    return 0;
}
