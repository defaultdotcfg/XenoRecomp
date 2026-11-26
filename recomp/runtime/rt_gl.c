/* rt_gl.c - OpenGL ES 2.0 (guest) -> desktop OpenGL 2.x+ (host). Guest pointers are identity-mapped, so most calls pass straight through. */
#include "rt.h"
#include <GL/gl.h>

#define SHIM(n) void imp_##n(CPU *c)
typedef char GLchar; typedef ptrdiff_t GLsizeiptr_; typedef ptrdiff_t GLintptr_;
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_RGBA8 0x8058
#define GL_DEPTH24_STENCIL8 0x88F0

#define PFNS \
 F(void, ActiveTexture, (GLenum)) F(void, AttachShader, (GLuint, GLuint)) F(void, BindBuffer, (GLenum, GLuint)) \
 F(void, BindFramebuffer, (GLenum, GLuint)) F(void, BindRenderbuffer, (GLenum, GLuint)) \
 F(void, BufferData, (GLenum, GLsizeiptr_, const void *, GLenum)) F(void, BufferSubData, (GLenum, GLintptr_, GLsizeiptr_, const void *)) \
 F(GLenum, CheckFramebufferStatus, (GLenum)) F(void, CompileShader, (GLuint)) \
 F(GLuint, CreateProgram, (void)) F(GLuint, CreateShader, (GLenum)) F(void, DeleteBuffers, (GLsizei, const GLuint *)) \
 F(void, DeleteFramebuffers, (GLsizei, const GLuint *)) F(void, DeleteProgram, (GLuint)) F(void, DeleteRenderbuffers, (GLsizei, const GLuint *)) \
 F(void, DeleteShader, (GLuint)) F(void, DisableVertexAttribArray, (GLuint)) F(void, EnableVertexAttribArray, (GLuint)) \
 F(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) F(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
 F(void, GenBuffers, (GLsizei, GLuint *)) F(void, GenFramebuffers, (GLsizei, GLuint *)) F(void, GenRenderbuffers, (GLsizei, GLuint *)) \
 F(void, GetAttachedShaders, (GLuint, GLsizei, GLsizei *, GLuint *)) F(GLint, GetAttribLocation, (GLuint, const GLchar *)) \
 F(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) F(void, GetProgramiv, (GLuint, GLenum, GLint *)) \
 F(void, GetRenderbufferParameteriv, (GLenum, GLenum, GLint *)) F(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
 F(void, GetShaderiv, (GLuint, GLenum, GLint *)) F(GLint, GetUniformLocation, (GLuint, const GLchar *)) F(GLboolean, IsBuffer, (GLuint)) \
 F(void, LinkProgram, (GLuint)) F(void, RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
 F(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
 F(void, Uniform1f, (GLint, GLfloat)) F(void, Uniform1fv, (GLint, GLsizei, const GLfloat *)) F(void, Uniform1i, (GLint, GLint)) \
 F(void, Uniform2f, (GLint, GLfloat, GLfloat)) F(void, Uniform3f, (GLint, GLfloat, GLfloat, GLfloat)) \
 F(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) F(void, Uniform4fv, (GLint, GLsizei, const GLfloat *)) \
 F(void, UniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *)) F(void, UseProgram, (GLuint)) \
 F(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
 F(void, BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
 F(void, GetFramebufferAttachmentParameteriv, (GLenum, GLenum, GLenum, GLint *)) \
 F(void, RenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))
#define F(r, n, a) static r (APIENTRY *p_##n) a;
PFNS
#undef F
extern volatile int g_vp[4];
int g_scale = 1;
static int g_ssdbg(void);
int g_gl_w = 1024, g_gl_h = 576;      /* backbuffer size the game believes it renders to (16:9 is the game's native layout; XW_W/XW_H override) */
static GLuint g_screen_fbo, g_screen_rb, g_rb_bound, g_fb_bound;
static GLint g_gvp[4], g_gsc[4]; static int g_gvp_set, g_gsc_set;   /* viewport / scissor as the game set them (in points) */
static void ss_reapply(void);
static int g_ready;

void rt_gl_setup(void) {
#define F(r, n, a) p_##n = (void *)wglGetProcAddress("gl" #n); if (!p_##n) rt_log("[gl] missing gl" #n "\n");
    PFNS
#undef F
    {   /* the supersampled screen buffer must fit the GPU limits, otherwise the FBO is incomplete / draws fail */
        GLint rb = 0, vp[2] = { 0, 0 }, tx = 0;
        glGetIntegerv(0x84E8, &rb); glGetIntegerv(0x0D3A, vp); glGetIntegerv(0x0D33, &tx);
        GLint lim = rb; if (vp[0] && vp[0] < lim) lim = vp[0]; if (tx && tx < lim) lim = tx;
        rt_log("[gl] limits: renderbuffer %d, viewport %dx%d, texture %d\n", rb, vp[0], vp[1], tx);
        if (lim > 0) { int m = lim / (g_gl_w > g_gl_h ? g_gl_w : g_gl_h); if (m < 1) m = 1; if (g_scale > m) { rt_log("[res] scale %d exceeds GPU limit, clamped to %d\n", g_scale, m); g_scale = m; } }
    }
    g_ready = 1;
}

/* present: copy the game's drawable framebuffer into the window, letterboxed */
void rt_gl_present(HDC hdc, int ww, int wh, const char *dump) {
    if (!g_ready) return;
    if (ww < 1) ww = 1;
    if (wh < 1) wh = 1;
    int vw = ww, vh = ww * g_gl_h / g_gl_w;
    if (vh > wh) { vh = wh; vw = wh * g_gl_w / g_gl_h; }
    int vx = (ww - vw) / 2, vy = (wh - vh) / 2;
    g_vp[0] = vx; g_vp[1] = vy; g_vp[2] = vw; g_vp[3] = vh;
    p_BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST); glColorMask(1, 1, 1, 1);
    glViewport(0, 0, ww, wh); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    if (g_screen_fbo) {
        /* two-step scale: screen FBO -> intermediate texture FBO of the final size (FBO-to-FBO scaling), then 1:1 copy to the window */
        static GLuint sfbo, stex; static int sw, sh;
        if (!sfbo) { p_GenFramebuffers(1, &sfbo); glGenTextures(1, &stex); }
        if (sw != vw || sh != vh) {
            glBindTexture(GL_TEXTURE_2D, stex); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, vw, vh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            p_BindFramebuffer(GL_DRAW_FRAMEBUFFER, sfbo); p_FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, stex, 0);
            sw = vw; sh = vh;
        }
        GLenum flt = (rt_genv("XW_FILTER") && !strcmp(rt_genv("XW_FILTER"), "nearest")) ? GL_NEAREST : GL_LINEAR;
        p_BindFramebuffer(GL_READ_FRAMEBUFFER, g_screen_fbo); p_BindFramebuffer(GL_DRAW_FRAMEBUFFER, sfbo);
        p_BlitFramebuffer(0, 0, g_gl_w * g_scale, g_gl_h * g_scale, 0, 0, vw, vh, GL_COLOR_BUFFER_BIT, flt);
        p_BindFramebuffer(GL_READ_FRAMEBUFFER, sfbo); p_BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        p_BlitFramebuffer(0, 0, vw, vh, vx, vy, vx + vw, vy + vh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    { static int pl; if (rt_genv("XW_PRESDBG") && pl++ < 3) { GLint rw = 0, rh = 0; if (g_screen_rb) { p_BindRenderbuffer(0x8D41, g_screen_rb); p_GetRenderbufferParameteriv(0x8D41, 0x8D42, &rw); p_GetRenderbufferParameteriv(0x8D41, 0x8D43, &rh); p_BindRenderbuffer(0x8D41, g_rb_bound); }
        rt_log("[pres] win %dx%d dst %d,%d %dx%d src %dx%d screen_fbo=%u rb=%dx%d err=%x\n", ww, wh, vx, vy, vw, vh, g_gl_w * g_scale, g_gl_h * g_scale, g_screen_fbo, rw, rh, glGetError()); } }
    if (dump) {
        uint8_t *px = malloc((size_t)ww * wh * 4);
        glReadBuffer(GL_BACK); glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE, px);
        FILE *f = fopen(dump, "wb");
        if (f) { fwrite(&ww, 4, 1, f); fwrite(&wh, 4, 1, f); fwrite(px, 1, (size_t)ww * wh * 4, f); fclose(f); }
        free(px);
        if (rt_genv("XW_DUMPFBO") && g_screen_fbo) {   /* debug: raw contents of the game's screen framebuffer */
            int sw = g_gl_w * g_scale, sh = g_gl_h * g_scale; uint8_t *sp = malloc((size_t)sw * sh * 4); char fn[MAX_PATH];
            p_BindFramebuffer(GL_READ_FRAMEBUFFER, g_screen_fbo); glReadBuffer(0x8CE0); glReadPixels(0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, sp);
            snprintf(fn, sizeof fn, "%s.fbo", dump); FILE *g = fopen(fn, "wb");
            if (g) { fwrite(&sw, 4, 1, g); fwrite(&sh, 4, 1, g); fwrite(sp, 1, (size_t)sw * sh * 4, g); fclose(g); }
            free(sp); p_BindFramebuffer(GL_READ_FRAMEBUFFER, 0); glReadBuffer(GL_BACK);
        }
    }
    SwapBuffers(hdc);
    p_BindFramebuffer(GL_FRAMEBUFFER, g_fb_bound);
    ss_reapply();
}
/* [EAGLContext renderbufferStorage:fromDrawable:] */
void rt_gl_drawable_storage(void) {
    p_RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, g_gl_w * g_scale, g_gl_h * g_scale);
    g_screen_rb = g_rb_bound;
}

SHIM(_glActiveTexture) { p_ActiveTexture(ARG(0)); }
SHIM(_glAttachShader) { p_AttachShader(ARG(0), ARG(1)); }
SHIM(_glBindBuffer) { p_BindBuffer(ARG(0), ARG(1)); }
SHIM(_glBindFramebuffer) { if (g_ssdbg()) { static int k; if (k++ < 40) rt_log("[ss] BindFramebuffer %x %u\n", ARG(0), ARG(1)); } if (ARG(0) != 0x8CA8) g_fb_bound = ARG(1); p_BindFramebuffer(ARG(0), ARG(1)); if (ARG(0) != 0x8CA8) ss_reapply(); }
SHIM(_glBindRenderbuffer) { g_rb_bound = ARG(1); p_BindRenderbuffer(ARG(0), ARG(1)); }
SHIM(_glBindTexture) { glBindTexture(ARG(0), ARG(1)); }
SHIM(_glBlendFunc) { glBlendFunc(ARG(0), ARG(1)); }
SHIM(_glBufferData) { p_BufferData(ARG(0), (GLsizeiptr_)ARG(1), ARG(2) ? GP(ARG(2)) : NULL, ARG(3)); }
SHIM(_glBufferSubData) { p_BufferSubData(ARG(0), (GLintptr_)ARG(1), (GLsizeiptr_)ARG(2), GP(ARG(3))); }
SHIM(_glCheckFramebufferStatus) { GLenum st = p_CheckFramebufferStatus(ARG(0)); if (st != 0x8CD5) rt_log("[gl] FBO incomplete: %x\n", st); RET(st); }
int g_cnt_clear, g_cnt_draw, g_cnt_shader, g_cnt_tex;
static int g_ssdbg(void) { static int n = -1; if (n < 0) n = rt_genv("XW_SSDBG") ? 1 : 0; return n; }
SHIM(_glClear) { g_cnt_clear++; if (g_ssdbg()) { static int k; if (k++ < 60) { GLint sb[4] = { 0 }; glGetIntegerv(0x0C10, sb); { GLint vv[4] = { 0 }; glGetIntegerv(0x0BA2, vv); rt_log("[ss] host viewport %d,%d %dx%d\n", vv[0], vv[1], vv[2], vv[3]); } rt_log("[ss] clear fb=%u screen=%u scissor=%d box=%d,%d %dx%d\n", g_fb_bound, g_screen_fbo, (int)glIsEnabled(0x0C11), sb[0], sb[1], sb[2], sb[3]); } } glClear(ARG(0)); }
SHIM(_glClearColor) { glClearColor(FARG(0), FARG(1), FARG(2), FARG(3)); }
SHIM(_glClearDepthf) { glClearDepth(FARG(0)); }
SHIM(_glColorMask) { glColorMask((GLboolean)ARG(0), (GLboolean)ARG(1), (GLboolean)ARG(2), (GLboolean)ARG(3)); }
SHIM(_glCompileShader) { p_CompileShader(ARG(0)); GLint ok = 0; p_GetShaderiv(ARG(0), 0x8B81, &ok); if (!ok) { char lg[1024] = ""; p_GetShaderInfoLog(ARG(0), 1023, NULL, lg); rt_log("[gl] SHADER COMPILE FAIL %u: %s\n", ARG(0), lg); } }
SHIM(_glCopyTexImage2D) { glCopyTexImage2D(ARG(0), (GLint)ARG(1), ARG(2), (GLint)ARG(3), (GLint)ARG(4), (GLsizei)ARG(5), (GLsizei)ARG(6), (GLint)ARG(7)); }
SHIM(_glCreateProgram) { RET(p_CreateProgram()); }
SHIM(_glCreateShader) { g_cnt_shader++; RET(p_CreateShader(ARG(0))); }
SHIM(_glCullFace) { glCullFace(ARG(0)); }
SHIM(_glDeleteBuffers) { p_DeleteBuffers((GLsizei)ARG(0), (const GLuint *)GP(ARG(1))); }
SHIM(_glDeleteFramebuffers) { p_DeleteFramebuffers((GLsizei)ARG(0), (const GLuint *)GP(ARG(1))); }
SHIM(_glDeleteProgram) { p_DeleteProgram(ARG(0)); }
SHIM(_glDeleteRenderbuffers) { p_DeleteRenderbuffers((GLsizei)ARG(0), (const GLuint *)GP(ARG(1))); }
SHIM(_glDeleteShader) { p_DeleteShader(ARG(0)); }
SHIM(_glDeleteTextures) { glDeleteTextures((GLsizei)ARG(0), (const GLuint *)GP(ARG(1))); }
SHIM(_glDepthFunc) { glDepthFunc(ARG(0)); }
SHIM(_glDepthMask) { glDepthMask((GLboolean)ARG(0)); }
SHIM(_glDisable) { glDisable(ARG(0)); }
SHIM(_glDisableVertexAttribArray) { p_DisableVertexAttribArray(ARG(0)); }
SHIM(_glDiscardFramebufferEXT) { (void)c; }
static void draw_chk(const char *w, uint32_t mode, uint32_t n) { static int ne; GLenum e = glGetError(); if (e && ne < 40) { ne++; GLint pr = 0, fb = 0; glGetIntegerv(0x8B8D, &pr); glGetIntegerv(0x8CA6, &fb); rt_log("[gl] ERROR 0x%x in %s mode=%x n=%u prog=%d fbo=%d\n", e, w, mode, n, pr, fb); } }
static void ss_reapply(void) {
    int m = g_fb_bound == g_screen_fbo ? g_scale : 1;
    if (g_gvp_set) glViewport(g_gvp[0] * m, g_gvp[1] * m, g_gvp[2] * m, g_gvp[3] * m);
    if (g_gsc_set) glScissor(g_gsc[0] * m, g_gsc[1] * m, g_gsc[2] * m, g_gsc[3] * m);
}
SHIM(_glDrawArrays) { g_cnt_draw++; glDrawArrays(ARG(0), (GLint)ARG(1), (GLsizei)ARG(2)); draw_chk("DrawArrays", ARG(0), ARG(2)); }
SHIM(_glDrawElements) { g_cnt_draw++; if (g_ssdbg()) { static int k; if (k++ < 400 && (k % 10 == 0)) { GLint vv[4] = { 0 }, ss = 0, pr = 0; glGetIntegerv(0x0BA2, vv); ss = glIsEnabled(0x0C11); glGetIntegerv(0x8B8D, &pr); rt_log("[ss] draw #%d fb=%u prog=%d host vp %d,%d %dx%d scissor=%d\n", k, g_fb_bound, pr, vv[0], vv[1], vv[2], vv[3], ss); } } glDrawElements(ARG(0), (GLsizei)ARG(1), ARG(2), GP(ARG(3))); draw_chk("DrawElements", ARG(0), ARG(1)); }
SHIM(_glEnable) { if (ARG(0) == 0x0C11 && g_ssdbg()) { static int k; if (k++ < 30) rt_log("[ss] enable SCISSOR fb=%u gsc_set=%d g=%d,%d %dx%d\n", g_fb_bound, g_gsc_set, g_gsc[0], g_gsc[1], g_gsc[2], g_gsc[3]); } glEnable(ARG(0)); }
SHIM(_glEnableVertexAttribArray) { p_EnableVertexAttribArray(ARG(0)); }
SHIM(_glFlush) { glFlush(); }
SHIM(_glFramebufferRenderbuffer) {
    p_FramebufferRenderbuffer(ARG(0), ARG(1), ARG(2), ARG(3));
    if (ARG(1) == GL_COLOR_ATTACHMENT0 && ARG(3) && ARG(3) == g_screen_rb) g_screen_fbo = g_fb_bound;
}
SHIM(_glFramebufferTexture2D) { if (g_ssdbg()) { static int k; if (k++ < 30) rt_log("[ss] FramebufferTexture2D fb=%u att=%x tex=%u\n", g_fb_bound, ARG(1), ARG(3)); } p_FramebufferTexture2D(ARG(0), ARG(1), ARG(2), ARG(3), (GLint)ARG(4)); }
SHIM(_glFrontFace) { glFrontFace(ARG(0)); }
SHIM(_glGenBuffers) { p_GenBuffers((GLsizei)ARG(0), (GLuint *)GP(ARG(1))); }
SHIM(_glGenFramebuffers) { p_GenFramebuffers((GLsizei)ARG(0), (GLuint *)GP(ARG(1))); }
SHIM(_glGenRenderbuffers) { p_GenRenderbuffers((GLsizei)ARG(0), (GLuint *)GP(ARG(1))); }
SHIM(_glGenTextures) { glGenTextures((GLsizei)ARG(0), (GLuint *)GP(ARG(1))); }
SHIM(_glGetAttachedShaders) { p_GetAttachedShaders(ARG(0), (GLsizei)ARG(1), (GLsizei *)GP(ARG(2)), (GLuint *)GP(ARG(3))); }
SHIM(_glGetAttribLocation) { RET((uint32_t)p_GetAttribLocation(ARG(0), S(ARG(1)))); }
SHIM(_glGetError) { RET(glGetError()); }
SHIM(_glGetIntegerv) {
    uint32_t pn = ARG(0); GLint *o = GP(ARG(1));
    if (pn == 0x0BA2 || pn == 0x0C10) { glGetIntegerv(pn, o); if (g_fb_bound == g_screen_fbo) for (int i = 0; i < 4; i++) o[i] /= g_scale; if (g_ssdbg()) { static int k; if (k++ < 20) rt_log("[ss] GetIntegerv %x -> %d,%d %dx%d fb=%u\n", pn, o[0], o[1], o[2], o[3], g_fb_bound); } return; }   /* VIEWPORT / SCISSOR_BOX in game points */
    if (pn == 0x8DFD) { glGetIntegerv(0x8B49, o); *o /= 4; return; }       /* MAX_FRAGMENT_UNIFORM_VECTORS */
    if (pn == 0x8DFB) { glGetIntegerv(0x8B4A, o); *o /= 4; return; }       /* MAX_VERTEX_UNIFORM_VECTORS */
    if (pn == 0x8DFC) { glGetIntegerv(0x8B4B, o); *o /= 4; return; }       /* MAX_VARYING_VECTORS */
    glGetIntegerv(pn, o);
}
SHIM(_glGetProgramInfoLog) { p_GetProgramInfoLog(ARG(0), (GLsizei)ARG(1), (GLsizei *)GP(ARG(2)), S(ARG(3))); }
SHIM(_glGetProgramiv) { p_GetProgramiv(ARG(0), ARG(1), (GLint *)GP(ARG(2))); }
SHIM(_glGetRenderbufferParameteriv) { GLint *o = (GLint *)GP(ARG(2)); p_GetRenderbufferParameteriv(ARG(0), ARG(1), o); if (g_rb_bound == g_screen_rb && (ARG(1) == 0x8D42 || ARG(1) == 0x8D43)) *o /= g_scale; if (g_ssdbg()) { static int k; if (k++ < 20) rt_log("[ss] GetRenderbufferParameteriv %x -> %d rb=%u screen_rb=%u\n", ARG(1), *o, g_rb_bound, g_screen_rb); } }
SHIM(_glGetShaderInfoLog) { p_GetShaderInfoLog(ARG(0), (GLsizei)ARG(1), (GLsizei *)GP(ARG(2)), S(ARG(3))); }
SHIM(_glGetShaderiv) { p_GetShaderiv(ARG(0), ARG(1), (GLint *)GP(ARG(2))); }
SHIM(_glGetString) {
    static uint32_t s[8]; uint32_t n = ARG(0);
    int k = n == 0x1F00 ? 0 : n == 0x1F01 ? 1 : n == 0x1F02 ? 2 : n == 0x8B8C ? 4 : 5;
    if (!s[k]) {
        const char *v = k == 0 ? "Apple Inc." : k == 1 ? "Apple A8 GPU" : k == 2 ? "OpenGL ES 2.0 Apple" : k == 4 ? "OpenGL ES GLSL ES 1.00" :
            "GL_OES_depth_texture GL_OES_standard_derivatives GL_EXT_texture_filter_anisotropic GL_IMG_texture_compression_pvrtc GL_OES_rgb8_rgba8 GL_APPLE_framebuffer_multisample GL_EXT_discard_framebuffer";
        s[k] = g_strdup(v);
    }
    RET(s[k]);
}
SHIM(_glGetUniformLocation) { RET((uint32_t)p_GetUniformLocation(ARG(0), S(ARG(1)))); }
SHIM(_glIsBuffer) { RET(p_IsBuffer(ARG(0))); }
SHIM(_glIsTexture) { RET(glIsTexture(ARG(0))); }
SHIM(_glLinkProgram) { p_LinkProgram(ARG(0)); GLint ok = 0; p_GetProgramiv(ARG(0), 0x8B82, &ok); if (!ok) { char lg[1024] = ""; p_GetProgramInfoLog(ARG(0), 1023, NULL, lg); rt_log("[gl] PROGRAM LINK FAIL %u: %s\n", ARG(0), lg); } }
SHIM(_glReadPixels) { glReadPixels((GLint)ARG(0), (GLint)ARG(1), (GLsizei)ARG(2), (GLsizei)ARG(3), ARG(4), ARG(5), GP(ARG(6))); }
SHIM(_glRenderbufferStorage) {
    uint32_t f = ARG(1);
    if (f == 0x81A5 || f == 0x81A6 || f == 0x8D44) f = GL_DEPTH24_STENCIL8;      /* DEPTH_COMPONENT16/24, STENCIL */
    int m = ((int)ARG(2) == g_gl_w && (int)ARG(3) == g_gl_h) ? g_scale : 1;   /* depth/stencil of the screen FBO must match the supersampled colour buffer */
    p_RenderbufferStorage(ARG(0), f, (GLsizei)ARG(2) * m, (GLsizei)ARG(3) * m);
}
SHIM(_glRenderbufferStorageMultisampleAPPLE) { int m = ((int)ARG(3) == g_gl_w && (int)ARG(4) == g_gl_h) ? g_scale : 1; p_RenderbufferStorageMultisample(ARG(0), (GLsizei)ARG(1), ARG(2), (GLsizei)ARG(3) * m, (GLsizei)ARG(4) * m); }
SHIM(_glResolveMultisampleFramebufferAPPLE) {
    GLint obj = 0, w = 0, h = 0; (void)c;
    p_GetFramebufferAttachmentParameteriv(0x8CA8, 0x8CE0, 0x8CD1, &obj);
    if (!obj) return;
    p_BindRenderbuffer(0x8D41, (GLuint)obj); p_GetRenderbufferParameteriv(0x8D41, 0x8D42, &w); p_GetRenderbufferParameteriv(0x8D41, 0x8D43, &h); p_BindRenderbuffer(0x8D41, g_rb_bound);
    static int n; if (n++ < 3) rt_log("[gl] resolve MSAA %dx%d\n", w, h);
    glDisable(GL_SCISSOR_TEST);
    p_BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}
#define SSX(v) ((GLint)(v) * (g_fb_bound == g_screen_fbo ? g_scale : 1))
SHIM(_glScissor) { if (g_ssdbg()) { static int k; if (k++ < 30) rt_log("[ss] glScissor %d,%d %dx%d fb=%u\n", (int)ARG(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), g_fb_bound); } for (int i = 0; i < 4; i++) g_gsc[i] = (GLint)ARG(i); g_gsc_set = 1; glScissor(SSX(ARG(0)), SSX(ARG(1)), SSX(ARG(2)), SSX(ARG(3))); }
SHIM(_glStencilFunc) { glStencilFunc(ARG(0), (GLint)ARG(1), ARG(2)); }
SHIM(_glStencilOp) { glStencilOp(ARG(0), ARG(1), ARG(2)); }
SHIM(_glTexParameteri) { glTexParameteri(ARG(0), ARG(1), (GLint)ARG(2)); }
SHIM(_glUniform1f) { p_Uniform1f((GLint)ARG(0), FARG(1)); }
SHIM(_glUniform1fv) { p_Uniform1fv((GLint)ARG(0), (GLsizei)ARG(1), (const GLfloat *)GP(ARG(2))); }
SHIM(_glUniform1i) { p_Uniform1i((GLint)ARG(0), (GLint)ARG(1)); }
SHIM(_glUniform2f) { p_Uniform2f((GLint)ARG(0), FARG(1), FARG(2)); }
SHIM(_glUniform3f) { p_Uniform3f((GLint)ARG(0), FARG(1), FARG(2), FARG(3)); }
SHIM(_glUniform4f) { p_Uniform4f((GLint)ARG(0), FARG(1), FARG(2), FARG(3), FARG(4)); }
SHIM(_glUniform4fv) { p_Uniform4fv((GLint)ARG(0), (GLsizei)ARG(1), (const GLfloat *)GP(ARG(2))); }
SHIM(_glUniformMatrix4fv) {
    static int n, lim = -1; if (lim < 0) lim = rt_genv("XW_GLTRACE") ? atoi(rt_genv("XW_GLTRACE")) : 0;
    if (n < lim) { n++; const float *m = (const float *)GP(ARG(3)); GLint pr = 0; glGetIntegerv(0x8B8D, &pr); rt_log("[um] prog=%d loc=%d cnt=%d tr=%d:", pr, (int)ARG(0), (int)ARG(1), (int)ARG(2)); for (int i = 0; i < 16; i++) rt_log(" %.3f", m[i]); rt_log("%s", "\n"); }
    p_UniformMatrix4fv((GLint)ARG(0), (GLsizei)ARG(1), (GLboolean)ARG(2), (const GLfloat *)GP(ARG(3))); }
SHIM(_glUseProgram) { p_UseProgram(ARG(0)); }
SHIM(_glVertexAttribPointer) { p_VertexAttribPointer(ARG(0), (GLint)ARG(1), ARG(2), (GLboolean)ARG(3), (GLsizei)ARG(4), GP(ARG(5))); }
SHIM(_glViewport) { if (g_ssdbg()) { static int k; if (k++ < 40) rt_log("[ss] glViewport %d,%d %dx%d fb=%u screen=%u\n", (int)ARG(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), g_fb_bound, g_screen_fbo); } for (int i = 0; i < 4; i++) g_gvp[i] = (GLint)ARG(i); g_gvp_set = 1; glViewport(SSX(ARG(0)), SSX(ARG(1)), SSX(ARG(2)), SSX(ARG(3))); }

/* GLSL ES 1.00 -> desktop GLSL 1.20: drop precision statements/qualifiers */
SHIM(_glShaderSource) {
    GLsizei n = (GLsizei)ARG(1); uint32_t strs = ARG(2), lens = ARG(3);
    size_t cap = 512;
    for (GLsizei i = 0; i < n; i++) cap += lens && ld32(lens + 4 * i) != 0xffffffffu ? ld32(lens + 4 * i) : strlen(S(ld32(strs + 4 * i)));
    char *buf = malloc(cap * 2 + 256), *o = buf;
    o += sprintf(o, "#version 120\n#define lowp\n#define mediump\n#define highp\n");
    for (GLsizei i = 0; i < n; i++) {
        const char *s = S(ld32(strs + 4 * i)); size_t len = lens && ld32(lens + 4 * i) != 0xffffffffu ? ld32(lens + 4 * i) : strlen(s);
        char *seg = malloc(len + 1); memcpy(seg, s, len); seg[len] = 0;
        for (char *p = seg; *p; ) {
            if (!strncmp(p, "precision ", 10) || !strncmp(p, "#version", 8) || !strncmp(p, "#extension", 10)) { *o++ = '/'; *o++ = '/'; }
            while (*p && *p != '\n') *o++ = *p++;
            if (*p) *o++ = *p++;
        }
        free(seg);
    }
    *o = 0;
    const char *list[1] = { buf };
    p_ShaderSource(ARG(0), 1, list, NULL);
    if (rt_genv("XW_SHADERS")) rt_log("[shader %u]\n%s\n", ARG(0), buf);
    free(buf);
}
int rt_tex_compressed(uint32_t target, int level, uint32_t fmt, int w, int h, uint32_t size, const void *data);
SHIM(_glCompressedTexImage2D) {
    if (!rt_tex_compressed(ARG(0), (int)ARG(1), ARG(2), (int)ARG(3), (int)ARG(4), ARG(6), GP(ARG(7)))) rt_log("[gl] unsupported compressed format %x\n", ARG(2));
}
SHIM(_glTexImage2D) {
    uint32_t data = ARG(8);
    glTexImage2D(ARG(0), (GLint)ARG(1), (GLint)ARG(2), (GLsizei)ARG(3), (GLsizei)ARG(4), (GLint)ARG(5), ARG(6), ARG(7), data ? GP(data) : NULL);
}
