/*
 * Replacements for the cabinet hardware layer and the ETS kernel services game code uses.
 *
 * Interrupt masking: on ETS, EtsSetInterruptFlag(0)/CLI keeps ISRs (the DiegoIO serial
 * receive, timers) out of a critical section. Here the "ISRs" are ordinary threads, so the
 * interrupt flag becomes a per-thread flag backed by one global recursive lock.
 */
#include "hle_hw.h"
#include "hook.h"
#include "host.h"
#include "input.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- interrupt flag */

static CRITICAL_SECTION g_intlock;
static DWORD g_if_tls; /* per thread: 1 = "interrupts disabled" (holding g_intlock) */

int hw_set_interrupt_flag(int enable)
{
    int disabled = (int)(intptr_t)TlsGetValue(g_if_tls);
    if (!enable && !disabled) {
        EnterCriticalSection(&g_intlock);
        TlsSetValue(g_if_tls, (void *)1);
    } else if (enable && disabled) {
        TlsSetValue(g_if_tls, (void *)0);
        LeaveCriticalSection(&g_intlock);
    }
    return !disabled; /* previous state: 1 = enabled */
}

static int __cdecl my_EtsSetInterruptFlag(int enable)
{
    return hw_set_interrupt_flag(enable);
}

/* ---------------------------------------------------------------- PIC masking (audio IRQ)
 * DCS brackets every change to its channel table with EtsPicEnable(audio_irq, 0) ... (…, 1) so
 * the sound interrupt (dcs_DrivePL) never sees half-updated state. The host's audio thread plays
 * the interrupt, so masking that IRQ is a lock the audio thread also takes. Other IRQs: no-op. */
static CRITICAL_SECTION g_audio_irq;
static DWORD g_audio_irq_depth_tls;

void hw_audio_irq_lock(int lock)
{
    intptr_t depth = (intptr_t)TlsGetValue(g_audio_irq_depth_tls);
    if (lock) {
        EnterCriticalSection(&g_audio_irq);
        TlsSetValue(g_audio_irq_depth_tls, (void *)(depth + 1));
    } else if (depth > 0) { /* ignore unmatched unmask (initial enables) */
        TlsSetValue(g_audio_irq_depth_tls, (void *)(depth - 1));
        LeaveCriticalSection(&g_audio_irq);
    }
}

static int __cdecl my_EtsPicEnable(int irq, int enable)
{
    if (irq == HW_AUDIO_IRQ)
        hw_audio_irq_lock(!enable);
    return 0;
}

/* ---------------------------------------------------------------- CPU / timing */

static uint64_t measure_tsc_hz(void)
{
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    uint64_t t0 = __rdtsc();
    do
        QueryPerformanceCounter(&b);
    while (b.QuadPart - a.QuadPart < f.QuadPart / 20);
    uint64_t t1 = __rdtsc();
    return (t1 - t0) * (uint64_t)f.QuadPart / (uint64_t)(b.QuadPart - a.QuadPart);
}

/* HardwareCaps: [0]=valid [1]=cpu family [2]=1 [3]=TSC Hz [4]=float K / Hz.
 * The original measures the CPU clock against the PIT (ports 0x40-0x43). */
static int __cdecl my_hardware_caps_driver_QueryHardwareCaps(uint32_t *caps)
{
    static uint32_t hz;
    if (!hz) {
        uint64_t m = measure_tsc_hz();
        hz = m > 0xffffffffu ? 0xffffffffu : (uint32_t)m; /* the game keeps it in 32 bits */
        hy_log("hwcaps: TSC %.1f MHz", m / 1e6);
    }
    caps[0] = 1;
    caps[1] = 6; /* P6 family */
    caps[2] = 1;
    caps[3] = hz;
    float k = *(float *)(uintptr_t)0x001ef984u;
    ((float *)caps)[4] = k / (float)hz;
    return 1;
}

static uint32_t __cdecl my_pcspeaker_ToggleSpeaker(void)
{
    return 0;
}

/* ---------------------------------------------------------------- persistence */

