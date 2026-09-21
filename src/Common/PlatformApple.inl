#include <stdio.h>
#include <cstring>
#include <stdlib.h> // for malloc
#include <unistd.h> // for rmdir
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/syslimits.h> // for PATH_MAX
#include <errno.h>
#include <stdarg.h>
#include <ctype.h>
#include <limits.h> // for PAGESIZE...
#include <math.h>
#include <sched.h>

#include <algorithm> // for min max
#include <stddef.h>
#include <cstddef>

#include <string>
#include <pthread.h>
#include <fcntl.h>
#include <sys/mman.h> // for mmap / munmap
#include <dirent.h>
#include <utime.h>
#include <ctime>

#include <xlocale.h> // for locale_t

#if defined(__aarch64__) || defined(_M_ARM64)
#ifndef __int64
#define __int64 long long
#endif
#include "sse2neon.h"
#endif

#ifdef ALIGN
#undef ALIGN
#endif
#define ALIGN(a) __attribute__((aligned(a)))
#ifndef _MM_ALIGN16
#define _MM_ALIGN16 __attribute__((aligned(16)))
#endif

#define _LINUX // for GameSpy

#define MAX_PATH PATH_MAX + 1
#define _MAX_DRIVE	3
#define _MAX_DIR	256
#define _MAX_FNAME	256
#define _MAX_EXT	256

#define WINAPI

#ifndef APIENTRY
#define APIENTRY
#endif

#ifndef interface
#define interface struct
#endif

#define _copysign copysign

#define _cdecl //__attribute__((cdecl))

#define __cdecl _cdecl
#define __stdcall
#define __fastcall

#define __pragma(...) _Pragma(#__VA_ARGS__)
#define CALLBACK
#ifndef PASCAL
#define PASCAL
#endif

#define __except(X) catch(X)

#define GetCurrentProcessId getpid

inline unsigned int GetCurrentThreadId()
{
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    return static_cast<unsigned int>(tid);
}

inline void Sleep(int ms)
{
    usleep(ms * 1000);
}

struct xr_beginthread_args {
    void (*func)(void*);
    void* arg;
};
inline void* xr_beginthread_stub(void* p) {
    xr_beginthread_args* a = static_cast<xr_beginthread_args*>(p);
    void (*func)(void*) = a->func;
    void* arg = a->arg;
    delete a;
    func(arg);
    return nullptr;
}
inline uintptr_t _beginthread(void (*func)(void*), unsigned stack_size, void* arg) {
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (stack_size > 0)
        pthread_attr_setstacksize(&attr, stack_size);
    xr_beginthread_args* a = new xr_beginthread_args{func, arg};
    pthread_create(&th, &attr, xr_beginthread_stub, a);
    pthread_attr_destroy(&attr);
    return (uintptr_t)th;
}

inline void _splitpath(const char* path, // Path Input
        char* drive, // Drive     : Output
        char* dir, // Directory : Output
        char* fname, // Filename  : Output
        char* ext // Extension : Output
        )
{
    if(!path)
        return;

    const char *p, *end;

    if(drive)
        strcpy(drive, "");

    end = NULL;
    for(p = path; *p; p++)
        if(*p == '/' || *p == '\\')
            end = p + 1;

    if(end)
    {
        if(dir)
        {
            memcpy(dir, path, end - path);
            dir[end - path] = 0;
        }
        path = end;
    }
    else if(dir)
        dir[0] = 0;

    end = strchr(path, '.');

    if(!end)
        end = p;

    if(fname)
    {
        memcpy(fname, path, end - path);
        fname[end - path] = 0;
    }

    if(ext)
        strcpy(ext, end);
}

inline int _splitpath_s(const char* path,
    char* drive, size_t drive_len,
    char* dir, size_t dir_len,
    char* fname, size_t fname_len,
    char* ext, size_t ext_len)
{
    char d[_MAX_DRIVE] = {0};
    char di[_MAX_DIR] = {0};
    char f[_MAX_FNAME] = {0};
    char e[_MAX_EXT] = {0};
    _splitpath(path, d, di, f, e);
    if (drive && drive_len) {
        size_t copy_len = std::min(strlen(d), drive_len - 1);
        memcpy(drive, d, copy_len);
        drive[copy_len] = '\0';
    }
    if (dir && dir_len) {
        size_t copy_len = std::min(strlen(di), dir_len - 1);
        memcpy(dir, di, copy_len);
        dir[copy_len] = '\0';
    }
    if (fname && fname_len) {
        size_t copy_len = std::min(strlen(f), fname_len - 1);
        memcpy(fname, f, copy_len);
        fname[copy_len] = '\0';
    }
    if (ext && ext_len) {
        size_t copy_len = std::min(strlen(e), ext_len - 1);
        memcpy(ext, e, copy_len);
        ext[copy_len] = '\0';
    }
    return 0;
}

#include <iostream>
inline void OutputDebugString(const char *str) // for linux debugger
{
    std::cerr << str;
}
#define OutputDebugStringA OutputDebugString

inline bool IsDebuggerPresent()
{
    return false;
}

inline unsigned long GetLastError()
{
    return 0;
}

inline int GetExceptionCode()
{
    return 0;
}

#ifndef SEM_FAILCRITICALERRORS
#define SEM_FAILCRITICALERRORS 0x0001
#endif

inline unsigned int SetErrorMode(unsigned int uMode)
{
    return 0;
}

inline void convert_path_separators(char * path);

#include <inttypes.h>
typedef void VOID;
typedef int32_t BOOL;
typedef uint8_t BYTE;
typedef BYTE* LPBYTE;
typedef const BYTE* LPCBYTE;
typedef uint16_t WORD;
typedef WORD* LPWORD;
typedef uint32_t DWORD;
typedef DWORD* LPDWORD;
typedef DWORD* PDWORD;
typedef int32_t LONG;
typedef LONG* LPLONG;
typedef uint32_t ULONG;
typedef ULONG* LPULONG;
typedef int* LPINT;
typedef unsigned int* LPUINT;
typedef BOOL* LPBOOL;
#ifndef _LIBRAW_TYPES_H
typedef int64_t INT64;
typedef uint64_t UINT64;
#endif

