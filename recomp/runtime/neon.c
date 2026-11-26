/* neon.c - NEON helpers for statically lifted ARMv7 code.  D registers are addressed by index (q = 2*Q).
 * Lane kinds: 0 = int/unsigned, 1 = signed, 2 = float(f32). */
#include "armrt.h"
#include "neon_ops.h"

typedef struct { uint8_t b[16]; } V;

static inline int64_t lg(const uint8_t *p, int esz, int sg) {
    switch (esz) {
    case 1: return sg ? (int64_t)*(const int8_t *)p : (int64_t)*(const uint8_t *)p;
    case 2: { if (sg) { int16_t v; memcpy(&v, p, 2); return v; } uint16_t v; memcpy(&v, p, 2); return v; }
    case 4: { if (sg) { int32_t v; memcpy(&v, p, 4); return v; } uint32_t v; memcpy(&v, p, 4); return v; }
    default: { int64_t v; memcpy(&v, p, 8); return v; }
    }
}
static inline void lp(uint8_t *p, int esz, uint64_t v) {
    switch (esz) {
    case 1: *p = (uint8_t)v; break;
    case 2: { uint16_t x = (uint16_t)v; memcpy(p, &x, 2); break; }
    case 4: { uint32_t x = (uint32_t)v; memcpy(p, &x, 4); break; }
    default: memcpy(p, &v, 8);
    }
}
static inline float lgf(const uint8_t *p) { float f; memcpy(&f, p, 4); return f; }
static inline void lpf(uint8_t *p, float f) { memcpy(p, &f, 4); }
#define D(r) (c->f.b + 8 * (r))

static inline void load_v(CPU *c, int r, int nb, V *v) { memset(v, 0, sizeof *v); if (r >= 0) memcpy(v->b, D(r), (size_t)nb); }
static inline void store_v(CPU *c, int r, int nb, const V *v) { memcpy(D(r), v->b, (size_t)nb); }

static uint64_t usat_n(int64_t x, int esz) { int64_t hi = esz >= 8 ? INT64_MAX : (1ll << (esz * 8)) - 1; if (x < 0) return 0; if (x > hi) return (uint64_t)hi; return (uint64_t)x; }
static uint64_t ssat_n(int64_t x, int esz) { int64_t hi = (1ll << (esz * 8 - 1)) - 1, lo = -hi - 1; if (x > hi) return (uint64_t)hi; if (x < lo) return (uint64_t)lo; return (uint64_t)x; }

