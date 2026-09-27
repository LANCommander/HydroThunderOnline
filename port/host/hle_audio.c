/*
 * YMF7xx (DS-1) audio replacement.
 *
 * On the cabinet the game mixes nothing itself: audio_dspcmd_* post per-voice commands into a
 * 32-entry mailbox, and every 256 samples (48 kHz) the chip interrupts; the service thread
 * turns pending commands into DS-1 play-control banks (audio_dspcmd_IRQISR) and then runs the
 * DCS sequencer tick (dcs_TimerCB). ymf_* above that is plain bookkeeping and stays original.
 *
 * Here a waveOut stream plays the chip: each 256-frame block consumes the mailbox, mixes the
 * voices, and calls the tick. That "interrupt" honours the game's IRQ masking (EtsPicEnable on
 * the audio IRQ) and suspends the game thread while it runs, like a single-CPU ISR.
 */
#include "hle_hw.h"
#include "hook.h"
#include "host.h"
#include "settings.h"

#include <windows.h>
#include <math.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 48000
#define FRAMES 256 /* one DS-1 interrupt */
#define NBUF 8
#define NVOICE 32

/* Mailbox written by audio_dspcmd_* (0x347430, stride 0x24). */
typedef struct DspCmd {
    volatile uint32_t flags[2]; /* one copy per play bank; bits: 1 play, 2 stop, 4 pitch, 8 gain */
    int32_t loop_default;       /* 0 = loop (engine drones start with 0 and never stop), 1 = one-shot */
    const int16_t *buf;
    int32_t len_bytes;
    int32_t rate;
    int32_t volume; /* 0..255, 256 = unity */
    int32_t pitch;  /* 0x80 = unity */
    int32_t pan;    /* 0..255 */
} DspCmd;
#define MAILBOX ((DspCmd *)(uintptr_t)0x00347430u)
#define MASTER_VOLUME (*(int32_t *)(uintptr_t)0x0029dcfcu)
#define ATTEN_L (*(float *)(uintptr_t)0x0029dd00u)
#define ATTEN_R (*(float *)(uintptr_t)0x0029dd04u)
#define SINE_MAP ((const float *)(uintptr_t)0x00718f70u) /* 256 entries, built by audio_dspcmd_Init */

typedef struct Voice {
    int active;
    int loop; /* play-command loop_default == 0 (engine drones, music); 1 = one-shot */
    const int16_t *buf;
    uint32_t len;  /* samples */
    uint64_t pos;  /* 32.32 */
    uint64_t step; /* 32.32 */
    float gl, gr;
} Voice;

typedef void(__cdecl *PFN_timer_cb)(float seconds);

static Voice g_voice[NVOICE];
static PFN_timer_cb g_timer_cb;
static HWAVEOUT g_wo;
static WAVEHDR g_hdr[NBUF];
static int16_t g_pcm[NBUF][FRAMES * 2];
static HANDLE g_event, g_thread;
static volatile LONG g_quit;
static uint32_t g_loop_values_seen;
static HANDLE g_game_thread;
static int g_stereo; /* HYDRO_AUDIO_STEREO=1: raw cabinet channels, whatever PC SETTINGS > AUDIO says */
/* Messages produced inside the ISR section, logged after the game thread resumes (it may be
 * suspended while holding the log lock). */
static char g_isr_msg[256], g_meter_msg[256];
static unsigned g_pan_hist[8]; /* gain updates per pan eighth (0 = hard left), for the meter line */

static void set_step(Voice *v, int32_t rate)
{
    v->step = ((uint64_t)(uint32_t)rate << 32) / RATE;
}

static void apply_commands(void)
{
    for (int i = 0; i < NVOICE; i++) {
        DspCmd *c = &MAILBOX[i];
        uint32_t f = c->flags[0] | c->flags[1];
        if (!f)
            continue;
        c->flags[0] = c->flags[1] = 0;
        Voice *v = &g_voice[i];
        if (f & 2) {
            v->active = 0;
            continue;
        }
        if (f & 1) {
            v->buf = c->buf;
            v->len = (uint32_t)c->len_bytes >> 1;
            v->pos = 0;
            v->active = v->buf && v->len;
            v->loop = c->loop_default == 0;
            set_step(v, c->rate);
            uint32_t bit = 1u << (c->loop_default & 31);
            if (!(g_loop_values_seen & bit) && !g_isr_msg[0]) {
                g_loop_values_seen |= bit;
                snprintf(g_isr_msg, sizeof g_isr_msg, "audio: voice %d play len=%d rate=%d loop_default=%d", i,
                         c->len_bytes, c->rate, c->loop_default);
            }
        }
        if (f & 4) {
            int32_t r = c->pitch <= 0x80 ? (c->pitch + 0x80) * c->rate >> 8 : c->rate * c->pitch >> 7;
            set_step(v, r);
        }
        if (f & 8) {
            int pan = c->pan & 0xff;
            g_pan_hist[pan >> 5]++;
            float g = (float)MASTER_VOLUME / 256.0f * (float)c->volume / 256.0f;
            v->gl = g * SINE_MAP[255 - pan] * ATTEN_L;
            v->gr = g * SINE_MAP[pan] * ATTEN_R;
        }
    }
}

