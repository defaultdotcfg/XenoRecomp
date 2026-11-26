/* rt_core.c - process setup, guest memory, dispatch, window; Xenowerk (iOS armv7 Thumb-2) static recompilation runtime */
#include "rt.h"
#include <direct.h>
#include <stdarg.h>
#include <GL/gl.h>
#include <dbghelp.h>
#include <mmsystem.h>

HWND g_hwnd; HDC g_hdc;
volatile int g_quit;
char g_appdir[MAX_PATH];
char g_datadir[MAX_PATH];   /* RecompLauncher: --data <dir> / RECOMPLAUNCHER_DATA; empty = saves next to the exe as before */
int rt_ipa_extract(const char *ipa, const char *dest, char *app_out, size_t cap);
int g_touch_down, g_touch_x, g_touch_y;
static FILE *g_logf;
extern int g_gl_w, g_gl_h;
volatile int g_vp[4] = { 0, 0, 1024, 768 };
volatile unsigned char g_keys[256]; volatile int g_wheel; extern volatile int g_mouse_game;


/* precise sleep: high-resolution waitable timer (Win10 1803+), falls back to Sleep(1); unaffected by the global timer resolution */
void rt_sleep_us(int us) {
    static HANDLE t; static int tried;
    if (!tried) { tried = 1; t = CreateWaitableTimerExW(NULL, NULL, 0x2 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS); }
    if (t) { LARGE_INTEGER due; due.QuadPart = -10LL * us; if (SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE)) { WaitForSingleObject(t, 100); return; } }
    Sleep(1);
}
const char *rt_genv(const char *name) {   /* getenv is slow (locks + linear scan): cache by call-site literal */
    static struct { const char *k; const char *v; } tab[256]; static int n;
    for (int i = 0; i < n; i++) if (tab[i].k == name) return tab[i].v;
    const char *v = getenv(name); if (n < 256) { tab[n].k = name; tab[n].v = v; n++; } return v;
}
void rt_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[2048]; vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    { static int quiet = -1; if (quiet < 0) quiet = rt_genv("XW_QUIET") != NULL; if (!quiet) { fputs(buf, stderr); fflush(stderr); } }
    if (g_logf) { fputs(buf, g_logf); fflush(g_logf); }
}

/* ------------------------------------------------------------ guest heap */
static CRITICAL_SECTION hlock;
static uint32_t heap_off, heap_committed, freel[64];
#define HDR 16u

static int size_class(uint32_t n, uint32_t *csz) {
    if (n <= 512) { uint32_t r = (n + 15) & ~15u; if (!r) r = 16; *csz = r; return (int)(r / 16); }
    uint32_t r = 1024; int k = 33;
    while (r < n) { r <<= 1; k++; }
    *csz = r; return k;
}
uint32_t g_malloc(uint32_t n) {
    uint32_t csz; int k = size_class(n ? n : 1, &csz);
    EnterCriticalSection(&hlock);
    uint32_t p = freel[k];
    if (p) { freel[k] = ld32(p); }
    else {
        uint32_t total = csz + HDR;
        if ((uint64_t)heap_off + total + HDR > GUEST_HEAP_SZ) { LeaveCriticalSection(&hlock); rt_log("guest heap exhausted\n"); return 0; }
        while (heap_off + total + HDR > heap_committed) {
            if (!VirtualAlloc((void *)(uintptr_t)(GUEST_HEAP_VA + heap_committed), 0x400000, MEM_COMMIT, PAGE_READWRITE)) { LeaveCriticalSection(&hlock); rt_log("heap commit failed\n"); return 0; }
            heap_committed += 0x400000;
        }
        p = GUEST_HEAP_VA + heap_off + HDR;
        heap_off += total;
        st32(p - 8, csz); st32(p - 4, 0xA110C8ED);
    }
    LeaveCriticalSection(&hlock);
    st32(p - 12, (uint32_t)k);
    return p;
}
uint32_t g_usable(uint32_t p) { return p ? ld32(p - 8) : 0; }
void g_free(uint32_t p) {
    if (!p) return;
    if (ld32(p - 4) != 0xA110C8ED) { rt_log("free of bad pointer %x\n", p); return; }
    uint32_t k = ld32(p - 12);
    EnterCriticalSection(&hlock);
    st32(p, freel[k]); freel[k] = p;
    LeaveCriticalSection(&hlock);
}
uint32_t g_realloc(uint32_t p, uint32_t n) {
    if (!p) return g_malloc(n);
    uint32_t u = g_usable(p);
    if (n <= u) return p;
    uint32_t q = g_malloc(n);
    if (q) memcpy(GP(q), GP(p), u);
    g_free(p);
    return q;
}
uint32_t g_strdup(const char *s) { uint32_t n = (uint32_t)strlen(s) + 1, p = g_malloc(n); memcpy(GP(p), s, n); return p; }
uint32_t g_calloc(uint32_t n) { uint32_t p = g_malloc(n); if (p) memset(GP(p), 0, n); return p; }

