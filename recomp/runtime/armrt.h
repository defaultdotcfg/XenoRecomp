/* armrt.h - runtime header for statically-lifted ARMv6 (ARM mode + VFPv2) code.
 * Guest addresses are 32-bit and identity-mapped into the host process
 * (the host must map the Mach-O image at its original vmaddr and keep all
 * guest allocations below 4 GiB). */
#ifndef ARMRT_H
#define ARMRT_H
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef struct CPU {
    uint32_t r[16];
    uint32_t n, z, c, v;          /* APSR flags, each 0/1 */
    uint32_t fn, fz, fc, fv;      /* FPSCR compare flags */
    uint32_t sc;                  /* scratch: shifter carry */
    uint32_t fpscr_other;
    uint32_t tls;                 /* c13 user r/o thread id */
    uint32_t qc;                  /* sticky saturation flag */
    union { float s[64]; double d[32]; uint32_t u[64]; uint64_t q[32]; uint8_t b[256]; } f;   /* d0-d31 / q0-q15; s0-s31 alias d0-d15 */
} CPU;

typedef void (*guest_fn)(CPU *);

#define R(n) (c->r[n])
#define GP(a) ((void *)(uintptr_t)(uint32_t)(a))

static inline uint32_t ld32(uint32_t a){ uint32_t v; memcpy(&v, GP(a), 4); return v; }
static inline uint32_t ld16(uint32_t a){ uint16_t v; memcpy(&v, GP(a), 2); return v; }
static inline uint32_t ld8 (uint32_t a){ return *(uint8_t *)GP(a); }
static inline uint32_t lds16(uint32_t a){ int16_t v; memcpy(&v, GP(a), 2); return (uint32_t)(int32_t)v; }
static inline uint32_t lds8 (uint32_t a){ return (uint32_t)(int32_t)*(int8_t *)GP(a); }
static inline void st32(uint32_t a, uint32_t v){ memcpy(GP(a), &v, 4); }
static inline void st16(uint32_t a, uint32_t v){ uint16_t x = (uint16_t)v; memcpy(GP(a), &x, 2); }
static inline void st8 (uint32_t a, uint32_t v){ *(uint8_t *)GP(a) = (uint8_t)v; }
static inline uint64_t ld64(uint32_t a){ uint64_t v; memcpy(&v, GP(a), 8); return v; }
static inline void st64(uint32_t a, uint64_t v){ memcpy(GP(a), &v, 8); }

static inline uint32_t ror32(uint32_t v, uint32_t n){ n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }

/* a + b + ci with NZCV */
static inline uint32_t adc_s(CPU *c, uint32_t a, uint32_t b, uint32_t ci){
    uint64_t r = (uint64_t)a + b + ci; uint32_t res = (uint32_t)r;
    c->n = res >> 31; c->z = (res == 0); c->c = (uint32_t)(r >> 32);
    c->v = ((a ^ res) & (b ^ res)) >> 31;
    return res;
}
#define SET_NZ(x) do { uint32_t _v = (x); c->n = _v >> 31; c->z = (_v == 0); } while (0)

/* register-specified shifts. type: 0 LSL 1 LSR 2 ASR 3 ROR. Sets c->sc. */
static inline uint32_t sh_reg(CPU *c, int type, uint32_t v, uint32_t amt){
    amt &= 0xff; c->sc = c->c;
    if (amt == 0) return v;
    switch (type) {
    case 0: if (amt < 32) { c->sc = (v >> (32 - amt)) & 1; return v << amt; }
            c->sc = (amt == 32) ? (v & 1) : 0; return 0;
    case 1: if (amt < 32) { c->sc = (v >> (amt - 1)) & 1; return v >> amt; }
            c->sc = (amt == 32) ? (v >> 31) : 0; return 0;
    case 2: if (amt < 32) { c->sc = (v >> (amt - 1)) & 1; return (uint32_t)((int32_t)v >> amt); }
            c->sc = v >> 31; return (uint32_t)((int32_t)v >> 31);
    default: amt &= 31; if (amt == 0) { c->sc = v >> 31; return v; }
            c->sc = (v >> (amt - 1)) & 1; return ror32(v, amt);
    }
}

static inline uint32_t bswap32_(uint32_t v){ return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24); }
static inline uint32_t rev16_(uint32_t v){ return ((v >> 8) & 0x00ff00ff) | ((v << 8) & 0xff00ff00); }
static inline uint32_t revsh_(uint32_t v){ return (uint32_t)(int32_t)(int16_t)(((v >> 8) & 0xff) | ((v & 0xff) << 8)); }
static inline uint32_t rbit_(uint32_t v){ v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1); v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2); v = ((v >> 4) & 0x0f0f0f0fu) | ((v & 0x0f0f0f0fu) << 4); return bswap32_(v); }
static inline uint32_t clz_(uint32_t v){ uint32_t n = 0; if (!v) return 32; while (!(v & 0x80000000u)) { v <<= 1; n++; } return n; }

static inline uint32_t ssat_(CPU *c, uint32_t v, int bits){ int64_t x = (int32_t)v, hi = (1ll << (bits - 1)) - 1, lo = -(1ll << (bits - 1)); if (x > hi) { c->qc = 1; return (uint32_t)hi; } if (x < lo) { c->qc = 1; return (uint32_t)lo; } return v; }
static inline uint32_t usat_(CPU *c, uint32_t v, int bits){ int64_t x = (int32_t)v, hi = (1ll << bits) - 1; if (x > hi) { c->qc = 1; return (uint32_t)hi; } if (x < 0) { c->qc = 1; return 0; } return v; }
/* VFP */
static inline uint32_t f2s_(double x){ if (x != x) return 0; if (x >= 2147483648.0) return 0x7fffffffu; if (x <= -2147483648.0) return 0x80000000u; return (uint32_t)(int32_t)x; }
static inline uint32_t f2u_(double x){ if (x != x || x <= 0) return 0; if (x >= 4294967296.0) return 0xffffffffu; return (uint32_t)x; }
static inline void vcmp_(CPU *c, double a, double b){
    if (a != a || b != b) { c->fn = 0; c->fz = 0; c->fc = 1; c->fv = 1; }
    else if (a == b)      { c->fn = 0; c->fz = 1; c->fc = 1; c->fv = 0; }
    else if (a < b)       { c->fn = 1; c->fz = 0; c->fc = 0; c->fv = 0; }
    else                  { c->fn = 0; c->fz = 0; c->fc = 1; c->fv = 0; }
}
static inline uint32_t fpscr_get_(CPU *c){ return (c->fn << 31) | (c->fz << 30) | (c->fc << 29) | (c->fv << 28) | c->fpscr_other; }
static inline void fpscr_set_(CPU *c, uint32_t v){ c->fn = v >> 31; c->fz = (v >> 30) & 1; c->fc = (v >> 29) & 1; c->fv = (v >> 28) & 1; c->fpscr_other = v & 0x0fffffffu; }
#define VS(n) (c->f.s[n])
#define VD(n) (c->f.d[n])
#define VU(n) (c->f.u[n])
#define VQ(n) (c->f.q[n])

/* provided by the host runtime */
void arm_dispatch(CPU *c, uint32_t addr);          /* indirect call to a guest address */
void arm_trap(uint32_t addr, const char *why);     /* unsupported / invalid */
void arm_svc(CPU *c, uint32_t imm);

#include <setjmp.h>
jmp_buf *xw_jb_slot(CPU *c);
#define XW_SETJMP(c) do { if (setjmp(*xw_jb_slot(c))) {} else (c)->r[0] = 0; } while (0)
#include "neon_ops.h"
#endif
