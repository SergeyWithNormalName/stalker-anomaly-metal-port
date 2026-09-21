#include "stdafx.h"
#include <errno.h>

#if defined(XR_PLATFORM_WINDOWS)
#include <malloc.h>

XRCORE_API void vminfo(size_t* _free, size_t* reserved, size_t* committed)
{
	MEMORY_BASIC_INFORMATION memory_info;
	memory_info.BaseAddress = 0;
	*_free = *reserved = *committed = 0;
	while (VirtualQuery(memory_info.BaseAddress, &memory_info, sizeof(memory_info)))
	{
		switch (memory_info.State)
		{
		case MEM_FREE:
			*_free += memory_info.RegionSize;
			break;
		case MEM_RESERVE:
			*reserved += memory_info.RegionSize;
			break;
		case MEM_COMMIT:
			*committed += memory_info.RegionSize;
			break;
		}
		memory_info.BaseAddress = (char*)memory_info.BaseAddress + memory_info.RegionSize;
	}
}

#elif defined(XR_PLATFORM_APPLE)
#include <malloc/malloc.h>
#include <sys/resource.h>
#include <mach/mach.h>
#include <mach/vm_statistics.h>
#include <mach/mach_init.h>
#include <mach/mach_host.h>

XRCORE_API void vminfo(size_t* _free, size_t* reserved, size_t* committed)
{
	vm_size_t page_size = 0;
	mach_port_t mach_port = mach_host_self();
	vm_statistics64_data_t vm_stats;
	mach_msg_type_number_t count = sizeof(vm_stats) / sizeof(natural_t);
	if (KERN_SUCCESS == host_page_size(mach_port, &page_size) &&
		KERN_SUCCESS == host_statistics64(mach_port, HOST_VM_INFO64, (host_info64_t)&vm_stats, &count))
	{
		*_free = (size_t)vm_stats.free_count * (size_t)page_size;
		*reserved = ((size_t)vm_stats.wire_count + (size_t)vm_stats.compressor_page_count) * (size_t)page_size;
		*committed = (size_t)vm_stats.active_count * (size_t)page_size;
	}
	else
	{
		*_free = *reserved = *committed = 0;
	}
}
#else
#include <malloc.h>
XRCORE_API void vminfo(size_t* _free, size_t* reserved, size_t* committed)
{
	*_free = *reserved = *committed = 0;
}
#endif

xrCriticalSection mem_lock;
XRCORE_API void log_vminfo()
{
    xrCriticalSectionGuard g(mem_lock);
	PROF_EVENT("log_vminfo");
	size_t w_free, w_reserved, w_committed;
	vminfo(&w_free, &w_reserved, &w_committed);
	Msg(
		"* [memory]: free[%llu K], reserved[%llu K], committed[%llu K]",
		static_cast<unsigned long long>(w_free / 1024),
		static_cast<unsigned long long>(w_reserved / 1024),
		static_cast<unsigned long long>(w_committed / 1024)
	);
}

size_t xrMemory::mem_usage(bool assert)
{
    xrCriticalSectionGuard g(mem_lock);
	PROF_EVENT("mem_usage");
#if defined(XR_PLATFORM_WINDOWS)
	_HEAPINFO hinfo = {};
	int status;
	size_t bytesUsed = 0;
	while ((status = _heapwalk(&hinfo)) == _HEAPOK)
	{
		if (hinfo._useflag == _USEDENTRY)
			bytesUsed += hinfo._size;
	}
	switch (status)
	{
	case _HEAPEMPTY:
		break;
	case _HEAPEND:
		break;
	case _HEAPBADPTR:
		if (assert) FATAL("bad pointer to heap");
		break;
	case _HEAPBADBEGIN:
        if (assert) FATAL("bad start of heap");
		break;
	case _HEAPBADNODE:
        if (assert) FATAL("bad node in heap");
		break;
	}
	return bytesUsed;
#elif defined(XR_PLATFORM_APPLE)
	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	return (size_t)ru.ru_maxrss;
#else
	return 0;
#endif
}