typedef char* LPSTR;
typedef char* PSTR;
typedef char* LPTSTR;
typedef const char* LPCSTR;
typedef const char* LPCTSTR;
#ifndef CHAR
typedef char CHAR;
#endif
#ifndef WCHAR
typedef wchar_t WCHAR;
#endif
typedef wchar_t* LPWSTR;
typedef const wchar_t* LPCWSTR;
typedef const void* LPCVOID;
typedef int INT;
typedef unsigned int UINT;
#ifndef _LARGE_INTEGER_DEFINED
#define _LARGE_INTEGER_DEFINED
typedef union _LARGE_INTEGER {
    struct {
        DWORD LowPart;
        LONG  HighPart;
    };
    struct {
        DWORD LowPart;
        LONG  HighPart;
    } u;
    int64_t QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;
#endif

typedef union _ULARGE_INTEGER {
    struct {
        DWORD LowPart;
        DWORD HighPart;
    };
    struct {
        DWORD LowPart;
        DWORD HighPart;
    } u;
    uint64_t QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

#include <mach/mach_time.h>

inline BOOL QueryPerformanceCounter(LARGE_INTEGER* lpPerformanceCount)
{
    if (!lpPerformanceCount) return FALSE;
    static mach_timebase_info_data_t tb = {0, 0};
    if (tb.denom == 0) mach_timebase_info(&tb);
    lpPerformanceCount->QuadPart = static_cast<int64_t>(mach_absolute_time() * tb.numer / tb.denom);
    return TRUE;
}

inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* lpFrequency)
{
    if (!lpFrequency) return FALSE;
    lpFrequency->QuadPart = 1000000000LL;
    return TRUE;
}

inline void timeBeginPeriod(UINT) {}
inline void timeEndPeriod(UINT) {}

#include <time.h>

inline void _tzset()
{
    tzset();
}

typedef long __time32_t;
inline __time32_t _time32(__time32_t* timer)
{
    time_t t;
    time(&t);
    if (timer) *timer = (__time32_t)t;
    return (__time32_t)t;
}

inline char* _strdate(char* buffer)
{
    time_t rawtime;
    time(&rawtime);
    struct tm timeinfo;
    localtime_r(&rawtime, &timeinfo);
    strftime(buffer, 9, "%m/%d/%y", &timeinfo);
    return buffer;
}

inline char* _strtime(char* buffer)
{
    time_t rawtime;
    time(&rawtime);
    struct tm timeinfo;
    localtime_r(&rawtime, &timeinfo);
    strftime(buffer, 9, "%H:%M:%S", &timeinfo);
    return buffer;
}

#include <malloc/malloc.h>

inline void* _aligned_malloc(size_t size, size_t alignment) {
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment < sizeof(void*) ? sizeof(void*) : alignment, size) != 0)
        return nullptr;
    return ptr;
}
inline void _aligned_free(void* ptr) {
    free(ptr);
}
inline void* _aligned_realloc(void* ptr, size_t size, size_t alignment) {
    if (!ptr) return _aligned_malloc(size, alignment);
    if (size == 0) { _aligned_free(ptr); return nullptr; }
    void* new_ptr = _aligned_malloc(size, alignment);
    if (!new_ptr) return nullptr;
    size_t old_size = malloc_size(ptr);
    memcpy(new_ptr, ptr, std::min(size, old_size));
    _aligned_free(ptr);
    return new_ptr;
}
inline size_t _aligned_msize(void* ptr, size_t, size_t) {
    return malloc_size(ptr);
}

typedef struct _EXCEPTION_POINTERS {
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS;

#if defined(XR_ARCHITECTURE_X64) || defined(XR_ARCHITECTURE_ARM64) || defined(XR_ARCHITECTURE_E2K)
typedef intptr_t INT_PTR;
typedef uintptr_t UINT_PTR;
typedef intptr_t LONG_PTR;
typedef uintptr_t ULONG_PTR;
#else
typedef int INT_PTR;
typedef unsigned int UINT_PTR;
typedef long LONG_PTR;
typedef unsigned long ULONG_PTR;
#endif

#ifndef _HANDLE_DEFINED
#define _HANDLE_DEFINED
typedef void* HANDLE;
typedef HANDLE* LPHANDLE;
#ifndef INVALID_HANDLE_VALUE
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#endif
#endif
typedef void* HMODULE;
typedef void* HINSTANCE;
#ifndef GetModuleHandle
#define GetModuleHandle(x) ((HMODULE)0)
#endif
typedef void* PVOID;
typedef void* LPVOID;
#if defined(XR_ARCHITECTURE_PPC64)
typedef LONG_PTR WPARAM;
#else
typedef UINT_PTR WPARAM;
#endif
typedef LONG_PTR LPARAM;
typedef int32_t HRESULT;
typedef long LRESULT;
typedef void* HWND;
typedef void* HDC;
typedef void* HMONITOR;
typedef uint32_t UINT32;
typedef uint32_t D3DCOLOR;

#ifndef D3DCOLOR_ARGB
#define D3DCOLOR_ARGB(a,r,g,b) ((D3DCOLOR)((((a)&0xffu)<<24)|(((r)&0xffu)<<16)|(((g)&0xffu)<<8)|((b)&0xffu)))
#endif
#ifndef D3DCOLOR_RGBA
#define D3DCOLOR_RGBA(r,g,b,a) D3DCOLOR_ARGB(a,r,g,b)
#endif
#ifndef D3DCOLOR_XRGB
#define D3DCOLOR_XRGB(r,g,b)   D3DCOLOR_ARGB(0xff,r,g,b)
#endif
#ifndef D3DCOLOR_COLORVALUE
#define D3DCOLOR_COLORVALUE(r,g,b,a) D3DCOLOR_RGBA((uint32_t)((r)*255.f),(uint32_t)((g)*255.f),(uint32_t)((b)*255.f),(uint32_t)((a)*255.f))
#endif

#define DLL_PROCESS_DETACH 0
#define DLL_PROCESS_ATTACH 1
#define DLL_THREAD_ATTACH 2
#define DLL_THREAD_DETACH 3

#ifndef _RECT_DEFINED
#define _RECT_DEFINED
typedef struct RECT {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
} RECT, *PRECT, *LPRECT;
typedef const RECT* LPCRECT;
#endif

#ifndef _POINT_DEFINED
#define _POINT_DEFINED
typedef struct POINT {
    LONG x;
    LONG y;
} POINT, *PPOINT, *LPPOINT;
#endif

#include <SDL.h>

inline BOOL GetCursorPos(LPPOINT lpPoint) {
    if (!lpPoint) return FALSE;
    int x = 0, y = 0;
    SDL_GetGlobalMouseState(&x, &y);
    lpPoint->x = x;
    lpPoint->y = y;
    return TRUE;
}

inline BOOL SetCursorPos(int X, int Y) {
    SDL_WarpMouseGlobal(X, Y);
    return TRUE;
}

#define FILE_ATTRIBUTE_READONLY  0x00000001
#define FILE_ATTRIBUTE_HIDDEN    0x00000002
#define FILE_ATTRIBUTE_SYSTEM    0x00000004
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_ARCHIVE   0x00000020
#define FILE_ATTRIBUTE_NORMAL    0x00000080

inline BOOL SetFileAttributes(const char* lpFileName, DWORD dwFileAttributes) {
    if (!lpFileName) return FALSE;
    chmod(lpFileName, 0666);
    return TRUE;
}

inline BOOL GetClientRect(HWND hWnd, LPRECT lpRect) {
    if (!lpRect) return FALSE;
    if (hWnd) {
        int w = 0, h = 0;
        SDL_GetWindowSize((SDL_Window*)hWnd, &w, &h);
        lpRect->left = 0;
        lpRect->top = 0;
        lpRect->right = w;
        lpRect->bottom = h;
    } else {
        lpRect->left = 0;
        lpRect->top = 0;
        lpRect->right = 1024;
        lpRect->bottom = 768;
    }
    return TRUE;
}

inline int MapWindowPoints(HWND hWndFrom, HWND hWndTo, LPPOINT lpPoints, UINT cPoints) {
    return 0;
}

#define CP_ACP   0
#define CP_OEMCP 1
#define CP_UTF8  65001

inline BOOL ClientToScreen(HWND hWnd, LPPOINT lpPoint) {
    if (!lpPoint) return FALSE;
    if (hWnd) {
        int wx = 0, wy = 0;
        SDL_GetWindowPosition((SDL_Window*)hWnd, &wx, &wy);
        lpPoint->x += wx;
        lpPoint->y += wy;
    }
    return TRUE;
}

inline BOOL ScreenToClient(HWND hWnd, LPPOINT lpPoint) {
    if (!lpPoint) return FALSE;
    if (hWnd) {
        int wx = 0, wy = 0;
        SDL_GetWindowPosition((SDL_Window*)hWnd, &wx, &wy);
        lpPoint->x -= wx;
        lpPoint->y -= wy;
    }
    return TRUE;
}

inline BOOL ClipCursor(const RECT* lpRect) {
    return TRUE;
}

inline int ShowCursor(BOOL bShow) {
    return SDL_ShowCursor(bShow ? SDL_ENABLE : SDL_DISABLE);
}

#ifndef GWL_STYLE
#define GWL_STYLE (-16)
#endif
#ifndef GWL_EXSTYLE
#define GWL_EXSTYLE (-20)
#endif
#ifndef WS_POPUP
#define WS_POPUP 0x80000000L
#endif
#ifndef WS_VISIBLE
#define WS_VISIBLE 0x10000000L
#endif
#ifndef WS_BORDER
#define WS_BORDER 0x00800000L
#endif
#ifndef WS_DLGFRAME
#define WS_DLGFRAME 0x00400000L
#endif
#ifndef WS_CAPTION
#define WS_CAPTION 0x00C00000L
#endif
#ifndef WS_SYSMENU
#define WS_SYSMENU 0x00080000L
#endif
#ifndef WS_THICKFRAME
#define WS_THICKFRAME 0x00040000L
#endif
#ifndef WS_MINIMIZEBOX
#define WS_MINIMIZEBOX 0x00020000L
#endif
#ifndef WS_MAXIMIZEBOX
#define WS_MAXIMIZEBOX 0x00010000L
#endif
#ifndef WS_OVERLAPPED
#define WS_OVERLAPPED 0x00000000L
#endif
#ifndef WS_OVERLAPPEDWINDOW
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
#endif
#ifndef WS_EX_TOPMOST
#define WS_EX_TOPMOST 0x00000008L
#endif
#ifndef WS_EX_APPWINDOW
#define WS_EX_APPWINDOW 0x00040000L
#endif
#ifndef CW_USEDEFAULT
#define CW_USEDEFAULT ((int)0x80000000)
#endif

#ifndef HWND_TOP
#define HWND_TOP ((HWND)(uintptr_t)0)
#define HWND_BOTTOM ((HWND)(uintptr_t)1)
#define HWND_TOPMOST ((HWND)(intptr_t)-1)
#define HWND_NOTOPMOST ((HWND)(intptr_t)-2)
#endif

#ifndef SWP_NOSIZE
#define SWP_NOSIZE 0x0001
#define SWP_NOMOVE 0x0002
#define SWP_NOZORDER 0x0004
#define SWP_NOREDRAW 0x0008
#define SWP_NOACTIVATE 0x0010
#define SWP_FRAMECHANGED 0x0020
#define SWP_DRAWFRAME SWP_FRAMECHANGED
#define SWP_SHOWWINDOW 0x0040
#define SWP_HIDEWINDOW 0x0080
#define SWP_NOCOPYBITS 0x0100
#define SWP_NOOWNERZORDER 0x0200
#define SWP_NOREPOSITION SWP_NOOWNERZORDER
#define SWP_NOSENDCHANGING 0x0400
#endif

inline LONG_PTR SetWindowLongPtr(HWND hWnd, int nIndex, LONG_PTR dwNewLong) { return 0; }
inline LONG SetWindowLong(HWND hWnd, int nIndex, LONG dwNewLong) { return 0; }
inline LONG_PTR GetWindowLongPtr(HWND hWnd, int nIndex) { return 0; }
inline LONG GetWindowLong(HWND hWnd, int nIndex) { return 0; }

inline BOOL SetWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags) {
    if (hWnd) {
        SDL_Window* window = (SDL_Window*)hWnd;
        if (!(uFlags & SWP_NOMOVE)) {
            SDL_SetWindowPosition(window, X, Y);
        }
        if (!(uFlags & SWP_NOSIZE)) {
            SDL_SetWindowSize(window, cx, cy);
        }

        // R4 creates the SDL window hidden, then exposes it through the
        // canonical Win32 SetWindowPos(..., SWP_SHOWWINDOW) path once the
        // swap chain is ready.  Preserve that visibility contract here.
        if (uFlags & SWP_HIDEWINDOW) {
            SDL_HideWindow(window);
        } else if (uFlags & SWP_SHOWWINDOW) {
            SDL_ShowWindow(window);
        }
    }
    return TRUE;
}

