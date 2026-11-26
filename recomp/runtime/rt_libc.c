/* rt_libc.c - C library shims for the lifted ARM code (soft-float APCS arguments) */
#include "rt.h"
#include <io.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/timeb.h>

#define SHIM(n) void imp_##n(CPU *c)

/* ------------------------------------------------------------ memory */
SHIM(_malloc) { RET(g_malloc(ARG(0))); }
SHIM(_free) { g_free(ARG(0)); }
SHIM(_realloc) { RET(g_realloc(ARG(0), ARG(1))); }
SHIM(_memcpy) { memcpy(S(ARG(0)), S(ARG(1)), ARG(2)); }
SHIM(_memmove) { memmove(S(ARG(0)), S(ARG(1)), ARG(2)); }
SHIM(_memset) { memset(S(ARG(0)), (int)ARG(1), ARG(2)); }
SHIM(_memcmp) { RET((uint32_t)memcmp(S(ARG(0)), S(ARG(1)), ARG(2))); }
SHIM(_memchr) { void *p = memchr(S(ARG(0)), (int)ARG(1), ARG(2)); RET((uint32_t)(uintptr_t)p); }
SHIM(_mmap) {
    uint32_t len = ARG(1); int fd = (int)ARG(4); uint32_t off = ARG(5);
    uint32_t p = g_malloc(len); if (!p) { RET(0xffffffffu); return; }
    memset(GP(p), 0, len);
    if (fd != -1) { _lseek(fd, (long)off, SEEK_SET); _read(fd, GP(p), len); }
    RET(p);
}
SHIM(_munmap) { g_free(ARG(0)); RET(0); }

/* ------------------------------------------------------------ strings */
SHIM(_strlen) { RET((uint32_t)strlen(S(ARG(0)))); }
SHIM(_strcmp) { RET((uint32_t)strcmp(S(ARG(0)), S(ARG(1)))); }
SHIM(_strncmp) { RET((uint32_t)strncmp(S(ARG(0)), S(ARG(1)), ARG(2))); }
SHIM(_strcasecmp) { RET((uint32_t)_stricmp(S(ARG(0)), S(ARG(1)))); }
SHIM(_strncasecmp) { RET((uint32_t)_strnicmp(S(ARG(0)), S(ARG(1)), ARG(2))); }
SHIM(_strcpy) { strcpy(S(ARG(0)), S(ARG(1))); }
SHIM(_strncpy) { strncpy(S(ARG(0)), S(ARG(1)), ARG(2)); }
SHIM(_strcat) { strcat(S(ARG(0)), S(ARG(1))); }
SHIM(_strchr) { RET((uint32_t)(uintptr_t)strchr(S(ARG(0)), (int)ARG(1))); }
SHIM(_strrchr) { RET((uint32_t)(uintptr_t)strrchr(S(ARG(0)), (int)ARG(1))); }
SHIM(_strstr) { RET((uint32_t)(uintptr_t)strstr(S(ARG(0)), S(ARG(1)))); }
SHIM(_atoi) { RET((uint32_t)atoi(S(ARG(0)))); }
SHIM(_atol) { RET((uint32_t)atol(S(ARG(0)))); }
SHIM(_atof) { retd(c, atof(S(ARG(0)))); }
SHIM(_strtod) { char *e; double d = strtod(S(ARG(0)), &e); if (ARG(1)) st32(ARG(1), (uint32_t)(uintptr_t)e); retd(c, d); }
SHIM(_strtol) { char *e; long v = strtol(S(ARG(0)), &e, (int)ARG(2)); if (ARG(1)) st32(ARG(1), (uint32_t)(uintptr_t)e); RET((uint32_t)v); }
SHIM(___tolower) { RET((uint32_t)tolower((int)ARG(0))); }
SHIM(___toupper) { RET((uint32_t)toupper((int)ARG(0))); }

typedef struct { CPU *c; uint32_t fn; } QCtx;
static int qcmp(void *ctx, const void *a, const void *b) {
    QCtx *q = ctx; return (int)g_call(q->c, q->fn, (uint32_t)(uintptr_t)a, (uint32_t)(uintptr_t)b, 0, 0);
}
SHIM(_qsort) { QCtx q = { c, ARG(3) }; qsort_s(S(ARG(0)), ARG(1), ARG(2), qcmp, &q); }
SHIM(_rand) { RET((uint32_t)rand()); }
SHIM(_srand) { srand(ARG(0)); }

