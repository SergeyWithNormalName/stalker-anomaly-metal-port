#include "stdafx.h"
#pragma hdrstop

#include "cpuid.h"

#if defined(XR_PLATFORM_APPLE) || defined(XR_ARCHITECTURE_ARM64)
#include <sys/types.h>
#include <sys/sysctl.h>

int _cpuid(_processor_info* pinfo)
{
	ZeroMemory(pinfo, sizeof(_processor_info));

	xr_strcpy(pinfo->v_name, sizeof(pinfo->v_name), "Apple");

	size_t len = sizeof(pinfo->model_name);
	if (sysctlbyname("machdep.cpu.brand_string", pinfo->model_name, &len, NULL, 0) != 0)
	{
		xr_strcpy(pinfo->model_name, sizeof(pinfo->model_name), "Apple Silicon");
	}

	int ncpu = 1;
	len = sizeof(ncpu);
	if (sysctlbyname("hw.ncpu", &ncpu, &len, NULL, 0) == 0)
	{
		pinfo->n_threads = static_cast<unsigned int>(ncpu);
	}
	else
	{
		pinfo->n_threads = 1;
	}

	int nphys = ncpu;
	len = sizeof(nphys);
	if (sysctlbyname("hw.physicalcpu", &nphys, &len, NULL, 0) == 0)
	{
		pinfo->n_cores = static_cast<unsigned int>(nphys);
	}
	else
	{
		pinfo->n_cores = pinfo->n_threads;
	}

	pinfo->affinity_mask = (pinfo->n_threads >= 32) ? 0xFFFFFFFFu : ((1u << pinfo->n_threads) - 1);

	pinfo->feature = _CPU_FEATURE_MMX | _CPU_FEATURE_SSE | _CPU_FEATURE_SSE2 | _CPU_FEATURE_SSE3 | _CPU_FEATURE_SSSE3 | _CPU_FEATURE_SSE4_1 | _CPU_FEATURE_SSE4_2;

	return pinfo->feature;
}
#else
#include <intrin.h>

#include <array>
#include <bitset>
#include <memory>

#ifdef _EDITOR
int _cpuid ( _processor_info *pinfo )
{
    ZeroMemory(pinfo, sizeof(_processor_info));

    pinfo->feature = _CPU_FEATURE_MMX | _CPU_FEATURE_SSE;
    return pinfo->feature;
}
#else

DWORD countSetBits(ULONG_PTR bitMask)
{
	DWORD LSHIFT = sizeof(ULONG_PTR) * 8 - 1;
	DWORD bitSetCount = 0;
	ULONG_PTR bitTest = static_cast<ULONG_PTR>(1) << LSHIFT;
	DWORD i;

	for (i = 0; i <= LSHIFT; ++i)
	{
		bitSetCount += ((bitMask & bitTest) ? 1 : 0);
		bitTest /= 2;
	}

	return bitSetCount;
}

#undef _CPUID_DEBUG

