#include "stdafx.h"
#pragma hdrstop
#include <thread>
#include <vector>
#include <chrono>

typedef struct TTAPI_WORKER_PARAMS
{
	volatile LONG vlFlag;
	LPPTTAPI_WORKER_FUNC lpWorkerFunc;
	LPVOID lpvWorkerFuncParams;
	DWORD dwPadding[13];
}* PTTAPI_WORKER_PARAMS;

typedef PTTAPI_WORKER_PARAMS LPTTAPI_WORKER_PARAMS;

static std::vector<std::thread> ttapi_threads;
static BOOL ttapi_initialized = FALSE;
static DWORD ttapi_workers_count = 0;
static DWORD ttapi_threads_count = 0;
static DWORD ttapi_assigned_workers = 0;
static LPTTAPI_WORKER_PARAMS ttapi_worker_params = NULL;

static DWORD ttapi_dwFastIter = 0;
static DWORD ttapi_dwSlowIter = 0;

struct
{
	volatile LONG size;
	DWORD dwPadding[15];
} ttapi_queue_size;

static void ttapiThreadProc(LPVOID lpParameter)
{
	LPTTAPI_WORKER_PARAMS pParams = (LPTTAPI_WORKER_PARAMS)lpParameter;
	DWORD i, dwFastIter = ttapi_dwFastIter, dwSlowIter = ttapi_dwSlowIter;

	while (TRUE)
	{
		// Fast
		for (i = 0; i < dwFastIter; ++i)
		{
			if (pParams->vlFlag == 0)
			{
				goto process;
			}
#if defined(__aarch64__) || defined(_M_ARM64)
			asm volatile("yield");
#elif defined(_MSC_VER) || defined(__i386__) || defined(__x86_64__)
			_mm_pause();
#endif
		}

		// Moderate
		for (i = 0; i < dwSlowIter; ++i)
		{
			if (pParams->vlFlag == 0)
			{
				goto process;
			}
			std::this_thread::yield();
		}

		// Slow
		while (pParams->vlFlag)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

	process:

		pParams->vlFlag = 1;

		if (pParams->lpWorkerFunc)
			pParams->lpWorkerFunc(pParams->lpvWorkerFuncParams);
		else
			break;

#if defined(_WIN32)
		_InterlockedDecrement(&ttapi_queue_size.size);
#else
		__sync_fetch_and_sub(&ttapi_queue_size.size, 1);
#endif
	}
}

DWORD ttapi_Init(_processor_info* ID)
{
	if (ttapi_initialized)
		return ttapi_workers_count;

	ttapi_workers_count = std::thread::hardware_concurrency();
	if (ttapi_workers_count == 0) ttapi_workers_count = 4;

	char szSearchFor[] = "-max-threads";
	const char* cmdLine = "";
#if defined(_WIN32)
	cmdLine = GetCommandLine();
#endif
	const char* pszTemp = strstr(cmdLine, szSearchFor);
	DWORD dwOverride = 0;
	if (pszTemp)
		if (sscanf_s(pszTemp + strlen(szSearchFor), "%u", &dwOverride))
			if ((dwOverride >= 1) && (dwOverride < ttapi_workers_count))
				ttapi_workers_count = dwOverride;

	ttapi_dwFastIter = 4096;
	ttapi_dwSlowIter = 0;

	ttapi_threads_count = ttapi_workers_count - 1;

	if ((ttapi_worker_params = (PTTAPI_WORKER_PARAMS)malloc(sizeof(TTAPI_WORKER_PARAMS) * ttapi_workers_count)) == NULL)
		return 0;

	memset(ttapi_worker_params, 0, sizeof(TTAPI_WORKER_PARAMS) * ttapi_workers_count);
	ttapi_threads.clear();
	ttapi_threads.reserve(ttapi_threads_count);

	for (DWORD i = 0; i < ttapi_threads_count; i++)
	{
		ttapi_worker_params[i].vlFlag = 1;
		ttapi_threads.emplace_back(ttapiThreadProc, &ttapi_worker_params[i]);
	}

	ttapi_initialized = TRUE;
	return ttapi_workers_count;
}

DWORD ttapi_GetWorkersCount()
{
	return ttapi_workers_count;
}

VOID ttapi_AddWorker(LPPTTAPI_WORKER_FUNC lpWorkerFunc, LPVOID lpvWorkerFuncParams)
{
	ttapi_worker_params[ttapi_assigned_workers].lpWorkerFunc = lpWorkerFunc;
	ttapi_worker_params[ttapi_assigned_workers].lpvWorkerFuncParams = lpvWorkerFuncParams;
	ttapi_assigned_workers++;
}

VOID ttapi_RunAllWorkers()
{
	DWORD ttapi_thread_workers = (ttapi_assigned_workers - 1);

	if (ttapi_thread_workers)
	{
		ttapi_queue_size.size = ttapi_thread_workers;

		for (DWORD i = 0; i < ttapi_thread_workers; ++i)
		{
#if defined(_WIN32)
			_InterlockedExchange(&ttapi_worker_params[i].vlFlag, 0);
#else
			__sync_lock_test_and_set(&ttapi_worker_params[i].vlFlag, 0);
#endif
		}

		ttapi_worker_params[ttapi_thread_workers].lpWorkerFunc(
			ttapi_worker_params[ttapi_thread_workers].lpvWorkerFuncParams);

		while (ttapi_queue_size.size)
		{
#if defined(__aarch64__) || defined(_M_ARM64)
			asm volatile("yield");
#elif defined(_MSC_VER) || defined(__i386__) || defined(__x86_64__)
			_mm_pause();
#endif
		}
	}
	else
	{
		ttapi_worker_params[ttapi_thread_workers].lpWorkerFunc(
			ttapi_worker_params[ttapi_thread_workers].lpvWorkerFuncParams);
	}

	ttapi_assigned_workers = 0;
}

VOID ttapi_Done()
{
	if (!ttapi_initialized)
		return;

	for (DWORD i = 0; i < ttapi_threads_count; i++)
	{
		ttapi_worker_params[i].lpWorkerFunc = NULL;
#if defined(_WIN32)
		_InterlockedExchange(&ttapi_worker_params[i].vlFlag, 0);
#else
		__sync_lock_test_and_set(&ttapi_worker_params[i].vlFlag, 0);
#endif
	}

	for (auto& th : ttapi_threads)
	{
		if (th.joinable())
			th.join();
	}
	ttapi_threads.clear();

	free(ttapi_worker_params);
	ttapi_worker_params = NULL;

	ttapi_workers_count = 0;
	ttapi_threads_count = 0;
	ttapi_assigned_workers = 0;

	ttapi_initialized = FALSE;
}
