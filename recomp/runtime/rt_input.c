/* rt_input.c - gamepad backends: XInput (Xbox & compatible) and DirectInput 8 (everything else: DualShock/DualSense, generic HID).
 * rt_pad_poll() fills a normalised PadState (Xbox-style layout) that the GameController emulation in rt_objc.c exposes to the game. */
#define DIRECTINPUT_VERSION 0x0800
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dinput.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rt.h"

#include "rt_pad.h"

extern HWND g_hwnd;

/* ---------------------------------------------------------------- XInput */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lX, lY, rX, rY; } XI_GAMEPAD;
typedef struct { DWORD packet; XI_GAMEPAD pad; } XI_STATE;
typedef DWORD (WINAPI *PFN_XIGetState)(DWORD, XI_STATE *);
static PFN_XIGetState p_xi; static int xi_tried;
static int xi_slot = -1; static DWORD xi_next_scan;

static void xi_load(void) {
    if (xi_tried) return; xi_tried = 1;
    static const char *const dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (int i = 0; i < 3 && !p_xi; i++) { HMODULE m = LoadLibraryA(dlls[i]); if (m) p_xi = (PFN_XIGetState)GetProcAddress(m, "XInputGetState"); }
    if (p_xi) rt_log("[pad] XInput available\n"); else rt_log("[pad] XInput not available\n");
}
static float stick(SHORT v, float dead) {
    float f = v < 0 ? v / 32768.0f : v / 32767.0f; float a = fabsf(f);
    if (a < dead) return 0; a = (a - dead) / (1 - dead); if (a > 1) a = 1; return f < 0 ? -a : a;
}
static int xi_poll(PadState *p, float dead) {
    if (!p_xi) return 0;
    XI_STATE s; int slot = -1;
    if (xi_slot >= 0 && p_xi(xi_slot, &s) == ERROR_SUCCESS) slot = xi_slot;
    else {   /* scanning empty slots is slow on some systems: rate-limit it */
        DWORD now = GetTickCount(); if (now < xi_next_scan) return 0; xi_next_scan = now + 3000; xi_slot = -1;
        for (int i = 0; i < 4; i++) if (p_xi(i, &s) == ERROR_SUCCESS) { slot = i; break; }
        if (slot < 0) return 0;
        xi_slot = slot; rt_log("[pad] XInput controller on slot %d\n", slot);
    }
    WORD b = s.pad.wButtons; unsigned o = 0;
    if (b & 0x1000) o |= PB_A; if (b & 0x2000) o |= PB_B; if (b & 0x4000) o |= PB_X; if (b & 0x8000) o |= PB_Y;
    if (b & 0x0100) o |= PB_LB; if (b & 0x0200) o |= PB_RB; if (b & 0x0020) o |= PB_BACK; if (b & 0x0010) o |= PB_START;
    if (b & 0x0040) o |= PB_LS; if (b & 0x0080) o |= PB_RS;
    if (b & 1) o |= PB_UP; if (b & 2) o |= PB_DOWN; if (b & 4) o |= PB_LEFT; if (b & 8) o |= PB_RIGHT;
    p->lx = stick(s.pad.lX, dead); p->ly = stick(s.pad.lY, dead); p->rx = stick(s.pad.rX, dead); p->ry = stick(s.pad.rY, dead);
    p->lt = s.pad.bLT > 30 ? (s.pad.bLT - 30) / 225.0f : 0; p->rt = s.pad.bRT > 30 ? (s.pad.bRT - 30) / 225.0f : 0;
    p->buttons = o; p->api = 1; strcpy(p->name, "Xbox Controller (XInput)");
    return 1;
}

/* ---------------------------------------------------------------- DirectInput 8 */
static IDirectInput8A *g_di; static IDirectInputDevice8A *g_dev; static int di_ds4; static char di_name[64];
static DWORD di_next_scan; static int di_xinput_dev;

