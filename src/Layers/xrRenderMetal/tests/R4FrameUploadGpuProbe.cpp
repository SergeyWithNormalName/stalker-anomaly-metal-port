#include "IR4Backend.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    const bool oldPath = argc == 2 && std::string(argv[1]) == "--old-path";
    const bool reprojOnly = argc == 2 && std::string(argv[1]) == "--reprojection";
    const bool selfTest = argc == 2 && std::string(argv[1]) == "--self-test";
    const bool recovery = argc == 2 && std::string(argv[1]) == "--private-upload-recovery";
    if (argc != 2 || (!oldPath && !selfTest && !reprojOnly && !recovery))
    {
        std::fprintf(stderr, "usage: R4FrameUploadGpuProbe --old-path|--self-test|--reprojection|--private-upload-recovery\n");
        return 2;
    }
    std::string error;
    if (recovery)
    {
        const bool passed = RunMetalR4PrivateUploadRecoveryProbe(error);
        std::fprintf(stderr, "METAL_PRIVATE_UPLOAD_RECOVERY_PROBE status=%s detail=\"%s\"\n",
            passed ? "PASS" : "FAIL", error.c_str());
        return passed ? 0 : 1;
    }
    if (reprojOnly)
    {
        const bool passed = RunMetalR4ReprojectionProbe(error);
        std::fprintf(stderr, "E04_REPROJECTION_PROBE status=%s detail=\"%s\"\n",
            passed ? "PASS" : "FAIL", error.c_str());
        return passed ? 0 : 1;
    }
    const bool uploadPassed = RunMetalR4FrameUploadGpuProbe(oldPath, error);
    std::fprintf(stderr, "E02_FRAME_UPLOAD_GPU mode=%s status=%s detail=\"%s\"\n",
        oldPath ? "old-path" : "arena", uploadPassed ? "PASS" : "FAIL", error.c_str());
    if (!uploadPassed)
        return 1;
    if (selfTest)
    {
        std::string reprojError;
        const bool reprojPassed = RunMetalR4ReprojectionProbe(reprojError);
        std::fprintf(stderr, "E04_REPROJECTION_PROBE status=%s detail=\"%s\"\n",
            reprojPassed ? "PASS" : "FAIL", reprojError.c_str());
        if (!reprojPassed)
            return 1;
    }
    return 0;
}
