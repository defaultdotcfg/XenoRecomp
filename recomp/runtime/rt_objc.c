/* rt_objc.c - Objective-C runtime for the lifted code: real class/method-list dispatch for the guest's own classes,
 * host implementations for the UIKit/Foundation classes it talks to, the app launch sequence and the frame loop. */
#include "rt.h"
#include <direct.h>
#include "rt_pad.h"

#define SHIM(n) void imp_##n(CPU *c)
#define CLASSLIST 4839592u
#define CLASSN 22u
#define CATLIST 4839684u
#define CATN 9u
const uint32_t rt_modinit_addr = 4738728u, rt_modinit_n = 32u;

static int g_trace;
static int in_img(uint32_t a) { return a >= 0x10000 && a < IMG_TOP; }

/* ------------------------------------------------------------ class registry */
typedef struct { uint32_t obj; char name[72]; int meta; } HClass;
static HClass hcl[512]; static int nhcl;
static uint32_t g_cfstr_cls;
static uint32_t gcls[CLASSN]; static char gname[CLASSN][72];
static struct { uint32_t cls, list; } gcat[CATN * 2]; static int ngcat;

static HClass *hclass_find(uint32_t o) { for (int i = 0; i < nhcl; i++) if (hcl[i].obj == o) return &hcl[i]; return NULL; }
static uint32_t hclass_get(const char *name) {
    for (int i = 0; i < nhcl; i++) if (!hcl[i].meta && !strcmp(hcl[i].name, name)) return hcl[i].obj;
    uint32_t o = g_calloc(256); if (nhcl < 512) { strncpy(hcl[nhcl].name, name, 71); hcl[nhcl].obj = o; hcl[nhcl++].meta = 0; }
    return o;
}
void rt_objc_register_slot(uint32_t obj, const char *name) {
    if (!strcmp(name, "___CFConstantStringClassReference")) { g_cfstr_cls = obj; return; }
    int meta = !strncmp(name, "_OBJC_METACLASS_$_", 18), cls = !strncmp(name, "_OBJC_CLASS_$_", 14);
    if (!meta && !cls) return;
    if (nhcl < 512) { strncpy(hcl[nhcl].name, name + (meta ? 18 : 14), 71); hcl[nhcl].obj = obj; hcl[nhcl++].meta = meta; }
}
static int is_guest_class(uint32_t o) { for (unsigned i = 0; i < CLASSN; i++) if (gcls[i] == o) return 1; return 0; }
static const char *class_name(uint32_t k) {
    if (is_guest_class(k)) for (unsigned i = 0; i < CLASSN; i++) if (gcls[i] == k) return gname[i];
    HClass *h = hclass_find(k); return h ? h->name : "?";
}
static uint32_t guest_class_by_name(const char *n) { for (unsigned i = 0; i < CLASSN; i++) if (!strcmp(gname[i], n)) return gcls[i]; return 0; }
static uint32_t ro_of(uint32_t cls) { return ld32(cls + 16) & ~3u; }

static uint8_t g_slid[CLASSN];
static void slide_class(unsigned idx) {                       /* non-fragile ivars: move a subclass past its superclass's real size */
    if (g_slid[idx]) return; g_slid[idx] = 1;
    uint32_t cls = gcls[idx], sup = ld32(cls + 4), ro = ro_of(cls);
    if (!sup || !is_guest_class(sup)) return;
    for (unsigned j = 0; j < CLASSN; j++) if (gcls[j] == sup) slide_class(j);
    uint32_t start = ld32(ro_of(sup) + 8), cur = ld32(ro + 4);
    if (start <= cur) return;
    uint32_t d = start - cur, il = ld32(ro + 28);
    if (il) for (uint32_t i = 0; i < ld32(il + 4); i++) { uint32_t e = il + 8 + 20 * i, op = ld32(e); if (op) st32(op, ld32(op) + d); }
    st32(ro + 4, start); st32(ro + 8, ld32(ro + 8) + d);
}
void rt_objc_init(void) {
    for (unsigned i = 0; i < CLASSN; i++) { gcls[i] = ld32(CLASSLIST + 4 * i); strncpy(gname[i], S(ld32(ro_of(gcls[i]) + 16)), 71); }
    for (unsigned i = 0; i < CLASSN; i++) slide_class(i);
    for (unsigned i = 0; i < CATN; i++) {
        uint32_t cat = ld32(CATLIST + 4 * i), cls = ld32(cat + 4);
        if (!in_img(cls)) continue;                               /* category on a system class: host-implemented anyway (bound later) */
        if (ld32(cat + 8)) { gcat[ngcat].cls = cls; gcat[ngcat++].list = ld32(cat + 8); }
        if (ld32(cat + 12)) { gcat[ngcat].cls = ld32(cls); gcat[ngcat++].list = ld32(cat + 12); }
    }
    g_trace = rt_genv("XW_TRACE") ? atoi(rt_genv("XW_TRACE")) : 0;
}

/* ------------------------------------------------------------ method lookup */
static uint32_t find_in_list(uint32_t ml, const char *sel) {
    if (!ml) return 0;
    uint32_t n = ld32(ml + 4);
    for (uint32_t i = 0; i < n; i++) { uint32_t e = ml + 8 + 12 * i; if (!strcmp(S(ld32(e)), sel)) return ld32(e + 8); }
    return 0;
}
static uint32_t find_method(uint32_t cls, const char *sel, uint32_t *hostcls) {
    uint32_t k = cls;
    for (; in_img(k); k = ld32(k + 4)) {
        uint32_t imp = find_in_list(ld32(ro_of(k) + 20), sel);
        if (imp) return imp;
        for (int i = 0; i < ngcat; i++) if (gcat[i].cls == k && (imp = find_in_list(gcat[i].list, sel))) return imp;
    }
    if (hostcls) *hostcls = k;
    return 0;
}

/* ------------------------------------------------------------ host objects */
static uint32_t mkstr(const char *s) {
    uint32_t o = g_calloc(16); st32(o, g_cfstr_cls); st32(o + 4, 0x7c8); st32(o + 8, g_strdup(s)); st32(o + 12, (uint32_t)strlen(s)); return o;
}
uint32_t rt_mkstr(const char *s) { return mkstr(s); }
static const char *cstr_of(uint32_t o) { return o && ld32(o) == g_cfstr_cls ? S(ld32(o + 8)) : NULL; }
static uint32_t mkarray(const uint32_t *el, uint32_t n);
static uint32_t mkarray(const uint32_t *el, uint32_t n) {
    static uint32_t cls; if (!cls) cls = hclass_get("NSArray");
    uint32_t o = g_calloc(16), v = g_calloc(4 * (n ? n : 1)); st32(o, cls); st32(o + 4, n); st32(o + 8, v); st32(o + 12, n ? n : 1);
    for (uint32_t i = 0; i < n; i++) st32(v + 4 * i, el[i]);
    return o;
}