/* XInput-capable devices also show up in DirectInput; they are handled by XInput, so skip them (the well-known "IG_" check) */
static int is_xinput_device(const GUID *prod) {
    UINT n = 0; if (GetRawInputDeviceList(NULL, &n, sizeof(RAWINPUTDEVICELIST)) != 0 || !n) return 0;
    RAWINPUTDEVICELIST *l = (RAWINPUTDEVICELIST *)malloc(sizeof *l * n); int found = 0;
    if (GetRawInputDeviceList(l, &n, sizeof *l) != (UINT)-1) {
        for (UINT i = 0; i < n && !found; i++) {
            if (l[i].dwType != RIM_TYPEHID) continue;
            RID_DEVICE_INFO inf; inf.cbSize = sizeof inf; UINT sz = sizeof inf;
            if ((int)GetRawInputDeviceInfoA(l[i].hDevice, RIDI_DEVICEINFO, &inf, &sz) <= 0) continue;
            if (MAKELONG(inf.hid.dwVendorId, inf.hid.dwProductId) != (LONG)prod->Data1) continue;
            char name[256]; UINT ns = sizeof name; if ((int)GetRawInputDeviceInfoA(l[i].hDevice, RIDI_DEVICENAME, name, &ns) > 0 && strstr(name, "IG_")) found = 1;
        }
    }
    free(l); return found;
}
static BOOL CALLBACK di_enum(const DIDEVICEINSTANCEA *inst, void *ctx) {
    if (is_xinput_device(&inst->guidProduct)) return DIENUM_CONTINUE;
    IDirectInputDevice8A *d = NULL;
    if (FAILED(IDirectInput8_CreateDevice(g_di, &inst->guidInstance, &d, NULL))) return DIENUM_CONTINUE;
    if (FAILED(IDirectInputDevice8_SetDataFormat(d, &c_dfDIJoystick2))) { IDirectInputDevice8_Release(d); return DIENUM_CONTINUE; }
    IDirectInputDevice8_SetCooperativeLevel(d, g_hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    DIPROPRANGE r; memset(&r, 0, sizeof r); r.diph.dwSize = sizeof r; r.diph.dwHeaderSize = sizeof r.diph; r.diph.dwHow = DIPH_DEVICE; r.lMin = -1000; r.lMax = 1000;
    static const DWORD axes[] = { DIJOFS_X, DIJOFS_Y, DIJOFS_Z, DIJOFS_RX, DIJOFS_RY, DIJOFS_RZ };
    for (int i = 0; i < 6; i++) { r.diph.dwObj = axes[i]; r.diph.dwHow = DIPH_BYOFFSET; IDirectInputDevice8_SetProperty(d, DIPROP_RANGE, &r.diph); }
    g_dev = d; strncpy(di_name, inst->tszProductName, 63); di_name[63] = 0;
    di_ds4 = strstr(di_name, "Wireless Controller") || strstr(di_name, "DualShock") || strstr(di_name, "DualSense") || strstr(di_name, "PS4") || strstr(di_name, "PS5");
    rt_log("[pad] DirectInput device: %s (%s layout)\n", di_name, di_ds4 ? "PlayStation" : "generic");
    (void)ctx; return DIENUM_STOP;
}
static float dstick(LONG v, float dead) { float f = v / 1000.0f; if (f > 1) f = 1; if (f < -1) f = -1; float a = fabsf(f); if (a < dead) return 0; a = (a - dead) / (1 - dead); return f < 0 ? -a : a; }
static int di_poll(PadState *p, float dead) {
    if (!g_di) { if (FAILED(DirectInput8Create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8A, (void **)&g_di, NULL))) return 0; }
    if (!g_dev) {
        DWORD now = GetTickCount(); if (now < di_next_scan) return 0; di_next_scan = now + 5000;
        IDirectInput8_EnumDevices(g_di, DI8DEVCLASS_GAMECTRL, di_enum, NULL, DIEDFL_ATTACHEDONLY);
        if (!g_dev) return 0;
    }
    HRESULT hr = IDirectInputDevice8_Poll(g_dev);
    if (FAILED(hr)) { hr = IDirectInputDevice8_Acquire(g_dev); if (FAILED(hr)) { if (hr == DIERR_UNPLUGGED || hr == DIERR_INPUTLOST) { IDirectInputDevice8_Release(g_dev); g_dev = NULL; rt_log("[pad] DirectInput device lost\n"); } return 0; } IDirectInputDevice8_Poll(g_dev); }
    DIJOYSTATE2 s; if (FAILED(IDirectInputDevice8_GetDeviceState(g_dev, sizeof s, &s))) { IDirectInputDevice8_Release(g_dev); g_dev = NULL; return 0; }
    unsigned o = 0; const BYTE *b = s.rgbButtons;
    #define BTN(i) ((b[i] & 0x80) != 0)
    if (di_ds4) { /* DS4/DS5 on DirectInput: 0 Square 1 Cross 2 Circle 3 Triangle 4 L1 5 R1 6 L2 7 R2 8 Share 9 Options 10 L3 11 R3 12 PS */
        if (BTN(1)) o |= PB_A; if (BTN(2)) o |= PB_B; if (BTN(0)) o |= PB_X; if (BTN(3)) o |= PB_Y; if (BTN(4)) o |= PB_LB; if (BTN(5)) o |= PB_RB;
        if (BTN(8)) o |= PB_BACK; if (BTN(9)) o |= PB_START; if (BTN(10)) o |= PB_LS; if (BTN(11)) o |= PB_RS;
        p->lt = BTN(6) ? 1.0f : 0; p->rt = BTN(7) ? 1.0f : 0;
        float lz = (s.lRx + 1000) / 2000.0f, rz = (s.lRy + 1000) / 2000.0f; if (lz > p->lt && lz > 0.05f) p->lt = lz; if (rz > p->rt && rz > 0.05f) p->rt = rz;
    } else {     /* generic Xbox-like: 0 A 1 B 2 X 3 Y 4 LB 5 RB 6 Back 7 Start 8 LS 9 RS */
        if (BTN(0)) o |= PB_A; if (BTN(1)) o |= PB_B; if (BTN(2)) o |= PB_X; if (BTN(3)) o |= PB_Y; if (BTN(4)) o |= PB_LB; if (BTN(5)) o |= PB_RB;
        if (BTN(6)) o |= PB_BACK; if (BTN(7)) o |= PB_START; if (BTN(8)) o |= PB_LS; if (BTN(9)) o |= PB_RS;
        p->lt = BTN(10) ? 1.0f : 0; p->rt = BTN(11) ? 1.0f : 0;
    }
    #undef BTN
    DWORD pov = s.rgdwPOV[0];
    if (LOWORD(pov) != 0xFFFF) { int a = (int)(pov / 100); if (a >= 315 || a <= 45) o |= PB_UP; if (a >= 45 && a <= 135) o |= PB_RIGHT; if (a >= 135 && a <= 225) o |= PB_DOWN; if (a >= 225 && a <= 315) o |= PB_LEFT; }
    p->lx = dstick(s.lX, dead); p->ly = -dstick(s.lY, dead);           /* DirectInput Y grows downwards */
    p->rx = dstick(s.lZ, dead); p->ry = -dstick(s.lRz, dead);
    p->buttons = o; p->api = 2; strncpy(p->name, di_name, 63); p->name[63] = 0;
    return 1;
}

