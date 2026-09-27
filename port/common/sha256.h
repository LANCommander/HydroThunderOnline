#pragma once
/* SHA-256 (FIPS 180-4), for lobby password challenges. Shared by hydro.exe and htmaster. */
#include <stddef.h>
#include <stdint.h>

typedef struct Sha256 {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t fill;
} Sha256;

void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const void *data, size_t n);
void sha256_final(Sha256 *s, uint8_t out[32]);

/* SHA-256 of the concatenation a|b as 64 lowercase hex digits (b may be NULL). */
void sha256_hex2(const char *a, const char *b, char out[65]);
