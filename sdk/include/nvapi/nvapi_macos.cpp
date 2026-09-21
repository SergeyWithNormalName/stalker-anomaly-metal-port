#if defined(__APPLE__)

#include "nvapi.h"

extern "C"
{
NvAPI_Status NvAPI_Initialize()
{
    return NVAPI_NVIDIA_DEVICE_NOT_FOUND;
}

NvAPI_Status NvAPI_EnumLogicalGPUs(NvLogicalGpuHandle[], NvU32* count)
{
    if (count)
        *count = 0;
    return NVAPI_NVIDIA_DEVICE_NOT_FOUND;
}

NvAPI_Status NvAPI_EnumPhysicalGPUs(NvPhysicalGpuHandle[], NvU32* count)
{
    if (count)
        *count = 0;
    return NVAPI_NVIDIA_DEVICE_NOT_FOUND;
}

NvAPI_Status NvAPI_GetPhysicalGPUsFromLogicalGPU(NvLogicalGpuHandle, NvPhysicalGpuHandle[], NvU32* count)
{
    if (count)
        *count = 0;
    return NVAPI_NVIDIA_DEVICE_NOT_FOUND;
}
}

#endif