/* d = n op m  (m < 0 -> zero vector).  lane >= 0: m is broadcast scalar lane of D register m */
void n_bin(CPU *c, int op, int esz, int kind, int d, int n, int m, int nb, int lane) {
    V a, b, r; load_v(c, n, nb, &a); load_v(c, m, nb, &b); load_v(c, d, nb, &r);
    if (lane >= 0) { V s; load_v(c, m, 8, &s); uint8_t t[8]; memcpy(t, s.b + lane * esz, (size_t)esz); for (int i = 0; i < nb; i += esz) memcpy(b.b + i, t, (size_t)esz); }
    int sg = kind == 1, fl = kind == 2;
    if (op >= N_AND && op <= N_ORN) {
        uint64_t *pa = (uint64_t *)a.b, *pb = (uint64_t *)b.b, *pr = (uint64_t *)r.b;
        for (int i = 0; i < nb / 8; i++) {
            switch (op) {
            case N_AND: pr[i] = pa[i] & pb[i]; break; case N_ORR: pr[i] = pa[i] | pb[i]; break; case N_EOR: pr[i] = pa[i] ^ pb[i]; break;
            case N_BIC: pr[i] = pa[i] & ~pb[i]; break; default: pr[i] = pa[i] | ~pb[i]; break;
            }
        }
        store_v(c, d, nb, &r); return;
    }
    if (op == N_PMAX || op == N_PMIN) {
        int ln = 8 / esz; V rr; memset(&rr, 0, sizeof rr);
        for (int i = 0; i < ln; i++) {
            const uint8_t *x = (i < ln / 2 ? a.b + 2 * i * esz : b.b + 2 * (i - ln / 2) * esz), *y = x + esz;
            if (fl) { float p = lgf(x), q = lgf(y); lpf(rr.b + i * esz, op == N_PMAX ? (p > q ? p : q) : (p < q ? p : q)); }
            else { int64_t p = lg(x, esz, sg), q = lg(y, esz, sg); if (!sg) { uint64_t up = (uint64_t)p, uq = (uint64_t)q; lp(rr.b + i * esz, esz, op == N_PMAX ? (up > uq ? up : uq) : (up < uq ? up : uq)); } else lp(rr.b + i * esz, esz, (uint64_t)(op == N_PMAX ? (p > q ? p : q) : (p < q ? p : q))); }
        }
        store_v(c, d, 8, &rr); return;
    }
    V o; memset(&o, 0, sizeof o);
    for (int i = 0; i < nb; i += esz) {
        const uint8_t *pa = a.b + i, *pb = b.b + i; uint8_t *po = o.b + i, *pr = r.b + i;
        if (fl) {
            float x = lgf(pa), y = lgf(pb), z = 0; uint32_t u = 0; int isu = 0;
            switch (op) {
            case N_ADD: z = x + y; break; case N_SUB: z = x - y; break; case N_MUL: z = x * y; break;
            case N_MLA: z = lgf(pr) + x * y; break; case N_MLS: z = lgf(pr) - x * y; break;
            case N_MIN: z = x < y ? x : y; break; case N_MAX: z = x > y ? x : y; break;
            case N_ABD: z = fabsf(x - y); break;
            case N_CGT: u = x > y ? ~0u : 0; isu = 1; break; case N_CGE: u = x >= y ? ~0u : 0; isu = 1; break;
            case N_CEQ: u = x == y ? ~0u : 0; isu = 1; break; case N_CLT: u = x < 0 ? ~0u : 0; isu = 1; break; case N_CLE: u = x <= 0 ? ~0u : 0; isu = 1; break;
            default: z = 0;
            }
            if (isu) memcpy(po, &u, 4); else lpf(po, z);
            continue;
        }
        int64_t x = lg(pa, esz, sg), y = lg(pb, esz, sg), z = 0; uint64_t ux = (uint64_t)x, uy = (uint64_t)y;
        if (esz < 8 && !sg) { ux = (uint64_t)x; uy = (uint64_t)y; }
        switch (op) {
        case N_ADD: z = (int64_t)(ux + uy); break; case N_SUB: z = (int64_t)(ux - uy); break; case N_MUL: z = (int64_t)(ux * uy); break;
        case N_MLA: z = (int64_t)((uint64_t)lg(pr, esz, 0) + ux * uy); break; case N_MLS: z = (int64_t)((uint64_t)lg(pr, esz, 0) - ux * uy); break;
        case N_MIN: z = sg ? (x < y ? x : y) : (int64_t)(ux < uy ? ux : uy); break; case N_MAX: z = sg ? (x > y ? x : y) : (int64_t)(ux > uy ? ux : uy); break;
        case N_ABD: z = sg ? (x > y ? x - y : y - x) : (int64_t)(ux > uy ? ux - uy : uy - ux); break;
        case N_HADD: z = (x + y) >> 1; break; case N_RHADD: z = (x + y + 1) >> 1; break;
        case N_QADD: z = (int64_t)(sg ? ssat_n(x + y, esz) : usat_n(x + y, esz)); break;
        case N_CGT: z = (sg ? x > y : ux > uy) ? -1 : 0; break; case N_CGE: z = (sg ? x >= y : ux >= uy) ? -1 : 0; break;
        case N_CEQ: z = x == y ? -1 : 0; break; case N_TST: z = (x & y) ? -1 : 0; break;
        case N_CLT: z = x < 0 ? -1 : 0; break; case N_CLE: z = x <= 0 ? -1 : 0; break;
        case N_SHLR: { int s = (int8_t)(uint8_t)y; int bits = esz * 8; uint64_t u = esz >= 8 ? (uint64_t)x : ((uint64_t)x & ((1ull << bits) - 1));
            if (s >= 0) z = s >= bits ? 0 : (int64_t)(u << s);
            else { int rs = -s; if (sg) z = rs >= bits ? (x < 0 ? -1 : 0) : (x >> rs); else z = rs >= bits ? 0 : (int64_t)(u >> rs); } break; }
        default: z = 0;
        }
        lp(po, esz, (uint64_t)z);
    }
    store_v(c, d, nb, &o);
}