inline BOOL IsIconic(HWND hWnd) {
    if (hWnd) {
        Uint32 flags = SDL_GetWindowFlags((SDL_Window*)hWnd);
        return (flags & SDL_WINDOW_MINIMIZED) != 0;
    }
    return FALSE;
}

extern "C" void MacOS_ActivateAppWindow(void* sdlWindow);

inline HWND SetActiveWindow(HWND hWnd) {
    if (hWnd) MacOS_ActivateAppWindow(hWnd);
    return hWnd;
}
inline BOOL SetForegroundWindow(HWND hWnd) {
    if (hWnd) {
        SDL_RaiseWindow((SDL_Window*)hWnd);
        MacOS_ActivateAppWindow(hWnd);
    }
    return TRUE;
}

#ifndef SW_HIDE
#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_NORMAL 1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWMAXIMIZED 3
#define SW_MAXIMIZE 3
#define SW_SHOWNOACTIVATE 4
#define SW_SHOW 5
#define SW_MINIMIZE 6
#define SW_SHOWMINNOACTIVE 7
#define SW_SHOWNA 8
#define SW_RESTORE 9
#define SW_SHOWDEFAULT 10
#endif

inline BOOL ShowWindow(HWND hWnd, int nCmdShow) {
    if (hWnd) {
        SDL_Window* win = (SDL_Window*)hWnd;
        if (nCmdShow == SW_HIDE)
            SDL_HideWindow(win);
        else if (nCmdShow == SW_MINIMIZE || nCmdShow == SW_SHOWMINIMIZED)
            SDL_MinimizeWindow(win);
        else if (nCmdShow == SW_SHOWMAXIMIZED)
            SDL_MaximizeWindow(win);
        else if (nCmdShow == SW_RESTORE)
            SDL_RestoreWindow(win);
        else
        {
            SDL_ShowWindow(win);
            MacOS_ActivateAppWindow(win);
        }
    }
    return TRUE;
}

inline void PostQuitMessage(int exitCode) {
    SDL_Event event;
    event.type = SDL_QUIT;
    SDL_PushEvent(&event);
}

#ifndef MB_OK
#define MB_OK 0x00000000L
#define MB_OKCANCEL 0x00000001L
#define MB_ABORTRETRYIGNORE 0x00000002L
#define MB_YESNOCANCEL 0x00000003L
#define MB_YESNO 0x00000004L
#define MB_RETRYCANCEL 0x00000005L
#define MB_ICONHAND 0x00000010L
#define MB_ICONQUESTION 0x00000020L
#define MB_ICONEXCLAMATION 0x00000030L
#define MB_ICONASTERISK 0x00000040L
#define MB_ICONWARNING MB_ICONEXCLAMATION
#define MB_ICONERROR MB_ICONHAND
#define MB_ICONINFORMATION MB_ICONASTERISK
#define MB_ICONSTOP MB_ICONHAND
#endif

#ifndef IDOK
#define IDOK 1
#define IDCANCEL 2
#define IDABORT 3
#define IDRETRY 4
#define IDIGNORE 5
#define IDYES 6
#define IDNO 7
#endif