/* ---------------------------------------------------------------- public */

/* ---------------------------------------------------------------- keyboard + mouse -> virtual pad */
extern volatile unsigned char g_keys[256]; extern volatile int g_wheel; volatile int g_mouse_game; extern int g_touch_x, g_touch_y, g_gl_w, g_gl_h;
enum { A_UP, A_DOWN, A_LEFT, A_RIGHT, A_FIRE, A_NEXT, A_PREV, A_RELOAD, A_USE, A_POWER, A_BACK, A_PAUSE, A_CAM, A_TOGGLE, A_N };
static const char *const act_name[A_N] = { "UP", "DOWN", "LEFT", "RIGHT", "FIRE", "NEXT", "PREV", "RELOAD", "USE", "POWER", "BACK", "PAUSE", "CAM", "TOGGLE" };
static const char *const act_defkey[A_N][2] = { { "W", "NONE" }, { "S", "NONE" }, { "A", "NONE" }, { "D", "NONE" }, { "MOUSE1", "NONE" }, { "WHEELDOWN", "E" }, { "WHEELUP", "Q" },
    { "R", "NONE" }, { "F", "NONE" }, { "SPACE", "NONE" }, { "C", "NONE" }, { "P", "ESC" }, { "V", "NONE" }, { "TAB", "NONE" } };