#define CMOS_SIZE 32000
#define EEPROM_SIZE 128

static uint8_t g_cmos[CMOS_SIZE];
static int g_cmos_valid;
static char g_cmos_name[32] = "cmos.bin";
static uint8_t g_eeprom[EEPROM_SIZE];

static void nv_path(char *out, size_t size, const char *name)
{
    snprintf(out, size, "%s\\%s", g_cfg.save_dir, name);
}

static int nv_read(const char *name, void *buf, size_t size)
{
    char p[MAX_PATH];
    nv_path(p, sizeof p, name);
    FILE *f = fopen(p, "rb");
    if (!f)
        return 0;
    size_t n = fread(buf, 1, size, f);
    fclose(f);
    return n == size;
}

static int nv_write(const char *name, const void *buf, size_t size)
{
    char p[MAX_PATH], tmp[MAX_PATH];
    nv_path(p, sizeof p, name);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return 0;
    int ok = fwrite(buf, 1, size, f) == size;
    ok &= fclose(f) == 0;
    return ok && MoveFileExA(tmp, p, MOVEFILE_REPLACE_EXISTING);
}

/* The CMOS is 8000 dwords: OperatorSettings[i] is dword 5+i (UNIT ID 0-3 at [0], network
 * disabled at [1], volume at [11]), and dword 3 is the sum of all the others. */
static void cmos_fix_checksum(void)
{
    uint32_t *w = (uint32_t *)g_cmos, sum = 0;
    for (int i = 0; i < CMOS_SIZE / 4; i++)
        if (i != 3)
            sum += w[i];
    w[3] = sum;
}

static void cmos_set_network_unit(int unit)
{
    uint32_t *w = (uint32_t *)g_cmos;
    w[5] = (uint32_t)(unit - 1);
    w[6] = 0;
    cmos_fix_checksum();
}

/* At exit: write the in-game volume (-/=) through to the CMOS file, which otherwise only gets it
 * at the end of a race. */
int operator_master_volume(void); /* game/operator.c */

void hw_cmos_flush_volume(void)
{
    uint32_t *w = (uint32_t *)g_cmos;
    uint32_t v = (uint32_t)operator_master_volume();
    if (!g_cmos_valid || w[5 + 11] == v || v > 255)
        return;
    hy_log("cmos: saving volume %u at exit", v);
    w[5 + 11] = v;
    cmos_fix_checksum();
    nv_write(g_cmos_name, g_cmos, CMOS_SIZE);
}

/* WRS keeps CMOS + a recovery journal in reserved sectors past the last partition. */
static uint32_t __cdecl my_wrs_Init(int recover, int show)
{
    (void)recover, (void)show;
    if (g_cfg.net_unit) {
        /* A linked-play test instance: its own copy of the CMOS (audits, hiscores), seeded from
         * cmos.bin, with NETWORK ENABLED on and UNIT ID = HYDRO_NET_UNIT. */
        snprintf(g_cmos_name, sizeof g_cmos_name, "cmos_unit%d.bin", g_cfg.net_unit);
        g_cmos_valid = nv_read(g_cmos_name, g_cmos, CMOS_SIZE) || nv_read("cmos.bin", g_cmos, CMOS_SIZE);
        if (g_cmos_valid)
            cmos_set_network_unit(g_cfg.net_unit);
        else
            hy_log("wrs_Init: no cmos.bin to seed %s from; run once without HYDRO_NET_UNIT", g_cmos_name);
    } else {
        g_cmos_valid = nv_read(g_cmos_name, g_cmos, CMOS_SIZE);
    }
    hy_log("wrs_Init: %s %s", g_cmos_name, g_cmos_valid ? "loaded" : "missing (defaults)");
    return 1;
}

static void __cdecl my_wrs_ReadCMOSBlock(uint8_t *dst, int size)
{
    if (size > CMOS_SIZE)
        size = CMOS_SIZE;
    if (g_cmos_valid)
        memcpy(dst, g_cmos, (size_t)size);
    else
        memset(dst, 0, (size_t)size);
}