inline int MessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType) {
    Uint32 flags = 0;
    if (uType & MB_ICONERROR)
        flags |= SDL_MESSAGEBOX_ERROR;
    else if (uType & MB_ICONWARNING)
        flags |= SDL_MESSAGEBOX_WARNING;
    else
        flags |= SDL_MESSAGEBOX_INFORMATION;
    SDL_ShowSimpleMessageBox(flags, lpCaption ? lpCaption : "Error", lpText ? lpText : "", (SDL_Window*)hWnd);
    return IDOK;
}
inline int MessageBoxW(HWND hWnd, LPCWSTR lpText, LPCWSTR lpCaption, UINT uType) {
    return IDOK;
}
#ifndef MessageBox
#define MessageBox MessageBoxA
#endif

inline HANDLE GetCurrentProcess() {
    return (HANDLE)(intptr_t)-1;
}

inline BOOL TerminateProcess(HANDLE hProcess, UINT uExitCode) {
    exit(uExitCode);
    return TRUE;
}

inline BOOL GetWindowRect(HWND hWnd, LPRECT lpRect) {
    if (!lpRect) return FALSE;
    if (hWnd) {
        int x = 0, y = 0, w = 0, h = 0;
        SDL_GetWindowPosition((SDL_Window*)hWnd, &x, &y);
        SDL_GetWindowSize((SDL_Window*)hWnd, &w, &h);
        lpRect->left = x;
        lpRect->top = y;
        lpRect->right = x + w;
        lpRect->bottom = y + h;
    } else {
        lpRect->left = 0;
        lpRect->top = 0;
        lpRect->right = 1024;
        lpRect->bottom = 768;
    }
    return TRUE;
}

inline BOOL AdjustWindowRect(LPRECT lpRect, DWORD dwStyle, BOOL bMenu) { return TRUE; }

inline BOOL SetRect(LPRECT lprc, int xLeft, int yTop, int xRight, int yBottom) {
    if (!lprc) return FALSE;
    lprc->left = xLeft;
    lprc->top = yTop;
    lprc->right = xRight;
    lprc->bottom = yBottom;
    return TRUE;
}

inline void DisableProcessWindowsGhosting() {}

typedef struct tagWINDOWINFO {
    DWORD cbSize;
    RECT  rcWindow;
    RECT  rcClient;
    DWORD dwStyle;
    DWORD dwExStyle;
    DWORD dwWindowStatus;
    UINT  cxWindowBorders;
    UINT  cyWindowBorders;
    WORD  atomWindowType;
    WORD  wCreatorVersion;
} WINDOWINFO, *PWINDOWINFO, *LPWINDOWINFO;

#ifndef WS_ACTIVECAPTION
#define WS_ACTIVECAPTION 0x0001
#endif

inline BOOL GetWindowInfo(HWND hwnd, PWINDOWINFO pwi) {
    if (pwi) {
        memset(pwi, 0, sizeof(WINDOWINFO));
        pwi->dwWindowStatus = WS_ACTIVECAPTION;
    }
    return TRUE;
}

#define MONITOR_DEFAULTTONULL    0x00000000
#define MONITOR_DEFAULTTOPRIMARY 0x00000001
#define MONITOR_DEFAULTTONEAREST 0x00000002

inline HMONITOR MonitorFromPoint(POINT pt, DWORD dwFlags) {
    return (HMONITOR)(uintptr_t)1;
}

inline HMONITOR MonitorFromRect(LPCRECT lprc, DWORD dwFlags) {
    return (HMONITOR)(uintptr_t)1;
}

inline HMONITOR MonitorFromWindow(HWND hwnd, DWORD dwFlags) {
    return (HMONITOR)(uintptr_t)1;
}

typedef struct tagMONITORINFO {
    DWORD cbSize;
    RECT  rcMonitor;
    RECT  rcWork;
    DWORD dwFlags;
} MONITORINFO, *LPMONITORINFO;

inline BOOL GetMonitorInfoA(HMONITOR hMonitor, LPMONITORINFO lpmi) {
    if (!lpmi) return FALSE;
    SDL_Rect rect;
    if (SDL_GetDisplayBounds(0, &rect) == 0) {
        lpmi->rcMonitor.left = rect.x;
        lpmi->rcMonitor.top = rect.y;
        lpmi->rcMonitor.right = rect.x + rect.w;
        lpmi->rcMonitor.bottom = rect.y + rect.h;
        lpmi->rcWork = lpmi->rcMonitor;
    } else {
        lpmi->rcMonitor.left = 0;
        lpmi->rcMonitor.top = 0;
        lpmi->rcMonitor.right = 1920;
        lpmi->rcMonitor.bottom = 1080;
        lpmi->rcWork = lpmi->rcMonitor;
    }
    return TRUE;
}

inline HWND GetDesktopWindow() {
    return nullptr;
}

typedef struct _devicemodeA {
    BYTE  dmDeviceName[32];
    WORD  dmSpecVersion;
    WORD  dmDriverVersion;
    WORD  dmSize;
    WORD  dmDriverExtra;
    DWORD dmFields;
    short dmOrientation;
    short dmPaperSize;
    short dmPaperLength;
    short dmPaperWidth;
    short dmScale;
    short dmCopies;
    short dmDefaultSource;
    short dmPrintQuality;
    short dmColor;
    short dmDuplex;
    short dmYResolution;
    short dmTTOption;
    short dmCollate;
    BYTE  dmFormName[32];
    WORD  dmLogPixels;
    DWORD dmBitsPerPel;
    DWORD dmPelsWidth;
    DWORD dmPelsHeight;
    DWORD dmDisplayFlags;
    DWORD dmDisplayFrequency;
} DEVMODE, *PDEVMODE, *LPDEVMODE;

#ifndef ENUM_CURRENT_SETTINGS
#define ENUM_CURRENT_SETTINGS ((DWORD)-1)
#endif

inline BOOL EnumDisplaySettings(const char* lpszDeviceName, DWORD iModeNum, DEVMODE* lpDevMode) {
    if (lpDevMode) {
        lpDevMode->dmDisplayFrequency = 60;
        SDL_DisplayMode mode;
        if (SDL_GetCurrentDisplayMode(0, &mode) == 0 && mode.refresh_rate > 0) {
            lpDevMode->dmDisplayFrequency = mode.refresh_rate;
        }
        return TRUE;
    }
    return FALSE;
}

inline DWORD timeGetTime() {
    return SDL_GetTicks();
}

template <typename T, typename V>
inline T InterlockedExchange(volatile T* target, V value) {
    return __atomic_exchange_n(target, (T)value, __ATOMIC_SEQ_CST);
}

template <typename T>
inline T InterlockedIncrement(volatile T* target) {
    return __atomic_add_fetch(target, 1, __ATOMIC_SEQ_CST);
}

template <typename T>
inline T InterlockedDecrement(volatile T* target) {
    return __atomic_sub_fetch(target, 1, __ATOMIC_SEQ_CST);
}

template <typename T, typename V>
inline T InterlockedExchangeAdd(volatile T* target, V value) {
    return __atomic_fetch_add(target, (T)value, __ATOMIC_SEQ_CST);
}

