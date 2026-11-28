/* Pageable PE image: file-backed read-only sections, COW on write, lazy BSS.
 *
 * The image is mapped as a slab with a PE provider instead of one calloc'd
 * region.  Pages are backed from the file on first touch, evicted through the
 * existing slab window, and a page that differs from its file bytes (the guest
 * wrote it) is pinned as an anonymous copy instead of being written back.
 * This test builds a synthetic PE32 image and checks:
 *   - untouched pages cost no host backing (BSS included),
 *   - every read returns the exact file bytes,
 *   - an evicted page is re-read from the file unchanged,
 *   - a written page survives eviction (COW) and the file is not modified.
 */
#include "guest.h"
#include "pe_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "guest_paging_smoke: FAIL line %d: %s\n", __LINE__, #c); \
    return 1; } } while (0)

#define BASE      0x10000000u
#define HEADER    0x200u
#define RAW       0x100000u          /* 1 MiB of file-backed .text          */
#define VSIZE     0x800000u          /* 8 MiB section: 7 MiB zero-fill BSS  */
#define IMAGE_SZ  0x802000u          /* covers va 0x1000 + 8 MiB section     */

static const char *g_path;

static void w16(unsigned char *p, unsigned v) { p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8); }
static void w32(unsigned char *p, unsigned v) {
    p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8);
    p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24);
}

static int write_pe(const char *path) {
    unsigned char *img = (unsigned char *)calloc(1, HEADER + RAW);
    if (!img) return -1;
    w16(img + 0, 0x5a4d);
    w32(img + 0x3c, 0x40);
    memcpy(img + 0x40, "PE\0\0", 4);
    w16(img + 0x44, 0x14c);          /* i386 */
    w16(img + 0x46, 1);              /* one section */
    w16(img + 0x54, 0xe0);           /* SizeOfOptionalHeader */
    unsigned char *opt = img + 0x58;
    w16(opt + 0, 0x10b);
    w32(opt + 16, 0x1000);           /* entry RVA */
    w32(opt + 28, BASE);             /* ImageBase (loads at preferred) */
    w32(opt + 32, 0x1000);           /* SectionAlignment */
    w32(opt + 36, 0x200);            /* FileAlignment */
    w32(opt + 56, IMAGE_SZ);         /* SizeOfImage */
    w32(opt + 60, HEADER);           /* SizeOfHeaders */
    unsigned char *sec = img + 0x58 + 0xe0;
    memcpy(sec, ".text\0\0\0", 8);
    w32(sec + 8, VSIZE);             /* VirtualSize */
    w32(sec + 12, 0x1000);           /* VirtualAddress */
    w32(sec + 16, RAW);              /* SizeOfRawData */
    w32(sec + 20, HEADER);           /* PointerToRawData */
    w32(sec + 36, 0x60000020u);      /* code | execute | read */
    for (unsigned i = 0; i < RAW; ++i)
        img[HEADER + i] = (unsigned char)(i * 31u + (i >> 9));
    FILE *f = fopen(path, "wb");
    if (!f) { free(img); return -1; }
    size_t n = fwrite(img, 1, HEADER + RAW, f);
    fclose(f);
    free(img);
    return n == HEADER + RAW ? 0 : -1;
}

static unsigned char pattern(unsigned off) {
    return (unsigned char)(off * 31u + (off >> 9));
}

static int file_byte(unsigned off) {
    FILE *f = fopen(g_path, "rb");
    if (!f || fseek(f, (long)(HEADER + off), SEEK_SET)) { if (f) fclose(f); return -1; }
    int c = fgetc(f);
    fclose(f);
    return c;
}

int main(void) {
    char path[256];
    const char *tmp = getenv("TEMP");
    if (!tmp || !*tmp) tmp = ".";
    snprintf(path, sizeof path, "%s/guest_paging_smoke.pe", tmp);
    g_path = path;
    CHECK(write_pe(path) == 0);

    /* 1 MiB window: sequential touches force evictions inside one section. */
#if defined(_WIN32)
    _putenv_s("N3DS_TEXT_WINDOW_MB", "1");
#else
    setenv("N3DS_TEXT_WINDOW_MB", "1", 1);
#endif
    GuestMemory m;
    guest_init(&m, 64u << 20);
    GuestImage im;
    int rc = pe_image_load(&m, path, 0, &im);
    if (rc) fprintf(stderr, "load rc=%d %s\n", rc, pe_error(rc));
    CHECK(rc == PE_OK);
    CHECK(im.base == BASE);
    /* Nothing is materialised at load: no section copy. */
    CHECK(guest_slab_backed(&m) == 0);

    /* Reading the first page backs exactly that page from the file. */
    unsigned char buf[64];
    CHECK(guest_read(&m, BASE + 0x1000, buf, sizeof buf) == 0);
    for (unsigned i = 0; i < sizeof buf; ++i) CHECK(buf[i] == pattern(i));
    CHECK(guest_slab_backed(&m) == 4096);
    CHECK(guest_read(&m, BASE + 0x1ff0, buf, 0x20) == 0);   /* cross page */
    CHECK(buf[0] == pattern(0xff0));
    CHECK(buf[16] == pattern(0x1000));                      /* next page */

    /* Untouched BSS stays unbacked: only 3 pages are resident now. */
    CHECK(guest_slab_backed(&m) <= 3u * 4096u);

    /* Write a file-backed page, force it out of the window, read it back. */
    unsigned char mark[4] = {0xde, 0xad, 0xbe, 0xef};
    CHECK(guest_write(&m, BASE + 0x1000 + 0x100, mark, sizeof mark) == 0);
    unsigned char scan[64];
    for (unsigned off = 0; off < 0x300000u; off += sizeof scan)   /* 3 MiB */
        CHECK(guest_read(&m, BASE + 0x1000 + off, scan, sizeof scan) == 0);
    CHECK(guest_read(&m, BASE + 0x1000 + 0x100, buf, sizeof mark) == 0);
    CHECK(memcmp(buf, mark, sizeof mark) == 0);      /* COW survived eviction */
    CHECK(file_byte(0x100) == pattern(0x100));       /* file untouched */

    /* A clean evicted page still matches the file exactly. */
    CHECK(guest_read(&m, BASE + 0x1000 + 0x40, buf, 4) == 0);
    for (unsigned i = 0; i < 4; ++i) CHECK(buf[i] == pattern(0x40 + i));

    /* BSS is zero on read, writable, and the write survives eviction. */
    unsigned bss = 0x600000u;
    CHECK(guest_read(&m, BASE + 0x1000 + bss, buf, 4) == 0);
    for (unsigned i = 0; i < 4; ++i) CHECK(buf[i] == 0);
    CHECK(guest_write(&m, BASE + 0x1000 + bss, mark, sizeof mark) == 0);
    for (unsigned off = 0; off < 0x300000u; off += sizeof scan)
        CHECK(guest_read(&m, BASE + 0x1000 + off, scan, sizeof scan) == 0);
    CHECK(guest_read(&m, BASE + 0x1000 + bss, buf, sizeof mark) == 0);
    CHECK(memcmp(buf, mark, sizeof mark) == 0);
    CHECK(file_byte(0) == pattern(0));               /* BSS never hits the file */

    /* The window bounds the resident file-backed pages. */
    CHECK(guest_slab_backed(&m) < (4u << 20));

    guest_destroy(&m);
    remove(path);
    printf("guest_paging_smoke: PASS\n");
    return 0;
}