/* unary: neg, abs, mvn, rev64 */
void n_un(CPU *c, int op, int esz, int kind, int d, int m, int nb) {
    V a, o; load_v(c, m, nb, &a); memset(&o, 0, sizeof o); int sg = kind == 1;
    if (op == N_REV64) {
        for (int g = 0; g < nb; g += 8) for (int i = 0; i < 8; i += esz) memcpy(o.b + g + i, a.b + g + 8 - esz - i, (size_t)esz);
        store_v(c, d, nb, &o); return;
    }
    for (int i = 0; i < nb; i += esz) {
        if (kind == 2) { float x = lgf(a.b + i); lpf(o.b + i, op == N_NEG ? -x : fabsf(x)); continue; }
        int64_t x = lg(a.b + i, esz, sg); uint64_t z;
        if (op == N_NEG) z = (uint64_t)(-x); else if (op == N_ABS) z = (uint64_t)(x < 0 ? -x : x); else z = ~(uint64_t)x;
        lp(o.b + i, esz, z);
    }
    store_v(c, d, nb, &o);
}

/* shift by immediate: shl / shr / sra */
void n_shi(CPU *c, int op, int esz, int kind, int d, int m, int nb, int imm) {
    V a, r, o; load_v(c, m, nb, &a); load_v(c, d, nb, &r); memset(&o, 0, sizeof o); int sg = kind == 1, bits = esz * 8;
    for (int i = 0; i < nb; i += esz) {
        int64_t x = lg(a.b + i, esz, sg); uint64_t u = esz >= 8 ? (uint64_t)x : ((uint64_t)x & ((1ull << bits) - 1)); uint64_t z;
        if (op == N_SHLI) z = imm >= 64 ? 0 : u << imm;
        else { if (sg) z = (uint64_t)(imm >= 64 ? (x < 0 ? -1 : 0) : (x >> imm)); else z = imm >= 64 ? 0 : (u >> imm); if (op == N_SRA) z += (uint64_t)lg(r.b + i, esz, 0); }
        lp(o.b + i, esz, z);
    }
    store_v(c, d, nb, &o);
}

/* long / wide ops.  esz = source element size.  d,n,m D indices (d is a Q pair). op: LADD LSUB LABD LMUL LMLA LMLS WADD WSUB */
void n_long(CPU *c, int op, int esz, int kind, int d, int n, int m, int lane) {
    V a, b, r; int sg = kind == 1, ln = 8 / esz, wide = (op == N_WADD || op == N_WSUB);
    load_v(c, n, wide ? 16 : 8, &a); load_v(c, m, 8, &b); load_v(c, d, 16, &r);
    if (lane >= 0) { uint8_t t[8]; memcpy(t, b.b + lane * esz, (size_t)esz); for (int i = 0; i < 8; i += esz) memcpy(b.b + i, t, (size_t)esz); }
    V o; memset(&o, 0, sizeof o);
    for (int i = 0; i < ln; i++) {
        int64_t x = wide ? lg(a.b + i * 2 * esz, 2 * esz, sg) : lg(a.b + i * esz, esz, sg), y = lg(b.b + i * esz, esz, sg), z;
        switch (op) {
        case N_LADD: case N_WADD: z = x + y; break; case N_LSUB: case N_WSUB: z = x - y; break;
        case N_LABD: z = x > y ? x - y : y - x; break; case N_LMUL: z = x * y; break;
        case N_LMLA: z = lg(r.b + i * 2 * esz, 2 * esz, 0) + x * y; break; default: z = lg(r.b + i * 2 * esz, 2 * esz, 0) - x * y; break;
        }
        lp(o.b + i * 2 * esz, 2 * esz, (uint64_t)z);
    }
    store_v(c, d, 16, &o);
}