template <typename T, typename V, typename C>
inline T InterlockedCompareExchange(volatile T* destination, V exchange, C comparand) {
    T comp = (T)comparand;
    __atomic_compare_exchange_n(destination, &comp, (T)exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comp;
}

#ifndef _InterlockedCompareExchange
#define _InterlockedCompareExchange InterlockedCompareExchange
#endif
#ifndef _InterlockedExchange
#define _InterlockedExchange InterlockedExchange
#endif
#ifndef _InterlockedIncrement
#define _InterlockedIncrement InterlockedIncrement
#endif
#ifndef _InterlockedDecrement
#define _InterlockedDecrement InterlockedDecrement
#endif

typedef int16_t SHORT;
typedef uint16_t USHORT;

#define VK_CAPITAL 0x14
#define VK_NUMLOCK 0x90
#define HKL_NEXT 1
#define HKL_PREV 0
typedef void* HKL;

#ifndef LOBYTE
#define LOBYTE(w) ((BYTE)(((uintptr_t)(w)) & 0xff))
#endif
#ifndef HIBYTE
#define HIBYTE(w) ((BYTE)((((uintptr_t)(w)) >> 8) & 0xff))
#endif
#ifndef LOWORD
#define LOWORD(l) ((WORD)(((uintptr_t)(l)) & 0xffff))
#endif
#ifndef HIWORD
#define HIWORD(l) ((WORD)((((uintptr_t)(l)) >> 16) & 0xffff))
#endif
#ifndef FIELD_OFFSET
#define FIELD_OFFSET(type, field) ((LONG)offsetof(type, field))
#endif

inline SHORT GetKeyState(int vKey) {
    SDL_Keymod mod = SDL_GetModState();
    if (vKey == VK_CAPITAL) {
        return (mod & KMOD_CAPS) ? 1 : 0;
    }
    if (vKey == VK_NUMLOCK) {
        return (mod & KMOD_NUM) ? 1 : 0;
    }
    return 0;
}

inline HKL ActivateKeyboardLayout(HKL hkl, UINT Flags) {
    return nullptr;
}

#ifndef VK_SHIFT
#define VK_SHIFT 0x10
#endif
#ifndef VK_CONTROL
#define VK_CONTROL 0x11
#endif
#ifndef VK_MENU
#define VK_MENU 0x12
#endif

#ifndef MAPVK_VK_TO_VSC
#define MAPVK_VK_TO_VSC 0
#endif
#ifndef MAPVK_VSC_TO_VK
#define MAPVK_VSC_TO_VK 1
#endif
#ifndef MAPVK_VK_TO_CHAR
#define MAPVK_VK_TO_CHAR 2
#endif
#ifndef MAPVK_VSC_TO_VK_EX
#define MAPVK_VSC_TO_VK_EX 4
#endif

#ifndef WC_NO_BEST_FIT_CHARS
#define WC_NO_BEST_FIT_CHARS 0x00000400
#endif

inline HKL GetKeyboardLayout(DWORD idThread) {
    return nullptr;
}

inline UINT MapVirtualKeyEx(UINT uCode, UINT uMapType, HKL dwhkl) {
    return 0;
}

inline UINT MapVirtualKey(UINT uCode, UINT uMapType) {
    return 0;
}

inline int ToUnicodeEx(UINT wVirtKey, UINT wScanCode, const BYTE *lpKeyState, LPWSTR pwszBuff, int cchBuff, UINT wFlags, HKL dwhkl) {
    return 0;
}

#ifndef WM_SYSCOMMAND
#define WM_SYSCOMMAND 0x0112
#endif
#ifndef SC_MINIMIZE
#define SC_MINIMIZE 0xF020
#endif

inline LRESULT SendMessage(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    return 0;
}

#ifndef WM_SYSKEYDOWN
#define WM_SYSKEYDOWN 0x0104
#endif
#ifndef WM_SYSKEYUP
#define WM_SYSKEYUP 0x0105
#endif
#ifndef WM_SETCURSOR
#define WM_SETCURSOR 0x0020
#endif
#ifndef SC_MOVE
#define SC_MOVE 0xF010
#endif
#ifndef SC_SIZE
#define SC_SIZE 0xF000
#endif
#ifndef SC_MAXIMIZE
#define SC_MAXIMIZE 0xF030
#endif
#ifndef SC_MONITORPOWER
#define SC_MONITORPOWER 0xF170
#endif
#ifndef WM_CLOSE
#define WM_CLOSE 0x0010
#endif
#ifndef WM_HOTKEY
#define WM_HOTKEY 0x0312
#endif
#ifndef WM_SYSCHAR
#define WM_SYSCHAR 0x0106
#endif
#ifndef WM_CHAR
#define WM_CHAR 0x0102
#endif
#ifndef WM_INPUTLANGCHANGE
#define WM_INPUTLANGCHANGE 0x0051
#endif
#ifndef WM_DISPLAYCHANGE
#define WM_DISPLAYCHANGE 0x007E
#endif
#ifndef WM_ACTIVATE
#define WM_ACTIVATE 0x0006
#endif
#ifndef WA_INACTIVE
#define WA_INACTIVE 0
#endif
#ifndef WA_ACTIVE
#define WA_ACTIVE 1
#endif
#ifndef WA_CLICKACTIVE
#define WA_CLICKACTIVE 2
#endif

inline LRESULT DefWindowProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    return 0;
}

#ifndef SM_SWAPBUTTON
#define SM_SWAPBUTTON 23
#endif
#ifndef SM_CXSCREEN
#define SM_CXSCREEN 0
#endif
#ifndef SM_CYSCREEN
#define SM_CYSCREEN 1
#endif

inline int GetSystemMetrics(int nIndex) {
    return 0;
}

#ifndef GUID_DEFINED
#define GUID_DEFINED
#ifndef _GUID_DEFINED
#define _GUID_DEFINED
#endif
typedef struct _GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} GUID;
typedef struct _GUID _GUID;
#define LPGUID LPGUID
typedef GUID* LPGUID;
typedef const GUID* LPCGUID;
#ifdef __cplusplus
typedef const GUID& REFGUID;
typedef const GUID& REFIID;
typedef const GUID& REFCLSID;
#ifndef GUID_OPERATORS_DEFINED
#define GUID_OPERATORS_DEFINED
inline bool operator==(const GUID& a, const GUID& b) { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }
inline bool operator!=(const GUID& a, const GUID& b) { return std::memcmp(&a, &b, sizeof(GUID)) != 0; }
#endif
#else
typedef const GUID* REFGUID;
typedef const GUID* REFIID;
typedef const GUID* REFCLSID;
#endif
typedef GUID IID;
typedef GUID CLSID;
#endif

#ifndef DEFINE_GUID
#ifdef INITGUID
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
#else
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    static const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
#endif
#endif

#ifndef DECLARE_INTERFACE
#define DECLARE_INTERFACE(iface) struct iface
#define DECLARE_INTERFACE_(iface, baseiface) struct iface : public baseiface
#define STDMETHOD(method) virtual HRESULT method
#define STDMETHOD_(type, method) virtual type method
#define PURE = 0
#define THIS_
#define THIS
#endif

#ifndef __uuidof
#ifdef __cplusplus
template <typename T>
constexpr GUID __uuidof_helper();

#define __uuidof(T) __uuidof_helper<T>()
#define __uuidof_var(T) __uuidof_helper<decltype(T)>()

#ifndef DECLARE_UUIDOF_HELPER
#define DECLARE_UUIDOF_HELPER(type, a, b, c, d, e, f, g, h, i, j, k) \
  extern "C++" { template <> constexpr GUID __uuidof_helper<type>() { return GUID{a,b,c,{d,e,f,g,h,i,j,k}}; } } \
  extern "C++" { template <> constexpr GUID __uuidof_helper<type*>() { return __uuidof_helper<type>(); } } \
  extern "C++" { template <> constexpr GUID __uuidof_helper<const type*>() { return __uuidof_helper<type>(); } } \
  extern "C++" { template <> constexpr GUID __uuidof_helper<type&>() { return __uuidof_helper<type>(); } } \
  extern "C++" { template <> constexpr GUID __uuidof_helper<const type&>() { return __uuidof_helper<type>(); } }