/* ------------------------------------------------------------ compiler helpers */
SHIM(___divsi3) { int32_t b = (int32_t)ARG(1); RET(b ? (uint32_t)((int32_t)ARG(0) / b) : 0); }
SHIM(___modsi3) { int32_t b = (int32_t)ARG(1); RET(b ? (uint32_t)((int32_t)ARG(0) % b) : 0); }
SHIM(___udivsi3) { uint32_t b = ARG(1); RET(b ? ARG(0) / b : 0); }
SHIM(___umodsi3) { uint32_t b = ARG(1); RET(b ? ARG(0) % b : 0); }
SHIM(___floatundidf) { uint64_t v = (uint64_t)ARG(0) | ((uint64_t)ARG(1) << 32); retd(c, (double)v); }
SHIM(___error) { static uint32_t cell; if (!cell) cell = g_malloc(16); RET(cell); }

/* ------------------------------------------------------------ math (double args in r0:r1 / r2:r3) */
#define M1D(n) SHIM(_##n) { retd(c, n(argd(c, 0))); }
#define M2D(n) SHIM(_##n) { retd(c, n(argd(c, 0), argd(c, 2))); }
#define M1F(n) SHIM(_##n) { RETF(n(FARG(0))); }
M1D(sin) M1D(cos) M1D(tan) M1D(asin) M1D(acos) M1D(atan) M1D(exp) M1D(log) M1D(log2) M1D(floor) M1D(ceil)
M2D(atan2) M2D(pow) M2D(fmod) M2D(fmax) M2D(fmin)
M1F(sinf) M1F(cosf) M1F(ceilf) M1F(floorf) M1F(roundf)
SHIM(_atan2f) { RETF(atan2f(FARG(0), FARG(1))); }
SHIM(_fesetround) { RET(0); }

/* ------------------------------------------------------------ printf family */
static uint32_t va_next(VA *v) {
    if (v->ptr) { uint32_t x = ld32(v->ptr); v->ptr += 4; return x; }
    CPU *c = v->c; int i = v->idx++; return ARG(i);
}
int g_format(char *out, size_t cap, const char *fmt, VA *va) {
    size_t n = 0; char tmp[512];
#define PUT(ch) do { if (n + 1 < cap) out[n] = (ch); n++; } while (0)
    for (const char *p = fmt; *p; ) {
        if (*p != '%') { PUT(*p++); continue; }
        char spec[32]; int sl = 0; spec[sl++] = *p++;
        while (*p && strchr("-+ #0", *p)) spec[sl++] = *p++;
        if (*p == '*') { sl += sprintf(spec + sl, "%d", (int)va_next(va)); p++; } else while (isdigit((unsigned char)*p)) spec[sl++] = *p++;
        if (*p == '.') { spec[sl++] = *p++; if (*p == '*') { sl += sprintf(spec + sl, "%d", (int)va_next(va)); p++; } else while (isdigit((unsigned char)*p)) spec[sl++] = *p++; }
        int ll = 0, h = 0;
        while (*p && strchr("hlLzjtq", *p)) { if (*p == 'l' || *p == 'q' || *p == 'j') ll++; if (*p == 'h') h++; p++; }
        char cv = *p ? *p++ : 0; spec[sl] = 0;
        int w = 0;
        switch (cv) {
        case 'd': case 'i': {
            if (ll >= 2) { uint64_t lo = va_next(va), hi = va_next(va); strcat(spec, "lld"); w = snprintf(tmp, sizeof tmp, spec, (long long)(lo | (hi << 32))); }
            else { int32_t v = (int32_t)va_next(va); if (h == 1) v = (int16_t)v; else if (h >= 2) v = (int8_t)v; strcat(spec, "d"); w = snprintf(tmp, sizeof tmp, spec, v); }
            break; }
        case 'u': case 'x': case 'X': case 'o': {
            char t[2] = { cv, 0 };
            if (ll >= 2) { uint64_t lo = va_next(va), hi = va_next(va); strcat(spec, "ll"); strcat(spec, t); w = snprintf(tmp, sizeof tmp, spec, (unsigned long long)(lo | (hi << 32))); }
            else { uint32_t v = va_next(va); if (h == 1) v &= 0xffff; else if (h >= 2) v &= 0xff; strcat(spec, t); w = snprintf(tmp, sizeof tmp, spec, v); }
            break; }
        case 'c': { strcat(spec, "c"); w = snprintf(tmp, sizeof tmp, spec, (int)va_next(va)); break; }
        case 's': { uint32_t a = va_next(va); strcat(spec, "s"); w = snprintf(tmp, sizeof tmp, spec, a ? S(a) : "(null)"); break; }
        case 'p': { w = snprintf(tmp, sizeof tmp, "0x%x", va_next(va)); break; }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            uint64_t lo = va_next(va), hi = va_next(va); uint64_t b = lo | (hi << 32); double d; memcpy(&d, &b, 8);
            char t[2] = { cv, 0 }; strcat(spec, t); w = snprintf(tmp, sizeof tmp, spec, d); break; }
        case '%': PUT('%'); continue;
        case 'n': (void)va_next(va); continue;
        default: PUT('%'); if (cv) PUT(cv); continue;
        }
        for (int i = 0; i < w && i < (int)sizeof tmp - 1; i++) PUT(tmp[i]);
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
    return (int)n;
}
#define VAFROM(c, first) VA va = { c, first, 0 }
SHIM(_sprintf) { VAFROM(c, 2); RET(g_format(S(ARG(0)), 0x7fffffff, S(ARG(1)), &va)); }
SHIM(_snprintf) { VAFROM(c, 3); RET(g_format(S(ARG(0)), ARG(1), S(ARG(2)), &va)); }
SHIM(_vsnprintf) { VA va = { c, 0, ARG(3) }; RET(g_format(S(ARG(0)), ARG(1), S(ARG(2)), &va)); }

