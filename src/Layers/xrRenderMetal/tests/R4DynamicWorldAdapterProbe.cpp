#include "MetalR4DynamicWorldAdapter.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    std::filesystem::path emissiveBundle;
    if (argc == 4 && std::string(argv[1]) == "--self-test" &&
        std::string(argv[2]) == "--emissive-bundle")
    {
        emissiveBundle = argv[3];
    }
    else if (argc != 2 || std::string(argv[1]) != "--self-test")
    {
        std::fprintf(stderr,
            "usage: R4DynamicWorldAdapterProbe --self-test [--emissive-bundle RECEIPTED_DIRECTORY]\n");
        return 2;
    }
    std::string error;
    if (!RunR4DynamicWorldAdapterSelfTest(error))
    {
        std::fprintf(stderr, "dynamic world adapter self-test failed: %s\n", error.c_str());
        return 1;
    }
    if (!emissiveBundle.empty() && !RunR4SelflightLMSCAdmissionSelfTest(emissiveBundle, error))
    {
        std::fprintf(stderr, "selflightl MSC admission self-test failed: %s\n", error.c_str());
        return 1;
    }
    return 0;
}

// Standalone adapter probes have no Lua VM; Core diagnostics may ask for its stack.
void printLuaStack() {}