#endif

#ifndef __CRT_UUID_DECL
#define __CRT_UUID_DECL(type, a, b, c, d, e, f, g, h, i, j, k) DECLARE_UUIDOF_HELPER(type, a, b, c, d, e, f, g, h, i, j, k)
#endif

#endif
#endif

#ifndef IUnknown_DEFINED
#define IUnknown_DEFINED
#ifndef __IUnknown_INTERFACE_DEFINED__
#define __IUnknown_INTERFACE_DEFINED__
struct IUnknown {
    virtual HRESULT QueryInterface(REFIID riid, void** ppvObject) = 0;
    virtual ULONG AddRef() = 0;
    virtual ULONG Release() = 0;

    template<class Q>
    HRESULT QueryInterface(Q** pp)
    {
        return QueryInterface(__uuidof(Q), (void**)pp);
    }
};
typedef struct IUnknown* LPUNKNOWN;
#endif
#endif

#ifndef _FILETIME_
#define _FILETIME_
typedef struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME, *PFILETIME, *LPFILETIME;
#endif

typedef struct tWAVEFORMATEX {
    uint16_t    wFormatTag;
    uint16_t    nChannels;
    uint32_t    nSamplesPerSec;
    uint32_t    nAvgBytesPerSec;
    uint16_t    nBlockAlign;
    uint16_t    wBitsPerSample;
    uint16_t    cbSize;
} WAVEFORMATEX, *PWAVEFORMATEX, *NPWAVEFORMATEX, *LPWAVEFORMATEX;
typedef const WAVEFORMATEX *LPCWAVEFORMATEX;

#ifndef WAVE_FORMAT_PCM
#define WAVE_FORMAT_PCM 1
#endif

#define WM_USER 0x0400

#define TRUE true
#define FALSE false
#define CONST const

#include <mutex>
#include <condition_variable>
#include <chrono>

#define INFINITE 0xFFFFFFFF
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define WAIT_FAILED 0xFFFFFFFF
#define WAIT_ABANDONED 0x00000080
typedef uintptr_t DWORD_PTR;
typedef uintptr_t ULONG_PTR;

struct XREventShim {
    std::mutex mtx;
    std::condition_variable cv;
    bool manual_reset;
    bool signaled;
};

inline HANDLE CreateEvent(void* /*lpEventAttributes*/, BOOL bManualReset, BOOL bInitialState, const char* /*lpName*/)
{
    XREventShim* ev = new XREventShim();
    ev->manual_reset = (bManualReset != 0);
    ev->signaled = (bInitialState != 0);
    return static_cast<HANDLE>(ev);
}

inline BOOL SetEvent(HANDLE hEvent)
{
    if (!hEvent) return FALSE;
    XREventShim* ev = static_cast<XREventShim*>(hEvent);
    {
        std::lock_guard<std::mutex> lock(ev->mtx);
        ev->signaled = true;
    }
    if (ev->manual_reset)
        ev->cv.notify_all();
    else
        ev->cv.notify_one();
    return TRUE;
}

inline BOOL ResetEvent(HANDLE hEvent)
{
    if (!hEvent) return FALSE;
    XREventShim* ev = static_cast<XREventShim*>(hEvent);
    std::lock_guard<std::mutex> lock(ev->mtx);
    ev->signaled = false;
    return TRUE;
}

inline DWORD WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds)
{
    if (!hHandle) return WAIT_FAILED;
    XREventShim* ev = static_cast<XREventShim*>(hHandle);
    std::unique_lock<std::mutex> lock(ev->mtx);
    if (dwMilliseconds == INFINITE)
    {
        ev->cv.wait(lock, [ev] { return ev->signaled; });
        if (!ev->manual_reset)
            ev->signaled = false;
        return WAIT_OBJECT_0;
    }
    else
    {
        bool success = ev->cv.wait_for(lock, std::chrono::milliseconds(dwMilliseconds), [ev] { return ev->signaled; });
        if (success)
        {
            if (!ev->manual_reset)
                ev->signaled = false;
            return WAIT_OBJECT_0;
        }
        return WAIT_TIMEOUT;
    }
}

inline BOOL CloseHandle(HANDLE hObject)
{
    if (!hObject) return FALSE;
    XREventShim* ev = static_cast<XREventShim*>(hObject);
    delete ev;
    return TRUE;
}

typedef dirent DirEntryType;

#define _O_WRONLY O_WRONLY
#define _O_RDONLY O_RDONLY
#define _O_TRUNC O_TRUNC
#define _O_CREAT O_CREAT
#define _SH_DENYNO 0
#define _S_IWRITE S_IWRITE
#define _S_IREAD S_IREAD
#define _O_BINARY 0
#define O_BINARY 0
#define O_SEQUENTIAL 0
#define SH_DENYWR 0

#if __has_include(<SDL_stdinc.h>)
#include <SDL_stdinc.h>
#define itoa SDL_itoa
#define _itoa SDL_itoa
#define _itoa_s SDL_itoa
#define ultoa(val, buf, rad) SDL_uitoa((unsigned int)(val), (buf), (rad))
#define _ultoa(val, buf, rad) SDL_uitoa((unsigned int)(val), (buf), (rad))
#else
#define itoa(...) do { static_assert(false, "SDL_stdinc.h is missing"); } while (false)
#define _itoa(...) do { static_assert(false, "SDL_stdinc.h is missing"); } while (false)
#define _itoa_s(...) do { static_assert(false, "SDL_stdinc.h is missing"); } while (false)
#define ultoa(...) do { static_assert(false, "SDL_stdinc.h is missing"); } while (false)
#define _ultoa(...) do { static_assert(false, "SDL_stdinc.h is missing"); } while (false)
#endif

#define _stricmp stricmp
#define strcmpi stricmp
#define lstrcpy strcpy
#define stricmp strcasecmp
inline char* strupr(char* str)
{
    if (!str) return nullptr;
    for (char* p = str; *p; ++p)
        *p = (char)toupper((unsigned char)*p);
    return str;
}
inline char* strlwr(char* str)
{
    if (!str) return nullptr;
    for (char* p = str; *p; ++p)
        *p = (char)tolower((unsigned char)*p);
    return str;
}
#define _strlwr strlwr
#define _strupr strupr
inline char* _strupr_s(char* str, size_t sz = 0) { return strupr(str); }
inline char* _strlwr_s(char* str, size_t sz = 0) { return strlwr(str); }
#define _strtoui64 strtoull
#define _atoi64 atoll
#define _strtoi64 strtoll

inline int _ui64toa_s(uint64_t val, char* buf, size_t buf_sz, int radix)
{
    if (!buf || buf_sz == 0) return EINVAL;
    if (radix == 10)
        snprintf(buf, buf_sz, "%llu", (unsigned long long)val);
    else if (radix == 16)
        snprintf(buf, buf_sz, "%llx", (unsigned long long)val);
    else if (radix == 8)
        snprintf(buf, buf_sz, "%llo", (unsigned long long)val);
    else
        return EINVAL;
    return 0;
}