int _cpuid(_processor_info* pinfo)
{
	ZeroMemory(pinfo, sizeof(_processor_info));

	std::bitset<32> f_1_ECX;
	std::bitset<32> f_1_EDX;
	/*std::bitset<32> f_7_EBX;
	std::bitset<32> f_7_ECX;
	std::bitset<32> f_81_ECX;*/
	std::bitset<32> f_81_EDX;

	xr_vector<std::array<int, 4>> data;
	std::array<int, 4> cpui;

	__cpuid(cpui.data(), 0);
	const int nIds = cpui[0];

	for (int i = 0; i <= nIds; ++i)
	{
		__cpuidex(cpui.data(), i, 0);
		data.push_back(cpui);
	}

	memset(pinfo->v_name, 0, sizeof(pinfo->v_name));
	*reinterpret_cast<int*>(pinfo->v_name) = data[0][1];
	*reinterpret_cast<int*>(pinfo->v_name + 4) = data[0][3];
	*reinterpret_cast<int*>(pinfo->v_name + 8) = data[0][2];

	//const bool isIntel = std::strncmp(pinfo->vendor, "GenuineIntel", 12);
	const bool isAmd = strncmp(pinfo->v_name, "AuthenticAMD", 12) != 0;

	// load bitset with flags for function 0x00000001
	if (nIds >= 1)
	{
		f_1_ECX = data[1][2];
		f_1_EDX = data[1][3];
	}

	// load bitset with flags for function 0x00000007
	/*if (nIds >= 7)
	{
	f_7_EBX = data[7][1];
	f_7_ECX = data[7][2];
	}*/

	__cpuid(cpui.data(), 0x80000000);
	const int nExIds_ = cpui[0];
	data.clear();

	for (int i = 0x80000000; i <= nExIds_; ++i)
	{
		__cpuidex(cpui.data(), i, 0);
		data.push_back(cpui);
	}

	// load bitset with flags for function 0x80000001
	if (nExIds_ >= 0x80000001)
	{
		//f_81_ECX = data[1][2];
		f_81_EDX = data[1][3];
	}

	memset(pinfo->model_name, 0, sizeof(pinfo->model_name));

	// Interpret CPU brand string if reported
	if (nExIds_ >= 0x80000004)
	{
		memcpy(pinfo->model_name, data[2].data(), sizeof(cpui));
		memcpy(pinfo->model_name + 16, data[3].data(), sizeof(cpui));
		memcpy(pinfo->model_name + 32, data[4].data(), sizeof(cpui));
	}

	if (f_1_EDX[23]) pinfo->feature |= static_cast<u32>(_CPU_FEATURE_MMX);
	if (f_1_EDX[25]) pinfo->feature |= static_cast<u32>(_CPU_FEATURE_SSE);
	if (f_1_EDX[26]) pinfo->feature |= static_cast<u32>(_CPU_FEATURE_SSE2);
	if (isAmd && f_81_EDX[31]) pinfo->feature |= static_cast<u32>(_CPU_FEATURE_3DNOW);

	if (f_1_ECX[0]) pinfo->feature |= static_cast<u32>(_CPU_FEATURE_SSE3);
	if (f_1_ECX[9]) pinfo->feature |= static_cast<u32>(_CPU_FEATURE_SSSE3);
	if (f_1_ECX[19]) pinfo->feature |= static_cast<u32>(_CPU_FEATURE_SSE4_1);
	if (f_1_ECX[20]) pinfo->feature |= static_cast<u32>(_CPU_FEATURE_SSE4_2);

	__cpuid(cpui.data(), 1);

	const bool hasMWait = (cpui[2] & 0x8) > 0;
	if (hasMWait) pinfo->feature |= static_cast<u32>(_CPU_FEATURE_MWAIT);

	pinfo->family = (cpui[0] >> 8) & 0xf;
	pinfo->model = (cpui[0] >> 4) & 0xf;
	pinfo->stepping = cpui[0] & 0xf;

	// Calculate available processors
	ULONG_PTR pa_mask_save, sa_mask_stub = 0;
	GetProcessAffinityMask(GetCurrentProcess(), &pa_mask_save, &sa_mask_stub);

	DWORD returnedLength = 0;
	DWORD byteOffset = 0;
	GetLogicalProcessorInformation(nullptr, &returnedLength);

	auto buffer = std::make_unique<u8[]>(returnedLength);
	auto ptr = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION>(buffer.get());
	GetLogicalProcessorInformation(ptr, &returnedLength);

	auto processorCoreCount = 0u;
	auto logicalProcessorCount = 0u;

	while (byteOffset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) <= returnedLength)
	{
		switch (ptr->Relationship)
		{
		case RelationProcessorCore:
			processorCoreCount++;

			// A hyperthreaded core supplies more than one logical processor.
			logicalProcessorCount += countSetBits(ptr->ProcessorMask);
			break;

		default:
			break;
		}

		byteOffset += sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
		ptr++;
	}

	if (logicalProcessorCount != processorCoreCount) pinfo->feature |= static_cast<u32>(_CPU_FEATURE_HTT);

	// All logical processors
	pinfo->n_threads = logicalProcessorCount;
	pinfo->affinity_mask = pa_mask_save;
	pinfo->n_cores = processorCoreCount;

	return pinfo->feature;
}
#endif
#endif