static const char *const act_defbtn[A_N] = { "", "", "", "", "RT", "RB", "LB", "X", "A", "Y", "B", "START", "RS", "" };
static int kb_on, kb_bind[A_N][2]; static unsigned kb_btn[A_N]; static float kb_btn_trig[A_N];
#define KV_WHEELUP 0x100
#define KV_WHEELDOWN 0x101
static int parse_key(const char *n) {
    if (!n || !*n || !_stricmp(n, "NONE")) return 0;
    if (!n[1]) { char ch = n[0]; if (ch >= 'a' && ch <= 'z') ch -= 32; return (unsigned char)ch; }
    static const struct { const char *n; int v; } t[] = { {"SPACE",VK_SPACE},{"SHIFT",VK_SHIFT},{"CTRL",VK_CONTROL},{"ALT",VK_MENU},{"TAB",VK_TAB},{"ESC",VK_ESCAPE},{"ENTER",VK_RETURN},{"BACKSPACE",VK_BACK},
        {"UP",VK_UP},{"DOWN",VK_DOWN},{"LEFT",VK_LEFT},{"RIGHT",VK_RIGHT},{"MOUSE1",VK_LBUTTON},{"MOUSE2",VK_RBUTTON},{"MOUSE3",VK_MBUTTON},{"MOUSE4",VK_XBUTTON1},{"MOUSE5",VK_XBUTTON2},
        {"WHEELUP",KV_WHEELUP},{"WHEELDOWN",KV_WHEELDOWN},{"INSERT",VK_INSERT},{"DELETE",VK_DELETE},{"HOME",VK_HOME},{"END",VK_END},{"PAGEUP",VK_PRIOR},{"PAGEDOWN",VK_NEXT} };
    for (int i = 0; i < (int)(sizeof t / sizeof *t); i++) if (!_stricmp(n, t[i].n)) return t[i].v;
    if ((n[0] == 'F' || n[0] == 'f') && atoi(n + 1) >= 1 && atoi(n + 1) <= 12) return VK_F1 + atoi(n + 1) - 1;
    if (!_strnicmp(n, "VK", 2)) return atoi(n + 2);
    return 0;
}
static unsigned parse_btn(const char *n, float *trig) {
    *trig = 0;
    static const struct { const char *n; unsigned b; } t[] = { {"A",PB_A},{"B",PB_B},{"X",PB_X},{"Y",PB_Y},{"LB",PB_LB},{"RB",PB_RB},{"START",PB_START},{"BACK",PB_BACK},{"LS",PB_LS},{"RS",PB_RS},
        {"DPADUP",PB_UP},{"DPADDOWN",PB_DOWN},{"DPADLEFT",PB_LEFT},{"DPADRIGHT",PB_RIGHT} };
    if (!_stricmp(n, "LT") || !_stricmp(n, "RT")) { *trig = !_stricmp(n, "LT") ? -1.0f : 1.0f; return 0; }
    for (int i = 0; i < (int)(sizeof t / sizeof *t); i++) if (!_stricmp(n, t[i].n)) return t[i].b;
    return 0;
}
static void kb_init(void) {
    const char *e = rt_genv("XW_KBM"); kb_on = e ? atoi(e) : 0;
    for (int a = 0; a < A_N; a++) {
        char nm[48]; const char *v;
        for (int k = 0; k < 2; k++) { snprintf(nm, sizeof nm, "XW_BIND_%s%s", act_name[a], k ? "2" : ""); v = getenv(nm); kb_bind[a][k] = parse_key(v ? v : act_defkey[a][k]); }
        snprintf(nm, sizeof nm, "XW_BTN_%s", act_name[a]); v = getenv(nm); kb_btn[a] = parse_btn(v ? v : act_defbtn[a], &kb_btn_trig[a]);
    }
}
static int kb_down(int vk) {
    if (!vk || vk >= 256) return 0;
    if (vk == VK_SHIFT) return g_keys[VK_SHIFT] || g_keys[VK_LSHIFT] || g_keys[VK_RSHIFT];
    return g_keys[vk];
}
static int kb_act(int a, int *pulse) {      /* held, or wheel pulse */
    int on = 0; for (int k = 0; k < 2; k++) { int v = kb_bind[a][k]; if (v == KV_WHEELUP) on |= pulse[0] > 0; else if (v == KV_WHEELDOWN) on |= pulse[1] > 0; else on |= kb_down(v); } return on;
}
/* adds the keyboard/mouse state to *p; returns 1 if any input was added (pad becomes 'connected') */
static void kb_apply(PadState *p) {
    static int init, pulse[2]; if (!init) { init = 1; kb_init(); } if (!kb_on) return;
    int w = g_wheel; g_wheel = 0; if (w > 0) pulse[0] = 4; else if (w < 0) pulse[1] = 4;
    int act[A_N]; for (int a = 0; a < A_N; a++) act[a] = kb_act(a, pulse);
    if (pulse[0] > 0) pulse[0]--; if (pulse[1] > 0) pulse[1]--;
    { static int ptog, ppause; if (act[A_TOGGLE] && !ptog) { g_mouse_game ^= 1; if (g_mouse_game) g_touch_down = 0; }
      if (act[A_PAUSE] && !ppause && g_mouse_game) g_mouse_game = 0;     /* pause menu needs the mouse as a pointer */
      ptog = act[A_TOGGLE]; ppause = act[A_PAUSE]; }
    float lx = (float)act[A_RIGHT] - (float)act[A_LEFT], ly = (float)act[A_UP] - (float)act[A_DOWN];
    float n = sqrtf(lx * lx + ly * ly); if (n > 1) { lx /= n; ly /= n; }
    if (lx * lx + ly * ly > p->lx * p->lx + p->ly * p->ly) { p->lx = lx; p->ly = ly; }
    for (int a = 4; a < A_N; a++) if (act[a]) { p->buttons |= kb_btn[a]; if (kb_btn_trig[a] > 0) p->rt = 1; else if (kb_btn_trig[a] < 0) p->lt = 1; }
    if (act[A_FIRE]) {      /* aim: direction from the screen centre (the hero) to the mouse pointer */
        float dx = (float)g_touch_x - g_gl_w * 0.5f, dy = (float)g_touch_y - g_gl_h * 0.5f, d = sqrtf(dx * dx + dy * dy);
        if (d > 4) { p->rx = dx / d; p->ry = -dy / d; }
    }
    /* menus: arrows = d-pad, Enter = A */
    if (kb_down(VK_RETURN)) p->buttons |= PB_A;
    static const int arrows[4] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT }; static const unsigned ab[4] = { PB_UP, PB_DOWN, PB_LEFT, PB_RIGHT };
    for (int i = 0; i < 4; i++) { int used = 0; for (int a = 0; a < A_N; a++) used |= kb_bind[a][0] == arrows[i] || kb_bind[a][1] == arrows[i]; if (!used && kb_down(arrows[i])) p->buttons |= ab[i]; }
    if (!p->connected) { p->connected = 1; p->api = 10; strcpy(p->name, "Teclado e Mouse"); }
}