inline int _i64toa_s(int64_t val, char* buf, size_t buf_sz, int radix)
{
    if (!buf || buf_sz == 0) return EINVAL;
    if (radix == 10)
        snprintf(buf, buf_sz, "%lld", (long long)val);
    else if (radix == 16)
        snprintf(buf, buf_sz, "%llx", (long long)val);
    else if (radix == 8)
        snprintf(buf, buf_sz, "%llo", (long long)val);
    else
        return EINVAL;
    return 0;
}
// error code numbers from original MS strcpy_s return value
inline int strcpy_s(char *dest, size_t num, const char *source)
{
    if(!dest)
        return EINVAL;

    if(0 == num)
    {
        dest[0] = '\0';
        return ERANGE;
    }

    if(!source)
    {
        dest[0] = '\0';
        return EINVAL;
    }

    size_t i;
    for(i = 0; i < num; i++)
    {
        if((dest[i] = source[i]) == '\0')
            return 0;
    }
    dest[0] = '\0';
    return ERANGE;
}

template <size_t num>
inline int strcpy_s(char (&dest)[num], const char *source) { return strcpy_s(dest, num, source); }

inline int strncpy_s(char * dest, size_t dst_size, const char * source, size_t num)
{
    if (!dest || (0 == dst_size))
        return EINVAL;

    if(0 == num)
    {
        dest[0] = '\0';
        return 0;
    }

    if (!source)
    {
        dest[0] = '\0';
        return EINVAL;
    }

    size_t i, end;
    if(num < dst_size)
        end = num;
    else
        end = dst_size - 1;

    for(i = 0; i < end && source[i]; i++)
        dest[i] = source[i];

    if(!source[i] || end == num)
    {
        dest[i] = '\0';
        return 0;
    }

    dest[0] = '\0';

    return EINVAL;
}

template <size_t dst_sz>
inline int strncpy_s(char (&dest)[dst_sz], const char * source, size_t num) { return strncpy_s(dest, dst_sz, source, num); }

inline int strcat_s(char * dest, size_t num, const char * source)
{
    if(!dest)
        return EINVAL;

    if(!source)
    {
        dest[0] = '\0';
        return EINVAL;
    }

    size_t i, j;
    for(i = 0; i < num; i++)
    {
        if(dest[i] == '\0')
        {
            for(j = 0; (j + i) < num; j++)
            {
                if((dest[j + i] = source[j]) == '\0')
                    return 0;
            }
        }
    }

    dest[0] = '\0';
    return ERANGE;
}

inline int strncat_s(char * dest, size_t num, const char * source, size_t count)
{
    if (!dest || !source)
        return EINVAL;

    size_t i, j;
    for(i = 0; i < num; i++)
    {
        if(dest[i] == '\0')
        {
            for(j = 0; (j + i) < num; j++)
            {
                if(j == count || (dest[j + i] = source[j]) == '\0')
                {
                    dest[j + i] = '\0';
                    return 0;
                }
            }
        }
    }

    return ERANGE;
}

#define _vsnprintf vsnprintf
inline int vsnprintf_s(char* buffer, size_t size, size_t, const char* format, va_list list)
{
    //TODO add bound check
    return vsnprintf(buffer, size, format, list);
}
template <size_t _Size>
inline int vsprintf_s(char (&_Dest)[_Size], const char* _Format, va_list _Args)
{
    return vsnprintf(_Dest, _Size, _Format, _Args);
}

inline int vsprintf_s(char* _Dest, size_t _Size, const char* _Format, va_list _Args)
{
    return vsnprintf(_Dest, _Size, _Format, _Args);
}

#define _snprintf snprintf

template <size_t _Size, typename... _Types>
inline int sprintf_s(char (&_Dest)[_Size], const char* _Format, _Types... _Args)
{
    return snprintf(_Dest, _Size, _Format, _Args...);
}

template <typename... _Types>
inline int sprintf_s(char* _Dest, size_t _Size, const char* _Format, _Types... _Args)
{
    return snprintf(_Dest, _Size, _Format, _Args...);
}
#define sscanf_s(buffer, format, ...) sscanf(buffer, format, ##__VA_ARGS__)
//#define GetProcAddress(handle, name) dlsym(handle, name)
#define _chdir chdir
#define _strnicmp strnicmp
#define strnicmp strncasecmp
#define _getcwd getcwd
#define _snwprintf swprintf
#define swprintf_s swprintf
#define wcsicmp _wcsicmp
#define _wcsicmp wcscmp
#define _tempnam tempnam
#define _access access
#define _open open
#define _close close
#define _sopen open
#define _utime utime
#define _utimbuf utimbuf
// MSVC's _sopen_s stores the descriptor through its first argument and returns
// zero on success.  Mapping it directly to open() loses the descriptor and
// inverts the success convention, so every caller treats a successfully opened
// file as a failure.
inline errno_t _sopen_s(int* handle, const char* filename, int oflag, int, int pmode)
{
    if (!handle || !filename)
        return EINVAL;

    // The locator keeps its virtual paths in the engine's Windows-style form.
    // Convert only at the POSIX I/O boundary.
    std::string native_filename(filename);
    convert_path_separators(native_filename.data());
    const int fd = ::open(native_filename.c_str(), oflag, pmode);
    if (fd == -1)
    {
        *handle = -1;
        return errno;
    }

    *handle = fd;
    return 0;
}
inline int _filelength(int fd)
{
    struct stat file_info;
    ::fstat(fd, &file_info);
    return file_info.st_size;
}
#define _fdopen fdopen
inline int _rmdir(const char *path)
{
    char* conv_fn = strdup(path);
    convert_path_separators(conv_fn);
    int result = rmdir(conv_fn);
    free(conv_fn);
    return result;
}
#define _write write
#define _strupr strupr
#define _read xr_read
#define _set_new_handler std::set_new_handler
#define _finite isfinite
inline int _mkdir(const char *dir) { return mkdir(dir, S_IRWXU); }

#define _wtoi(arg) wcstol(arg, NULL, 10)
#define _wtoi64(arg) wcstoll(arg, NULL, 10)
#undef min
#undef max
#define __max(a, b) std::max(a, b)
#define __min(a, b) std::min(a, b)

#define _locale_t locale_t
#define _isalpha_l isalpha_l
#define _create_locale(category, arg) newlocale(category, arg, (locale_t) 0)

#define ZeroMemory(p, sz) memset((p), 0, (sz))
#define CopyMemory(d, s, n) memcpy(d, s, n)

#define RGB(r,g,b) ( ((DWORD)(uint8_t)r)|((DWORD)((uint8_t)g)<<8)|((DWORD)((uint8_t)b)<<16) )
#ifndef SEVERITY_SUCCESS
#define SEVERITY_SUCCESS 0
#endif
#ifndef SEVERITY_ERROR
#define SEVERITY_ERROR 1
#endif

#ifndef FACILITY_NULL
#define FACILITY_NULL 0
#endif
#ifndef FACILITY_RPC
#define FACILITY_RPC 1
#endif
#ifndef FACILITY_DISPATCH
#define FACILITY_DISPATCH 2
#endif
#ifndef FACILITY_STORAGE
#define FACILITY_STORAGE 3
#endif
#ifndef FACILITY_ITF
#define FACILITY_ITF 4
#endif
#ifndef FACILITY_WIN32
#define FACILITY_WIN32 7
#endif
#ifndef FACILITY_WINDOWS
#define FACILITY_WINDOWS 8
#endif