/* Subwoofer low-pass: a 2nd-order Butterworth biquad (RBJ), audio thread only. */
static struct {
    int hz;
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
} g_sub;

static void sub_filter_update(int hz)
{
    if (hz == g_sub.hz)
        return;
    memset(&g_sub, 0, sizeof g_sub);
    g_sub.hz = hz;
    if (hz <= 0)
        return;
    const float w = 2.0f * 3.14159265f * (float)hz / RATE, c = cosf(w), alpha = sinf(w) / (2.0f * 0.7071f);
    const float a0 = 1.0f + alpha;
    g_sub.b0 = g_sub.b2 = (1.0f - c) / 2.0f / a0;
    g_sub.b1 = (1.0f - c) / a0;
    g_sub.a1 = -2.0f * c / a0;
    g_sub.a2 = (1.0f - alpha) / a0;
}

static float sub_filter(float x)
{
    if (!g_sub.hz)
        return x;
    float y = g_sub.b0 * x + g_sub.b1 * g_sub.x1 + g_sub.b2 * g_sub.x2 - g_sub.a1 * g_sub.y1 - g_sub.a2 * g_sub.y2;
    g_sub.x2 = g_sub.x1;
    g_sub.x1 = x;
    g_sub.y2 = g_sub.y1;
    g_sub.y1 = y;
    return y;
}

static void render(int16_t *out)
{
    float mix[FRAMES * 2] = {0};
    for (int i = 0; i < NVOICE; i++) {
        Voice *v = &g_voice[i];
        if (!v->active)
            continue;
        uint64_t end = (uint64_t)v->len << 32;
        for (int n = 0; n < FRAMES; n++) {
            float s = v->buf[v->pos >> 32];
            mix[2 * n] += s * v->gl;
            mix[2 * n + 1] += s * v->gr;
            v->pos += v->step;
            if (v->pos >= end) {
                if (!v->loop) {
                    v->active = 0;
                    break;
                }
                v->pos %= end;
            }
        }
    }
    /* The cabinet's channels aren't a stereo image: all speech and music use pan 0 (hard left,
     * the headrest speakers), engines 0x7f (both), and some effects go hard right (the subwoofer
     * under the seat). Full-range samples on that channel only ever came out as bass there, so at
     * full level they swamp the rest on PC speakers. PC SETTINGS > AUDIO sets each channel's level
     * and an optional low-pass standing in for the subwoofer, then folds both into the centre
     * unless OUTPUT is CABINET L/R. */
    const Audio *au = &g_settings.audio;
    float hl = (float)au->headrest / 100.0f, sl = (float)au->subwoofer / 100.0f;
    sub_filter_update(au->crossover);
    int fold = !g_stereo && !au->output;
    for (int n = 0; n < FRAMES; n++) {
        float l = mix[2 * n] * hl, r = sub_filter(mix[2 * n + 1]) * sl;
        if (fold)
            l = r = (l + r) * 0.7071f; /* -3 dB */
        mix[2 * n] = l;
        mix[2 * n + 1] = r;
    }
    for (int n = 0; n < FRAMES * 2; n++) {
        /* linear up to -2.5 dBFS, then a soft knee instead of hard clipping */
        float x = mix[n] / 32768.0f, a = fabsf(x);
        if (a > 0.75f)
            x = copysignf(0.75f + 0.25f * tanhf((a - 0.75f) / 0.25f), x);
        out[n] = (int16_t)(x * 32767.0f);
    }

    /* Level meter in the log, every ~10 s: makes "no sound" diagnosable from hydro.log. */
    static unsigned ticks;
    static int peak, clipped;
    for (int n = 0; n < FRAMES * 2; n++) {
        int a = out[n] < 0 ? -out[n] : out[n];
        if (a > peak)
            peak = a;
        clipped += a >= 32000;
    }
    if (++ticks % (10 * RATE / FRAMES) == 0) {
        int active = 0;
        for (int i = 0; i < NVOICE; i++)
            active += g_voice[i].active;
        snprintf(g_meter_msg, sizeof g_meter_msg,
                 "audio: %d voices active, peak %d, %d clipped; master %d; pans L..R %u %u %u %u %u %u %u %u",
                 active, peak, clipped, MASTER_VOLUME, g_pan_hist[0], g_pan_hist[1], g_pan_hist[2],
                 g_pan_hist[3], g_pan_hist[4], g_pan_hist[5], g_pan_hist[6], g_pan_hist[7]);
        memset(g_pan_hist, 0, sizeof g_pan_hist);
        peak = clipped = 0;
    }
}

