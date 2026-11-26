/* rt_libc2.c - remaining libc / libSystem shims for Xenowerk, C++ ABI, pthread no-ops, zlib, audio and texture stubs */
#include "rt.h"
#include <io.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <wchar.h>

#define SHIM(n) void imp_##n(CPU *c)

SHIM(_calloc) { uint32_t n = ARG(0) * ARG(1), p = g_malloc(n); if (p) memset(GP(p), 0, n); RET(p); }
SHIM(_bzero) { memset(S(ARG(0)), 0, ARG(1)); }
SHIM(___memcpy_chk) { memcpy(S(ARG(0)), S(ARG(1)), ARG(2)); RET(ARG(0)); }
SHIM(___memset_chk) { memset(S(ARG(0)), (int)ARG(1), ARG(2)); RET(ARG(0)); }
SHIM(___strcat_chk) { strcat(S(ARG(0)), S(ARG(1))); RET(ARG(0)); }
SHIM(_memset_pattern16) { uint8_t *d = (uint8_t *)S(ARG(0)); const uint8_t *p = (const uint8_t *)S(ARG(1)); for (uint32_t i = 0; i < ARG(2); i++) d[i] = p[i & 15]; }
SHIM(___stack_chk_fail) { arm_trap(c->r[14], "stack check failed"); }
SHIM(_abort) { arm_trap(c->r[14], "abort()"); }
SHIM(_strerror) { static uint32_t p; if (!p) p = g_strdup("error"); RET(p); }
SHIM(_strtok) { static char *save; (void)save; static char *st; char *r = strtok_s(ARG(0) ? S(ARG(0)) : NULL, S(ARG(1)), &st); RET((uint32_t)(uintptr_t)r); }
SHIM(_strtoul) { char *e; unsigned long v = strtoul(S(ARG(0)), &e, (int)ARG(2)); if (ARG(1)) st32(ARG(1), (uint32_t)(uintptr_t)e); RET((uint32_t)v); }
SHIM(_wcscpy) { uint32_t d = ARG(0), s = ARG(1); for (;; d += 4, s += 4) { uint32_t ch = ld32(s); st32(d, ch); if (!ch) break; } RET(ARG(0)); }
SHIM(_getenv) { RET(0); }
SHIM(_getpid) { RET(1000); }
SHIM(_getuid) { RET(501); }
SHIM(_dlsym) { RET(0); }
SHIM(_pthread_self) { RET(1); }
SHIM(_pthread_mutexattr_destroy) { RET(0); }
SHIM(_pthread_mutex_lock) { RET(0); }
SHIM(_pthread_mutex_unlock) { RET(0); }
SHIM(_srand48) { srand(ARG(0)); }
SHIM(_lrand48) { RET(((uint32_t)rand() << 15 ^ (uint32_t)rand()) & 0x7fffffffu); }
SHIM(_mrand48) { RET(((uint32_t)rand() << 16) ^ (uint32_t)rand()); }
SHIM(_inet_ntoa) { static uint32_t p; if (!p) p = g_malloc(32); uint32_t a = ARG(0); sprintf(S(p), "%u.%u.%u.%u", a & 255, (a >> 8) & 255, (a >> 16) & 255, a >> 24); RET(p); }
SHIM(_getifaddrs) { RET((uint32_t)-1); }
SHIM(_freeifaddrs) { (void)c; }
SHIM(_socket) { RET((uint32_t)-1); }
SHIM(_connect) { RET((uint32_t)-1); }
SHIM(_select) { RET(0); }
SHIM(_mach_absolute_time) {
    LARGE_INTEGER n, f; QueryPerformanceCounter(&n); QueryPerformanceFrequency(&f);
    uint64_t ns = (uint64_t)((double)n.QuadPart * 1e9 / (double)f.QuadPart); c->r[0] = (uint32_t)ns; c->r[1] = (uint32_t)(ns >> 32);
}
SHIM(_mach_timebase_info) { st32(ARG(0), 1); st32(ARG(0) + 4, 1); RET(0); }
SHIM(_gmtime_r) {
    time_t t = (time_t)ld32(ARG(0)); struct tm tm; gmtime_s(&tm, &t); int *w = (int *)GP(ARG(1));
    w[0] = tm.tm_sec; w[1] = tm.tm_min; w[2] = tm.tm_hour; w[3] = tm.tm_mday; w[4] = tm.tm_mon; w[5] = tm.tm_year; w[6] = tm.tm_wday; w[7] = tm.tm_yday; w[8] = 0; w[9] = 0; w[10] = 0; RET(ARG(1));
}
SHIM(___maskrune) {
    int ch = (int)ARG(0); uint32_t m = ARG(1), r = 0;
    if (ch < 0 || ch > 255) { RET(0); return; }
    if (isalpha(ch)) r |= 0x100; if (iscntrl(ch)) r |= 0x200; if (isdigit(ch)) r |= 0x400; if (isgraph(ch)) r |= 0x800; if (islower(ch)) r |= 0x1000;
    if (ispunct(ch)) r |= 0x2000; if (isspace(ch)) r |= 0x4000; if (isupper(ch)) r |= 0x8000; if (isxdigit(ch)) r |= 0x10000; if (ch == ' ' || ch == '\t') r |= 0x20000;
    if (isprint(ch)) r |= 0x40000; RET(r & m);
}
/* 64-bit helpers: arguments in r0:r1 / r2:r3 */
static uint64_t a64(CPU *c, int i) { return (uint64_t)ARG(i) | ((uint64_t)ARG(i + 1) << 32); }
static void r64(CPU *c, uint64_t v) { c->r[0] = (uint32_t)v; c->r[1] = (uint32_t)(v >> 32); }
SHIM(___divdi3) { int64_t b = (int64_t)a64(c, 2); r64(c, b ? (uint64_t)((int64_t)a64(c, 0) / b) : 0); }
SHIM(___udivdi3) { uint64_t b = a64(c, 2); r64(c, b ? a64(c, 0) / b : 0); }
SHIM(___umoddi3) { uint64_t b = a64(c, 2); r64(c, b ? a64(c, 0) % b : 0); }
SHIM(___floatdisf) { RETF((float)(int64_t)a64(c, 0)); }
SHIM(___floatundisf) { RETF((float)a64(c, 0)); }
SHIM(___sincosf_stret) { uint32_t out = c->r[0]; float x = FARG(1); st32(out, f2u(sinf(x))); st32(out + 4, f2u(cosf(x))); } /* 8-byte struct return: hidden pointer in r0, angle in r1 */
#define M1F2(n) SHIM(_##n) { RETF(n(FARG(0))); }
M1F2(tanf) M1F2(asinf) M1F2(acosf) M1F2(atanf) M1F2(expf)
SHIM(_fmodf) { RETF(fmodf(FARG(0), FARG(1))); }
SHIM(_powf) { RETF(powf(FARG(0), FARG(1))); }
SHIM(_ferror) { RET(0); }
SHIM(_vfprintf) { extern int g_format(char *, size_t, const char *, VA *); VA va = { c, 0, ARG(2) }; char buf[4096]; g_format(buf, sizeof buf, S(ARG(1)), &va); if (ARG(0) >= 0x7f000001 && ARG(0) <= 0x7f000002) rt_log("%s", buf); RET(0); }
SHIM(_vswprintf) { RET(0); }
SHIM(_asprintf) { VA va = { c, 2, 0 }; char buf[4096]; int n = g_format(buf, sizeof buf, S(ARG(1)), &va); uint32_t p = g_strdup(buf); st32(ARG(0), p); RET(n); }
SHIM(_rename) { RET((uint32_t)rename(S(ARG(0)), S(ARG(1)))); }
SHIM(_read) { int r = _read((int)ARG(0), S(ARG(1)), ARG(2)); RET((uint32_t)r); }
SHIM(_write) { int fd = (int)ARG(0); if (fd == 1 || fd == 2) { rt_log("%.*s", (int)ARG(2), S(ARG(1))); RET(ARG(2)); return; } RET((uint32_t)_write(fd, S(ARG(1)), ARG(2))); }
SHIM(_fstat) {
    struct _stat64 st; memset(GP(ARG(1)), 0, 128);
    if (_fstat64((int)ARG(0), &st)) { RET((uint32_t)-1); return; }
    st32(ARG(1) + 4, (st.st_mode & _S_IFDIR) ? 0x41ed : 0x81a4);   /* mode (u16) */
    uint64_t sz = (uint64_t)st.st_size; st32(ARG(1) + 60, (uint32_t)sz); st32(ARG(1) + 64, (uint32_t)(sz >> 32));
    RET(0);
}
SHIM(_hash_create) { RET(g_calloc(64)); }
SHIM(_hash_search) { RET(0); }