/* ---- generic host collections: array/set share {isa,count,ptr,cap}; dictionary adds keys ptr at +16 (vals at +8) ---- */
static int name_kind(const char *n) { if (strstr(n, "Array")) return 1; if (strstr(n, "Set")) return 2; if (strstr(n, "Dictionary")) return 3; return 0; }
static int host_kind(uint32_t o) {
    static uint32_t lc; static int lk; uint32_t k = ld32(o);
    if (k == lc) return lk;
    HClass *h = hclass_find(k); int kk = (h && !h->meta) ? name_kind(h->name) : 0; lc = k; lk = kk; return kk;
}
static int obj_eq(uint32_t a, uint32_t b) { if (a == b) return 1; const char *x = cstr_of(a), *y = cstr_of(b); return x && y && !strcmp(x, y); }
static void cgrow(uint32_t o, int dict) {
    uint32_t n = ld32(o + 4), cap = ld32(o + 12);
    if (n < cap && ld32(o + 8)) return;
    uint32_t nc = cap ? cap * 2 : 8, nv = g_calloc(4 * nc); if (ld32(o + 8)) memcpy(GP(nv), GP(ld32(o + 8)), 4 * n); st32(o + 8, nv);
    if (dict) { uint32_t nk = g_calloc(4 * nc); if (ld32(o + 16)) memcpy(GP(nk), GP(ld32(o + 16)), 4 * n); st32(o + 16, nk); }
    st32(o + 12, nc);
}
static int cfind(uint32_t o, uint32_t x, int dict) { uint32_t base = ld32(o + (dict ? 16 : 8)); for (uint32_t i = 0; i < ld32(o + 4); i++) if (obj_eq(ld32(base + 4 * i), x)) return (int)i; return -1; }
static void cdel(uint32_t o, int i, int dict) {
    uint32_t n = ld32(o + 4), v = ld32(o + 8), k = ld32(o + 16);
    for (uint32_t j = (uint32_t)i; j + 1 < n; j++) { st32(v + 4 * j, ld32(v + 4 * j + 4)); if (dict) st32(k + 4 * j, ld32(k + 4 * j + 4)); }
    st32(o + 4, n - 1);
}
static int coll_send(CPU *c, uint32_t self, const char *sel, const uint32_t *a, int kind) {
    int d = kind == 3; uint32_t n = ld32(self + 4);
    if (!strcmp(sel, "count")) { RET(n); return 1; }
    if (!strcmp(sel, "countByEnumeratingWithState:objects:count:")) {
        uint32_t st = a[0];
        if (ld32(st) == 0) { st32(st, 1); st32(st + 4, ld32(self + (d ? 16 : 8))); st32(st + 8, st + 24); RET(n); } else RET(0);
        return 1;
    }
    if (!strcmp(sel, "removeAllObjects")) { st32(self + 4, 0); RET(0); return 1; }
    if (!strcmp(sel, "copy") || !strcmp(sel, "mutableCopy")) { RET(self); return 1; }
    if (d) {
        if (!strcmp(sel, "setObject:forKey:") || !strcmp(sel, "setObject:forKeyedSubscript:")) {
            int i = cfind(self, a[1], 1);
            if (i < 0) { cgrow(self, 1); i = (int)ld32(self + 4); st32(self + 4, i + 1); st32(ld32(self + 16) + 4 * i, a[1]); }
            st32(ld32(self + 8) + 4 * i, a[0]); RET(0); return 1;
        }
        if (!strcmp(sel, "objectForKey:") || !strcmp(sel, "objectForKeyedSubscript:") || !strcmp(sel, "valueForKey:")) { int i = cfind(self, a[0], 1); RET(i < 0 ? 0 : ld32(ld32(self + 8) + 4 * i)); return 1; }
        if (!strcmp(sel, "removeObjectForKey:")) { int i = cfind(self, a[0], 1); if (i >= 0) cdel(self, i, 1); RET(0); return 1; }
        if (!strcmp(sel, "allKeys")) { RET(mkarray(n ? (const uint32_t *)GP(ld32(self + 16)) : NULL, n)); return 1; }
        if (!strcmp(sel, "allValues")) { RET(mkarray(n ? (const uint32_t *)GP(ld32(self + 8)) : NULL, n)); return 1; }
        return 0;
    }
    if (!strcmp(sel, "addObject:") || !strcmp(sel, "setByAddingObject:")) {
        if (kind == 2 && cfind(self, a[0], 0) >= 0) { RET(0); return 1; }
        cgrow(self, 0); st32(ld32(self + 8) + 4 * n, a[0]); st32(self + 4, n + 1); RET(0); return 1;
    }
    if (!strcmp(sel, "addObjectsFromArray:") || !strcmp(sel, "unionSet:")) {
        uint32_t o = a[0]; if (o && host_kind(o) && host_kind(o) != 3) for (uint32_t i = 0; i < ld32(o + 4); i++) { uint32_t e = ld32(ld32(o + 8) + 4 * i); if (kind == 2 && cfind(self, e, 0) >= 0) continue; cgrow(self, 0); uint32_t m = ld32(self + 4); st32(ld32(self + 8) + 4 * m, e); st32(self + 4, m + 1); }
        RET(0); return 1;
    }
    if (!strcmp(sel, "insertObject:atIndex:")) {
        uint32_t at = a[1] > n ? n : a[1]; cgrow(self, 0); uint32_t v = ld32(self + 8);
        for (uint32_t j = n; j > at; j--) st32(v + 4 * j, ld32(v + 4 * j - 4));
        st32(v + 4 * at, a[0]); st32(self + 4, n + 1); RET(0); return 1;
    }
    if (!strcmp(sel, "removeObject:") || !strcmp(sel, "removeObjectIdenticalTo:")) { int i; while ((i = cfind(self, a[0], 0)) >= 0) cdel(self, i, 0); RET(0); return 1; }
    if (!strcmp(sel, "removeObjectAtIndex:")) { if (a[0] < n) cdel(self, (int)a[0], 0); RET(0); return 1; }
    if (!strcmp(sel, "removeLastObject")) { if (n) st32(self + 4, n - 1); RET(0); return 1; }
    if (!strcmp(sel, "containsObject:") || !strcmp(sel, "member:")) { int i = cfind(self, a[0], 0); RET(!strcmp(sel, "member:") ? (i >= 0 ? ld32(ld32(self + 8) + 4 * i) : 0) : i >= 0); return 1; }
    if (!strcmp(sel, "indexOfObject:")) { int i = cfind(self, a[0], 0); RET(i < 0 ? 0x7fffffff : (uint32_t)i); return 1; }
    if (!strcmp(sel, "objectAtIndex:")) { RET(a[0] < n ? ld32(ld32(self + 8) + 4 * a[0]) : 0); return 1; }
    if (!strcmp(sel, "lastObject")) { RET(n ? ld32(ld32(self + 8) + 4 * (n - 1)) : 0); return 1; }
    if (!strcmp(sel, "firstObject") || !strcmp(sel, "anyObject")) { RET(n ? ld32(ld32(self + 8)) : 0); return 1; }
    if (!strcmp(sel, "allObjects")) { RET(mkarray(n ? (const uint32_t *)GP(ld32(self + 8)) : NULL, n)); return 1; }
    return 0;
}
static int is_array(uint32_t o) { static uint32_t cls; if (!cls) cls = hclass_get("NSArray"); return o && ld32(o) == cls; }
static uint32_t singleton(const char *cname, const char *sel) {
    static struct { char k[96]; uint32_t v; } t[64]; static int n; char key[96]; snprintf(key, sizeof key, "%s.%s", cname, sel);
    for (int i = 0; i < n; i++) if (!strcmp(t[i].k, key)) return t[i].v;
    uint32_t v = g_calloc(0x200); st32(v, hclass_get(cname));
    if (n < 64) { strcpy(t[n].k, key); t[n++].v = v; }
    return v;
}
static void app_path(char *out, size_t cap, const char *rel) {
    char cwd[MAX_PATH]; GetCurrentDirectoryA(MAX_PATH, cwd); snprintf(out, cap, "%s%s%s", cwd, rel && *rel ? "\\" : "", rel ? rel : "");
}
static const char *docs_dir(void) {
    static char d[MAX_PATH]; if (!d[0]) { char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH); char *s = strrchr(exe, '\\'); if (s) *s = 0; const char *pr = rt_genv("XW_PROFILE"); extern char g_datadir[]; snprintf(d, sizeof d, "%s\\save", g_datadir[0] ? g_datadir : exe); _mkdir(d); if (pr && *pr) { size_t n = strlen(d); snprintf(d + n, sizeof d - n, "\\%s", pr); _mkdir(d); } }
    return d;
}