int g_pad_enabled = 1; float g_pad_dead = 0.18f;
static int pad_poll_hw(PadState *p) {
    memset(p, 0, sizeof *p);
    static int init;
    if (!init) { init = 1; const char *e = rt_genv("XW_PAD"); if (e) g_pad_enabled = atoi(e); const char *d = rt_genv("XW_PADDEAD"); if (d) { float v = (float)atof(d); if (v >= 0 && v < 0.9f) g_pad_dead = v; } xi_load(); }
    if (!g_pad_enabled) return 0;
    xi_load();
    if (rt_genv("XW_FAKEPAD")) {      /* test aid: a virtual pad whose left stick sweeps and A toggles once a second */
        static DWORD t0; if (!t0) t0 = GetTickCount(); DWORD t = GetTickCount() - t0;
        p->connected = 1; p->api = 9; strcpy(p->name, "Fake Pad"); p->lx = sinf(t / 700.0f); p->ly = cosf(t / 700.0f); p->rx = 0.5f; p->buttons = ((t / 500) & 1) ? PB_A : 0; if (rt_genv("XW_FAKESTART") && t > 12000 && t < 12300) p->buttons |= PB_START;
        return 1;
    }
    if (xi_poll(p, g_pad_dead)) { p->connected = 1; return 1; }
    if (di_poll(p, g_pad_dead)) { p->connected = 1; return 1; }
    return 0;
}

/* Device polling runs on its own thread: XInput slot scans, DirectInput enumeration (registry + HID I/O) and raw-input listings can take
 * tens of milliseconds and used to stall the game thread every second or two. The game thread only copies the latest state. */
static CRITICAL_SECTION pad_cs; static PadState pad_latest; static int pad_thread_started;
static DWORD WINAPI pad_thread(LPVOID u) {
    (void)u;
    for (;;) { PadState t; int c = pad_poll_hw(&t); t.connected = c; EnterCriticalSection(&pad_cs); pad_latest = t; LeaveCriticalSection(&pad_cs); Sleep(c ? 2 : 50); }
    return 0;
}
int rt_pad_poll(PadState *p) {
    if (!pad_thread_started) { pad_thread_started = 1; InitializeCriticalSection(&pad_cs); memset(&pad_latest, 0, sizeof pad_latest); CreateThread(NULL, 0, pad_thread, NULL, 0, NULL); }
    EnterCriticalSection(&pad_cs); *p = pad_latest; LeaveCriticalSection(&pad_cs);
    kb_apply(p); return p->connected;
}