/* ---- C++ ABI ---- */
SHIM(__Znwm) { uint32_t p = g_malloc(ARG(0)); if (!p) arm_trap(0, "operator new: out of memory"); RET(p); }
SHIM(__Znam) { uint32_t p = g_malloc(ARG(0)); if (!p) arm_trap(0, "operator new[]: out of memory"); RET(p); }
SHIM(__ZdlPv) { g_free(ARG(0)); }
SHIM(__ZdaPv) { g_free(ARG(0)); }
SHIM(___cxa_atexit) { RET(0); }
SHIM(___cxa_guard_acquire) { RET(ld8(ARG(0)) ? 0 : 1); }
SHIM(___cxa_guard_release) { st8(ARG(0), 1); }
SHIM(___cxa_guard_abort) { (void)c; }
SHIM(___cxa_allocate_exception) { RET(g_calloc(ARG(0) + 16) + 16); }
SHIM(___cxa_throw) { rt_log("[C++ throw] type=%x lr=%x\n", ARG(1), c->r[14]); arm_trap(c->r[14], "C++ exception thrown (unsupported)"); }
SHIM(___cxa_begin_catch) { arm_trap(c->r[14], "C++ exception caught (unsupported)"); }
SHIM(___cxa_end_catch) { (void)c; }
SHIM(__ZSt9terminatev) { arm_trap(c->r[14], "std::terminate"); }
SHIM(__Unwind_SjLj_Register) { (void)c; }
SHIM(__Unwind_SjLj_Unregister) { (void)c; }
SHIM(__Unwind_SjLj_Resume) { arm_trap(c->r[14], "unwind resume"); }

/* ---- zlib (implemented in rt_zlib.c) ---- */
/* ---- audio: no backend yet, AudioToolbox calls fail with paramErr ---- */

SHIM(_pthread_mutexattr_init) { RET(0); }
SHIM(_pthread_mutexattr_settype) { RET(0); }
SHIM(_pthread_mutex_init) { RET(0); }
SHIM(_pthread_mutex_destroy) { RET(0); }
SHIM(_CFUUIDCreate) { RET(g_calloc(16)); }
SHIM(_CFUUIDCreateString) { static int n; char b[48]; sprintf(b, "00000000-0000-4000-8000-%012d", ++n); extern uint32_t rt_mkstr(const char *); RET(rt_mkstr(b)); }