/* ------------------------------------------------------------ messaging */
static int is_kind(uint32_t cls, const char *name) {
    for (uint32_t k = cls; k; k = in_img(k) ? ld32(k + 4) : 0) if (!strcmp(class_name(k), name)) return 1;
    return 0;
}
int g_gl_w_pt(void); int g_gl_h_pt(void);
extern int g_gl_w, g_gl_h;
static uint32_t intern_sel(const char *n);
#define intern_sel_p intern_sel
static uint32_t g_touch_view;
static uint32_t alloc_guest(CPU *c, uint32_t cls) {
    uint32_t sz = is_guest_class(cls) ? ld32(ro_of(cls) + 8) : 0x200, o = g_calloc(sz < 8 ? 8 : sz); st32(o, cls);
    uint32_t chain[16]; int n = 0;
    for (uint32_t k = cls; in_img(k) && n < 16; k = ld32(k + 4)) chain[n++] = k;
    for (int i = n - 1; i >= 0; i--) {                     /* root-most class first, like object_cxxConstruct */
        uint32_t imp = find_in_list(ld32(ro_of(chain[i]) + 20), ".cxx_construct");
        if (imp) { CPU t = *c; t.r[0] = o; t.r[1] = 0; t.r[14] = 0; arm_dispatch(&t, imp); }
    }
    if (in_img(cls) && find_method(cls, "touchesBegan:withEvent:", NULL)) g_touch_view = o;
    return o;
}
/* side table for properties the real UIKit objects would hold (view of a controller, delegates...) */
static struct { uint32_t obj; char key[24]; uint32_t val; } hprop[512]; static int nhprop;
static void hprop_set(uint32_t o, const char *k, uint32_t v) { for (int i = 0; i < nhprop; i++) if (hprop[i].obj == o && !strcmp(hprop[i].key, k)) { hprop[i].val = v; return; } if (nhprop < 512) { hprop[nhprop].obj = o; strncpy(hprop[nhprop].key, k, 23); hprop[nhprop++].val = v; } }
static uint32_t hprop_get(uint32_t o, const char *k) { for (int i = 0; i < nhprop; i++) if (hprop[i].obj == o && !strcmp(hprop[i].key, k)) return hprop[i].val; return 0; }
static uint32_t g_delegate;

/* ------------------------------------------------------------ GameController emulation (backed by XInput / DirectInput, see rt_input.c) */
void rt_send_t(CPU *t, uint32_t self, uint32_t sel);
void imp___Block_copy(CPU *c);
static PadState g_pad; static uint32_t g_ctl, g_gp, g_pausedh, g_vch, g_snap_obj, g_snap_buf; static int g_pad_was;
static struct { uint32_t obs, sel; } g_obs[48]; static int g_nobs;
static uint32_t gin_tab[64];
static uint32_t gc_obj(const char *cls, const char *prop, uint32_t v) { uint32_t o = g_calloc(0x40); st32(o, hclass_get(cls)); hprop_set(o, prop, v); return o; }
static void gc_ensure(void) { if (!g_ctl) { g_ctl = gc_obj("GCController", "gcctl", 1); g_gp = gc_obj("GCExtendedGamepad", "gcgp", 1); } }
static void dpad_xy(int d, float *x, float *y) {
    const PadState *p = &g_pad;
    if (d == 0) { *x = ((p->buttons & PB_RIGHT) ? 1.0f : 0.0f) - ((p->buttons & PB_LEFT) ? 1.0f : 0.0f); *y = ((p->buttons & PB_UP) ? 1.0f : 0.0f) - ((p->buttons & PB_DOWN) ? 1.0f : 0.0f); }
    else if (d == 1) { *x = p->lx; *y = p->ly; } else { *x = p->rx; *y = p->ry; }
}
/* input element ids: 0 dpad, 1 left stick, 2 right stick; 3 A 4 B 5 X 6 Y 7 LB 8 RB 9 LT 10 RT; 16+2d(+1) axes; 32+4d+{0 up,1 down,2 left,3 right} */
static float gin_value(int id) {
    const PadState *p = &g_pad; float x, y;
    switch (id) {
    case 3: return (p->buttons & PB_A) ? 1.0f : 0.0f; case 4: return (p->buttons & PB_B) ? 1.0f : 0.0f;
    case 5: return (p->buttons & PB_X) ? 1.0f : 0.0f; case 6: return (p->buttons & PB_Y) ? 1.0f : 0.0f;
    case 7: return (p->buttons & PB_LB) ? 1.0f : 0.0f; case 8: return (p->buttons & PB_RB) ? 1.0f : 0.0f;
    case 9: return p->lt; case 10: return p->rt;
    }
    if (id >= 16 && id < 32) { dpad_xy((id - 16) / 2, &x, &y); return ((id - 16) & 1) ? y : x; }
    if (id >= 32 && id < 44) { int d = (id - 32) / 4, k = (id - 32) & 3; dpad_xy(d, &x, &y); return k == 0 ? (y > 0 ? y : 0) : k == 1 ? (y < 0 ? -y : 0) : k == 2 ? (x < 0 ? -x : 0) : (x > 0 ? x : 0); }
    return 0;
}
static uint32_t gin(int id) {
    if (!gin_tab[id]) gin_tab[id] = gc_obj(id < 3 ? "GCControllerDirectionPad" : id >= 16 && id < 32 ? "GCControllerAxisInput" : "GCControllerButtonInput", "gcin", (uint32_t)id + 1);
    return gin_tab[id];
}
static void pad_snapshot(uint32_t dst, int extended) {     /* GCGamepadSnapShotDataV100 (8 floats) / GCExtendedGamepadSnapshotData (14 floats) */
    float x, y; dpad_xy(0, &x, &y);
    float f[14] = { x, y, gin_value(3), gin_value(4), gin_value(5), gin_value(6), gin_value(7), gin_value(8), g_pad.lx, g_pad.ly, g_pad.rx, g_pad.ry, g_pad.lt, g_pad.rt };
    int n = extended ? 14 : 8;
    st32(dst, 0x0100u | ((uint32_t)(4 + 4 * n) << 16));
    for (int i = 0; i < n; i++) { uint32_t b; memcpy(&b, &f[i], 4); st32(dst + 4 + 4 * i, b); }
}
static void call_block(CPU *c, uint32_t blk, uint32_t a0, uint32_t a1) {
    if (!blk) return; uint32_t inv = ld32(blk + 12); if (!inv) return;
    CPU t = *c; t.r[0] = blk; t.r[1] = a0; t.r[2] = a1; t.r[14] = 0; arm_dispatch(&t, inv);
}
static void pad_notify(CPU *c, const char *selname) {
    uint32_t note = gc_obj("NSNotification", "gcnote", 1);
    for (int i = 0; i < g_nobs; i++) {
        if (!g_obs[i].sel || strcmp(S(g_obs[i].sel), selname)) continue;
        CPU t = *c; t.r[0] = g_obs[i].obs; t.r[2] = note; t.r[3] = 0; t.r[14] = 0;
        rt_log("[pad] -> %s\n", selname); rt_send_t(&t, g_obs[i].obs, g_obs[i].sel);
    }
}
/* once per frame: poll the backends, announce (dis)connection, fire the pause handler on Start */
void rt_pad_frame(CPU *c) {
    static unsigned prev; PadState p; int con = rt_pad_poll(&p);
    if (con) g_pad = p; else memset(&g_pad, 0, sizeof g_pad);
    if (con && !g_pad_was) { gc_ensure(); rt_log("[pad] connected: %s\n", g_pad.name); g_pad_was = 1; pad_notify(c, "addController:"); }
    else if (!con && g_pad_was) { rt_log("[pad] disconnected\n"); g_pad_was = 0; pad_notify(c, "removeController:"); }
    if (con && (g_pad.buttons & PB_START) && !(prev & PB_START) && g_pausedh) call_block(c, g_pausedh, g_ctl, 0);
    prev = con ? g_pad.buttons : 0;
}
void imp__GCExtendedGamepadSnapshotDataFromNSData(CPU *c) { pad_snapshot(ARG(0), 1); RET(1); }
void imp__GCExtendedGamepadSnapShotDataV100FromNSData(CPU *c) { pad_snapshot(ARG(0), 1); RET(1); }
void imp__GCGamepadSnapShotDataV100FromNSData(CPU *c) { pad_snapshot(ARG(0), 0); RET(1); }

