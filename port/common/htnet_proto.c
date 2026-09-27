#ifdef _WIN32
#define _CRT_RAND_S
#include <windows.h>
#else
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include <unistd.h>
#endif

#include "htnet_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ht_oob_parse(const uint8_t *pkt, int n, char *buf, size_t cap, const char **info)
{
    if (n < 5 || cap < 2 || pkt[0] != 0xff || pkt[1] != 0xff || pkt[2] != 0xff || pkt[3] != 0xff)
        return 0;
    size_t len = (size_t)n - 4 < cap - 1 ? (size_t)n - 4 : cap - 1;
    memcpy(buf, pkt + 4, len);
    buf[len] = 0;
    char *nl = strchr(buf, '\n');
    if (nl) {
        *nl = 0;
        *info = nl + 1;
    } else {
        *info = buf + len;
    }
    return 1;
}

int ht_oob_build(uint8_t *out, size_t cap, const char *text)
{
    size_t len = strlen(text);
    if (len + 4 > cap)
        len = cap - 4;
    memset(out, 0xff, 4);
    memcpy(out + 4, text, len);
    return (int)(len + 4);
}

int ht_info_get(const char *info, const char *key, char *out, size_t n)
{
    size_t klen = strlen(key);
    const char *p = info;
    if (n)
        out[0] = 0;
    while (p && *p == '\\') {
        const char *k = p + 1;
        const char *ke = strchr(k, '\\');
        if (!ke)
            return 0;
        const char *v = ke + 1;
        const char *ve = v;
        while (*ve && *ve != '\\' && *ve != '\n')
            ve++;
        if ((size_t)(ke - k) == klen && !memcmp(k, key, klen)) {
            size_t vlen = (size_t)(ve - v);
            if (n) {
                if (vlen >= n)
                    vlen = n - 1;
                memcpy(out, v, vlen);
                out[vlen] = 0;
            }
            return 1;
        }
        p = ve;
    }
    return 0;
}

int ht_info_get_int(const char *info, const char *key, int def)
{
    char v[24];
    return ht_info_get(info, key, v, sizeof v) && v[0] ? atoi(v) : def;
}

void ht_sanitize(char *s)
{
    for (; *s; s++)
        if (*s < 0x20 || *s > 0x7e || *s == '\\' || *s == '"' || *s == ';')
            *s = '_';
}

void ht_info_set(char *info, size_t cap, const char *key, const char *value)
{
    char v[256];
    snprintf(v, sizeof v, "%s", value ? value : "");
    ht_sanitize(v);
    size_t len = strlen(info);
    if (len < cap)
        snprintf(info + len, cap - len, "\\%s\\%s", key, v);
}

void ht_info_set_int(char *info, size_t cap, const char *key, int value)
{
    char v[16];
    snprintf(v, sizeof v, "%d", value);
    ht_info_set(info, cap, key, v);
}

/* ---------------------------------------------------------------- game options */

#define GAMEOPTS_FORMAT 1

/* min, max, step per field, in HtGameOpts order (the difficulties repeat per track) */
static void gameopts_range(int i, int *lo, int *hi, int *step)
{
    static const int k[9][3] = {{0, 1, 1}, {0, 1, 1}, {0, 1, 1}, {10, 60, 1}, {10, 60, 1},
                                {10, 60, 1}, {10, 60, 1}, {0, 1, 1}, {5, 50, 5}};
    if (i < 9) {
        *lo = k[i][0], *hi = k[i][1], *step = k[i][2];
    } else {
        *lo = 1, *hi = 100, *step = 1;
    }
}

void ht_gameopts_defaults(HtGameOpts *o)
{
    memset(o, 0, sizeof *o);
    o->freeplay = 1; /* the port's default (operator_install) */
    o->t_track = 15;
    o->t_boat = 15;
    o->t_hiscore = 20;
    o->t_continue = 17;
    o->limit_pct = 20;
    for (int t = 0; t < HTNET_TRACKS; t++) {
        o->track_diff[t] = 40;
        o->ai_diff[t] = 50;
    }
}

void ht_gameopts_clamp(HtGameOpts *o)
{
    int *v = (int *)o;
    for (int i = 0; i < HTNET_GAMEOPTS_INTS; i++) {
        int lo, hi, step;
        gameopts_range(i, &lo, &hi, &step);
        if (v[i] < lo)
            v[i] = lo;
        if (v[i] > hi)
            v[i] = hi;
        v[i] -= (v[i] - lo) % step;
    }
}

void ht_gameopts_encode(const HtGameOpts *o, char *out, size_t cap)
{
    const int *v = (const int *)o;
    size_t len = (size_t)snprintf(out, cap, "%d", GAMEOPTS_FORMAT);
    for (int i = 0; i < HTNET_GAMEOPTS_INTS && len < cap; i++)
        len += (size_t)snprintf(out + len, cap - len, ",%d", v[i]);
}

int ht_gameopts_decode(const char *s, HtGameOpts *o)
{
    char *end;
    if (!s || strtol(s, &end, 10) != GAMEOPTS_FORMAT)
        return 0;
    int v[HTNET_GAMEOPTS_INTS];
    for (int i = 0; i < HTNET_GAMEOPTS_INTS; i++) {
        if (*end != ',')
            return 0;
        s = end + 1;
        v[i] = (int)strtol(s, &end, 10);
        if (end == s)
            return 0;
    }
    memcpy(o, v, sizeof v);
    ht_gameopts_clamp(o);
    return 1;
}

uint64_t ht_now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
#endif
}

uint32_t ht_random32(void)
{
    uint32_t r = 0;
#ifdef _WIN32
    unsigned v;
    if (rand_s(&v) == 0)
        r = v;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(&r, sizeof r, 1, f) != 1)
            r = 0;
        fclose(f);
    }
#endif
    if (!r) {
        static uint32_t x = 0x9e3779b9u;
        x ^= (uint32_t)ht_now_ms() * 2654435761u;
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        r = x;
    }
    return r ? r : 1;
}
