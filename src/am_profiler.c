/*
 * am_profiler.c - a small sampling profiler for the busiest threads.
 *
 * RECOMP_PROFILE=1 starts it. About 1,000 times a second it suspends each of
 * the (up to four) threads that used the most CPU over the last second,
 * reads its instruction pointer and resumes it. Every 10 seconds it prints,
 * per thread, the functions the samples landed in -- generated functions
 * (sub_XXXXXXXX, i.e. the guest function), runtime functions and system
 * DLLs alike, resolved through the PDB that Release builds now emit.
 *
 * Why: races run at ~29 fps (60 halved by vsync) while no thread is pinned
 * at 100%, so "which code is too slow" has to be measured, not guessed.
 * Suspending a thread 1,000 times a second costs it a little; numbers are
 * relative, so leave it off for normal play.
 */
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROF_THREADS 4
#define PROF_SLOTS   16384           /* per-thread RIP table, power of two */
#define PROF_TOP     14

typedef struct {
    DWORD tid;
    HANDLE h;
    unsigned long long rip[PROF_SLOTS];
    unsigned cnt[PROF_SLOTS];
    unsigned samples, dropped;
} ProfThread;

static ProfThread *s_pt;             /* PROF_THREADS entries */
static DWORD s_self;

/* The up-to-four threads of this process with the most CPU since last time. */
static void pick_threads(void)
{
    enum { MAXT = 256 };
    static DWORD ids[MAXT];
    static ULONGLONG last[MAXT];
    static int n;
    DWORD me = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD best[PROF_THREADS] = {0};
    ULONGLONG bestd[PROF_THREADS] = {0};
    int i, k;

    if (snap == INVALID_HANDLE_VALUE) return;
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) do {
        HANDLE h;
        FILETIME c, e, kt, ut;
        ULONGLONG t, d;
        int f = -1;
        if (te.th32OwnerProcessID != me || te.th32ThreadID == s_self) continue;
        h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        if (GetThreadTimes(h, &c, &e, &kt, &ut)) {
            t = ((ULONGLONG)kt.dwHighDateTime << 32 | kt.dwLowDateTime)
              + ((ULONGLONG)ut.dwHighDateTime << 32 | ut.dwLowDateTime);
            for (i = 0; i < n; i++) if (ids[i] == te.th32ThreadID) { f = i; break; }
            if (f < 0 && n < MAXT) { f = n++; ids[f] = te.th32ThreadID; last[f] = t; }
            if (f >= 0) {
                d = t - last[f];
                last[f] = t;
                for (k = 0; k < PROF_THREADS; k++)
                    if (d > bestd[k]) {
                        int m;
                        for (m = PROF_THREADS - 1; m > k; m--) { best[m] = best[m-1]; bestd[m] = bestd[m-1]; }
                        best[k] = te.th32ThreadID; bestd[k] = d;
                        break;
                    }
            }
        }
        CloseHandle(h);
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);

    /* Keep threads that stay hot (their tables keep accumulating). */
    for (k = 0; k < PROF_THREADS; k++) {
        if (!best[k] || bestd[k] < 100000) continue;     /* < 10 ms in the last second */
        for (i = 0; i < PROF_THREADS; i++) if (s_pt[i].tid == best[k]) break;
        if (i < PROF_THREADS) continue;
        for (i = 0; i < PROF_THREADS; i++) if (!s_pt[i].tid) break;
        if (i == PROF_THREADS) continue;                 /* full: keep the current set */
        s_pt[i].h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT
                               | THREAD_QUERY_INFORMATION, FALSE, best[k]);
        if (s_pt[i].h) s_pt[i].tid = best[k];
    }
}

static void sample(ProfThread *p)
{
    CONTEXT ctx;
    unsigned long long rip;
    unsigned h, j;

    if (SuspendThread(p->h) == (DWORD)-1) { p->dropped++; return; }
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (!GetThreadContext(p->h, &ctx)) { ResumeThread(p->h); p->dropped++; return; }
    ResumeThread(p->h);
    rip = ctx.Rip;
    p->samples++;
    h = (unsigned)((rip >> 4) * 2654435761u) & (PROF_SLOTS - 1);
    for (j = 0; j < 64; j++, h = (h + 1) & (PROF_SLOTS - 1)) {
        if (p->rip[h] == rip) { p->cnt[h]++; return; }
        if (!p->rip[h]) { p->rip[h] = rip; p->cnt[h] = 1; return; }
    }
    p->dropped++;
}