static void host_send(CPU *c, uint32_t self, const char *sel, uint32_t hcls, const uint32_t *a, int stret_ptr) {
    int cside = is_guest_class(self) || hclass_find(self) != NULL;
    const char *cn = cside ? class_name(self) : class_name(hcls);
    c->r[1] = 0;
    if (hprop_get(self, "touch")) {
        float tx, ty; uint32_t bx = hprop_get(self, "x"), by = hprop_get(self, "y"); memcpy(&tx, &bx, 4); memcpy(&ty, &by, 4);
        if (!strcmp(sel, "locationInView:") || !strcmp(sel, "previousLocationInView:")) {
            if (!strcmp(sel, "previousLocationInView:")) { bx = hprop_get(self, "px"); by = hprop_get(self, "py"); }
            if (stret_ptr) { st32(stret_ptr, bx); st32(stret_ptr + 4, by); RET(0); } else { c->r[0] = bx; c->r[1] = by; } return; }
        if (!strcmp(sel, "phase")) { RET(hprop_get(self, "phase")); return; }
        if (!strcmp(sel, "tapCount")) { RET(1); return; }
        if (!strcmp(sel, "view") || !strcmp(sel, "window")) { RET(g_touch_view); return; }
        if (!strcmp(sel, "timestamp")) { double t = GetTickCount64() / 1000.0; memcpy(&c->r[0], &t, 8); return; }
    }
    if (hprop_get(self, "uievent")) {
        if (!strcmp(sel, "allTouches") || !strcmp(sel, "touchesForView:")) { RET(hprop_get(self, "set")); return; }
        if (!strcmp(sel, "type")) { RET(0); return; }
        if (!strcmp(sel, "timestamp")) { double t = GetTickCount64() / 1000.0; memcpy(&c->r[0], &t, 8); return; }
    }
    if (rt_genv("XW_PADDBG") && (hprop_get(self, "gcnote") || hprop_get(self, "gcctl") || hprop_get(self, "gcgp") || hprop_get(self, "gcin") || hprop_get(self, "gcdata"))) {
        static int k; if (k++ < 400) rt_log("[padmsg] %s%s\n", hprop_get(self, "gcin") ? "(input) " : hprop_get(self, "gcgp") ? "(gamepad) " : hprop_get(self, "gcctl") ? "(controller) " : "", sel);
    }
    if (hprop_get(self, "gcnote")) {            /* NSNotification posted for controller (dis)connect: object = the controller */
        if (!strcmp(sel, "object")) { RET(g_ctl); return; }
        if (!strcmp(sel, "name") || !strcmp(sel, "userInfo")) { RET(0); return; }
        self = g_ctl;
    }
    if (!strcmp(sel, "respondsToSelector:") && (hprop_get(self, "gcctl") || hprop_get(self, "gcgp") || hprop_get(self, "gcin") || hprop_get(self, "gcdata"))) { RET(1); return; }
    if (hprop_get(self, "gcctl")) {
        if (!strcmp(sel, "vendorName")) { RET(mkstr(g_pad.name[0] ? g_pad.name : "Controller")); return; }
        if (!strcmp(sel, "extendedGamepad") || !strcmp(sel, "gamepad")) { RET(g_gp); return; }
        if (!strcmp(sel, "playerIndex")) { RET((uint32_t)-1); return; }
        if (!strcmp(sel, "setPlayerIndex:")) { RET(0); return; }
        if (!strcmp(sel, "setControllerPausedHandler:")) { { CPU t = *c; t.r[0] = a[0]; imp___Block_copy(&t); g_pausedh = t.r[0]; } RET(0); return; }
        if (!strcmp(sel, "controllerPausedHandler")) { RET(g_pausedh); return; }
        if (!strcmp(sel, "isAttachedToDevice")) { RET(0); return; }
        if (!strcmp(sel, "motion")) { RET(0); return; }
    }
    if (hprop_get(self, "gcgp")) {
        static const struct { const char *n; int id; } m[] = { { "dpad", 0 }, { "leftThumbstick", 1 }, { "rightThumbstick", 2 }, { "buttonA", 3 }, { "buttonB", 4 }, { "buttonX", 5 }, { "buttonY", 6 },
            { "leftShoulder", 7 }, { "rightShoulder", 8 }, { "leftTrigger", 9 }, { "rightTrigger", 10 } };
        for (int i = 0; i < 11; i++) if (!strcmp(sel, m[i].n)) { RET(gin(m[i].id)); return; }
        if (!strcmp(sel, "controller")) { RET(g_ctl); return; }
        if (!strcmp(sel, "setValueChangedHandler:")) { g_vch = a[0]; RET(0); return; }
        if (!strcmp(sel, "valueChangedHandler")) { RET(g_vch); return; }
        if (!strcmp(sel, "saveSnapshot") || !strcmp(sel, "snapshot")) {
            if (!g_snap_obj) { g_snap_obj = gc_obj("GCExtendedGamepadSnapshot", "gcdata", 1); g_snap_buf = g_calloc(96); }
            pad_snapshot(g_snap_buf, 1); RET(g_snap_obj); return;
        }
    }
    if (hprop_get(self, "gcdata")) {            /* snapshot / NSData holding the current pad state */
        if (!strcmp(sel, "snapshotData")) { RET(self); return; }
        if (!strcmp(sel, "bytes")) { pad_snapshot(g_snap_buf, 1); RET(g_snap_buf); return; }
        if (!strcmp(sel, "length")) { RET(60); return; }
        if (!strcmp(sel, "getBytes:length:")) { pad_snapshot(g_snap_buf, 1); memcpy(GP(a[0]), GP(g_snap_buf), a[1] < 60 ? a[1] : 60); RET(0); return; }
    }
    if (hprop_get(self, "gcin")) {
        int id = (int)hprop_get(self, "gcin") - 1; float v = gin_value(id);
        if (!strcmp(sel, "value")) { if (rt_genv("XW_PADDBG")) { static unsigned seen[64][2]; static int ns; int f = 0; for (int i = 0; i < ns; i++) if (seen[i][0] == (unsigned)id && seen[i][1] == c->r[14]) f = 1; if (!f && ns < 64) { seen[ns][0] = id; seen[ns][1] = c->r[14]; ns++; rt_log("[padval] id=%d lr=%x\n", id, c->r[14]); } } RETF(v); return; }
        if (!strcmp(sel, "isPressed")) { RET(v > (id == 9 || id == 10 ? 0.12f : 0.5f)); return; }
        if (!strcmp(sel, "isAnalog")) { RET(id < 3 || id == 9 || id == 10); return; }
        if (!strcmp(sel, "xAxis") && id < 3) { RET(gin(16 + id * 2)); return; }
        if (!strcmp(sel, "yAxis") && id < 3) { RET(gin(16 + id * 2 + 1)); return; }
        if (!strcmp(sel, "up") && id < 3) { RET(gin(32 + id * 4)); return; }
        if (!strcmp(sel, "down") && id < 3) { RET(gin(32 + id * 4 + 1)); return; }
        if (!strcmp(sel, "left") && id < 3) { RET(gin(32 + id * 4 + 2)); return; }
        if (!strcmp(sel, "right") && id < 3) { RET(gin(32 + id * 4 + 3)); return; }
        if (!strcmp(sel, "setValueChangedHandler:") || !strcmp(sel, "setPressedChangedHandler:") || !strcmp(sel, "valueChangedHandler")) { RET(0); return; }
        if (!strcmp(sel, "collection")) { RET(g_gp); return; }
    }
    /* universal */
    if (!strcmp(sel, "retain") || !strcmp(sel, "release") || !strcmp(sel, "autorelease") || !strcmp(sel, "self") || !strcmp(sel, "copy") || !strcmp(sel, "mutableCopy") || !strcmp(sel, "copyWithZone:")) { RET(self); return; }
    if (!strcmp(sel, "setCurrentContext:")) { RET(1); return; }
    if (!strcmp(sel, "presentRenderbuffer:")) { rt_present(c); RET(1); return; }
    if (!strcmp(sel, "dealloc") || !strcmp(sel, "finalize")) { RET(0); return; }
    if (!strcmp(sel, "class")) { RET(cside ? self : (in_img(hcls) || hclass_find(hcls) ? hcls : ld32(self))); return; }
    if (!strcmp(sel, "isKindOfClass:") || !strcmp(sel, "isMemberOfClass:")) { RET(!cside && ld32(self) && is_kind(ld32(self), class_name(a[0]))); return; }
    if (!strcmp(sel, "respondsToSelector:")) { RET(find_method(cside ? ld32(self) : ld32(self), S(a[0]), NULL) != 0); return; }
    if (!strcmp(sel, "isEqual:")) { RET(self == a[0]); return; }
    if (!strcmp(sel, "hash")) { RET(self); return; }
    if (!strcmp(sel, "description")) { RET(mkstr(cn)); return; }
    if (cside) {
        if (!strcmp(sel, "alloc") || !strcmp(sel, "allocWithZone:")) {
            RET(alloc_guest(c, self)); return;
        }
        if (!strcmp(sel, "new")) { RET(alloc_guest(c, self)); return; }
        if (name_kind(cn) && !is_guest_class(self)) {
            int k = name_kind(cn); uint32_t o = g_calloc(0x200); st32(o, self);
            if (!strcmp(sel, "array") || !strcmp(sel, "set") || !strcmp(sel, "dictionary") || !strncmp(sel, "arrayWithCapacity", 17) || !strncmp(sel, "dictionaryWithCapacity", 22) || !strncmp(sel, "setWithCapacity", 15)) { RET(o); return; }
            if (!strcmp(sel, "arrayWithObject:") || !strcmp(sel, "setWithObject:")) { coll_send(c, o, "addObject:", a, k); RET(o); return; }
            if (!strcmp(sel, "arrayWithArray:") || !strcmp(sel, "setWithArray:") || !strcmp(sel, "setWithSet:")) { coll_send(c, o, "addObjectsFromArray:", a, k); RET(o); return; }
            if (!strcmp(sel, "dictionaryWithObjects:forKeys:count:") && k == 3) { for (uint32_t i = 0; i < a[2]; i++) { uint32_t kv[2] = { ld32(a[0] + 4 * i), ld32(a[1] + 4 * i) }; coll_send(c, o, "setObject:forKey:", kv, 3); } RET(o); return; }
            if (!strcmp(sel, "dictionaryWithObject:forKey:") && k == 3) { coll_send(c, o, "setObject:forKey:", a, 3); RET(o); return; }
        }
        if (!strcmp(sel, "load") || !strcmp(sel, "initialize")) { RET(0); return; }
        if (!strncmp(sel, "shared", 6) || !strncmp(sel, "default", 7) || !strncmp(sel, "standard", 8) || !strncmp(sel, "main", 4) || !strncmp(sel, "current", 7) || !strcmp(sel, "sharedInstance")) {
            RET(singleton(cn, sel)); return;
        }
        if (!strcmp(cn, "NSLocale") && !strcmp(sel, "preferredLanguages") && rt_genv("XW_LANG") && *rt_genv("XW_LANG")) {   /* game language chosen in the launcher */
            uint32_t el[1] = { mkstr(rt_genv("XW_LANG")) }; RET(mkarray(el, 1)); return;
        }
        if (!strcmp(cn, "GCController")) {
            if (!strcmp(sel, "controllers")) {
                PadState p; uint32_t el[1]; el[0] = 0;
                if (rt_pad_poll(&p)) { g_pad = p; gc_ensure(); el[0] = g_ctl; g_pad_was = 1; rt_log("[pad] +controllers -> %s\n", p.name); }
                RET(mkarray(el, el[0] ? 1 : 0)); return;
            }
            if (!strcmp(sel, "startWirelessControllerDiscoveryWithCompletionHandler:")) { call_block(c, a[0], 0, 0); RET(0); return; }
            if (!strcmp(sel, "stopWirelessControllerDiscovery")) { RET(0); return; }
        }
        if (!strcmp(cn, "NSString")) {
            if (!strcmp(sel, "stringWithUTF8String:") || !strcmp(sel, "stringWithCString:encoding:") || !strcmp(sel, "stringWithCString:")) { RET(a[0] ? mkstr(S(a[0])) : 0); return; }
            if (!strcmp(sel, "stringWithFormat:")) { char buf[4096]; VA va = { c, 0, 0 }; (void)va; const char *fmt = cstr_of(a[0]); strncpy(buf, fmt ? fmt : "", sizeof buf - 1); buf[sizeof buf - 1] = 0; RET(mkstr(buf)); return; }
        }
        if (!strcmp(cn, "UIDevice")) {}
        if (!strcmp(cn, "NSBundle") && !strcmp(sel, "mainBundle")) { RET(singleton("NSBundle", "mainBundle")); return; }
    } else {
        if (!strncmp(sel, "init", 4)) { RET(self); return; }
        if (!strcmp(cn, "NSNotificationCenter")) {     /* remember observers: the controller (dis)connect notifications are delivered by rt_pad_frame */
            if (!strcmp(sel, "addObserver:selector:name:object:")) { if (g_nobs < 48 && a[1]) { g_obs[g_nobs].obs = a[0]; g_obs[g_nobs].sel = a[1]; g_nobs++; } RET(0); return; }
            if (!strcmp(sel, "removeObserver:") || !strcmp(sel, "removeObserver:name:object:")) { for (int i = 0; i < g_nobs; i++) if (g_obs[i].obs == a[0]) g_obs[i].sel = 0; RET(0); return; }
        }
        if (!strcmp(cn, "NSBundle") || !strcmp(cn, "NSObject")) {
            char p[MAX_PATH];
            if (!strcmp(sel, "bundlePath") || !strcmp(sel, "resourcePath")) { app_path(p, sizeof p, ""); RET(mkstr(p)); return; }
            if (!strcmp(sel, "pathForResource:ofType:")) {
                const char *n = cstr_of(a[0]), *t = cstr_of(a[1]); if (!n) { RET(0); return; }
                app_path(p, sizeof p, ""); strcat(p, "\\"); strcat(p, n); if (t && *t) { strcat(p, "."); strcat(p, t); }
                RET(GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES ? mkstr(p) : 0); return;
            }
            if (!strcmp(sel, "objectForInfoDictionaryKey:")) { RET(0); return; }
        }
        const char *s = cstr_of(self);
        if (s) {
            if (!strcmp(sel, "UTF8String") || !strcmp(sel, "cString") || !strcmp(sel, "fileSystemRepresentation")) { RET(ld32(self + 8)); return; }
            if (!strcmp(sel, "length")) { RET(ld32(self + 12)); return; }
            if (!strcmp(sel, "isEqualToString:")) { const char *o = cstr_of(a[0]); RET(o && !strcmp(s, o)); return; }
            if (!strcmp(sel, "hasPrefix:")) { const char *o = cstr_of(a[0]); RET(o && !strncmp(s, o, strlen(o))); return; }
            if (!strcmp(sel, "hasSuffix:")) { const char *o = cstr_of(a[0]); size_t l = strlen(s), m = o ? strlen(o) : 0; RET(o && l >= m && !strcmp(s + l - m, o)); return; }
            if (!strcmp(sel, "intValue") || !strcmp(sel, "integerValue")) { RET(atoi(s)); return; }
            if (!strcmp(sel, "floatValue")) { RETF((float)atof(s)); return; }
            if (!strcmp(sel, "stringByAppendingString:") || !strcmp(sel, "stringByAppendingPathComponent:")) {
                const char *o = cstr_of(a[0]); char buf[1024]; snprintf(buf, sizeof buf, "%s%s%s", s, sel[16] == 'P' ? "\\" : "", o ? o : ""); RET(mkstr(buf)); return;
            }
            if (!strcmp(sel, "stringByDeletingLastPathComponent")) { char buf[1024]; strncpy(buf, s, 1023); buf[1023] = 0; char *q = strrchr(buf, '\\'); if (!q) q = strrchr(buf, '/'); if (q) *q = 0; RET(mkstr(buf)); return; }
            if (!strcmp(sel, "lowercaseString")) { char buf[1024]; strncpy(buf, s, 1023); buf[1023] = 0; _strlwr(buf); RET(mkstr(buf)); return; }
        }
        { int hk = !cside && self ? host_kind(self) : 0; if (hk && coll_send(c, self, sel, a, hk)) return; }
        if (is_array(self)) {
            if (!strcmp(sel, "count")) { RET(ld32(self + 4)); return; }
            if (!strcmp(sel, "objectAtIndex:")) { RET(a[0] < ld32(self + 4) ? ld32(ld32(self + 8) + 4 * a[0]) : 0); return; }
            if (!strcmp(sel, "countByEnumeratingWithState:objects:count:")) {
                uint32_t st = a[0];
                if (ld32(st) == 0) { st32(st, 1); st32(st + 4, ld32(self + 8)); st32(st + 8, st + 24); RET(ld32(self + 4)); } else RET(0);
                return;
            }
            if (!strcmp(sel, "anyObject")) { RET(ld32(self + 4) ? ld32(ld32(self + 8)) : 0); return; }
            if (!strcmp(sel, "firstObject")) { RET(ld32(self + 4) ? ld32(ld32(self + 8)) : 0); return; }
        }
        if (stret_ptr && (!strcmp(sel, "frame") || !strcmp(sel, "bounds") || !strcmp(sel, "applicationFrame"))) {
            float r[4] = { 0, 0, (float)g_gl_w_pt(), (float)g_gl_h_pt() }; memcpy(GP(stret_ptr), r, 16); RET(0); return;
        }
        if (!strcmp(sel, "contentScaleFactor") || !strcmp(sel, "scale") || !strcmp(sel, "nativeScale")) { RETF(1.0f); return; }
        if (!strcmp(cn, "UIScreen")) {
            if (!strcmp(sel, "bounds") || !strcmp(sel, "applicationFrame") || !strcmp(sel, "nativeBounds")) {
                if (stret_ptr) { float r[4] = { 0, 0, (float)g_gl_w_pt(), (float)g_gl_h_pt() }; memcpy(GP(stret_ptr), r, 16); } RET(0); return;
            }
            if (!strcmp(sel, "scale") || !strcmp(sel, "nativeScale")) { RETF(1.0f); return; }
        }
        if (!strcmp(sel, "boolForKey:") || !strcmp(sel, "integerForKey:") || !strcmp(sel, "objectForKey:") || !strcmp(sel, "stringForKey:")) { RET(0); return; }
        if (!strcmp(sel, "synchronize")) { RET(1); return; }
        if (!strcmp(sel, "delegate") && !strcmp(cn, "UIApplication")) { RET(g_delegate); return; }
        if (!strcmp(sel, "setRootViewController:")) { hprop_set(self, "root", a[0]); RET(0); return; }
        if (!strcmp(sel, "rootViewController") || !strcmp(sel, "keyWindow")) { RET(hprop_get(self, "root")); return; }
        if (!strcmp(sel, "layer")) { uint32_t l = hprop_get(self, "layer"); if (!l) { l = g_calloc(64); st32(l, hclass_get("CAEAGLLayer")); hprop_set(self, "layer", l); } RET(l); return; }
        if (!strcmp(sel, "setView:")) { hprop_set(self, "view", a[0]); RET(0); return; }
        if (!strcmp(sel, "view") && hprop_get(self, "view")) { RET(hprop_get(self, "view")); return; }
        if (!strcmp(cn, "SKReceiptRefreshRequest")) {            /* pretend the receipt refresh succeeded */
            if (!strcmp(sel, "setDelegate:")) { st32(self + 8, a[0]); RET(0); return; }
            if (!strcmp(sel, "start")) { uint32_t d = ld32(self + 8); extern void rt_send_t(CPU *, uint32_t, uint32_t); if (d) { CPU t = *c; t.r[0] = d; t.r[2] = self; t.r[3] = 0; t.r[14] = 0; rt_send_t(&t, d, intern_sel_p("requestDidFinish:")); } RET(0); return; }
        }
        if (!strncmp(sel, "performSelector:", 16)) {          /* run immediately; delays are ignored */
            CPU t = *c; t.r[0] = self; t.r[1] = a[0]; t.r[2] = a[1]; t.r[3] = 0; t.r[14] = 0;
            extern void rt_send_t(CPU *, uint32_t, uint32_t);
            rt_send_t(&t, self, a[0]); c->r[0] = t.r[0]; return;
        }
        if (!strcmp(sel, "presentRenderbuffer:")) { rt_present(c); RET(1); return; }
        if (!strcmp(sel, "renderbufferStorage:fromDrawable:")) { extern void rt_gl_drawable_storage(void); rt_gl_drawable_storage(); RET(1); return; }
        if (!strcmp(sel, "setCurrentContext:")) { RET(1); return; }
    }
    static char seen[512][64]; static int n;
    char key[96]; snprintf(key, sizeof key, "%s%c%s", cn, cside ? '+' : '-', sel);
    for (int i = 0; i < n; i++) if (!strcmp(seen[i], key)) { RET(0); return; }
    if (n < 512) { strncpy(seen[n], key, 63); n++; rt_log("[objc] %s (self=%x)\n", key, self); }
    RET(0);
}
int g_gl_w_pt(void) { return g_gl_w; }
int g_gl_h_pt(void) { return g_gl_h; }

