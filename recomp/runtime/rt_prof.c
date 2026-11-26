/* rt_prof.c - optional performance diagnostics (XW_PROF=1): samples the game thread every ~1 ms, groups the samples by function (and by
 * "slow frame" = runFrame > 14 ms) and writes perf_log.txt every 10 s and on exit. Needs xenowerk.pdb next to the exe for function names. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#define MAXS 900000
void rt_prof_dump(int top);
static char g_logpath[MAX_PATH];
static HANDLE g_main; static DWORD64 *g_ip; static int *g_fr; static volatile int g_n, g_frame; static float *g_dur; static CRITICAL_SECTION g_cs;
static volatile long g_pres_n, g_pres_gt20, g_pres_gt25, g_pres_gt33, g_pres_gt50; static LARGE_INTEGER g_pq, g_pf;
static DWORD WINAPI samp(LPVOID u) {
    (void)u; DWORD last = GetTickCount();
    for (;;) {
        if (g_n < MAXS) {
            SuspendThread(g_main); CONTEXT cx; cx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(g_main, &cx)) { g_ip[g_n] = cx.Rip; g_fr[g_n] = g_frame; g_n++; }
            ResumeThread(g_main);
        }
        Sleep(1);
        if (GetTickCount() - last > 10000) { last = GetTickCount(); rt_prof_dump(60); }
    }
    return 0;
}
void rt_prof_start(void) {
    static int started; if (started) return; started = 1;
    { GetModuleFileNameA(NULL, g_logpath, MAX_PATH); char *sl = strrchr(g_logpath, 92); if (sl) strcpy(sl + 1, "perf_log.txt"); else strcpy(g_logpath, "perf_log.txt"); }
    g_ip = calloc(MAXS, 8); g_fr = calloc(MAXS, 4); g_dur = calloc(MAXS, 4); InitializeCriticalSection(&g_cs); QueryPerformanceFrequency(&g_pf);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CreateThread(NULL, 0, samp, NULL, 0, NULL);
}
void rt_prof_frame(float ms) { if (g_frame < MAXS - 1) g_dur[g_frame] = ms; g_frame++; }
void rt_prof_present(void) {      /* interval between consecutive presents = what the eye sees */
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    if (g_pq.QuadPart) { double ms = (double)(q.QuadPart - g_pq.QuadPart) * 1000.0 / g_pf.QuadPart; g_pres_n++; if (ms > 20) g_pres_gt20++; if (ms > 25) g_pres_gt25++; if (ms > 33) g_pres_gt33++; if (ms > 50) g_pres_gt50++; }
    g_pq = q;
}
typedef struct { char name[110]; int all, slow; } Ent;
static Ent *g_e; static int g_ne;
static Ent *find(const char *n) { for (int i = 0; i < g_ne; i++) if (!strcmp(g_e[i].name, n)) return &g_e[i]; if (g_ne >= 6000) return &g_e[0]; strncpy(g_e[g_ne].name, n, 109); return &g_e[g_ne++]; }
static int cmp(const void *a, const void *b) { const Ent *x = a, *y = b; return y->slow != x->slow ? y->slow - x->slow : y->all - x->all; }
void rt_prof_dump(int top) {
    if (!g_ip) return; rt_log("[prof] dump begin n=%d\n", g_n); EnterCriticalSection(&g_cs);
    static int symok; HANDLE hp = GetCurrentProcess(); if (!symok) { SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); SymInitialize(hp, NULL, TRUE); symok = 1; }
    int n = g_n, nF = g_frame < MAXS ? g_frame : MAXS, slowS = 0, nslowF = 0, gt20 = 0, gt33 = 0, gt50 = 0; double worst = 0;
    for (int i = 0; i < nF; i++) { if (g_dur[i] > 14) nslowF++; if (g_dur[i] > 20) gt20++; if (g_dur[i] > 33) gt33++; if (g_dur[i] > 50) gt50++; if (g_dur[i] > worst) worst = g_dur[i]; }
    free(g_e); g_e = calloc(6000, sizeof(Ent)); g_ne = 0;
    static struct { DWORD64 ip; char nm[110]; } cache[65536];
    for (int i = 0; i < n; i++) {
        DWORD64 ip = g_ip[i]; unsigned h = (unsigned)((ip >> 2) * 2654435761u) & 65535u;
        if (cache[h].ip != ip) {
            char buf[sizeof(SYMBOL_INFO) + 128]; SYMBOL_INFO *si = (SYMBOL_INFO *)buf; memset(si, 0, sizeof buf); si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 120; DWORD64 d = 0;
            char nm[110]; HMODULE m = NULL; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ip, &m);
            char mn[MAX_PATH] = ""; if (m) GetModuleFileNameA(m, mn, MAX_PATH); const char *bn = strrchr(mn, 92); bn = bn ? bn + 1 : mn;
            if (SymFromAddr(hp, ip, &d, si)) snprintf(nm, sizeof nm, "%s", si->Name); else snprintf(nm, sizeof nm, "[%s]", bn[0] ? bn : "?");
            cache[h].ip = ip; strcpy(cache[h].nm, nm);
        }
        Ent *e = find(cache[h].nm); e->all++; int f = g_fr[i]; if (f < MAXS && g_dur[f] > 14.0f) { e->slow++; slowS++; }
    }
    qsort(g_e, g_ne, sizeof(Ent), cmp);
    FILE *fp = fopen(g_logpath, "w");
    if (fp) {
        fprintf(fp, "frames=%d  runFrame>14ms=%d  >20ms=%d  >33ms=%d  >50ms=%d  worst=%.1fms\n", nF, nslowF, gt20, gt33, gt50, worst);
        fprintf(fp, "presents=%ld  interval>20ms=%ld  >25ms=%ld  >33ms=%ld  >50ms=%ld\n", g_pres_n, g_pres_gt20, g_pres_gt25, g_pres_gt33, g_pres_gt50);
        fprintf(fp, "samples=%d  samples_in_slow_frames=%d\n\n  slow    all  function\n", n, slowS);
        for (int i = 0; i < top && i < g_ne; i++) fprintf(fp, "%6d %6d  %s\n", g_e[i].slow, g_e[i].all, g_e[i].name);
        fclose(fp);
    }
    LeaveCriticalSection(&g_cs); rt_log("[prof] dump end\n");
}