static DWORD WINAPI audio_thread(void *arg)
{
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    while (!g_quit) {
        WaitForSingleObject(g_event, 100);
        for (int b = 0; b < NBUF && !g_quit; b++) {
            if (!(g_hdr[b].dwFlags & WHDR_DONE))
                continue;
            /* The chip's interrupt: waits while the game has the IRQ masked, then runs atomically
             * with respect to the game thread, as on the single-CPU cabinet. */
            hw_audio_irq_lock(1);
            CONTEXT ctx;
            ctx.ContextFlags = CONTEXT_CONTROL;
            int suspended = g_game_thread && SuspendThread(g_game_thread) != (DWORD)-1;
            if (suspended)
                GetThreadContext(g_game_thread, &ctx); /* forces the suspension to complete */
            apply_commands();
            render(g_pcm[b]);
            if (g_timer_cb)
                g_timer_cb((float)FRAMES / RATE);
            if (suspended)
                ResumeThread(g_game_thread);
            hw_audio_irq_lock(0);
            if (g_isr_msg[0]) {
                hy_log("%s", g_isr_msg);
                g_isr_msg[0] = 0;
            }
            if (g_meter_msg[0]) {
                hy_log("%s", g_meter_msg);
                g_meter_msg[0] = 0;
            }
            g_hdr[b].dwFlags &= ~WHDR_DONE;
            waveOutWrite(g_wo, &g_hdr[b], sizeof g_hdr[b]);
        }
    }
    return 0;
}

static uint32_t __cdecl my_ymf_Init(PFN_timer_cb cb)
{
    WAVEFORMATEX wf = {WAVE_FORMAT_PCM, 2, RATE, RATE * 4, 4, 16, 0};
    g_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &wf, (DWORD_PTR)g_event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        hy_log("audio: waveOutOpen failed; running without sound");
        return 1;
    }
    /* The original ymf_Init also builds the constant-power pan table (sineMap); without it
     * every voice gain is zero. It's pure math, so run the game's own code. */
    ((void(__cdecl *)(void))(uintptr_t)sym_addr("audio_dspcmd_Init"))();
    /* ymf's IRQ number (from PCI config on the cabinet): DCS masks it via EtsPicEnable. */
    *(int32_t *)(uintptr_t)0x006461a8u = HW_AUDIO_IRQ;
    /* The operator volume (OperatorSettings[11], 0..255, -/=) comes from the CMOS: gameloop_Start
     * applies it with audio_SetMasterVolume right after audio_Init returns. The port's factory
     * default is full volume (operator_install). */
    /* ymf_Init runs on the game thread: that's the thread the "interrupt" must preempt. */
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_game_thread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    const char *st = getenv("HYDRO_AUDIO_STEREO");
    g_stereo = st && *st == '1';
    memset(g_voice, 0, sizeof g_voice);
    memset(&g_sub, 0, sizeof g_sub);
    g_timer_cb = cb;
    g_quit = 0;
    for (int b = 0; b < NBUF; b++) {
        memset(g_pcm[b], 0, sizeof g_pcm[b]);
        g_hdr[b].lpData = (LPSTR)g_pcm[b];
        g_hdr[b].dwBufferLength = sizeof g_pcm[b];
        waveOutPrepareHeader(g_wo, &g_hdr[b], sizeof g_hdr[b]);
        g_hdr[b].dwFlags |= WHDR_DONE; /* first pass fills every buffer */
    }
    g_thread = CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
    hy_log("audio: %d Hz stereo, %d-frame ticks, %d buffers; pan table [0]=%.3f [64]=%.3f [128]=%.3f [255]=%.3f",
           RATE, FRAMES, NBUF, SINE_MAP[0], SINE_MAP[64], SINE_MAP[128], SINE_MAP[255]);
    hy_log("audio: %s, headrest %d%%, subwoofer %d%%, subwoofer filter %d Hz",
           g_stereo || g_settings.audio.output ? "cabinet L/R" : "mixed", g_settings.audio.headrest,
           g_settings.audio.subwoofer, g_settings.audio.crossover);
    return 0;
}

static uint32_t __cdecl my_ymf_Shutdown(void)
{
    if (g_thread) {
        g_quit = 1;
        SetEvent(g_event);
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_wo) {
        waveOutReset(g_wo);
        for (int b = 0; b < NBUF; b++)
            waveOutUnprepareHeader(g_wo, &g_hdr[b], sizeof g_hdr[b]);
        waveOutClose(g_wo);
        g_wo = NULL;
    }
    g_timer_cb = NULL;
    if (g_game_thread) {
        CloseHandle(g_game_thread);
        g_game_thread = NULL;
    }
    return 0;
}

void hle_audio_install(void)
{
    hook_name("ymf_Init", my_ymf_Init);
    hook_name("ymf_Shutdown", my_ymf_Shutdown);
}