/* ------------------------------------------------------------ stdio */
#define MAXF 512
static FILE *files[MAXF];
static unsigned char is_front[MAXF];

/* Player name: the profile name chosen in the launcher (XW_PLAYER) replaces the default "Jogador"/"Player" (string id "ui_player") in every
 * language table of frontend.bar. The text tables are stored uncompressed, so each node is rewritten in place with the same byte length
 * (compact syntax + space padding) and overlaid on whatever the game reads from those file offsets. */
static struct { long off; int len; char *bytes; } name_patch[16]; static int npatch, patch_built;
static void build_name_patches(const char *path) {
    if (patch_built) return; patch_built = 1;
    const char *nm = rt_genv("XW_PLAYER"); if (!nm || !*nm) return;
    FILE *f = fopen(path, "rb"); if (!f) return;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)sz); if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return; } fclose(f);
    static const char key[] = "string id = \"ui_player\";"; size_t kl = sizeof key - 1; char nmc[32]; int ni = 0;
    for (const char *q = nm; *q && ni < 13; q++) if (*q != '"' && *q != '\\' && *q != ';' && (unsigned char)*q >= 32) nmc[ni++] = *q;
    nmc[ni] = 0; if (!ni) { free(b); return; }
    for (long i = 0; i + (long)kl < sz && npatch < 16; i++) {
        if (b[i] != 's' || memcmp(b + i, key, kl)) continue;
        long e = i + (long)kl; long lim = e + 60 < sz ? e + 60 : sz; long d = -1;
        for (long j = e; j + 15 < lim; j++) if (!memcmp(b + j, "string data = \"", 15)) { d = j + 15; break; }
        if (d < 0) continue;
        long q = d; while (q < lim && b[q] != '"') q++; if (q + 1 >= lim || b[q + 1] != ';') continue;
        long L = q + 2 - i;                                   /* original node text: string id = "ui_player";\r\n\t\tstring data = "...";  */
        char rep[160]; int rl = snprintf(rep, sizeof rep, "string id=\"ui_player\";\r\nstring data=\"%s\";", nmc);
        if (rl > L) continue;
        char *pb = (char *)malloc((size_t)L); memcpy(pb, rep, (size_t)rl); memset(pb + rl, ' ', (size_t)(L - rl));
        name_patch[npatch].off = i; name_patch[npatch].len = (int)L; name_patch[npatch].bytes = pb; npatch++;
        i += L;
    }
    free(b); rt_log("[player] name \"%s\" patched in %d text table(s)\n", nmc, npatch);
}
static void apply_name_patches(char *buf, long pos, long n) {
    for (int k = 0; k < npatch; k++) {
        long a = name_patch[k].off, e = a + name_patch[k].len; long lo = a > pos ? a : pos, hi = e < pos + n ? e : pos + n;
        if (lo < hi) memcpy(buf + (lo - pos), name_patch[k].bytes + (lo - a), (size_t)(hi - lo));
    }
}
static FILE *hf(uint32_t h) {
    if (h == 0x7f000000) return stdin; if (h == 0x7f000001) return stdout; if (h == 0x7f000002) return stderr;
    if (h >= 0x7f000010 && h < 0x7f000010 + MAXF) return files[h - 0x7f000010];
    return NULL;
}
SHIM(_fopen) {
    char mode[16]; strncpy(mode, S(ARG(1)), 10); mode[10] = 0;
    if (!strchr(mode, 'b') && !strchr(mode, 't')) strcat(mode, "b");
    FILE *f = fopen(S(ARG(0)), mode);
    if (rt_genv("XW_FILES")) rt_log("[fopen] %s %s -> %s\n", S(ARG(0)), mode, f ? "ok" : "FAIL");
    if (!f) { RET(0); return; }
    for (int i = 0; i < MAXF; i++) if (!files[i]) {
        files[i] = f; is_front[i] = 0;
        { const char *p = S(ARG(0)); size_t l = strlen(p); if (l >= 12 && !_stricmp(p + l - 12, "frontend.bar") && !strchr(mode, 'w')) { is_front[i] = 1; build_name_patches(p); } }
        RET(0x7f000010 + i); return; }
    fclose(f); RET(0);
}
SHIM(_fclose) { FILE *f = hf(ARG(0)); if (f && ARG(0) >= 0x7f000010) { fclose(f); files[ARG(0) - 0x7f000010] = NULL; } RET(0); }
unsigned long long g_cnt_read_bytes; int g_cnt_read;
SHIM(_fread) {
    g_cnt_read++; g_cnt_read_bytes += (unsigned long long)ARG(1) * ARG(2); FILE *f = hf(ARG(3));
    long pos = (f && ARG(3) >= 0x7f000010 && is_front[ARG(3) - 0x7f000010] && npatch) ? ftell(f) : -1;
    uint32_t n = f ? (uint32_t)fread(S(ARG(0)), ARG(1), ARG(2), f) : 0;
    if (pos >= 0 && n) apply_name_patches(S(ARG(0)), pos, (long)n * (long)ARG(1));
    RET(n);
}
SHIM(_fwrite) { FILE *f = hf(ARG(3)); RET(f ? (uint32_t)fwrite(S(ARG(0)), ARG(1), ARG(2), f) : 0); }
SHIM(_fseek) { FILE *f = hf(ARG(0)); RET(f ? (uint32_t)fseek(f, (long)ARG(1), (int)ARG(2)) : (uint32_t)-1); }
SHIM(_ftell) { FILE *f = hf(ARG(0)); RET(f ? (uint32_t)ftell(f) : (uint32_t)-1); }
SHIM(_rewind) { FILE *f = hf(ARG(0)); if (f) rewind(f); }
SHIM(_feof) { FILE *f = hf(ARG(0)); RET(f ? (uint32_t)feof(f) : 1); }
SHIM(_clearerr) { FILE *f = hf(ARG(0)); if (f) clearerr(f); }
SHIM(_fflush) { FILE *f = hf(ARG(0)); if (f) fflush(f); RET(0); }
SHIM(_fgetc) { FILE *f = hf(ARG(0)); RET(f ? (uint32_t)fgetc(f) : (uint32_t)-1); }
SHIM(_fgets) { FILE *f = hf(ARG(2)); char *r = f ? fgets(S(ARG(0)), (int)ARG(1), f) : NULL; RET(r ? ARG(0) : 0); }
SHIM(_fputs) { FILE *f = hf(ARG(1)); if (f) fputs(S(ARG(0)), f); RET(0); }
SHIM(_puts) { puts(S(ARG(0))); RET(0); }
SHIM(_perror) { rt_log("perror: %s\n", S(ARG(0))); }
SHIM(_remove) { RET((uint32_t)remove(S(ARG(0)))); }
SHIM(_chdir) { RET(SetCurrentDirectoryA(S(ARG(0))) ? 0 : (uint32_t)-1); }
SHIM(_fprintf) { VAFROM(c, 2); char buf[4096]; g_format(buf, sizeof buf, S(ARG(1)), &va); FILE *f = hf(ARG(0)); if (f) fputs(buf, f); RET(0); }
SHIM(_printf) { VAFROM(c, 1); char buf[4096]; g_format(buf, sizeof buf, S(ARG(0)), &va); fputs(buf, stdout); RET(0); }

