/*
 * Windows XP stand-ins for the Vista+ functions that the static C runtime imports (Microsoft's current CRT no longer
 * targets XP; everything else in both executables is built for it). winxp_crt.asm points these functions' import
 * address table entries here, so the CRT's calls land in this file and kernel32.lib's entries for them are never
 * linked: the executables load on XP. Each one uses the real function when Windows has it, else:
 *
 *   FlsAlloc/Free/GetValue/SetValue -> TlsAlloc/Free/GetValue/SetValue. TLS has no destructor callback, so on XP
 *                                      the CRT's per-thread block (a few hundred bytes) leaks when a thread exits.
 *   InitializeCriticalSectionEx     -> InitializeCriticalSectionAndSpinCount.
 *   AcquireSRWLockExclusive,        -> a spin lock that yields. The CRT holds these locks for a few instructions
 *   ReleaseSRWLockExclusive            (thread-safe static initialization).
 *   SleepConditionVariableSRW,      -> unlock, sleep 1 ms, relock: a spurious wakeup, which every caller handles by
 *   WakeAllConditionVariable           rechecking its condition in a loop; wake does nothing.
 *
 * These run from the CRT's startup on, so they find the real functions lazily, with nothing but kernel32 calls.
 * Each group is all real or all stand-in (an SRW lock must never be taken one way and released the other).
 * tools/check_imports.py fails the build's CI if the CRT starts importing something else XP lacks.
 */
#include <windows.h>

static FARPROC kernel32(const char *name)
{
    return GetProcAddress(GetModuleHandleW(L"kernel32.dll"), name);
}

/* ---------------------------------------------------------------- fiber-local storage */

static struct {
    DWORD(WINAPI *alloc)(void *callback);
    BOOL(WINAPI *free)(DWORD index);
    void *(WINAPI *get)(DWORD index);
    BOOL(WINAPI *set)(DWORD index, void *value);
    volatile LONG resolved;
} g_fls;

static void fls_resolve(void)
{
    if (g_fls.resolved)
        return;
    FARPROC a = kernel32("FlsAlloc"), f = kernel32("FlsFree");
    FARPROC g = kernel32("FlsGetValue"), s = kernel32("FlsSetValue");
    if (a && f && g && s) {
        g_fls.alloc = (DWORD(WINAPI *)(void *))a;
        g_fls.free = (BOOL(WINAPI *)(DWORD))f;
        g_fls.get = (void *(WINAPI *)(DWORD))g;
        g_fls.set = (BOOL(WINAPI *)(DWORD, void *))s;
    }
    InterlockedExchange(&g_fls.resolved, 1);
}

DWORD WINAPI xp_FlsAlloc(void *callback)
{
    fls_resolve();
    return g_fls.alloc ? g_fls.alloc(callback) : TlsAlloc();
}

BOOL WINAPI xp_FlsFree(DWORD index)
{
    fls_resolve();
    return g_fls.free ? g_fls.free(index) : TlsFree(index);
}

void *WINAPI xp_FlsGetValue(DWORD index)
{
    fls_resolve();
    return g_fls.get ? g_fls.get(index) : TlsGetValue(index);
}

BOOL WINAPI xp_FlsSetValue(DWORD index, void *value)
{
    fls_resolve();
    return g_fls.set ? g_fls.set(index, value) : TlsSetValue(index, value);
}

/* ---------------------------------------------------------------- critical sections */

BOOL WINAPI xp_InitializeCriticalSectionEx(CRITICAL_SECTION *cs, DWORD spin, DWORD flags)
{
    static BOOL(WINAPI * real)(CRITICAL_SECTION *, DWORD, DWORD);
    static volatile LONG resolved;
    if (!resolved) {
        real = (BOOL(WINAPI *)(CRITICAL_SECTION *, DWORD, DWORD))kernel32("InitializeCriticalSectionEx");
        InterlockedExchange(&resolved, 1);
    }
    return real ? real(cs, spin, flags) : InitializeCriticalSectionAndSpinCount(cs, spin);
}

/* ---------------------------------------------------------------- SRW locks and condition variables */

/* SRWLOCK and CONDITION_VARIABLE are each one pointer, zero when free / when nobody waits. */
static struct {
    void(WINAPI *acquire)(void **lock);
    void(WINAPI *release)(void **lock);
    BOOL(WINAPI *sleep)(void **cv, void **lock, DWORD ms, ULONG flags);
    void(WINAPI *wake_all)(void **cv);
    volatile LONG resolved;
} g_srw;

static void srw_resolve(void)
{
    if (g_srw.resolved)
        return;
    FARPROC a = kernel32("AcquireSRWLockExclusive"), r = kernel32("ReleaseSRWLockExclusive");
    FARPROC s = kernel32("SleepConditionVariableSRW"), w = kernel32("WakeAllConditionVariable");
    if (a && r && s && w) {
        g_srw.acquire = (void(WINAPI *)(void **))a;
        g_srw.release = (void(WINAPI *)(void **))r;
        g_srw.sleep = (BOOL(WINAPI *)(void **, void **, DWORD, ULONG))s;
        g_srw.wake_all = (void(WINAPI *)(void **))w;
    }
    InterlockedExchange(&g_srw.resolved, 1);
}

void WINAPI xp_AcquireSRWLockExclusive(void **lock)
{
    srw_resolve();
    if (g_srw.acquire) {
        g_srw.acquire(lock);
        return;
    }
    while (InterlockedCompareExchangePointer(lock, (void *)1, NULL))
        SwitchToThread();
}

void WINAPI xp_ReleaseSRWLockExclusive(void **lock)
{
    srw_resolve();
    if (g_srw.release)
        g_srw.release(lock);
    else
        InterlockedExchangePointer(lock, NULL);
}

BOOL WINAPI xp_SleepConditionVariableSRW(void **cv, void **lock, DWORD ms, ULONG flags)
{
    srw_resolve();
    if (g_srw.sleep)
        return g_srw.sleep(cv, lock, ms, flags);
    xp_ReleaseSRWLockExclusive(lock);
    Sleep(ms ? 1 : 0);
    xp_AcquireSRWLockExclusive(lock);
    return TRUE;
}

void WINAPI xp_WakeAllConditionVariable(void **cv)
{
    srw_resolve();
    if (g_srw.wake_all)
        g_srw.wake_all(cv);
}
