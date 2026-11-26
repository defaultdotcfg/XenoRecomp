/* rt_tex.c - PVRTC (4 bpp) decoder: compressed iOS textures are expanded to RGBA8 on the CPU */
#include "rt.h"
#include <windows.h>
#include <GL/gl.h>

typedef struct { int r, g, b, a; } Px;
static int ex5(int v) { return (v << 3) | (v >> 2); }
static int ex4(int v) { return v * 17; }
static int ex3(int v) { return (v << 5) | (v << 2) | (v >> 1); }
static void colA(uint32_t w, Px *p) {
    uint32_t f = (w >> 1) & 0x7fff;
    if (f & 0x4000) { int r = (f >> 9) & 31, g = (f >> 4) & 31, b = f & 15; p->r = ex5(r); p->g = ex5(g); p->b = ex5((b << 1) | (b >> 3)); p->a = 255; }
    else { int a = (f >> 11) & 7, r = (f >> 7) & 15, g = (f >> 3) & 15, b = f & 7; p->a = ex3(a); p->r = ex4(r); p->g = ex4(g); p->b = ex3(b); }
}
static void colB(uint32_t w, Px *p) {
    uint32_t f = (w >> 16) & 0x7fff;
    if (w & 0x80000000u) { int r = (f >> 10) & 31, g = (f >> 5) & 31, b = f & 31; p->r = ex5(r); p->g = ex5(g); p->b = ex5(b); p->a = 255; }
    else { int a = (f >> 12) & 7, r = (f >> 8) & 15, g = (f >> 4) & 15, b = f & 15; p->a = ex3(a); p->r = ex4(r); p->g = ex4(g); p->b = ex4(b); }
}
static uint32_t twiddle(uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh) {   /* Morton order, y in the low bit */
    uint32_t r = 0, bit = 1, out = 1, mn = bw < bh ? bw : bh;
    for (uint32_t m = 1; m < mn; m <<= 1) { if (by & bit) r |= out; out <<= 1; if (bx & bit) r |= out; out <<= 1; bit <<= 1; }
    uint32_t hi = (bw > bh ? bx : by); uint32_t shift = 0; for (uint32_t m = 1; m < mn; m <<= 1) shift++;
    if (bw != bh) r |= (hi >> shift) << (2 * shift);
    return r;
}
typedef struct { const Px *A, *B; const uint32_t *M; uint8_t *dst; int w, h, bw, bh, cy0, cy1; } TexJob;
static void tex_cells(const TexJob *J) {
    static const int mw[4] = { 0, 3, 5, 8 };
    const Px *A = J->A, *B = J->B; const uint32_t *M = J->M; uint8_t *dst = J->dst; int w = J->w, h = J->h, bw = J->bw, bh = J->bh, cy0 = J->cy0, cy1 = J->cy1;
    /* walk the (offset by -2,-2) 4x4 interpolation cells: the four corner blocks are fixed inside a cell, so no per-pixel division/modulo */
    for (int cy = cy0; cy < cy1; cy++) {
        int y0 = cy < 0 ? bh - 1 : cy, y1 = (cy + 1) % bh;
        for (int cx = -1; cx < bw; cx++) {
            int x0 = cx < 0 ? bw - 1 : cx, x1 = (cx + 1) % bw;
            const Px *pa[4] = { &A[y0 * bw + x0], &A[y0 * bw + x1], &A[y1 * bw + x0], &A[y1 * bw + x1] }, *pb[4] = { &B[y0 * bw + x0], &B[y0 * bw + x1], &B[y1 * bw + x0], &B[y1 * bw + x1] };
            for (int fy = 0; fy < 4; fy++) {
                int y = cy * 4 + fy + 2; if (y < 0 || y >= h) continue;
                const uint32_t *Mrow = &M[(y >> 2) * bw];
                for (int fx = 0; fx < 4; fx++) {
                    int x = cx * 4 + fx + 2; if (x < 0 || x >= w) continue;
                    int w0 = (4 - fx) * (4 - fy), w1 = fx * (4 - fy), w2 = (4 - fx) * fy, w3 = fx * fy;
                    int m = (Mrow[x >> 2] >> (((y & 3) * 4 + (x & 3)) * 2)) & 3, mm = mw[m], ma = 8 - mm;
                    uint8_t *o = dst + 4 * ((size_t)y * w + x);
                    int a0 = pa[0]->r * w0 + pa[1]->r * w1 + pa[2]->r * w2 + pa[3]->r * w3, b0 = pb[0]->r * w0 + pb[1]->r * w1 + pb[2]->r * w2 + pb[3]->r * w3;
                    int a1 = pa[0]->g * w0 + pa[1]->g * w1 + pa[2]->g * w2 + pa[3]->g * w3, b1 = pb[0]->g * w0 + pb[1]->g * w1 + pb[2]->g * w2 + pb[3]->g * w3;
                    int a2 = pa[0]->b * w0 + pa[1]->b * w1 + pa[2]->b * w2 + pa[3]->b * w3, b2 = pb[0]->b * w0 + pb[1]->b * w1 + pb[2]->b * w2 + pb[3]->b * w3;
                    int a3 = pa[0]->a * w0 + pa[1]->a * w1 + pa[2]->a * w2 + pa[3]->a * w3, b3 = pb[0]->a * w0 + pb[1]->a * w1 + pb[2]->a * w2 + pb[3]->a * w3;
                    o[0] = (uint8_t)((((a0 * ma + b0 * mm) >> 3) + 8) >> 4); o[1] = (uint8_t)((((a1 * ma + b1 * mm) >> 3) + 8) >> 4);
                    o[2] = (uint8_t)((((a2 * ma + b2 * mm) >> 3) + 8) >> 4); o[3] = (uint8_t)((((a3 * ma + b3 * mm) >> 3) + 8) >> 4);
                }
            }
        }
    }
}
static DWORD WINAPI tex_thread(LPVOID p) { tex_cells((const TexJob *)p); return 0; }
static void decode4(const uint8_t *src, int w, int h, uint8_t *dst) {
    int bw = w / 4 < 2 ? 2 : w / 4, bh = h / 4 < 2 ? 2 : h / 4;
    Px *A = malloc(sizeof(Px) * bw * bh), *B = malloc(sizeof(Px) * bw * bh); uint32_t *M = malloc(4 * bw * bh);
    for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
        const uint32_t *blk = (const uint32_t *)(src + 8 * twiddle(bx, by, bw, bh));
        M[by * bw + bx] = blk[0]; colA(blk[1], &A[by * bw + bx]); colB(blk[1], &B[by * bw + bx]);
    }
    TexJob J = { A, B, M, dst, w, h, bw, bh, -1, bh };
    int nt = (w * h >= 128 * 128) ? 4 : 1;            /* big textures: split the cell rows over a few threads */
    if (nt == 1) tex_cells(&J);
    else {
        TexJob jobs[4]; HANDLE th[4]; int rows = bh + 1, per = (rows + nt - 1) / nt;
        for (int t = 0; t < nt; t++) { jobs[t] = J; jobs[t].cy0 = -1 + t * per; jobs[t].cy1 = -1 + (t + 1) * per; if (jobs[t].cy1 > bh) jobs[t].cy1 = bh; if (jobs[t].cy0 > bh) jobs[t].cy0 = bh; }
        for (int t = 1; t < nt; t++) th[t] = CreateThread(NULL, 0, tex_thread, &jobs[t], 0, NULL);
        tex_cells(&jobs[0]);
        for (int t = 1; t < nt; t++) if (th[t]) { WaitForSingleObject(th[t], INFINITE); CloseHandle(th[t]); } else tex_cells(&jobs[t]);
    }
    free(A); free(B); free(M);
}
int rt_tex_compressed(uint32_t target, int level, uint32_t fmt, int w, int h, uint32_t size, const void *data) {
    (void)size;
    if (fmt != 0x8C02 && fmt != 0x8C00) return 0;           /* 4 bpp RGBA / RGB */
    uint8_t *px = malloc((size_t)w * h * 4);
    decode4((const uint8_t *)data, w, h, px);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(target, level, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    free(px); return 1;
}