static uint32_t __cdecl my_wrs_WriteCMOSBlockToDisk(const uint8_t *src, uint32_t size)
{
    if (size > CMOS_SIZE)
        size = CMOS_SIZE;
    memcpy(g_cmos, src, size);
    g_cmos_valid = 1;
    return nv_write(g_cmos_name, g_cmos, CMOS_SIZE);
}

static uint32_t __cdecl my_wrs_FileSystemWriteMode(uint32_t arg)
{
    (void)arg;
    return 1;
}

/* ---------------------------------------------------------------- DiegoIO board */

/*
 * The DiegoIO board talks over a UART: diego_io_flush() builds an 8-byte frame and passes it
 * down encrypt -> check -> slip -> uart; the UART ISR decodes the reply and hands it to
 * diego_io_receive(). We answer each transmit immediately with a synthesized reply.
 *
 * Transmit frame: [0] flags (0x10 coin-counter toggle, 0x40 eeprom toggle, 0x20 security clk)
 *                 [1] coin counter id  [2..4] DAC/LED/lamp  [5] eeprom addr | 0x80 read  [6] eeprom data
 * Reply frame:    [0] echoed toggles  [1..2] five 3-bit coin-drop counters  [3] ADC1  [4] ADC0
 *                 [5] eeprom data  [6] switch byte
 */
typedef void(__cdecl *PFN_diego_io_receive)(const uint8_t *frame);
static PFN_diego_io_receive g_diego_receive;
static uint8_t g_last_eeprom_toggle, g_eeprom_dirty;

static void __cdecl my_encrypt_init(int ch)
{
    (void)ch;
    g_last_eeprom_toggle = 0;
}

static void __cdecl my_encrypt_transmit(int ch, const uint8_t *tx, int len)
{
    (void)ch;
    if (len < 7)
        return;
    uint8_t rx[8] = {0};
    InputState in;
    input_poll(&in);

    uint8_t toggle = tx[0] & 0x40;
    if (toggle != g_last_eeprom_toggle) {
        g_last_eeprom_toggle = toggle;
        uint8_t addr = tx[5] & 0x7f;
        if (tx[5] & 0x80) {
            rx[5] = g_eeprom[addr];
        } else {
            g_eeprom[addr] = tx[6];
            g_eeprom_dirty = 1;
        }
    } else if (tx[5] & 0x80) {
        rx[5] = g_eeprom[tx[5] & 0x7f];
    }
    if (g_eeprom_dirty) {
        g_eeprom_dirty = 0;
        nv_write("eeprom.bin", g_eeprom, EEPROM_SIZE);
    }

    rx[0] = tx[0] & 0x70; /* ack coin counter + eeprom; hold the security clock steady */
    /* coin-drop counters c0..c4, 3 bits each, cycling 1..7 (0 = reset); see diego_io_receive */
    rx[2] = (uint8_t)((in.coin[0] & 7) | (in.coin[1] & 7) << 3 | (in.coin[2] & 3) << 6);
    rx[1] = (uint8_t)((in.coin[2] >> 2 & 1) | (in.coin[3] & 7) << 1 | (in.coin[4] & 7) << 4);
    rx[3] = in.adc[1];
    rx[4] = in.adc[0];
    rx[6] = in.switches;
    g_diego_receive(rx);
}

/* The original returns the switch byte cached from the last reply frame. On the cabinet the board
 * is polled continuously; here a frame only arrives when the game flushes. That leaves the byte
 * stale across the gap between the game loop seeing Test and the operator menu's first poll, and
 * the menu would read the stale Test press as "select" (START THE GAME). So read it live. */
static uint32_t __cdecl my_diego_io_read_switch_byte(int ch)
{
    (void)ch;
    InputState in;
    input_poll(&in);
    return in.switches;
}

/* ---------------------------------------------------------------- ETS port I/O */