/* scanf family: convert piecewise with the host scanf */
static int scan_one(const char **sp, FILE *f, const char **fp, VA *va) {
    const char *fmt = *fp; char spec[32]; int sl = 0, ll = 0, skip = 0;
    spec[sl++] = *fmt++;
    if (*fmt == '*') { skip = 1; fmt++; }
    while (isdigit((unsigned char)*fmt)) spec[sl++] = *fmt++;
    while (*fmt == 'l' || *fmt == 'h') { if (*fmt == 'l') ll++; fmt++; }
    char cv = *fmt++; *fp = fmt;
    union { long long i; double d; char buf[256]; } t; int consumed = 0, got = 0; spec[sl] = 0;
    char fmt2[48];
    if (cv == 'd' || cv == 'i' || cv == 'u' || cv == 'x') { snprintf(fmt2, sizeof fmt2, "%s%c%%n", spec, cv); int v; got = f ? fscanf(f, fmt2, &v, &consumed) : sscanf(*sp, fmt2, &v, &consumed); if (got < 1) return 0; if (!skip) st32(va_next(va), (uint32_t)v); }
    else if (cv == 'f' || cv == 'e' || cv == 'g') { snprintf(fmt2, sizeof fmt2, "%slf%%n", spec); got = f ? fscanf(f, fmt2, &t.d, &consumed) : sscanf(*sp, fmt2, &t.d, &consumed); if (got < 1) return 0;
        if (!skip) { uint32_t a = va_next(va); if (ll) memcpy(GP(a), &t.d, 8); else { float fl = (float)t.d; memcpy(GP(a), &fl, 4); } } }
    else if (cv == 's') { snprintf(fmt2, sizeof fmt2, "%ss%%n", spec); got = f ? fscanf(f, fmt2, t.buf, &consumed) : sscanf(*sp, fmt2, t.buf, &consumed); if (got < 1) return 0; if (!skip) strcpy(S(va_next(va)), t.buf); }
    else if (cv == 'c') { snprintf(fmt2, sizeof fmt2, "%sc%%n", spec); got = f ? fscanf(f, fmt2, t.buf, &consumed) : sscanf(*sp, fmt2, t.buf, &consumed); if (got < 1) return 0; if (!skip) *S(va_next(va)) = t.buf[0]; }
    else return -1;
    if (!f) *sp += consumed;
    return skip ? 2 : 1;
}
static int g_scan(const char *str, FILE *f, const char *fmt, VA *va) {
    int count = 0; const char *sp = str;
    while (*fmt) {
        if (isspace((unsigned char)*fmt)) { fmt++; if (f) { int ch; while ((ch = fgetc(f)) != EOF && isspace(ch)) ; if (ch != EOF) ungetc(ch, f); } else while (isspace((unsigned char)*sp)) sp++; continue; }
        if (*fmt == '%') { if (fmt[1] == '%') { fmt += 2; continue; } int r = scan_one(&sp, f, &fmt, va); if (r <= 0) break; if (r == 1) count++; continue; }
        if (f) { int ch = fgetc(f); if (ch != *fmt) { if (ch != EOF) ungetc(ch, f); break; } } else if (*sp != *fmt) break; else sp++;
        fmt++;
    }
    return count;
}
SHIM(_sscanf) { VAFROM(c, 2); RET((uint32_t)g_scan(S(ARG(0)), NULL, S(ARG(1)), &va)); }
SHIM(_fscanf) { VAFROM(c, 2); FILE *f = hf(ARG(0)); RET(f ? (uint32_t)g_scan(NULL, f, S(ARG(1)), &va) : (uint32_t)-1); }