/* vmovl: widen each lane of D m into Q d */
void n_movl(CPU *c, int esz, int kind, int d, int m) {
    V a, o; load_v(c, m, 8, &a); memset(&o, 0, sizeof o);
    for (int i = 0; i < 8 / esz; i++) lp(o.b + i * 2 * esz, 2 * esz, (uint64_t)lg(a.b + i * esz, esz, kind == 1));
    store_v(c, d, 16, &o);
}
/* narrowing: op VMOVN (shift ignored), VSHRN (shift right), VADDHN. esz = source element size, n only for addhn */
void n_narrow(CPU *c, int op, int esz, int d, int n, int m, int imm) {
    V a, b, o; load_v(c, m, 16, &a); load_v(c, n, 16, &b); memset(&o, 0, sizeof o); int ne = esz / 2;
    for (int i = 0; i < 16 / esz; i++) {
        uint64_t x = (uint64_t)lg(a.b + i * esz, esz, 0), z;
        if (op == N_NMOV) z = x; else if (op == N_NSHR) z = x >> imm; else z = (x + (uint64_t)lg(b.b + i * esz, esz, 0)) >> (ne * 8);
        lp(o.b + i * ne, ne, z);
    }
    store_v(c, d, 8, &o);
}

/* immediate: op IMOV/IMVN/IORR/IBIC.  imm is the already expanded constant (64-bit for esz 8) */
void n_imm(CPU *c, int op, int d, int nb, uint64_t imm, int esz) {
    V r, o; load_v(c, d, nb, &r);
    for (int i = 0; i < nb; i += esz) {
        if (esz == 8) memcpy(o.b + i, &imm, 8); else lp(o.b + i, esz, imm);
    }
    uint64_t *pr = (uint64_t *)r.b, *po = (uint64_t *)o.b;
    for (int i = 0; i < nb / 8; i++) {
        switch (op) { case N_IMOV: pr[i] = po[i]; break; case N_IMVN: pr[i] = ~po[i]; break; case N_IORR: pr[i] |= po[i]; break; default: pr[i] &= ~po[i]; }
    }
    store_v(c, d, nb, &r);
}

void n_dup(CPU *c, int d, int nb, int esz, uint32_t val) { V o; for (int i = 0; i < nb; i += esz) lp(o.b + i, esz, val); store_v(c, d, nb, &o); }
void n_dupl(CPU *c, int d, int nb, int esz, int s, int lane) { uint64_t v; memcpy(&v, D(s) + lane * esz, (size_t)esz > 8 ? 8 : (size_t)esz); V o; for (int i = 0; i < nb; i += esz) memcpy(o.b + i, D(s) + lane * esz, (size_t)esz); store_v(c, d, nb, &o); (void)v; }
uint32_t n_getlane(CPU *c, int d, int esz, int sg, int lane) { return (uint32_t)lg(D(d) + lane * esz, esz, sg); }
void n_setlane(CPU *c, int d, int esz, int lane, uint32_t v) { lp(D(d) + lane * esz, esz, v); }