uint32_t g_call(CPU *c, uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    CPU t = *c;
    t.r[0] = a0; t.r[1] = a1; t.r[2] = a2; t.r[3] = a3; t.r[14] = 0;
    arm_dispatch(&t, addr);
    return t.r[0];
}

/* ------------------------------------------------------------ dispatch */
extern const struct { uint32_t addr; guest_fn fn; } arm_func_table[];
extern const unsigned arm_func_count;
extern const struct { uint32_t addr; guest_fn fn; const char *name; } rt_stub_table[];
extern const unsigned rt_stub_count;
extern const struct { uint32_t addr, val, addend; const char *name; } rt_slots[];

static uint32_t dyn_addr[64]; static guest_fn dyn_fn[64]; static int ndyn;
uint32_t rt_dyn_register(guest_fn fn) { dyn_addr[ndyn] = 0x7FF00000u + 16u * ndyn; dyn_fn[ndyn] = fn; return dyn_addr[ndyn++]; }

void arm_dispatch(CPU *c, uint32_t addr) {
    addr &= ~1u;
    if (addr >= 0x604000u && addr < 0x610000u) addr -= 0x600000u;
    static struct { uint32_t a; guest_fn f; } dc[4096];            /* direct-mapped dispatch cache */
    unsigned h = (addr >> 1) * 2654435761u >> 20 & 4095u;
    if (dc[h].a == addr && dc[h].f) { dc[h].f(c); return; }
    unsigned lo = 0, hi = arm_func_count;
    while (lo < hi) { unsigned m = (lo + hi) / 2; if (arm_func_table[m].addr < addr) lo = m + 1; else hi = m; }
    if (lo < arm_func_count && arm_func_table[lo].addr == addr) { dc[h].a = addr; dc[h].f = arm_func_table[lo].fn; arm_func_table[lo].fn(c); return; }
    lo = 0; hi = rt_stub_count;
    while (lo < hi) { unsigned m = (lo + hi) / 2; if (rt_stub_table[m].addr < addr) lo = m + 1; else hi = m; }
    if (lo < rt_stub_count && rt_stub_table[lo].addr == addr) { rt_stub_table[lo].fn(c); return; }
    for (int i = 0; i < ndyn; i++) if (dyn_addr[i] == addr) { dyn_fn[i](c); return; }
    rt_log("[dispatch] unknown target %x lr=%x r0=%x r1=%x r2=%x r3=%x\n", addr, c->r[14], c->r[0], c->r[1], c->r[2], c->r[3]);
    arm_trap(addr, "dispatch to unknown address");
}
void arm_trap(uint32_t addr, const char *why) {
    rt_log("[trap] guest 0x%x: %s\n", addr, why);
    fflush(NULL);
    ExitProcess(3);
}
void arm_svc(CPU *c, uint32_t imm) { rt_log("[svc %x] r0=%x r12=%x\n", imm, c->r[0], c->r[12]); RET(0); }

void rt_unimpl(CPU *c, const char *name) {
    static char seen[512][48]; static int n;
    for (int i = 0; i < n; i++) if (!strcmp(seen[i], name)) { RET(0); return; }
    if (n < 512) { strncpy(seen[n], name, 47); n++; }
    rt_log("[unimplemented import] %s(r0=%x r1=%x r2=%x r3=%x)\n", name, c->r[0], c->r[1], c->r[2], c->r[3]);
    RET(0);
}

