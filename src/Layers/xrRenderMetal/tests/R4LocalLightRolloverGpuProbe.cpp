#include "IR4Backend.h"

#include <cstdio>
#include <cstdlib>
#include <string>

void InitializeR4RolloverProbeRuntime();
void ShutdownR4RolloverProbeRuntime();

int main(int argc, char** argv)
{
    if (argc != 5 || std::string(argv[1]) != "--gpu")
    {
        std::fprintf(stderr,
            "usage: R4LocalLightRolloverGpuProbe --gpu <local-light-msc-dir> <light-count> <artifact-dir>\n");
        return 2;
    }

    InitializeR4RolloverProbeRuntime();
    std::string error;
    const std::size_t lightCount = static_cast<std::size_t>(std::strtoul(argv[3], nullptr, 10));
    const bool passed = RunMetalR4LocalLightRolloverGpuProbe(argv[2], argv[4], lightCount, error);
    std::fprintf(stderr, "E05_LOCAL_LIGHT_ROLLOVER_GPU status=%s detail=\"%s\"\n",
        passed ? "PASS" : "FAIL", error.c_str());
    ShutdownR4RolloverProbeRuntime();
    return passed ? 0 : 1;
}
