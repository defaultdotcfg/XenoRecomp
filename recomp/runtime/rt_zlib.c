/* rt_zlib.c - guest zlib API (inflate/deflate/crc32/adler32) on host memory. 32-bit z_stream layout:
 * 0 next_in 4 avail_in 8 total_in 12 next_out 16 avail_out 20 total_out 24 msg 28 state 32 zalloc 36 zfree 40 opaque 44 data_type 48 adler */
#include "rt.h"
#define SHIM(n) void imp_##n(CPU *c)
enum { Z_OK = 0, Z_STREAM_END = 1, Z_STREAM_ERROR = -2, Z_DATA_ERROR = -3, Z_BUF_ERROR = -5 };

static uint32_t crc_t[256]; static int crc_ready;
static uint32_t crc_upd(uint32_t crc, const uint8_t *p, size_t n) {
    if (!crc_ready) { for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1; crc_t[i] = v; } crc_ready = 1; }
    crc = ~crc; while (n--) crc = crc_t[(crc ^ *p++) & 255] ^ (crc >> 8); return ~crc;
}
static uint32_t adler_upd(uint32_t ad, const uint8_t *p, size_t n) {
    uint32_t a = ad & 0xffff, b = ad >> 16;
    while (n) { size_t k = n < 5552 ? n : 5552; n -= k; while (k--) { a += *p++; b += a; } a %= 65521; b %= 65521; }
    return (b << 16) | a;
}
SHIM(_crc32) { RET(crc_upd(ARG(0), ARG(1) ? (const uint8_t *)GP(ARG(1)) : (const uint8_t *)"", ARG(2))); }
SHIM(_adler32) { RET(adler_upd(ARG(0), ARG(1) ? (const uint8_t *)GP(ARG(1)) : (const uint8_t *)"", ARG(2))); }

/* ---- inflate: resumable, table-driven decoder. Input is accumulated by the shim; the decoder keeps its position, bit buffer and
 * block state between calls and only suspends at symbol boundaries (so nothing is ever decoded twice). ---- */