/* ------------------------------------------------------------ window / input */
static void mouse_to_logical(LPARAM l) {
    int px = (short)LOWORD(l), py = (short)HIWORD(l), vw = g_vp[2] ? g_vp[2] : g_gl_w, vh = g_vp[3] ? g_vp[3] : g_gl_h;
    int pw = g_gl_w, ph = g_gl_h;
    int x = (px - g_vp[0]) * pw / vw, y = (py - g_vp[1]) * ph / vh;
    g_touch_x = x < 0 ? 0 : x >= pw ? pw - 1 : x; g_touch_y = y < 0 ? 0 : y >= ph ? ph - 1 : y;
}
static void toggle_fullscreen(void) {
    static WINDOWPLACEMENT wp = { sizeof wp }; static int full;
    LONG st = GetWindowLongA(g_hwnd, GWL_STYLE);
    if (!full) {
        MONITORINFO mi = { sizeof mi }; GetWindowPlacement(g_hwnd, &wp);
        GetMonitorInfoA(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongA(g_hwnd, GWL_STYLE, st & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
        full = 1;   /* the mouse is the touch screen, so the cursor stays visible in fullscreen */
    } else {
        SetWindowLongA(g_hwnd, GWL_STYLE, st | WS_OVERLAPPEDWINDOW); SetWindowPlacement(g_hwnd, &wp);
        SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED); full = 0;
    }
}
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CLOSE: g_quit = 1; if (rt_genv("XW_PROF")) { extern void rt_prof_dump(int); rt_prof_dump(60); } ExitProcess(0); return 0;
    case WM_LBUTTONDOWN: SetCapture(h); mouse_to_logical(l); g_touch_down = g_mouse_game ? 0 : 1; g_keys[VK_LBUTTON] = 1; return 0;
    case WM_SETCURSOR: if (LOWORD(l) == HTCLIENT) { SetCursor(LoadCursor(NULL, g_mouse_game ? IDC_CROSS : IDC_ARROW)); return TRUE; } break;
    case WM_MOUSEMOVE: mouse_to_logical(l); return 0;
    case WM_RBUTTONDOWN: g_keys[VK_RBUTTON] = 1; return 0;
    case WM_RBUTTONUP: g_keys[VK_RBUTTON] = 0; return 0;
    case WM_MBUTTONDOWN: g_keys[VK_MBUTTON] = 1; return 0;
    case WM_MBUTTONUP: g_keys[VK_MBUTTON] = 0; return 0;
    case WM_MOUSEWHEEL: g_wheel += (short)HIWORD(w) / 120; return 0;
    case WM_LBUTTONUP: ReleaseCapture(); g_touch_down = 0; g_keys[VK_LBUTTON] = 0; return 0;
    case WM_KILLFOCUS: memset((void *)g_keys, 0, sizeof g_keys); g_touch_down = 0; return 0;
    case WM_SYSKEYDOWN: if (w == VK_RETURN && !(l & (1 << 30))) { toggle_fullscreen(); return 0; } break;
    case WM_KEYDOWN:
        if (w == VK_F11 && !(l & (1 << 30))) { toggle_fullscreen(); return 0; }
        if (w < 256) g_keys[w] = 1; return 0;
    case WM_KEYUP: if (w < 256) g_keys[w] = 0; return 0;
    }
    return DefWindowProcA(h, m, w, l);
}
void rt_pump(void) {
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
}
static int create_window(void) {
    WNDCLASSA wc = { 0 }; wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "XenowerkRecomp"; wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    int pct = rt_genv("XW_WINPCT") ? atoi(rt_genv("XW_WINPCT")) : 70; if (pct < 20 || pct > 100) pct = 70;
    int h = GetSystemMetrics(SM_CYSCREEN) * pct / 100; h &= ~1; int w = h * g_gl_w / g_gl_h;
    { int sw = GetSystemMetrics(SM_CXSCREEN) * 95 / 100; if (w > sw) { w = sw; h = (w * g_gl_h / g_gl_w) & ~1; } }   /* ultrawide: keep the window on screen */
    if (rt_genv("XW_WINW") && rt_genv("XW_WINH")) { int ew = atoi(rt_genv("XW_WINW")), eh = atoi(rt_genv("XW_WINH")); if (ew >= 320 && eh >= 240 && ew <= 16384 && eh <= 16384) { w = ew; h = eh; } }   /* explicit window (client area) resolution */
    RECT r = { 0, 0, w, h }; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowA("XenowerkRecomp", "Xenowerk (static recompilation)", WS_OVERLAPPEDWINDOW | (rt_genv("XW_HIDDEN") ? 0 : WS_VISIBLE),
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    g_hdc = GetDC(g_hwnd);
    PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 24, 8, 0, PFD_MAIN_PLANE };
    SetPixelFormat(g_hdc, ChoosePixelFormat(g_hdc, &pfd), &pfd);
    HGLRC rc = wglCreateContext(g_hdc);
    wglMakeCurrent(g_hdc, rc);
    { GLint v0[4] = { 0 }; RECT cr0; GetClientRect(g_hwnd, &cr0); glGetIntegerv(GL_VIEWPORT, v0); rt_log("[win] client %ldx%ld, initial GL viewport %dx%d\n", cr0.right, cr0.bottom, v0[2], v0[3]); }
    if (rt_genv("XW_VSYNC")) { typedef BOOL (WINAPI *PFNSI)(int); PFNSI si = (PFNSI)wglGetProcAddress("wglSwapIntervalEXT"); if (si) si(atoi(rt_genv("XW_VSYNC")) ? 1 : 0); }
    if (rt_genv("XW_FULLSCREEN") && atoi(rt_genv("XW_FULLSCREEN")) && !rt_genv("XW_HIDDEN")) toggle_fullscreen();
    return rc != NULL;
}
void rt_gl_setup(void);
void rt_gl_present(HDC hdc, int ww, int wh, const char *dump);
void rt_objc_frame_input(CPU *c);