#ifndef MAKE_HRESULT
#define MAKE_HRESULT(sev,fac,code) \
    ((HRESULT) (((unsigned long)(sev)<<31) | ((unsigned long)(fac)<<16) | ((unsigned long)(code))) )
#endif

#ifndef ERROR_INVALID_ACCESS
#define ERROR_INVALID_ACCESS 12L
#endif
#ifndef ERROR_READ_FAULT
#define ERROR_READ_FAULT 30L
#endif

#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define S_OK ((HRESULT)0x00000000L)
#define S_FALSE ((HRESULT)0x00000001L)
#define E_FAIL ((HRESULT)0x80004005L)
#define E_NOTIMPL ((HRESULT)0x80004001L)
#define E_NOINTERFACE ((HRESULT)0x80004002L)
#define E_POINTER ((HRESULT)0x80004003L)
#define E_ABORT ((HRESULT)0x80004004L)
#define E_INVALIDARG ((HRESULT)0x80070057L)
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)

#ifndef MB_PRECOMPOSED
#define MB_PRECOMPOSED 1
#endif

inline int MultiByteToWideChar(UINT CodePage, DWORD dwFlags, LPCSTR lpMultiByteStr, int cbMultiByte, LPWSTR lpWideCharStr, int cchWideChar) {
    if (!lpMultiByteStr) return 0;
    std::string src;
    if (cbMultiByte < 0) {
        src = lpMultiByteStr;
    } else {
        src.assign(lpMultiByteStr, cbMultiByte);
    }
    if (cchWideChar == 0) {
        size_t len = mbstowcs(nullptr, src.c_str(), 0);
        return (len == (size_t)-1) ? 0 : (int)(len + (cbMultiByte < 0 ? 1 : 0));
    }
    size_t converted = mbstowcs(lpWideCharStr, src.c_str(), cchWideChar);
    if (converted == (size_t)-1) return 0;
    if (cbMultiByte < 0 && converted < (size_t)cchWideChar) {
        lpWideCharStr[converted] = L'\0';
        return (int)(converted + 1);
    }
    return (int)converted;
}

inline int WideCharToMultiByte(UINT CodePage, DWORD dwFlags, LPCWSTR lpWideCharStr, int cchWideChar, LPSTR lpMultiByteStr, int cbMultiByte, LPCSTR lpDefaultChar, LPBOOL lpUsedDefaultChar) {
    if (lpUsedDefaultChar) *lpUsedDefaultChar = FALSE;
    if (!lpWideCharStr) return 0;
    std::wstring src;
    if (cchWideChar < 0) {
        src = lpWideCharStr;
    } else {
        src.assign(lpWideCharStr, cchWideChar);
    }
    if (cbMultiByte == 0) {
        size_t len = wcstombs(nullptr, src.c_str(), 0);
        return (len == (size_t)-1) ? 0 : (int)(len + (cchWideChar < 0 ? 1 : 0));
    }
    size_t converted = wcstombs(lpMultiByteStr, src.c_str(), cbMultiByte);
    if (converted == (size_t)-1) return 0;
    if (cchWideChar < 0 && converted < (size_t)cbMultiByte) {
        lpMultiByteStr[converted] = '\0';
        return (int)(converted + 1);
    }
    return (int)converted;
}

typedef DWORD           FOURCC;

typedef struct _AVIINDEXENTRY {
    DWORD	ckid;
    DWORD	dwFlags;
    DWORD	dwChunkOffset;
    DWORD	dwChunkLength;
} AVIINDEXENTRY;

typedef void *HIC;

#ifndef BITMAPINFOHEADER_DEFINED
#define BITMAPINFOHEADER_DEFINED
typedef struct tagBITMAPINFOHEADER {
    DWORD biSize;
    LONG  biWidth;
    LONG  biHeight;
    WORD  biPlanes;
    WORD  biBitCount;
    DWORD biCompression;
    DWORD biSizeImage;
    LONG  biXPelsPerMeter;
    LONG  biYPelsPerMeter;
    DWORD biClrUsed;
    DWORD biClrImportant;
} BITMAPINFOHEADER, *PBITMAPINFOHEADER, *LPBITMAPINFOHEADER;
#endif

inline BOOL SwitchToThread() { return (0 == sched_yield()); }

template <typename T>
decltype(auto) do_nothing(const T& obj)
{
    return obj;
}

#define xr_fs_strlwr(str) do_nothing(str)
#define xr_fs_nostrlwr(str) xr_strlwr(str)

/// For backward compability of FS, for real filesystem delimiter set to back
inline void restore_path_separators(char * path)
{
    while (char* sep = strchr(path, '/')) *sep = '\\'; //
}

inline void convert_path_separators(char * path)
{
    while (char* sep = strchr(path, '\\')) *sep = '/';
}

inline int xr_unlink(const char *path)
{
    char* conv_fn = strdup(path);
    convert_path_separators(conv_fn);
    int result = unlink(conv_fn);
    free(conv_fn);
    return result;
}
#define _unlink xr_unlink

inline tm* localtime_safe(const time_t *time, struct tm* result){ return localtime_r(time, result); }
inline tm* _localtime64(const time_t *time) { return localtime(time); }

inline uint32_t GetTickCount()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

#define xr_strerror(errno, buffer, bufferSize) strerror_r(errno, buffer, sizeof(buffer))

using xrpid_t = pid_t;

// This is a drop-in replacement for calls to linux 'read' function. We use this because unlike other OSes
// Linux 'read' function can return less then the requested number of bytes and might need to be called multiple times.
// Apart from this, it behaves exactly as you would expect and matches the other OSes implementation.
// See also: https://www.man7.org/linux/man-pages/man2/read.2.html
inline ssize_t xr_read(int file_handle, void *buffer, size_t count)
{
    ssize_t total_r_bytes = 0;
    do
    {
        const ssize_t r_bytes =
            read(file_handle, reinterpret_cast<unsigned char*>(buffer) + total_r_bytes, count - total_r_bytes);

        // Check for error
        if (r_bytes == -1)
            return -1;

        // Check for EOF otherwise we would loop indefinitely
        if (r_bytes == 0)
            return total_r_bytes;

        total_r_bytes += r_bytes;
    } while (static_cast<size_t>(total_r_bytes) < count);

    return total_r_bytes;
}

#ifndef _SYSTEMTIME_DEFINED
#define _SYSTEMTIME_DEFINED
typedef struct _SYSTEMTIME {
    uint16_t wYear;
    uint16_t wMonth;
    uint16_t wDayOfWeek;
    uint16_t wDay;
    uint16_t wHour;
    uint16_t wMinute;
    uint16_t wSecond;
    uint16_t wMilliseconds;
} SYSTEMTIME, *PSYSTEMTIME, *LPSYSTEMTIME;

inline void GetLocalTime(SYSTEMTIME* st)
{
    if (!st) return;
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tm_buf;
    localtime_r(&tv.tv_sec, &tm_buf);
    st->wYear = (uint16_t)(tm_buf.tm_year + 1900);
    st->wMonth = (uint16_t)(tm_buf.tm_mon + 1);
    st->wDayOfWeek = (uint16_t)tm_buf.tm_wday;
    st->wDay = (uint16_t)tm_buf.tm_mday;
    st->wHour = (uint16_t)tm_buf.tm_hour;
    st->wMinute = (uint16_t)tm_buf.tm_min;
    st->wSecond = (uint16_t)tm_buf.tm_sec;
    st->wMilliseconds = (uint16_t)(tv.tv_usec / 1000);
}
#endif