typedef struct {
    int stage;                 /* 0 block header, 1 codes, 2 stored, 3 finished */
    int last, lbits, dbits; uint32_t stored_left;
    uint64_t bb; int bc; size_t ipos;
    uint16_t *lt, *dt;         /* (symbol << 4 | length) indexed by the next lbits/dbits stream bits; 0 = invalid */
} Inf;
static const uint16_t lbase[] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 }, lext[] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t dbase[] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 }, dext[] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
/* builds a direct lookup table of 2^maxlen entries; returns malloc'd table (NULL on error), *bits = maxlen */
static uint16_t *build_tab(const uint8_t *len, int n, int *bits) {
    int cnt[16] = { 0 }, mx = 0; for (int i = 0; i < n; i++) { cnt[len[i]]++; if (len[i] > mx) mx = len[i]; }
    if (mx == 0) mx = 1;
    int next[17]; int code = 0; cnt[0] = 0; for (int l = 1; l <= 15; l++) { code = (code + cnt[l - 1]) << 1; next[l] = code; }
    uint16_t *t = calloc((size_t)1 << mx, 2); if (!t) return NULL;
    for (int i = 0; i < n; i++) {
        int l = len[i]; if (!l) continue; int c = next[l]++, r = 0; for (int k = 0; k < l; k++) r |= ((c >> k) & 1) << (l - 1 - k);
        if (r >= (1 << l) || c >= (1 << l)) { free(t); return NULL; }
        for (int k = r; k < (1 << mx); k += 1 << l) t[k] = (uint16_t)((i << 4) | l);
    }
    *bits = mx; return t;
}
static inline void refill(Inf *f, const uint8_t *in, size_t n) { while (f->bc <= 56 && f->ipos < n) { f->bb |= (uint64_t)in[f->ipos++] << f->bc; f->bc += 8; } }
static int getb(Inf *f, const uint8_t *in, size_t n, int k, uint32_t *v) {
    refill(f, in, n); if (f->bc < k) return 0; *v = (uint32_t)(f->bb & (((uint64_t)1 << k) - 1)); f->bb >>= k; f->bc -= k; return 1;
}
typedef struct { int wrap; uint8_t *in; size_t inlen, incap; uint8_t *out; size_t olen, opos, ocap; int done, started; uint32_t adler; Inf f; } IState;
static void out_room(IState *s, size_t extra) {
    if (s->olen + extra > s->ocap) { size_t nc = s->ocap ? s->ocap : 65536; while (nc < s->olen + extra) nc *= 2; s->out = realloc(s->out, nc); s->ocap = nc; }
}
/* returns 1 = stream complete, 0 = need more input, -1 = data error */
static int inf_run(IState *s) {
    Inf *f = &s->f; const uint8_t *in = s->in; size_t n = s->inlen;
    for (;;) {
        if (f->stage == 3) return 1;
        if (f->stage == 0) {
            uint64_t sb = f->bb; int sc = f->bc; size_t sp = f->ipos; uint32_t last, type;
            if (!getb(f, in, n, 1, &last) || !getb(f, in, n, 2, &type)) goto need0;
            if (type == 0) {
                uint32_t len, nlen; f->bb >>= (f->bc & 7); f->bc -= f->bc & 7;
                if (!getb(f, in, n, 16, &len) || !getb(f, in, n, 16, &nlen)) goto need0;
                f->stored_left = len; f->stage = 2;
            } else if (type == 1) {
                static uint16_t *flt, *fdt; static int flb, fdb;
                if (!flt) { uint8_t l[288], d[30]; int i = 0; for (; i < 144; i++) l[i] = 8; for (; i < 256; i++) l[i] = 9; for (; i < 280; i++) l[i] = 7; for (; i < 288; i++) l[i] = 8; for (i = 0; i < 30; i++) d[i] = 5; flt = build_tab(l, 288, &flb); fdt = build_tab(d, 30, &fdb); }
                free(f->lt); free(f->dt); f->lt = NULL; f->dt = NULL;
                f->lt = malloc(((size_t)2) << flb); f->dt = malloc(((size_t)2) << fdb); memcpy(f->lt, flt, ((size_t)2) << flb); memcpy(f->dt, fdt, ((size_t)2) << fdb); f->lbits = flb; f->dbits = fdb; f->stage = 1;
            } else if (type == 2) {
                static const uint8_t ord[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
                uint32_t hl, hd, hc, v; uint8_t l[320], ll[330];
                if (!getb(f, in, n, 5, &hl) || !getb(f, in, n, 5, &hd) || !getb(f, in, n, 4, &hc)) goto need0;
                int nl = (int)hl + 257, nd = (int)hd + 1, nc = (int)hc + 4; if (nl > 286 || nd > 30) return -1;
                memset(l, 0, sizeof l); for (int i = 0; i < nc; i++) { if (!getb(f, in, n, 3, &v)) goto need0; l[ord[i]] = (uint8_t)v; }
                int cb; uint16_t *ct = build_tab(l, 19, &cb); if (!ct) return -1;
                int idx = 0;
                while (idx < nl + nd) {
                    refill(f, in, n); uint16_t e = ct[f->bb & (((uint64_t)1 << cb) - 1)]; int cl = e & 15;
                    if (!cl) { free(ct); if (f->bc < cb) goto need0; return -1; }
                    if (cl > f->bc) { free(ct); goto need0; }
                    f->bb >>= cl; f->bc -= cl; int sy = e >> 4;
                    if (sy < 16) ll[idx++] = (uint8_t)sy;
                    else {
                        int rep, val = 0, eb = sy == 16 ? 2 : sy == 17 ? 3 : 7;
                        if (!getb(f, in, n, eb, &v)) { free(ct); goto need0; }
                        if (sy == 16) { if (!idx) { free(ct); return -1; } val = ll[idx - 1]; rep = 3 + (int)v; } else if (sy == 17) rep = 3 + (int)v; else rep = 11 + (int)v;
                        if (idx + rep > nl + nd) { free(ct); return -1; } while (rep--) ll[idx++] = (uint8_t)val;
                    }
                }
                free(ct); free(f->lt); free(f->dt); f->lt = build_tab(ll, nl, &f->lbits); f->dt = build_tab(ll + nl, nd, &f->dbits);
                if (!f->lt || !f->dt) return -1;
                f->stage = 1;
            } else return -1;
            f->last = (int)last; continue;
        need0:
            f->bb = sb; f->bc = sc; f->ipos = sp; return 0;
        }
        if (f->stage == 2) {
            while (f->stored_left) {
                out_room(s, 1);
                if (f->bc >= 8) { s->out[s->olen++] = (uint8_t)f->bb; f->bb >>= 8; f->bc -= 8; }
                else if (f->ipos < n) s->out[s->olen++] = in[f->ipos++];
                else return 0;
                f->stored_left--;
            }
            if (f->last) { f->stage = 3; return 1; } f->stage = 0; continue;
        }
        /* stage 1: literal/length + distance symbols */
        {
            const uint16_t *lt = f->lt, *dt = f->dt; uint64_t lmask = ((uint64_t)1 << f->lbits) - 1, dmask = ((uint64_t)1 << f->dbits) - 1;
            for (;;) {
                refill(f, in, n);
                uint64_t sb = f->bb; int sc = f->bc; size_t sp = f->ipos;
                uint16_t e = lt[f->bb & lmask]; int l = e & 15;
                if (!l) { if (f->bc < f->lbits) return 0; return -1; }
                if (l > f->bc) return 0;
                f->bb >>= l; f->bc -= l; int sy = e >> 4;
                if (sy < 256) { out_room(s, 1); s->out[s->olen++] = (uint8_t)sy; continue; }
                if (sy == 256) { if (f->last) { f->stage = 3; return 1; } f->stage = 0; break; }
                sy -= 257; if (sy >= 29) return -1;
                int eb = lext[sy]; if (f->bc < eb) goto back;
                int len = lbase[sy] + (int)(f->bb & (((uint64_t)1 << eb) - 1)); f->bb >>= eb; f->bc -= eb;
                uint16_t de = dt[f->bb & dmask]; int dl = de & 15;
                if (!dl) { if (f->bc < f->dbits) goto back; return -1; }
                if (dl > f->bc) goto back;
                f->bb >>= dl; f->bc -= dl; int ds = de >> 4; if (ds >= 30) return -1;
                int db = dext[ds]; if (f->bc < db) goto back;
                size_t dist = dbase[ds] + (size_t)(f->bb & (((uint64_t)1 << db) - 1)); f->bb >>= db; f->bc -= db;
                if (dist > s->olen) return -1;
                out_room(s, 258);
                { uint8_t *o = s->out + s->olen, *src = o - dist; if (dist >= (size_t)len) memcpy(o, src, len); else for (int k = 0; k < len; k++) o[k] = src[k]; s->olen += len; }
                continue;
            back:
                f->bb = sb; f->bc = sc; f->ipos = sp; return 0;
            }
        }
    }
}
static void *zst[4096]; static int nz = 1;
static uint32_t zh_new(void *p) { int i; for (i = 1; i < nz; i++) if (!zst[i]) break; if (i == nz) { if (nz == 4096) return 0; nz++; } zst[i] = p; return (uint32_t)i; }
#define ZS(z) zst[ld32((z) + 28)]
static void inf_free(IState *s) { free(s->f.lt); free(s->f.dt); s->f.lt = s->f.dt = NULL; }
static void inf_reset(uint32_t z, IState *s) { inf_free(s); free(s->in); free(s->out); int w = s->wrap; memset(s, 0, sizeof *s); s->wrap = w; st32(z + 8, 0); st32(z + 20, 0); st32(z + 24, 0); }
static int inf_init(CPU *c, int wbits) {
    uint32_t z = ARG(0); if (rt_genv("XW_ZLOG")) rt_log("[zlib] init z=%x lr=%x\n", z, c->r[14]); IState *s = calloc(1, sizeof *s); s->wrap = wbits < 0 ? 0 : wbits > 15 ? 2 : 1;
    st32(z + 28, zh_new(s)); st32(z + 8, 0); st32(z + 20, 0); st32(z + 24, 0); return Z_OK;
}
SHIM(_inflateInit_) { RET(inf_init(c, 15)); }
SHIM(_inflateInit2_) { RET(inf_init(c, (int)ARG(1))); }
SHIM(_inflateEnd) { uint32_t z = ARG(0); IState *s = (IState *)ZS(z); if (s) { inf_free(s); free(s->in); free(s->out); free(s); zst[ld32(z + 28)] = NULL; } RET(Z_OK); }
/* raw deflate one-shot, used by the IPA extractor (rt_ipa.c): 0 = ok */
int rt_inflate_raw(const uint8_t *in, size_t n, uint8_t *out, size_t outn) {
    IState s; memset(&s, 0, sizeof s); s.in = (uint8_t *)in; s.inlen = n;
    int r = inf_run(&s), ok = r == 1 && s.olen == outn; if (ok) memcpy(out, s.out, outn);
    inf_free(&s); free(s.out); return ok ? 0 : -1;
}
SHIM(_inflateReset) { uint32_t z = ARG(0); inf_reset(z, (IState *)ZS(z)); RET(Z_OK); }
SHIM(_inflate) {
    uint32_t z = ARG(0); IState *s = (IState *)ZS(z); uint32_t ain = ld32(z + 4);
    if (!s) { rt_log("[zlib] inflate on null state z=%x slot=%u lr=%x ain=%u\n", z, ld32(z + 28), c->r[14], ld32(z + 4)); s = calloc(1, sizeof *s); s->wrap = 1; st32(z + 28, zh_new(s)); }
    if (ain) {
        if (s->inlen + ain > s->incap) { s->incap = (s->inlen + ain) * 2; s->in = realloc(s->in, s->incap); }
        memcpy(s->in + s->inlen, GP(ld32(z)), ain); s->inlen += ain; st32(z, ld32(z) + ain); st32(z + 4, 0); st32(z + 8, ld32(z + 8) + ain);
        s->done = 0;
    }
    if (!s->done && ain) {
        int r;
        if (!s->started) {
            size_t hdr = 0; if (s->wrap == 1) hdr = 2; else if (s->wrap == 2) { hdr = 10; if (s->inlen > 3 && (s->in[3] & 8)) { while (hdr < s->inlen && s->in[hdr++]) ; } }
            if (s->inlen < hdr) { RET(Z_OK); return; }
            s->f.ipos = hdr; s->started = 1;
        }
        r = inf_run(s);
        if (r < 0) { RET(Z_DATA_ERROR); return; }
        s->done = r == 1;
    }
    uint32_t avail = ld32(z + 16), n = (uint32_t)(s->olen - s->opos); if (n > avail) n = avail;
    if (n) { memcpy(GP(ld32(z + 12)), s->out + s->opos, n); s->opos += n; st32(z + 12, ld32(z + 12) + n); st32(z + 16, avail - n); st32(z + 20, ld32(z + 20) + n); }
    if (s->done && s->opos == s->olen) { RET(Z_STREAM_END); return; }
    RET(n || ain ? Z_OK : Z_BUF_ERROR);
}

/* ---- deflate: stored blocks only (valid, uncompressed) ---- */
typedef struct { int wrap; uint8_t *pend; size_t plen, ppos, pcap; int hdr, fin; uint32_t adler; } DState;
static void pend_put(DState *s, const void *p, size_t n) { if (s->plen + n > s->pcap) { s->pcap = (s->plen + n) * 2; s->pend = realloc(s->pend, s->pcap); } memcpy(s->pend + s->plen, p, n); s->plen += n; }
static int def_init(CPU *c, int wbits) {
    uint32_t z = ARG(0); DState *s = calloc(1, sizeof *s); s->wrap = wbits < 0 ? 0 : 1; s->adler = 1;
    st32(z + 28, zh_new(s)); st32(z + 8, 0); st32(z + 20, 0); st32(z + 24, 0); return Z_OK;
}
SHIM(_deflateInit_) { RET(def_init(c, 15)); }
SHIM(_deflateInit2_) { RET(def_init(c, (int)ARG(3))); }
SHIM(_deflateEnd) { uint32_t z = ARG(0); DState *s = (DState *)ZS(z); if (s) { free(s->pend); free(s); zst[ld32(z + 28)] = NULL; } RET(Z_OK); }
SHIM(_deflate) {
    uint32_t z = ARG(0), flush = ARG(1); DState *s = (DState *)ZS(z);
    if (!s->hdr) { s->hdr = 1; if (s->wrap) { static const uint8_t h[2] = { 0x78, 0x01 }; pend_put(s, h, 2); } }
    uint32_t ain = ld32(z + 4); const uint8_t *in = ain ? (const uint8_t *)GP(ld32(z)) : NULL;
    if (ain) s->adler = adler_upd(s->adler, in, ain);
    int finishing = flush == 4 && !s->fin;
    uint32_t off = 0;
    while (off < ain || finishing) {
        uint32_t len = ain - off > 65535 ? 65535 : ain - off; int last = finishing && off + len == ain;
        uint8_t hdr[5] = { (uint8_t)last, (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)~len, (uint8_t)(~len >> 8) };
        pend_put(s, hdr, 5); pend_put(s, in + off, len); off += len;
        if (last) { s->fin = 1; if (s->wrap) { uint8_t t[4] = { (uint8_t)(s->adler >> 24), (uint8_t)(s->adler >> 16), (uint8_t)(s->adler >> 8), (uint8_t)s->adler }; pend_put(s, t, 4); } break; }
        if (!ain) break;
    }
    st32(z, ld32(z) + ain); st32(z + 4, 0); st32(z + 8, ld32(z + 8) + ain);
    uint32_t avail = ld32(z + 16), n = (uint32_t)(s->plen - s->ppos); if (n > avail) n = avail;
    if (n) { memcpy(GP(ld32(z + 12)), s->pend + s->ppos, n); s->ppos += n; st32(z + 12, ld32(z + 12) + n); st32(z + 16, avail - n); st32(z + 20, ld32(z + 20) + n); }
    if (s->ppos == s->plen) { s->plen = s->ppos = 0; if (s->fin) { RET(Z_STREAM_END); return; } }
    RET(Z_OK);
}
