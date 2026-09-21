#include "stdafx.h"
#include "profiler.h"

#ifdef PROFILE_CRITICAL_SECTIONS
static add_profile_portion_callback add_profile_portion = 0;
void set_add_profile_portion(add_profile_portion_callback callback)
{
    add_profile_portion = callback;
}

struct profiler
{
    u64 m_time;
    LPCSTR m_timer_id;

    IC profiler::profiler(LPCSTR timer_id)
    {
        if (!add_profile_portion)
            return;

        m_timer_id = timer_id;
        m_time = CPU::QPC();
    }

    IC profiler::~profiler()
    {
        if (!add_profile_portion)
            return;

        u64 time = CPU::QPC();
        (*add_profile_portion)(m_timer_id, time - m_time);
    }
};
#endif // PROFILE_CRITICAL_SECTIONS

#ifdef XR_PLATFORM_POSIX
xrCriticalSection::xrCriticalSection()
{
	pmutex = xr_new<std::recursive_mutex>();
}

xrCriticalSection::~xrCriticalSection()
{
	auto* m = (std::recursive_mutex*)pmutex;
	xr_delete(m);
	pmutex = nullptr;
}

void xrCriticalSection::Enter()
{
	((std::recursive_mutex*)pmutex)->lock();
}

void xrCriticalSection::Leave()
{
	((std::recursive_mutex*)pmutex)->unlock();
}

BOOL xrCriticalSection::TryEnter()
{
	return ((std::recursive_mutex*)pmutex)->try_lock() ? TRUE : FALSE;
}

xrSRWLock::xrSRWLock()
{
	pthread_rwlock_init(&smutex, nullptr);
}

xrSRWLock::~xrSRWLock()
{
	pthread_rwlock_destroy(&smutex);
}

void xrSRWLock::AcquireExclusive()
{
	pthread_rwlock_wrlock(&smutex);
}

void xrSRWLock::ReleaseExclusive()
{
	pthread_rwlock_unlock(&smutex);
}

void xrSRWLock::AcquireShared()
{
	pthread_rwlock_rdlock(&smutex);
}

void xrSRWLock::ReleaseShared()
{
	pthread_rwlock_unlock(&smutex);
}

BOOL xrSRWLock::TryAcquireExclusive()
{
	return pthread_rwlock_trywrlock(&smutex) == 0 ? TRUE : FALSE;
}

BOOL xrSRWLock::TryAcquireShared()
{
	return pthread_rwlock_tryrdlock(&smutex) == 0 ? TRUE : FALSE;
}

xrCriticalSection::raii::raii(xrCriticalSection* critical_section)
	: critical_section(critical_section)
{
	VERIFY(critical_section);
	critical_section->Enter();
}

xrCriticalSection::raii::~raii()
{
	critical_section->Leave();
}
#else
#ifdef PROFILE_CRITICAL_SECTIONS
xrCriticalSection::xrCriticalSection(LPCSTR id) : m_id(id)
#else // PROFILE_CRITICAL_SECTIONS
xrCriticalSection::xrCriticalSection()
#endif // PROFILE_CRITICAL_SECTIONS
{
	pmutex = xr_alloc<CRITICAL_SECTION>(1);
	InitializeCriticalSection((CRITICAL_SECTION*)pmutex);
}

xrCriticalSection::~xrCriticalSection()
{
	DeleteCriticalSection((CRITICAL_SECTION*)pmutex);
	xr_free(pmutex);
}

#ifdef DEBUG
extern void OutputDebugStackTrace(const char* header);
#endif // DEBUG

void xrCriticalSection::Enter()
{
	//PROF_EVENT("xrCriticalSection::Enter");
#ifdef PROFILE_CRITICAL_SECTIONS
# if 0//def DEBUG
    static bool show_call_stack = false;
    if (show_call_stack)
        OutputDebugStackTrace("----------------------------------------------------");
# endif // DEBUG
    profiler temp(m_id);
#endif // PROFILE_CRITICAL_SECTIONS
    EnterCriticalSection((CRITICAL_SECTION*)pmutex);
}

void xrCriticalSection::Leave()
{
	//PROF_EVENT("xrCriticalSection::Leave");
	LeaveCriticalSection((CRITICAL_SECTION*)pmutex);
}

BOOL xrCriticalSection::TryEnter()
{
	//PROF_EVENT("xrCriticalSection::TryEnter");
	return TryEnterCriticalSection((CRITICAL_SECTION*)pmutex);
}

xrCriticalSection::raii::raii(xrCriticalSection* critical_section)
	: critical_section(critical_section)
{
	VERIFY(critical_section);
	critical_section->Enter();
}

xrCriticalSection::raii::~raii()
{
	critical_section->Leave();
}

xrSRWLock::xrSRWLock()
{
    InitializeSRWLock(&smutex);
}

xrSRWLock::~xrSRWLock()
{
}

void xrSRWLock::AcquireExclusive()
{
	PROF_EVENT("xrSRWLock::AcquireExclusive");
    AcquireSRWLockExclusive(&smutex);
}

void xrSRWLock::ReleaseExclusive()
{
	PROF_EVENT("xrSRWLock::ReleaseExclusive");
    ReleaseSRWLockExclusive(&smutex);
}

void xrSRWLock::AcquireShared()
{
	PROF_EVENT("xrSRWLock::AcquireShared");
    AcquireSRWLockShared(&smutex);
}

void xrSRWLock::ReleaseShared()
{
	PROF_EVENT("xrSRWLock::ReleaseShared");
    ReleaseSRWLockShared(&smutex);
}

BOOL xrSRWLock::TryAcquireExclusive()
{
    return TryAcquireSRWLockExclusive(&smutex);
}

BOOL xrSRWLock::TryAcquireShared()
{
    return TryAcquireSRWLockShared(&smutex);
}
#endif


xrSRWLockGuard::xrSRWLockGuard(xrSRWLock* lock, bool shared)
    : lock(lock), shared(shared)
{
    if (shared)
        lock->AcquireShared();
    else
        lock->AcquireExclusive();
}

xrSRWLockGuard::xrSRWLockGuard(xrSRWLock& lock, bool shared)
    : lock(&lock), shared(shared)
{
    if (shared)
        lock.AcquireShared();
    else
        lock.AcquireExclusive();
}

xrSRWLockGuard::~xrSRWLockGuard()
{
    if (shared)
        lock->ReleaseShared();
    else
        lock->ReleaseExclusive();
}