void n_ext(CPU *c, int d, int n, int m, int nb, int boff) {
    uint8_t t[32]; memcpy(t, D(n), (size_t)nb); memcpy(t + nb, D(m), (size_t)nb); memcpy(D(d), t + boff, (size_t)nb);
}
/* vzip / vuzp / vtrn, in place on the register pair (d, m) */
void n_perm(CPU *c, int op, int esz, int d, int m, int nb) {
    uint8_t a[16], b[16], ra[16], rb[16]; memcpy(a, D(d), (size_t)nb); memcpy(b, D(m), (size_t)nb); int ln = nb / esz;
    if (op == N_ZIP) { for (int i = 0; i < ln / 2; i++) { memcpy(ra + 2 * i * esz, a + i * esz, (size_t)esz); memcpy(ra + (2 * i + 1) * esz, b + i * esz, (size_t)esz);
                                                           memcpy(rb + 2 * i * esz, a + (ln / 2 + i) * esz, (size_t)esz); memcpy(rb + (2 * i + 1) * esz, b + (ln / 2 + i) * esz, (size_t)esz); } }
    else if (op == N_UZP) { uint8_t cat[32]; memcpy(cat, a, (size_t)nb); memcpy(cat + nb, b, (size_t)nb);
                            for (int i = 0; i < ln; i++) { memcpy(ra + i * esz, cat + (2 * i) * esz, (size_t)esz); memcpy(rb + i * esz, cat + (2 * i + 1) * esz, (size_t)esz); } }
    else { memcpy(ra, a, (size_t)nb); memcpy(rb, b, (size_t)nb); for (int i = 0; i + 1 < ln; i += 2) { memcpy(ra + (i + 1) * esz, b + i * esz, (size_t)esz); memcpy(rb + i * esz, a + (i + 1) * esz, (size_t)esz); } }
    memcpy(D(d), ra, (size_t)nb); memcpy(D(m), rb, (size_t)nb);
}
void n_swp(CPU *c, int d, int m, int nb) { uint8_t t[16]; memcpy(t, D(d), (size_t)nb); memcpy(D(d), D(m), (size_t)nb); memcpy(D(m), t, (size_t)nb); }
/* vtbl/vtbx: table = nregs consecutive D regs starting at first */
void n_tbl(CPU *c, int d, int first, int nregs, int m, int ext) {
    uint8_t tab[32], idx[8], o[8]; memcpy(idx, D(m), 8); for (int i = 0; i < nregs; i++) memcpy(tab + 8 * i, D(first + i), 8); memcpy(o, D(d), 8);
    for (int i = 0; i < 8; i++) { if (idx[i] < 8 * nregs) o[i] = tab[idx[i]]; else if (!ext) o[i] = 0; }
    memcpy(D(d), o, 8);
}
/* bit select: BSL d = (d&n)|(~d&m); BIT d = (d&~m)|(n&m); BIF d = (d&m)|(n&~m) */
void n_bsel(CPU *c, int op, int d, int n, int m, int nb) {
    for (int i = 0; i < nb / 8; i++) {
        uint64_t x, y, z; memcpy(&x, D(d) + 8 * i, 8); memcpy(&y, D(n) + 8 * i, 8); memcpy(&z, D(m) + 8 * i, 8);
        uint64_t r = op == N_BSL ? ((x & y) | (~x & z)) : op == N_BIT ? ((x & ~z) | (y & z)) : ((x & z) | (y & ~z));
        memcpy(D(d) + 8 * i, &r, 8);
    }
}
/* float <-> int lane conversion (round toward zero, saturating), optional fixed-point bits */
void n_cvt(CPU *c, int mode, int d, int m, int nb, int fbits) {
    V a, o; load_v(c, m, nb, &a); memset(&o, 0, sizeof o); double sc = (double)(1ull << fbits);
    for (int i = 0; i < nb; i += 4) {
        if (mode == N_CVT_F2S) { double x = (double)lgf(a.b + i) * sc; uint32_t r = f2s_(x); memcpy(o.b + i, &r, 4); }
        else if (mode == N_CVT_F2U) { double x = (double)lgf(a.b + i) * sc; uint32_t r = f2u_(x); memcpy(o.b + i, &r, 4); }
        else if (mode == N_CVT_S2F) { int32_t x; memcpy(&x, a.b + i, 4); lpf(o.b + i, (float)((double)x / sc)); }
        else { uint32_t x; memcpy(&x, a.b + i, 4); lpf(o.b + i, (float)((double)x / sc)); }
    }
    store_v(c, d, nb, &o);
}
/* vldN/vstN multiple structures. R = register list (D indices), nR entries; n = structure size (1..4) */
void n_ldst(CPU *c, int store, int n, int esz, const int *R, int nR, uint32_t addr) {
    int per = nR / n, ne = 8 / esz;
    if (n == 1) { for (int i = 0; i < nR; i++) { if (store) memcpy(GP(addr + 8 * i), D(R[i]), 8); else memcpy(D(R[i]), GP(addr + 8 * i), 8); } return; }
    for (int r = 0; r < per; r++) for (int e = 0; e < ne; e++) for (int i = 0; i < n; i++) {
        uint8_t *reg = D(R[r + i * per]) + e * esz;
        if (store) memcpy(GP(addr), reg, (size_t)esz); else memcpy(reg, GP(addr), (size_t)esz);
        addr += (uint32_t)esz;
    }
}