volatile int g_presented;
void rt_present(CPU *c) {
    g_presented++;
    static int frame; static DWORD t0; static char pend[MAX_PATH];
    const char *shot = rt_genv("XW_SHOT"); char p[MAX_PATH]; const char *dump = NULL;
    if (shot) { int want = 0; const char *sf = rt_genv("XW_SHOTF"); for (const char *q = sf; q && *q; ) { int f0 = 0, n2 = 0; sscanf(q, "%d%n", &f0, &n2); if (f0 == frame) want = 1; q += n2; if (*q == ',') q++; if (!n2) break; }
        if (want) { snprintf(p, sizeof p, "%s_%d.rgba", shot, frame); dump = p; } }
    {   /* control file: XW_CTL=<path>; lines "tap x y", "shot <file.rgba>" (consumed every 10 frames) */
        static const char *ctl; static int init, rel;
        if (!init) { init = 1; ctl = rt_genv("XW_CTL"); }
        if (rel && --rel == 0) g_touch_down = 0;
        if (pend[0] && !dump) { dump = pend; }
        if (ctl && frame % 10 == 0) {
            FILE *f = fopen(ctl, "rb");
            if (f) { char ln[600]; while (fgets(ln, sizeof ln, f)) { int x, y; char nm[MAX_PATH];
                    if (sscanf(ln, "tap %d %d", &x, &y) == 2) { g_touch_down = 1; g_touch_x = x; g_touch_y = y; rel = 6; }
                    else if (sscanf(ln, "key %d %d", &x, &y) == 2) { if (x >= 0 && x < 256) g_keys[x] = (unsigned char)y; }
                    else if (sscanf(ln, "shot %259s", nm) == 1) { strncpy(pend, nm, MAX_PATH - 1); dump = pend; } }
                fclose(f); remove(ctl); }
        }
    }
    {   /* frame limiter: XW_FPS = target (default 60); 0 = unlimited (use VSync to follow the monitor refresh rate) */
        static LARGE_INTEGER fq, nxt; static int fps = -1; LARGE_INTEGER now; QueryPerformanceCounter(&now);
        if (fps < 0) { fps = rt_genv("XW_FPS") ? atoi(rt_genv("XW_FPS")) : 60; if (fps < 0 || fps > 1000) fps = 60; timeBeginPeriod(1); }
        if (!fq.QuadPart) { QueryPerformanceFrequency(&fq); nxt = now; }
        if (fps > 0) {
            nxt.QuadPart += fq.QuadPart * 95 / 100 / fps;   /* slightly faster than the engine step: the engine paces itself, this is only a cap */
            for (;;) {                  /* sleep while far from the deadline, spin for the last ~1.5 ms for an accurate pace */
                QueryPerformanceCounter(&now); long long left = nxt.QuadPart - now.QuadPart; if (left <= 0) break;
                if (left * 1000000 / fq.QuadPart >= 2000) rt_sleep_us((int)(left * 1000000 / fq.QuadPart) - 1500); else YieldProcessor();
            }
            if (now.QuadPart - nxt.QuadPart > fq.QuadPart / 10) nxt = now;
        }
    }
    if (rt_genv("XW_PROF")) { extern void rt_prof_present(void); rt_prof_present(); }
    { RECT cr; GetClientRect(g_hwnd, &cr); rt_gl_present(g_hdc, cr.right, cr.bottom, dump); if (dump == pend) pend[0] = 0; }
    rt_pump(); rt_objc_frame_input(c);
    frame++;
    if (frame % 120 == 0) { DWORD t = GetTickCount(); rt_log("[frame %d] %.1f fps\n", frame, t0 ? 120000.0 / (t - t0) : 0.0); t0 = t; }
}