#define CACHEN 8192
static struct { uint32_t cls, sel, imp; } mcache[CACHEN];

static void send_common(CPU *c, uint32_t self, uint32_t selp, uint32_t start_cls, int stret) {
    if (!self && !strcmp(S(selp), "isReadyToUse")) { c->r[0] = 1; c->r[1] = 0; return; }
    if (!self) { if (g_trace) { static char seen[512][48]; static int n; int k; for (k = 0; k < n; k++) if (!strcmp(seen[k], S(selp))) break; if (k == n && n < 512) { strncpy(seen[n++], S(selp), 47); rt_log("[nil] -%s lr=%x\n", S(selp), c->r[14]); } } c->r[0] = 0; c->r[1] = 0; return; }
    uint32_t isa = start_cls ? start_cls : ld32(self), imp = 0, hcls = 0;
    unsigned h = ((isa >> 2) ^ (selp >> 2) * 31u) & (CACHEN - 1);
    if (mcache[h].cls == isa && mcache[h].sel == selp) imp = mcache[h].imp;
    else {
        imp = find_method(isa, S(selp), &hcls);
        if (imp) { mcache[h].cls = isa; mcache[h].sel = selp; mcache[h].imp = imp; }
    }
    if (g_trace > 1) rt_log("[send] %s %c%s -> %x\n", class_name(is_guest_class(isa) ? isa : self), is_guest_class(self) ? '+' : '-', S(selp), imp);
    if (imp) { arm_dispatch(c, imp); if (g_trace > 2) rt_log("[ret] %s -> %x\n", S(selp), c->r[0]); return; }
    uint32_t a[8]; const int base = stret ? 3 : 2;
    for (int i = 0; i < 8; i++) a[i] = ARG(base + i);
    host_send(c, self, S(selp), hcls, a, stret ? c->r[0] : 0);
}
SHIM(_objc_msgSend) { send_common(c, c->r[0], c->r[1], 0, 0); }
SHIM(_objc_msgSend_stret) { send_common(c, c->r[1], c->r[2], 0, 1); }
SHIM(_objc_msgSendSuper2) {
    uint32_t sp = c->r[0], recv = ld32(sp), cls = ld32(sp + 4);
    c->r[0] = recv; send_common(c, recv, c->r[1], ld32(cls + 4), 0);
}
SHIM(_objc_alloc) {
    RET(alloc_guest(c, c->r[0]));
}
SHIM(_objc_constructInstance) { st32(c->r[1], c->r[0]); RET(c->r[1]); }