/* Print the top functions for each sampled thread, then reset. */
static void report(void)
{
    HANDLE proc = GetCurrentProcess();
    enum { MAXF = 4096 };
    static unsigned long long faddr[MAXF];
    static unsigned fcnt[MAXF];
    static char fname[MAXF][64];
    char symbuf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)symbuf;
    int t, i, nf;

    for (t = 0; t < PROF_THREADS; t++) {
        ProfThread *p = &s_pt[t];
        unsigned other = 0;
        if (!p->tid || !p->samples) continue;
        nf = 0;
        for (i = 0; i < PROF_SLOTS; i++) {
            DWORD64 disp = 0;
            unsigned long long key;
            const char *name;
            char modname[64];
            int f;
            if (!p->rip[i]) continue;
            memset(symbuf, 0, sizeof symbuf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            if (SymFromAddr(proc, p->rip[i], &disp, sym)) {
                key = sym->Address;
                name = sym->Name;
            } else {
                /* No symbol: group by module. */
                HMODULE m = NULL;
                modname[0] = 0;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                       | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)(uintptr_t)p->rip[i], &m) && m) {
                    char path[MAX_PATH], *b;
                    GetModuleFileNameA(m, path, sizeof path);
                    b = strrchr(path, '\\');
                    snprintf(modname, sizeof modname, "[%s]", b ? b + 1 : path);
                } else
                    snprintf(modname, sizeof modname, "[unknown]");
                key = (unsigned long long)(uintptr_t)m;
                name = modname;
            }
            for (f = 0; f < nf; f++) if (faddr[f] == key) break;
            if (f == nf) {
                if (nf == MAXF) { other += p->cnt[i]; continue; }
                faddr[nf] = key; fcnt[nf] = 0;
                snprintf(fname[nf], sizeof fname[nf], "%s", name);
                nf++;
            }
            fcnt[f] += p->cnt[i];
        }
        fprintf(stderr, "[PROFILE] tid %lu: %u samples\n", (unsigned long)p->tid, p->samples);
        for (i = 0; i < PROF_TOP; i++) {
            int b = -1, f;
            for (f = 0; f < nf; f++) if (fcnt[f] && (b < 0 || fcnt[f] > fcnt[b])) b = f;
            if (b < 0) break;
            fprintf(stderr, "[PROFILE]   %5.1f%%  %s\n", 100.0 * fcnt[b] / p->samples, fname[b]);
            fcnt[b] = 0;
        }
        memset(p->rip, 0, sizeof p->rip);
        memset(p->cnt, 0, sizeof p->cnt);
        p->samples = p->dropped = 0;
    }
    fflush(stderr);
}

static DWORD WINAPI profiler_thread(LPVOID unused)
{
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL,
                        0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                        TIMER_ALL_ACCESS);
    DWORD last_pick = 0, last_report = GetTickCount();
    (void)unused;
    s_self = GetCurrentThreadId();
    for (;;) {
        DWORD now = GetTickCount();
        int t;
        if (now - last_pick >= 1000) { pick_threads(); last_pick = now; }
        for (t = 0; t < PROF_THREADS; t++)
            if (s_pt[t].tid) sample(&s_pt[t]);
        if (now - last_report >= 10000) { report(); last_report = now; }
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -10000;                       /* 1 ms */
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, 20);
        } else
            Sleep(1);
    }
    return 0;
}

/* Called from WinMain after SymInitialize. */
void am_profiler_start(void)
{
    const char *e = getenv("RECOMP_PROFILE");
    if (!e || e[0] != '1') return;
    s_pt = (ProfThread *)calloc(PROF_THREADS, sizeof *s_pt);
    if (!s_pt) return;
    CloseHandle(CreateThread(NULL, 0, profiler_thread, NULL, 0, NULL));
    fprintf(stderr, "[PROFILE] sampling the 4 busiest threads at ~1 kHz; report every 10 s\n");
}