/* ------------------------------------------------------------ extern data slots */
void rt_objc_register_slot(uint32_t obj, const char *name);
static void fill_slots(void) {
    for (int i = 0; rt_slots[i].addr; i++) {
        uint32_t v = rt_slots[i].val;
        const char *n = rt_slots[i].name;
        if (!v) {
            v = g_malloc(256); memset(GP(v), 0, 256);
            if (!strcmp(n, "___stdoutp")) st32(v, 0x7f000001);
            else if (!strcmp(n, "___stderrp")) st32(v, 0x7f000002);
            else if (!strcmp(n, "___stdinp")) st32(v, 0x7f000000);
            rt_objc_register_slot(v, n);
        }
        st32(rt_slots[i].addr, v + rt_slots[i].addend);
    }
}

/* ------------------------------------------------------------ main */
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    rt_log("[crash] exception %lx at host %p, access addr %p\n", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress,
           ep->ExceptionRecord->NumberParameters > 1 ? (void *)ep->ExceptionRecord->ExceptionInformation[1] : NULL);
    CONTEXT ctx = *ep->ContextRecord; char line[2048]; int n = 0; uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    n += snprintf(line + n, sizeof line - n, "[crash] stack (offsets in xenowerk.exe):");
    for (int i = 0; i < 32 && ctx.Rip; i++) {
        DWORD64 ib, ef; PVOID hd; PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &ib, NULL);
        if (ctx.Rip >= base && ctx.Rip < base + 0x10000000) n += snprintf(line + n, sizeof line - n, " %llx", (unsigned long long)(ctx.Rip - base));
        if (!rf) { ctx.Rip = *(DWORD64 *)ctx.Rsp; ctx.Rsp += 8; }
        else RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, ctx.Rip, rf, &ctx, &hd, &ef, NULL);
    }
    rt_log("%s\n", line);
    return EXCEPTION_EXECUTE_HANDLER;
}

static HANDLE g_main_thread;
static DWORD WINAPI watchdog(LPVOID p) {
    (void)p;
    for (;;) {
        Sleep(rt_genv("XW_WATCHMS") ? (DWORD)atoi(rt_genv("XW_WATCHMS")) : 4000);
        CONTEXT ctx; memset(&ctx, 0, sizeof ctx); ctx.ContextFlags = CONTEXT_FULL;
        SuspendThread(g_main_thread);
        if (GetThreadContext(g_main_thread, &ctx)) {
            char line[2048]; int n = 0; uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
            n += snprintf(line + n, sizeof line - n, "[watch] stack:");
            for (int i = 0; i < 48 && ctx.Rip; i++) {
                DWORD64 ib, ef; PVOID hd; PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &ib, NULL);
                if (ctx.Rip >= base && ctx.Rip < base + 0x10000000) n += snprintf(line + n, sizeof line - n, " %llx", (unsigned long long)(ctx.Rip - base));
                if (!rf) { ctx.Rip = *(DWORD64 *)ctx.Rsp; ctx.Rsp += 8; }
                else RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, ctx.Rip, rf, &ctx, &hd, &ef, NULL);
            }
            rt_log("%s\n", line);
        }
        ResumeThread(g_main_thread);
    }
}

extern const uint32_t rt_modinit_addr, rt_modinit_n;
void rt_objc_init(void);
void rt_app_run(CPU *c);