/* ------------------------------------------------------------ objc C API */
static uint32_t cls_by_name(const char *n) { uint32_t g = guest_class_by_name(n); return g ? g : hclass_get(n); }
SHIM(_objc_getClass) { RET(cls_by_name(S(ARG(0)))); }
SHIM(_objc_lookUpClass) { RET(cls_by_name(S(ARG(0)))); }
SHIM(_objc_getRequiredClass) { RET(cls_by_name(S(ARG(0)))); }
SHIM(_objc_getMetaClass) { uint32_t k = cls_by_name(S(ARG(0))); RET(is_guest_class(k) ? ld32(k) : k); }
SHIM(_objc_getProtocol) { RET(0); }
SHIM(_NSClassFromString) { const char *s = cstr_of(ARG(0)); RET(s ? cls_by_name(s) : 0); }
SHIM(_NSStringFromClass) { RET(mkstr(class_name(ARG(0)))); }
SHIM(_object_getClass) { RET(ARG(0) ? ld32(ARG(0)) : 0); }
SHIM(_class_getSuperclass) { RET(in_img(ARG(0)) ? ld32(ARG(0) + 4) : 0); }
SHIM(_class_isMetaClass) { RET(in_img(ARG(0)) ? (ld32(ro_of(ARG(0))) & 1) : 0); }
SHIM(_class_getInstanceSize) { RET(in_img(ARG(0)) ? ld32(ro_of(ARG(0)) + 8) : 0x200); }
SHIM(_class_respondsToSelector) { RET(find_method(ARG(0), S(ARG(1)), NULL) != 0); }
SHIM(_class_getInstanceMethod) { RET(0); }
SHIM(_class_getProperty) { RET(0); }
SHIM(_class_getIvarLayout) { RET(0); }
SHIM(_class_addMethod) { RET(0); }
SHIM(_class_replaceMethod) { RET(0); }
SHIM(_class_addProperty) { RET(0); }
SHIM(_class_addProtocol) { RET(0); }
SHIM(_method_setImplementation) { RET(0); }
SHIM(_objc_initializeClassPair) { RET(0); }
SHIM(_objc_registerClassPair) { (void)c; }
SHIM(_property_copyAttributeList) { RET(0); }
SHIM(_protocol_getMethodDescription) { c->r[0] = 0; c->r[1] = 0; }
SHIM(_class_getInstanceVariable) {
    for (uint32_t k = ARG(0); in_img(k); k = ld32(k + 4)) {
        uint32_t il = ld32(ro_of(k) + 28); if (!il) continue;
        for (uint32_t i = 0; i < ld32(il + 4); i++) { uint32_t e = il + 8 + 20 * i; if (!strcmp(S(ld32(e + 4)), S(ARG(1)))) { RET(e); return; } }
    }
    RET(0);
}
SHIM(_ivar_getName) { RET(ARG(0) ? ld32(ARG(0) + 4) : 0); }
SHIM(_ivar_getOffset) { RET(ARG(0) ? ld32(ld32(ARG(0))) : 0); }
SHIM(_object_getIvar) { RET(ARG(1) ? ld32(ARG(0) + ld32(ld32(ARG(1)))) : 0); }
SHIM(_object_setIvar) { if (ARG(1)) st32(ARG(0) + ld32(ld32(ARG(1))), ARG(2)); }
static uint32_t intern_sel(const char *n) {
    static struct { char *n; uint32_t p; } t[2048]; static int cnt;
    for (int i = 0; i < cnt; i++) if (!strcmp(t[i].n, n)) return t[i].p;
    uint32_t p = g_strdup(n); if (cnt < 2048) { t[cnt].n = _strdup(n); t[cnt++].p = p; } return p;
}
SHIM(_sel_getUid) { RET(intern_sel(S(ARG(0)))); }
SHIM(_objc_retain) { RET(ARG(0)); }
SHIM(_objc_release) { (void)c; }
SHIM(_objc_autorelease) { RET(ARG(0)); }
SHIM(_objc_autoreleaseReturnValue) { RET(ARG(0)); }
SHIM(_objc_retainAutorelease) { RET(ARG(0)); }
SHIM(_objc_retainAutoreleaseReturnValue) { RET(ARG(0)); }
SHIM(_objc_retainAutoreleasedReturnValue) { RET(ARG(0)); }
SHIM(_objc_storeStrong) { st32(ARG(0), ARG(1)); }
SHIM(_objc_initWeak) { st32(ARG(0), ARG(1)); RET(ARG(1)); }
SHIM(_objc_storeWeak) { st32(ARG(0), ARG(1)); RET(ARG(1)); }
SHIM(_objc_loadWeakRetained) { RET(ld32(ARG(0))); }
SHIM(_objc_destroyWeak) { (void)c; }
SHIM(_objc_copyWeak) { st32(ARG(0), ld32(ARG(1))); }
SHIM(_objc_setProperty_atomic) { st32(ARG(0) + ARG(3), ARG(2)); }
SHIM(_objc_setProperty_atomic_copy) { st32(ARG(0) + ARG(3), ARG(2)); }
SHIM(_objc_setProperty_nonatomic_copy) { st32(ARG(0) + ARG(3), ARG(2)); }
SHIM(_objc_getProperty) { RET(ld32(ARG(0) + ARG(2))); }
SHIM(_objc_sync_enter) { RET(0); }
SHIM(_objc_sync_exit) { RET(0); }
SHIM(_objc_autoreleasePoolPush) { RET(1); }
SHIM(_objc_autoreleasePoolPop) { (void)c; }
SHIM(_objc_enumerationMutation) { (void)c; }
SHIM(__Block_copy) {
    uint32_t blk = ARG(0); if (!blk) { RET(0); return; }
    uint32_t flags = ld32(blk + 8), desc = ld32(blk + 16);
    if ((flags & (1u << 24)) || (flags & (1u << 28)) || !desc) { RET(blk); return; }   /* already heap / global */
    uint32_t sz = ld32(desc + 4); if (sz < 20 || sz > 4096) { RET(blk); return; }
    uint32_t cp = g_malloc(sz); memcpy(GP(cp), GP(blk), sz); st32(cp + 8, flags | (1u << 24)); st32(cp + 4, 0);
    RET(cp);
}
SHIM(__Block_object_assign) { st32(ARG(0), ARG(1)); }
SHIM(__Block_object_dispose) { (void)c; }
SHIM(_dispatch_get_global_queue) { RET(1); }
SHIM(_dispatch_async) { g_call(c, ld32(ARG(1) + 12), ARG(1), 0, 0, 0); }
SHIM(_dispatch_sync) { g_call(c, ld32(ARG(1) + 12), ARG(1), 0, 0, 0); }
SHIM(_dispatch_once) { if (!ld32(ARG(0))) { st32(ARG(0), ~0u); g_call(c, ld32(ARG(1) + 12), ARG(1), 0, 0, 0); } }
SHIM(_dispatch_once_f) { if (!ld32(ARG(0))) { st32(ARG(0), ~0u); g_call(c, ARG(2), ARG(1), 0, 0, 0); } }
SHIM(_dispatch_semaphore_create) { RET(g_calloc(16)); }
SHIM(_dispatch_semaphore_wait) { RET(0); }
SHIM(_dispatch_semaphore_signal) { RET(0); }
SHIM(_NSHomeDirectory) { RET(mkstr(docs_dir())); }
SHIM(_NSSearchPathForDirectoriesInDomains) { uint32_t s = mkstr(docs_dir()); RET(mkarray(&s, 1)); }
SHIM(_NSLog) {
    const char *f = cstr_of(ARG(0)); if (!f) return;
    char buf[2048]; VA va = { c, 1, 0 };
    char fixed[2048]; size_t j = 0; for (const char *p = f; *p && j < sizeof fixed - 2; p++) { if (p[0] == '%' && p[1] == '@') { fixed[j++] = '%'; fixed[j++] = 'x'; p++; } else fixed[j++] = *p; }
    fixed[j] = 0; g_format(buf, sizeof buf, fixed, &va); rt_log("[NSLog] %s\n", buf);
}
SHIM(_UIApplicationMain) { rt_log("UIApplicationMain bypassed\n"); }
SHIM(_UIAccessibilityIsVoiceOverRunning) { RET(0); }
SHIM(_MTLCreateSystemDefaultDevice) { RET(0); }
SHIM(_CFRelease) { (void)c; }
SHIM(_CFRunLoopGetCurrent) { RET(1); }
SHIM(_CFStringGetCStringPtr) { const char *s = cstr_of(ARG(0)); RET(s ? ld32(ARG(0) + 8) : 0); }
SHIM(__dyld_register_func_for_add_image) { (void)c; }

