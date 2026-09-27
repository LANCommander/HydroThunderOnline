/*
 * Map HYDRO.EXE at its fixed ImageBase and rebuild DATA/.bss the way the ETS loader does:
 * those sections have no file bytes; their contents come from a stream of fill/copy
 * records stored in CODE.
 */
#include "host.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { INIT_OP_FILL = 0x04, INIT_OP_COPY = 0x06 };

static uint8_t g_tdhack[4096]; /* target of the lone embkern.dll import (__p_tdhack) */

/*
 * Address-space placeholder. By the time main() runs, the kernel has already put the initial
 * stack, heaps and NLS mappings in low memory, so 0x100000 cannot be claimed at runtime.
 * Instead hydro.exe is linked at 0x10000 and carries this uninitialized section (no file
 * space). Wherever the linker places it relative to the host's other sections, it spans
 * 0x100000-0x7a3000 as long as the host's own sections stay under ~900 KB.
 */
#define HY_SPACE_SIZE 0x7a0000u
#pragma bss_seg(".hyimg")
static uint8_t g_image_space[HY_SPACE_SIZE];
#pragma bss_seg()

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        hy_fatal("cannot open %s", path);
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(*size);
    if (!d || fread(d, 1, *size, f) != *size)
        hy_fatal("cannot read %s", path);
    fclose(f);
    return d;
}

static void describe_region(uint32_t base)
{
    MEMORY_BASIC_INFORMATION mbi;
    for (uint32_t a = base; a < base + 0x800000 && VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof mbi);
         a = (uint32_t)(uintptr_t)mbi.BaseAddress + (uint32_t)mbi.RegionSize) {
        if (mbi.State != MEM_FREE)
            hy_log("  %08x +%08x state=%lx type=%lx", (uint32_t)(uintptr_t)mbi.BaseAddress,
                   (uint32_t)mbi.RegionSize, mbi.State, mbi.Type);
    }
}

uint32_t image_load(const char *exe_path)
{
    size_t size;
    uint8_t *d = read_file(exe_path, &size);
    if (size < 0x400 || d[0] != 'M' || d[1] != 'Z')
        hy_fatal("%s is not a PE file", exe_path);

    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(d + *(uint32_t *)(d + 0x3c));
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    unsigned nsec = nt->FileHeader.NumberOfSections;
    uint32_t base = nt->OptionalHeader.ImageBase;
    uint32_t image_size = nt->OptionalHeader.SizeOfImage;
    if (base != HY_IMAGE_BASE)
        hy_fatal("unexpected ImageBase %08x", base);

    uint8_t *img = (uint8_t *)(uintptr_t)base;
    uintptr_t lo_space = (uintptr_t)g_image_space, hi_space = lo_space + HY_SPACE_SIZE;
    DWORD old;
    if (base < lo_space || base + image_size > hi_space ||
        !VirtualProtect(img, image_size, PAGE_EXECUTE_READWRITE, &old)) {
        hy_log("placeholder .hyimg is %08x-%08x; memory around the image base:", (uint32_t)lo_space,
               (uint32_t)hi_space);
        describe_region(base);
        hy_fatal("cannot map the image at %08x-%08x", base, base + image_size);
    }

    memcpy(img, d, nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER *code = NULL, *data = NULL, *bss = NULL;
    for (unsigned i = 0; i < nsec; i++) {
        IMAGE_SECTION_HEADER *s = &sec[i];
        if (s->PointerToRawData && s->SizeOfRawData) {
            uint32_t n = s->SizeOfRawData;
            if (s->Misc.VirtualSize && s->Misc.VirtualSize < n)
                n = s->Misc.VirtualSize;
            memcpy(img + s->VirtualAddress, d + s->PointerToRawData, n);
        }
        if (!memcmp(s->Name, "CODE", 5))
            code = s;
        else if (!memcmp(s->Name, "DATA", 5))
            data = s;
        else if (!memcmp(s->Name, ".bss", 5))
            bss = s;
    }
    if (!code || !data || !bss)
        hy_fatal("missing CODE/DATA/.bss sections");

    /* The stream opens with a fill record targeting the base of DATA. */
    uint8_t opener[5] = {INIT_OP_FILL};
    *(uint32_t *)(opener + 1) = base + data->VirtualAddress;
    uint8_t *p = NULL, *stop = d + code->PointerToRawData + code->SizeOfRawData;
    for (uint8_t *q = d + code->PointerToRawData; q + 5 <= stop; q++)
        if (!memcmp(q, opener, 5)) {
            p = q;
            break;
        }
    if (!p)
        hy_fatal("ETS data-init stream not found");

    uint32_t lo = base + data->VirtualAddress;
    uint32_t hi = base + bss->VirtualAddress + bss->Misc.VirtualSize;
    unsigned records = 0;
    while (p + 7 <= stop && (*p == INIT_OP_FILL || *p == INIT_OP_COPY)) {
        uint32_t addr = *(uint32_t *)(p + 1);
        uint16_t len = *(uint16_t *)(p + 5);
        if (len == 0 || addr < lo || addr + len > hi)
            break;
        if (*p == INIT_OP_FILL) {
            memset((void *)(uintptr_t)addr, p[7], len);
            p += 8;
        } else {
            memcpy((void *)(uintptr_t)addr, p + 7, len);
            p += 7 + len;
        }
        records++;
    }

    /* Imports hold absolute VAs. The only one is embkern.dll!__p_tdhack (data). */
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir->VirtualAddress) {
        IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(img + dir->VirtualAddress);
        for (; imp->Name; imp++) {
            uint32_t *iat = (uint32_t *)(uintptr_t)(imp->FirstThunk >= base ? imp->FirstThunk
                                                                           : base + imp->FirstThunk);
            for (; *iat; iat++)
                *iat = (uint32_t)(uintptr_t)g_tdhack;
        }
    }

    free(d);
    hy_log("image: %08x-%08x mapped, %u data-init records", base, base + image_size, records);
    return image_size;
}