int main(int argc, char **argv) {
    SetProcessDPIAware();   /* otherwise Windows scales the window bitmap and GetClientRect lies about the GL surface size */
    InitializeCriticalSection(&hlock);
    SetUnhandledExceptionFilter(crash_filter);
    /* RecompLauncher options (all optional; without them nothing below changes): --game <.ipa | Xenowerk.app folder>, --data <dir>, --prepare */
    const char *a_game = NULL, *a_data = NULL; int a_prep = 0, a_launcher = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--prepare")) { a_prep = 1; a_launcher = 1; }
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) { a_game = argv[++i]; a_launcher = 1; }
        else if (!strncmp(argv[i], "--game=", 7)) { a_game = argv[i] + 7; a_launcher = 1; }
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) { a_data = argv[++i]; a_launcher = 1; }
        else if (!strncmp(argv[i], "--data=", 7)) { a_data = argv[i] + 7; a_launcher = 1; }
    }
    if (!a_data && a_launcher && getenv("RECOMPLAUNCHER_DATA")) a_data = getenv("RECOMPLAUNCHER_DATA");
    if (a_data && *a_data) { GetFullPathNameA(a_data, MAX_PATH, g_datadir, NULL); _mkdir(g_datadir); }
    if (a_launcher) {
        char gdir[MAX_PATH], exe0[MAX_PATH]; GetModuleFileNameA(NULL, exe0, MAX_PATH); { char *s0 = strrchr(exe0, '\\'); if (s0) *s0 = 0; }
        snprintf(gdir, sizeof gdir, "%s\\game", g_datadir[0] ? g_datadir : exe0);
        size_t gl = a_game ? strlen(a_game) : 0;
        if (a_game && gl > 4 && !_stricmp(a_game + gl - 4, ".ipa")) {
            _mkdir(gdir);
            int rc = rt_ipa_extract(a_game, gdir, g_appdir, sizeof g_appdir);
            if (rc) return rc;
            if (a_prep) { printf("progress 1 Game ready\n"); return 0; }
        } else if (a_game && *a_game) strcpy(g_appdir, a_game);
        else if (a_prep) { fprintf(stderr, "--prepare needs --game <file.ipa>\n"); return 2; }
        else {   /* started by the launcher after --prepare: the game extracted under the data folder */
            WIN32_FIND_DATAA fd; char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\*.app", gdir); HANDLE h = FindFirstFileA(pat, &fd);
            if (h != INVALID_HANDLE_VALUE) { snprintf(g_appdir, sizeof g_appdir, "%s\\%s", gdir, fd.cFileName); FindClose(h); }
        }
        if (!a_prep) FreeConsole();
    }
    if (g_appdir[0]) { /* set by the launcher options above */ }
    else if (argc > 1 && argv[1][0] != '-') strcpy(g_appdir, argv[1]);
    else {
        char exe[MAX_PATH], cand[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
        char *sl = strrchr(exe, '\\'); if (sl) *sl = 0;
        static const char *const rel[] = { "\\Xenowerk.app", "\\Payload\\Xenowerk.app", "\\..\\Payload\\Xenowerk.app", "\\..\\..\\..\\Payload\\Xenowerk.app", "\\..\\..\\..\\..\\Payload\\Xenowerk.app" };
        g_appdir[0] = 0;
        for (int i = 0; i < 5 && !g_appdir[0]; i++) { snprintf(cand, sizeof cand, "%s%s", exe, rel[i]); if (GetFileAttributesA(cand) != INVALID_FILE_ATTRIBUTES) strcpy(g_appdir, cand); }
    }
    char full[MAX_PATH]; if (g_appdir[0] && GetFullPathNameA(g_appdir, MAX_PATH, full, NULL)) strcpy(g_appdir, full);
    if (!g_appdir[0] || !SetCurrentDirectoryA(g_appdir)) {
        MessageBoxA(NULL, "Xenowerk.app folder not found.\nRun: xenowerk.exe <path to Xenowerk.app>", "Xenowerk", MB_ICONERROR);
        return 1;
    }
    g_logf = fopen("xenowerk_log.txt", "w");
    if (rt_genv("XW_W")) g_gl_w = atoi(rt_genv("XW_W"));
    if (rt_genv("XW_H")) g_gl_h = atoi(rt_genv("XW_H"));
    {   /* points -> pixels: the game sees a 1024x768 pt screen with contentScaleFactor = XW_SCALE (supersampling factor XW_SCALE, default 2) */
        extern int g_scale; g_scale = rt_genv("XW_SCALE") ? atoi(rt_genv("XW_SCALE")) : 2; if (g_scale < 1) g_scale = 1; if (g_scale > 4) g_scale = 4;
        rt_log("[res] game sees %dx%d pt (scale 1); screen framebuffer supersampled x%d = %dx%d px\n", g_gl_w, g_gl_h, g_scale, g_gl_w * g_scale, g_gl_h * g_scale);
    }

    /* map the Mach-O image identity at its vmaddr (the thin armv7 slice is located inside the fat file) */
    FILE *f = fopen("Xenowerk", "rb"); if (!f) { fprintf(stderr, "Xenowerk binary not found\n"); return 1; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(fsz); fread(file, 1, fsz, f); fclose(f);
    uint32_t slice = 0;
    if (*(uint32_t *)file == 0xbebafeca) {          /* fat (big-endian header) */
        uint32_t n = _byteswap_ulong(*(uint32_t *)(file + 4));
        for (uint32_t i = 0; i < n; i++) { uint32_t *a = (uint32_t *)(file + 8 + 20 * i); if (_byteswap_ulong(a[0]) == 12) slice = _byteswap_ulong(a[2]); }
        if (!slice) { fprintf(stderr, "no armv7 slice\n"); return 1; }
    }
    uint8_t *img = file + slice;
    if (!VirtualAlloc((void *)0x10000, IMG_TOP - 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) { fprintf(stderr, "cannot map image\n"); return 1; }
    uint32_t ncmds = *(uint32_t *)(img + 16), off = 28;
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = *(uint32_t *)(img + off), sz = *(uint32_t *)(img + off + 4);
        if (cmd == 1) {
            uint32_t vm = *(uint32_t *)(img + off + 24), fo = *(uint32_t *)(img + off + 32), fs = *(uint32_t *)(img + off + 36);
            if (vm < IMG_TOP && fs) {
                uint32_t skip = vm < 0x10000 ? 0x10000 - vm : 0;
                if (skip < fs) memcpy((void *)(uintptr_t)(vm + skip), img + fo + skip, fs - skip);
            }
        }
        off += sz;
    }
    static uint8_t mirror_src[0xC000];
    { uint32_t off2 = 28; for (uint32_t i = 0; i < ncmds; i++) { uint32_t cmd = *(uint32_t *)(img + off2), sz = *(uint32_t *)(img + off2 + 4);
        if (cmd == 1 && *(uint32_t *)(img + off2 + 24) == 0x4000) memcpy(mirror_src, img + *(uint32_t *)(img + off2 + 32) + 0, 0xC000); off2 += sz; } }
    free(file);
    if (!VirtualAlloc((void *)(uintptr_t)GUEST_HEAP_VA, GUEST_HEAP_SZ, MEM_RESERVE, PAGE_READWRITE)) { fprintf(stderr, "cannot reserve guest heap\n"); return 1; }
    if (!VirtualAlloc((void *)(uintptr_t)GUEST_STACK_VA, GUEST_STACK_SZ, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) { fprintf(stderr, "cannot map guest stack\n"); return 1; }
    /* mirror of the code below 64 KiB (cannot be mapped on Windows) so literal pools read via adr stay reachable */
    if (!VirtualAlloc((void *)0x604000, 0xC000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) { fprintf(stderr, "cannot map code mirror\n"); return 1; }
    memcpy((void *)0x604000, mirror_src, 0xC000);
    rt_objc_init();
    fill_slots();
    if (!create_window()) { fprintf(stderr, "cannot create GL context\n"); return 1; }
    rt_gl_setup();
    if (rt_genv("XW_WATCH")) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
        CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    }
    rt_log("runtime ready: image mapped, window and GL context created\n");

    static CPU cpu;
    cpu.r[13] = GUEST_STACK_VA + GUEST_STACK_SZ - 64;
    for (uint32_t i = 0; i < rt_modinit_n; i++) {
        uint32_t fn = ld32(rt_modinit_addr + 4 * i); if (!fn) continue;
        CPU t = cpu; t.r[14] = 0; arm_dispatch(&t, fn);
    }
    rt_log("static initializers done\n");
    rt_app_run(&cpu);
    return 0;
}
