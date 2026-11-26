/* rt_audio.c - AudioToolbox RemoteIO emulation: the game registers a render callback; we pull PCM from it on the main
 * thread once per frame (no races with the engine) and stream it to the default device through waveOut. */
#include "rt.h"
#include <windows.h>
#include <math.h>
#include <mmsystem.h>
#define SHIM(n) void imp_##n(CPU *c)

static uint32_t g_cb, g_ref;                       /* render callback + refcon */
static double g_rate = 44100; static uint32_t g_flags = 0xC, g_chan = 2, g_bits = 16;
static int g_started;
static HWAVEOUT g_wo; static int g_wo_ok;
#define NBUF 8
static struct { WAVEHDR h; short *pcm; int cap; } g_buf[NBUF];
static uint32_t g_abl, g_data, g_flagsp, g_ts;     /* guest-side scratch: AudioBufferList, sample buffer, flags, timestamp */
#define MAXFR 4096

SHIM(_AudioComponentFindNext) { RET(ARG(0) == 0 ? 0x7e000100u : 0); }
SHIM(_AudioComponentInstanceNew) { st32(ARG(1), 0x7e000001u); RET(0); }
SHIM(_AudioComponentInstanceDispose) { RET(0); }
SHIM(_AudioUnitInitialize) { RET(0); }
SHIM(_AudioUnitUninitialize) { RET(0); }
SHIM(_AudioUnitSetProperty) {
    uint32_t id = ARG(1), data = ARG(4);
    if (id == 23 && data) { g_cb = ld32(data); g_ref = ld32(data + 4); rt_log("[audio] render callback %x refcon %x\n", g_cb, g_ref); }
    else if (id == 8 && data) {
        uint64_t r = (uint64_t)ld32(data) | ((uint64_t)ld32(data + 4) << 32); memcpy(&g_rate, &r, 8);
        g_flags = ld32(data + 12); g_chan = ld32(data + 28); g_bits = ld32(data + 32);
        if (g_chan < 1 || g_chan > 2) g_chan = 2;
        rt_log("[audio] format rate=%.0f flags=%x ch=%u bits=%u\n", g_rate, g_flags, g_chan, g_bits);
    }
    RET(0);
}
SHIM(_AudioOutputUnitStart) { g_started = 1; RET(0); }
SHIM(_AudioOutputUnitStop) { g_started = 0; if (g_wo_ok) waveOutReset(g_wo); RET(0); }

static int open_device(void) {
    if (g_wo_ok) return 1;
    if (rt_genv("XW_NOAUDIO")) return 0;
    WAVEFORMATEX f = { 0 }; f.wFormatTag = WAVE_FORMAT_PCM; f.nChannels = (WORD)g_chan; f.nSamplesPerSec = (DWORD)g_rate; f.wBitsPerSample = 16;
    f.nBlockAlign = (WORD)(f.nChannels * 2); f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign;
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { rt_log("[audio] waveOutOpen failed\n"); return 0; }
    g_wo_ok = 1; return 1;
}

void rt_audio_pump(CPU *c) {
    if (!g_started || !g_cb) return;
    int dev = open_device();
    if (!g_abl) { g_abl = g_calloc(64); g_data = g_calloc(MAXFR * 8 * 2); g_flagsp = g_calloc(16); g_ts = g_calloc(64); }
    int queued = 0;
    if (dev) for (int i = 0; i < NBUF; i++) if (g_buf[i].h.dwFlags & WHDR_PREPARED) { if (g_buf[i].h.dwFlags & WHDR_DONE) { waveOutUnprepareHeader(g_wo, &g_buf[i].h, sizeof(WAVEHDR)); } else queued++; }
    static int depth; if (!depth) { const char *e = rt_genv("XW_AUDIOBUF"); depth = e ? atoi(e) : 6; if (depth < 3) depth = 3; if (depth > 8) depth = 8; }
    int want = dev ? depth - queued : 1;
    for (int n = 0; n < want && n < 4; n++) {
        uint32_t frames = (uint32_t)(g_rate / 60.0 + 0.5); if (frames > MAXFR) frames = MAXFR;
        int ni = (g_flags & 0x20) != 0, fl = (g_flags & 1) != 0, w32 = !fl && g_bits == 32, bytes = (fl || w32) ? 4 : 2;
        uint32_t nb = ni ? g_chan : 1, per = ni ? frames * bytes : frames * bytes * g_chan;
        st32(g_abl, nb);
        for (uint32_t b = 0; b < nb; b++) { st32(g_abl + 4 + 12 * b, ni ? 1 : g_chan); st32(g_abl + 8 + 12 * b, per); st32(g_abl + 12 + 12 * b, g_data + b * per); }
        memset(GP(g_data), 0, per * nb); st32(g_flagsp, 0);
        CPU t = *c; t.r[0] = g_ref; t.r[1] = g_flagsp; t.r[2] = g_ts; t.r[3] = 0; t.r[13] -= 16; st32(t.r[13], frames); st32(t.r[13] + 4, g_abl); t.r[14] = 0;
        arm_dispatch(&t, g_cb);
        if (rt_genv("XW_AUDIODBG")) { static int cnt; static float pk; const int32_t *q = (const int32_t *)GP(g_data); for (uint32_t i = 0; i < frames * (ni ? 1 : g_chan) && (g_bits == 32); i++) { float v = fabsf(q[i] / 16777216.0f); if (v > pk) pk = v; } if (++cnt % 120 == 0) { rt_log("[audio] peak=%.3f\n", pk); pk = 0; } }
        if (!dev) continue;
        int k; for (k = 0; k < NBUF; k++) if (!(g_buf[k].h.dwFlags & WHDR_PREPARED)) break;
        if (k == NBUF) break;
        int ns = (int)(frames * g_chan);
        if (g_buf[k].cap < ns) { free(g_buf[k].pcm); g_buf[k].pcm = malloc(ns * 2); g_buf[k].cap = ns; }
        short *o = g_buf[k].pcm;
        for (uint32_t i = 0; i < frames; i++) for (uint32_t ch = 0; ch < g_chan; ch++) {
            uint32_t off = ni ? ch * per + i * bytes : (i * g_chan + ch) * bytes; const uint8_t *src = (const uint8_t *)GP(g_data + off);
            float v; if (fl) memcpy(&v, src, 4); else if (w32) { int32_t iv; memcpy(&iv, src, 4); v = iv / 16777216.0f; } else { short sv; memcpy(&sv, src, 2); v = sv / 32768.0f; }
            if (v > 1) v = 1; if (v < -1) v = -1; o[i * g_chan + ch] = (short)(v * 32767.0f);
        }
        memset(&g_buf[k].h, 0, sizeof(WAVEHDR)); g_buf[k].h.lpData = (LPSTR)o; g_buf[k].h.dwBufferLength = ns * 2;
        waveOutPrepareHeader(g_wo, &g_buf[k].h, sizeof(WAVEHDR)); waveOutWrite(g_wo, &g_buf[k].h, sizeof(WAVEHDR));
    }
}