static uint32_t port_in(uint32_t port, int size)
{
    static volatile LONG logged;
    if (InterlockedIncrement(&logged) < 64)
        hy_log("port in%c %04x", "?bw?d"[size], port);
    return 0xffffffffu >> (32 - 8 * size);
}

static void port_out(uint32_t port, uint32_t v, int size)
{
    static volatile LONG logged;
    if (InterlockedIncrement(&logged) < 64)
        hy_log("port out%c %04x <- %x", "?bw?d"[size], port, v);
}

uint32_t hw_port_in(uint32_t port, int size) { return port_in(port, size); }
void hw_port_out(uint32_t port, uint32_t v, int size) { port_out(port, v, size); }

static uint32_t __cdecl my_EtsInp(uint32_t port) { return port_in(port, 1); }
static void __cdecl my_EtsOutp(uint32_t port, uint32_t v) { port_out(port, v, 1); }
static void __cdecl my_EtsOutpw(uint32_t port, uint32_t v) { port_out(port, v, 2); }

static void __cdecl my_EtsDisplayError(const char *msg)
{
    hy_log("EtsDisplayError: %s", msg ? msg : "(null)");
}

/* ---------------------------------------------------------------- install */

void hle_hw_install(void)
{
    InitializeCriticalSection(&g_intlock);
    g_if_tls = TlsAlloc();
    InitializeCriticalSection(&g_audio_irq);
    g_audio_irq_depth_tls = TlsAlloc();

    if (!nv_read("eeprom.bin", g_eeprom, EEPROM_SIZE))
        memset(g_eeprom, 0xff, EEPROM_SIZE);

    hook_name("EtsSetInterruptFlag", my_EtsSetInterruptFlag);
    hook_name("hardware_caps_driver_QueryHardwareCaps", my_hardware_caps_driver_QueryHardwareCaps);
    hook_name("pcspeaker_ToggleSpeaker", my_pcspeaker_ToggleSpeaker);

    hook_name("wrs_Init", my_wrs_Init);
    hook_name("wrs_ReadCMOSBlock", my_wrs_ReadCMOSBlock);
    hook_name("wrs_WriteCMOSBlockToDisk", my_wrs_WriteCMOSBlockToDisk);
    hook_name("wrs_EnterFileSystemWriteMode", my_wrs_FileSystemWriteMode);
    hook_name("wrs_ExitFileSystemWriteMode", my_wrs_FileSystemWriteMode);

    g_diego_receive = (PFN_diego_io_receive)(uintptr_t)sym_addr("diego_io_receive");
    hook_name("encrypt_init", my_encrypt_init);
    hook_name("encrypt_transmit", my_encrypt_transmit);
    hook_name("diego_io_read_switch_byte", my_diego_io_read_switch_byte);

    hook_name("EtsInp", my_EtsInp);
    hook_name("EtsOutp", my_EtsOutp);
    hook_name("EtsOutpw", my_EtsOutpw);
    hook_name("EtsDisplayError", my_EtsDisplayError);
    /* The operator menu (menus(), entered with the Test switch) runs as original code: it draws
     * through Glide's LFB and reads DiegoIO. Its only ETS dependency is this call, whose critical
     * section is set up by the ETS init we skip, so entering it would deadlock. */
    hook_stub("EtsSelectFileSystem", 0);

    /* Interrupt controller / ISR plumbing: nothing to program on a PC. */
    hook_stub("EtsSetInterruptHandler", 1);
    hook_stub("EtsSaveInterruptHandler", 0);
    hook_stub("EtsRestoreInterruptHandler", 1);
    hook_stub("EtsPicEOI", 0);
    hook_name("EtsPicEnable", my_EtsPicEnable);
    hook_stub("EtsSetISRPriority", 0);
    hook_stub("EtsClearISRPriority", 0);
    hook_stub("EtsSetDebugName", 0);
    hook_stub("EtsRegisterCallback", 1);
    /* No PCI bus to scan: every lookup fails. */
    hook_stub("EtsPCIInit", 0);
    hook_stub("EtsPCIFindDevice", 0);
    hook_stub("EtsPCIFindDeviceByClass", 0);
}