/* low-level file io (iOS flag values) */
SHIM(_open) {
    uint32_t fl = ARG(1); int acc = fl & 3, h = _O_BINARY;
    h |= acc == 0 ? _O_RDONLY : acc == 1 ? _O_WRONLY : _O_RDWR;
    if (fl & 0x200) h |= _O_CREAT; if (fl & 0x400) h |= _O_TRUNC; if (fl & 8) h |= _O_APPEND;
    { int r = _open(S(ARG(0)), h, _S_IREAD | _S_IWRITE); if (rt_genv("XW_FILES")) rt_log("[open] %s -> %d\n", S(ARG(0)), r); RET((uint32_t)r); }
}
SHIM(_close) { RET((uint32_t)_close((int)ARG(0))); }
/* Darwin lseek(int, off_t(64-bit), int): APCS passes the offset in r1:r2 (no even-register alignment), whence in r3; result is r0:r1 */
SHIM(_lseek) {
    int64_t off = (int64_t)(((uint64_t)ARG(2) << 32) | ARG(1));
    int64_t r = _lseeki64((int)ARG(0), off, (int)ARG(3));
    c->r[0] = (uint32_t)r; c->r[1] = (uint32_t)((uint64_t)r >> 32);
}

/* ------------------------------------------------------------ time */
SHIM(_time) { uint32_t t = (uint32_t)time(NULL); if (ARG(0)) st32(ARG(0), t); RET(t); }
SHIM(_gettimeofday) {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    uint64_t t = (((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000ull; t /= 10;
    if (ARG(0)) { st32(ARG(0), (uint32_t)(t / 1000000)); st32(ARG(0) + 4, (uint32_t)(t % 1000000)); }
    RET(0);
}
SHIM(_localtime) {
    static uint32_t buf; if (!buf) buf = g_malloc(64);
    time_t t = (time_t)ld32(ARG(0)); struct tm tm; localtime_s(&tm, &t);
    int *w = (int *)GP(buf); w[0] = tm.tm_sec; w[1] = tm.tm_min; w[2] = tm.tm_hour; w[3] = tm.tm_mday; w[4] = tm.tm_mon; w[5] = tm.tm_year; w[6] = tm.tm_wday; w[7] = tm.tm_yday; w[8] = tm.tm_isdst; w[9] = 0; w[10] = 0;
    RET(buf);
}
SHIM(_sleep) { Sleep(ARG(0) * 1000); RET(0); }
SHIM(_usleep) { Sleep(ARG(0) / 1000); RET(0); }
SHIM(_sysctlbyname) { RET((uint32_t)-1); }
SHIM(_exit) { rt_log("[exit(%d)]\n", (int)ARG(0)); fflush(NULL); ExitProcess(ARG(0)); }
#include <setjmp.h>
typedef struct { uint32_t jb, regs[11]; jmp_buf buf; } XwJB;
static XwJB g_jbt[64]; static int g_jbn;
jmp_buf *xw_jb_slot(CPU *c) {
    uint32_t jb = c->r[0]; XwJB *e = NULL; int i;
    for (i = 0; i < 64; i++) if (g_jbt[i].jb == jb) { e = &g_jbt[i]; break; }
    if (!e) e = &g_jbt[g_jbn++ & 63];
    e->jb = jb; for (i = 0; i < 8; i++) e->regs[i] = c->r[4 + i]; e->regs[8] = c->r[13]; e->regs[9] = c->r[14];
    return &e->buf;
}
SHIM(_setjmp) { RET(0); } /* fallback; lifted call sites use XW_SETJMP */
SHIM(_longjmp) {
    uint32_t jb = ARG(0), v = ARG(1); int i;
    for (i = 0; i < 64; i++) if (g_jbt[i].jb == jb) {
        XwJB *e = &g_jbt[i]; for (int k = 0; k < 8; k++) c->r[4 + k] = e->regs[k]; c->r[13] = e->regs[8]; c->r[14] = e->regs[9];
        c->r[0] = v ? v : 1; longjmp(e->buf, 1);
    }
    arm_trap(c->r[14], "longjmp without matching setjmp");
}

