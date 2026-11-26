/* rt.h - host runtime shared declarations (Windows x64, guest = identity-mapped 32-bit ARM) */
#ifndef RT_H
#define RT_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "armrt.h"

/* argument access: r0-r3 then stack (APCS: 4-byte slots, no 8-byte alignment) */
#define ARG(i) ((i) < 4 ? c->r[i] : ld32(c->r[13] + 4u * ((i) - 4)))
#define RET(v) (c->r[0] = (uint32_t)(v))
#define S(a) ((char *)GP(a))

static inline float u2f(uint32_t u) { union { uint32_t u; float f; } x; x.u = u; return x.f; }
static inline uint32_t f2u(float f) { union { uint32_t u; float f; } x; x.f = f; return x.u; }
static inline double argd(CPU *c, int i) { uint64_t v = (uint64_t)ARG(i) | ((uint64_t)ARG(i + 1) << 32); double d; memcpy(&d, &v, 8); return d; }
static inline void retd(CPU *c, double d) { uint64_t v; memcpy(&v, &d, 8); c->r[0] = (uint32_t)v; c->r[1] = (uint32_t)(v >> 32); }
#define FARG(i) u2f(ARG(i))
#define RETF(x) (c->r[0] = f2u(x))

/* guest heap */
uint32_t g_malloc(uint32_t n);
uint32_t g_calloc(uint32_t n);
void g_free(uint32_t p);
uint32_t g_realloc(uint32_t p, uint32_t n);
uint32_t g_strdup(const char *s);
uint32_t g_usable(uint32_t p);

/* call a guest function from a shim */
uint32_t g_call(CPU *c, uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);

/* printf-family on guest va args */
typedef struct { CPU *c; int idx; uint32_t ptr; } VA;
int g_format(char *out, size_t cap, const char *fmt, VA *va);

/* misc services */
void rt_sleep_us(int us);
const char *rt_genv(const char *name);
void rt_log(const char *fmt, ...);
void rt_pump(void);
void rt_present(CPU *c);
int rt_gl_init(void *hwnd_hdc_unused);
extern HWND g_hwnd;
extern HDC g_hdc;
extern volatile int g_quit;
extern char g_appdir[MAX_PATH];
extern uint32_t g_objc_trace;
void rt_unimpl(CPU *c, const char *name);

/* touch input (set by window proc, consumed by rt_input) */
extern int g_touch_down; extern int g_touch_x, g_touch_y;

#define GUEST_STACK_VA 0x20000000u
#define GUEST_STACK_SZ 0x1000000u
#define GUEST_HEAP_VA  0x30000000u
#define GUEST_HEAP_SZ  0x20000000u
#endif

#define IMG_TOP 0x4d0000u