void rt_send_t(CPU *t, uint32_t self, uint32_t sel) { send_common(t, self, sel, 0, 0); }
/* ------------------------------------------------------------ app launch + frame loop */
static uint32_t g_delegate;
static uint32_t send0(CPU *c, uint32_t obj, const char *sel, uint32_t a0, uint32_t a1) {
    CPU t = *c; t.r[0] = obj; t.r[1] = intern_sel(sel); t.r[2] = a0; t.r[3] = a1; t.r[14] = 0;
    send_common(&t, obj, t.r[1], 0, 0); return t.r[0];
}
extern int g_touch_down, g_touch_x, g_touch_y;
extern volatile unsigned char g_keys[256];
/* three virtual fingers: 0 = mouse, 1 = WASD (left stick), 2 = arrow keys (right stick = aim + fire) */
typedef struct { int down, was; float x, y, lx, ly; uint32_t touch, event, set; } Finger;
static void finger_update(CPU *c, Finger *f) {
    const char *sel = NULL; uint32_t phase = 0;
    if (f->down && !f->was) { sel = "touchesBegan:withEvent:"; phase = 0; f->lx = f->x; f->ly = f->y; }
    else if (f->down && (f->x != f->lx || f->y != f->ly)) { sel = "touchesMoved:withEvent:"; phase = 1; }
    else if (!f->down && f->was) { sel = "touchesEnded:withEvent:"; phase = 3; }
    f->was = f->down;
    if (!sel) return;
    if (!f->touch) {
        f->touch = g_calloc(0x40); st32(f->touch, hclass_get("UITouch")); hprop_set(f->touch, "touch", 1);
        f->event = g_calloc(0x40); st32(f->event, hclass_get("UIEvent")); hprop_set(f->event, "uievent", 1);
        uint32_t el[1] = { f->touch }; f->set = mkarray(el, 1); hprop_set(f->event, "set", f->set);
    }
    uint32_t bx, by, px, py; memcpy(&bx, &f->x, 4); memcpy(&by, &f->y, 4); memcpy(&px, &f->lx, 4); memcpy(&py, &f->ly, 4);
    hprop_set(f->touch, "px", px); hprop_set(f->touch, "py", py); hprop_set(f->touch, "x", bx); hprop_set(f->touch, "y", by); hprop_set(f->touch, "phase", phase);
    f->lx = f->x; f->ly = f->y;
    send0(c, g_touch_view, sel, f->set, f->event);
}
static void key_stick(Finger *f, int l, int r, int u, int d, float cx, float cy, float rad) {
    float dx = (float)(g_keys[r] ? 1 : 0) - (g_keys[l] ? 1 : 0), dy = (float)(g_keys[d] ? 1 : 0) - (g_keys[u] ? 1 : 0);
    if (dx == 0 && dy == 0) { f->down = 0; return; }
    float n = sqrtf(dx * dx + dy * dy); f->down = 1; f->x = cx + dx / n * rad; f->y = cy + dy / n * rad;
}
void rt_objc_frame_input(CPU *c) {
    static Finger fm, fl, fr2;
    if (!g_touch_view) return;
    { static int fr, nt = -1, tf[16], tx[16], ty[16]; if (nt < 0) { nt = 0; const char *e = rt_genv("XW_TAP"); while (e && *e && nt < 16) { if (sscanf(e, "%d:%d:%d", &tf[nt], &tx[nt], &ty[nt]) == 3) nt++; e = strchr(e, ','); if (e) e++; } } fr++;
      for (int k = 0; k < nt; k++) { if (fr == tf[k]) { g_touch_down = 1; g_touch_x = tx[k]; g_touch_y = ty[k]; } if (fr == tf[k] + 6) g_touch_down = 0; } }
    fm.down = g_touch_down; fm.x = (float)g_touch_x; fm.y = (float)g_touch_y;
    float s = (float)g_gl_w_pt() / 1008.0f;
    static int kbm = -1; if (kbm < 0) kbm = rt_genv("XW_KBM") ? atoi(rt_genv("XW_KBM")) : 0;
    if (!kbm) {
    key_stick(&fl, 'A', 'D', 'W', 'S', 150 * s, 605 * s, 40 * s);
    key_stick(&fr2, VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, 873 * s, 605 * s, 40 * s);
    }
    finger_update(c, &fm); finger_update(c, &fl); finger_update(c, &fr2);
}
void rt_app_run(CPU *c) {
    uint32_t cls = guest_class_by_name("iFuseAppDelegate");
    g_delegate = alloc_guest(c, cls);
    uint32_t app = singleton("UIApplication", "sharedApplication");
    rt_log("launching: -[iFuseAppDelegate applicationDidFinishLaunching:]\n");
    send0(c, g_delegate, "applicationDidFinishLaunching:", app, 0);
    rt_log("launch returned; entering frame loop\n");
    if (rt_genv("XW_PROF")) { extern void rt_prof_start(void); rt_prof_start(); }
    for (;;) {
        LARGE_INTEGER q0; QueryPerformanceCounter(&q0);
        send0(c, g_delegate, "runFrame:", 0, 0);
        if (rt_genv("XW_PROF")) { extern void rt_prof_frame(float); LARGE_INTEGER q1, qf; QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&qf); rt_prof_frame((float)((double)(q1.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart)); }
        if (rt_genv("XW_SPIKE")) { LARGE_INTEGER q1, qf; QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&qf); double ms = (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart; if (ms > 12.0) rt_log("[spike] runFrame %.1f ms\n", ms); }
        { static int tf = -2; if (tf == -2) { const char *e = rt_genv("XW_FPS"); tf = e ? atoi(e) : 60; if (tf < 0 || tf > 1000) tf = 60; if (tf == 0) tf = 500; }
          if (tf != 60) { uint32_t g = ld32(0x4cfcf8); if (g) { double dd = 1.0 / tf; uint64_t q; memcpy(&q, &dd, 8); st32(g + 0x20, (uint32_t)q); st32(g + 0x24, (uint32_t)(q >> 32)); } } }
        if (rt_genv("XW_LOOPDBG")) { extern volatile int g_presented; static int it; static DWORD t0; if (++it % 600 == 0) { DWORD t = GetTickCount(); rt_log("[loop] %d runFrame calls in %lu ms, presents so far %d\n", 600, (unsigned long)(t - t0), g_presented); t0 = t; } }
        { extern void rt_audio_pump(CPU *c); rt_audio_pump(c); }
        rt_pad_frame(c);
        { extern volatile int g_presented; static int lastp; if (g_presented == lastp) { extern void rt_sleep_us(int); rt_sleep_us(400); } lastp = g_presented; }
        rt_pump();
        { static int fr; if (++fr % 3000 == 0) { extern int g_cnt_clear, g_cnt_draw, g_cnt_shader, g_cnt_read; extern unsigned long long g_cnt_read_bytes; rt_log("[dbg] gl clear=%d draw=%d shaders=%d reads=%d bytes=%llu\n", g_cnt_clear, g_cnt_draw, g_cnt_shader, g_cnt_read, g_cnt_read_bytes); }
        if (fr == 200 || fr == 2000) { uint32_t g = ld32(0x4cfcf8), vt = g ? ld32(g) : 0; rt_log("[dbg] frame %d: g=%x flag=%x vt=%x fn=%x\n", fr, g, g ? ld8(g + 4) : 0, vt, vt ? ld32(vt + 0x24) : 0); if (g) { double dd; uint32_t lo = ld32(g + 0x20), hi = ld32(g + 0x24); uint64_t q = ((uint64_t)hi << 32) | lo; memcpy(&dd, &q, 8); rt_log("[dbg] step@g+0x20=%g flag1cc=%d\n", dd, ld8(g - 0x754 + 0x1cc)); } } }
    }
}
