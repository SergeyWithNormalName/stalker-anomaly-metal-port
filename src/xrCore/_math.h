#ifndef __XR_MATH_H__
#define __XR_MATH_H__

#include "cpuid.h"

namespace FPU
{
	XRCORE_API void m24(void);
	XRCORE_API void m24r(void);
	XRCORE_API void m53(void);
	XRCORE_API void m53r(void);
	XRCORE_API void m64(void);
	XRCORE_API void m64r(void);
};

namespace CPU
{
	XRCORE_API extern u64 clk_per_second;
	XRCORE_API extern u64 clk_per_milisec;
	XRCORE_API extern u64 clk_per_microsec;
	XRCORE_API extern u64 clk_overhead;
	XRCORE_API extern float clk_to_seconds;
	XRCORE_API extern float clk_to_milisec;
	XRCORE_API extern float clk_to_microsec;

	XRCORE_API extern u64 qpc_freq;
	XRCORE_API extern u64 qpc_overhead;
	XRCORE_API extern u32 qpc_counter;

	XRCORE_API extern _processor_info ID;
	XRCORE_API extern u64 QPC();

#ifdef M_VISUAL
#if defined(_MSC_VER) && !defined(_M_AMD64)
#pragma warning(push)
#pragma warning(disable:4035)
IC u64 GetCLK(void)
{
    _asm _emit 0x0F;
    _asm _emit 0x31;
}
#pragma warning(pop)
#elif defined(_MSC_VER) || defined(__x86_64__) || defined(_M_X64)
	IC u64 GetCLK(void)
	{
		return __rdtsc();
	}
#elif defined(__aarch64__) || defined(_M_ARM64)
	IC u64 GetCLK(void)
	{
		uint64_t val;
		asm volatile("mrs %0, cntvct_el0" : "=r"(val));
		return val;
	}
#else
	IC u64 GetCLK(void)
	{
		return QPC();
	}
#endif
#endif

#ifdef M_BORLAND
XRCORE_API u64 __fastcall GetCLK (void);
#endif
};

extern XRCORE_API void _initialize_cpu();
extern XRCORE_API void _initialize_cpu_thread();

// threading
typedef void xr_thread_t(void*);
#ifndef __APPLE__
typedef xr_thread_t thread_t;
#endif
extern XRCORE_API void thread_name(const char* name);
extern XRCORE_API void thread_spawn(
	xr_thread_t* entry,
	const char* name,
	unsigned stack,
	void* arglist
);

// device callbacks for math conversions without circular dependencies
struct xr_device_callbacks
{
	void (*hud_to_world)(void* v);
	void (*world_to_hud)(void* v);
	void (*hud_to_world_dir)(void* v);
	void (*world_to_hud_dir)(void* v);
	void (*matrix_hud_to_world)(void* m);
	void (*matrix_world_to_hud)(void* m);
	float (*get_delta_time)();
};
extern XRCORE_API xr_device_callbacks g_device_callbacks;

#endif //__XR_MATH_H__
