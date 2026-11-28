#include "pe_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>

static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | (uint16_t)b[1]<<8); }
/* Page-touch measurement for the first image loaded: section spans are recorded
 * so guest_touch_bitmap() can be reported per section (hot .text set). */
typedef struct { char name[9]; uint32_t va, size; } PeTouchSection;
static struct {
    int active;
    uint32_t base, size, hdr;
    unsigned count;
    PeTouchSection sec[96];
} g_touch;

static unsigned long long touch_pages(uint32_t va, uint32_t size,
                                      unsigned long long *pages,
                                      unsigned long long *reused) {
    uint32_t first = va / GUEST_PAGE_SIZE;
    uint32_t last = (va + size + GUEST_PAGE_SIZE - 1u) / GUEST_PAGE_SIZE;
    const uint8_t *bits = guest_touch_bitmap();
    uint32_t total = guest_touch_page_count();
    unsigned long long touched = 0, seen = 0, again = 0;
    for (uint32_t p = first; p < last && p < total; ++p) {
        ++seen;
        if (bits[p]) ++touched;
        if (bits[p] > 1) ++again;
    }
    if (pages) *pages = seen;
    if (reused) *reused = again;
    return touched;
}
void pe_image_touch_report(FILE *f, const char *phase) {
    if (!f || !g_touch.active || !guest_touch_bitmap()) return;
    uint64_t accesses = 0, repeats = 0;
    guest_touch_stats(&accesses, &repeats);
    fprintf(f, "[touch] phase=%s image %08x size %.2f MiB accesses=%llu repeats=%llu\n",
            phase ? phase : "?", (unsigned)g_touch.base,
            (double)g_touch.size / (1024.0 * 1024.0),
            (unsigned long long)accesses, (unsigned long long)repeats);
    unsigned long long all = 0;
    for (unsigned i = 0; i <= g_touch.count; ++i) {
        const char *name;
        uint32_t va, size;
        if (i == 0) { name = "headers"; va = 0; size = g_touch.hdr; }
        else if (i <= g_touch.count - 1) { name = g_touch.sec[i - 1].name; va = g_touch.sec[i - 1].va; size = g_touch.sec[i - 1].size; }
        else { name = "(total)"; va = 0; size = g_touch.size; }
        unsigned long long seen = 0, reused = 0;
        unsigned long long touched = touch_pages(va, size, &seen, &reused);
        all += touched;
        fprintf(f, "[touch] %-8s %6.2f MiB pages=%llu touched=%llu reused=%llu "
                   "(%llu/%llu pages read twice, %.2f MiB touched, %.1f%%)\n",
                name, (double)size / (1024.0 * 1024.0),
                (unsigned long long)seen, touched, reused, reused, seen,
                (double)touched * GUEST_PAGE_SIZE / (1024.0 * 1024.0),
                seen ? 100.0 * (double)touched / (double)seen : 0.0);
    }
    fprintf(f, "[touch] hot set %.2f MiB of %.2f MiB image\n",
            (double)all * GUEST_PAGE_SIZE / (1024.0 * 1024.0),
            (double)g_touch.size / (1024.0 * 1024.0));
}
static uint32_t rd32(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1]<<8 | (uint32_t)b[2]<<16 | (uint32_t)b[3]<<24; }
static int within(uint32_t pos, uint32_t n, uint32_t limit) { return pos<=limit && n<=limit-pos; }
static int at(FILE *f, uint32_t pos, void *buf, size_t n) {
    return pos>INT32_MAX || fseek(f,(long)pos,SEEK_SET) || fread(buf,1,n,f)!=n;
}

/* ---- file-backed image provider ---------------------------------------------
 * The image is mapped as a slab: no section is copied at load. A page is read
 * from the PE file on first touch and stays in the slab window while it is
 * used. Writable sections (guest globals, BSS) are pinned at load so they are
 * never evicted; the window only bounds read-only code and resource pages. */
typedef struct {
    FILE *f;
    uint32_t hdr, nsections, base;
    struct { uint32_t va, bytes, off, extent, writable; } sec[96];
} PeProvider;

/* Port compatibility patches applied when a read-only image page is filled.
 * The startup branch in WinMain (0x69ddf9, `je load-path`) chooses between the
 * main menu and the PC start-cell/save probe.  This port only supports the main
 * menu, and on device that probe can take the load path; replace the branch
 * with two NOPs so the menu path is always taken.  The image itself is never
 * modified on disk and every other byte is untouched. */
typedef struct { uint32_t va; uint8_t old[4], now[4]; unsigned len; } PePatch;
static const PePatch g_pe_patches[] = {
    {0x0069ddf9u, {0x74, 0x59, 0, 0}, {0x90, 0x90, 0, 0}, 2},
};
static int pe_apply_patches(PeProvider *p, uint32_t rva, uint32_t n,
                            uint8_t *dst) {
    int applied = 0;
    if (!p || p->base != 0x00400000u) return 0;
    for (unsigned i = 0; i < sizeof g_pe_patches / sizeof g_pe_patches[0]; ++i) {
        const PePatch *q = &g_pe_patches[i];
        uint32_t qrva = q->va - p->base;
        if (qrva < rva || qrva + q->len > rva + n) continue;
        uint8_t *at = dst + (qrva - rva);
        if (memcmp(at, q->old, q->len)) continue;
        memcpy(at, q->now, q->len);
        static unsigned logged;
        if (logged < 8) { ++logged;
            fprintf(stderr, "[patch] image patch %08lx: %u byte(s) applied "
                            "(startup always takes the main-menu path)\n",
                    (unsigned long)q->va, q->len); }
        applied = 1;
    }
    return applied;
}

static int pe_fill(void *ctx, uint64_t offset, uint8_t *dst, size_t n,
                   size_t *got) {
    PeProvider *p = (PeProvider *)ctx;
    if (!p || !p->f || offset > UINT32_MAX) return -1;
    uint32_t rva = (uint32_t)offset;
    uint64_t r0 = rva, r1 = (uint64_t)rva + n;
    memset(dst, 0, n);
    if (rva < p->hdr) {
        uint64_t b = r1 < p->hdr ? r1 : p->hdr;
        if (b > r0 && at(p->f, rva, dst, (size_t)(b - r0))) return -1;
    }
    for (unsigned i = 0; i < p->nsections; ++i) {
        uint64_t s0 = p->sec[i].va, s1 = s0 + p->sec[i].bytes;
        uint64_t a = s0 > r0 ? s0 : r0, b = s1 < r1 ? s1 : r1;
        if (a >= b) continue;
        if (at(p->f, p->sec[i].off + (uint32_t)(a - s0), dst + (a - r0),
               (size_t)(b - a))) return -1;
    }
    pe_apply_patches(p, rva, (uint32_t)n, dst);
    *got = n;
    return 0;
}
static void pe_provider_close(void *ctx) {
    PeProvider *p = (PeProvider *)ctx;
    if (!p) return;
    if (p->f) fclose(p->f);
    free(p);
}
/* Evictable-page budget for the read-only part of an image. The measured hot
 * set at the main menu is 3.18 MiB of .text + 0.87 MiB of .rdata + headers,
 * so 5 MiB fits it with headroom; 0 disables the window. */
static uint32_t pe_window_pages(void) {
    static int cached = -1;
    static unsigned long mb = 5;
    if (cached < 0) {
        const char *s = getenv("N3DS_TEXT_WINDOW_MB");
        cached = 1;
        if (s && *s) mb = strtoul(s, NULL, 0);
    }
    return mb > UINT32_MAX / 256u ? 0 : (uint32_t)(mb * 256u);
}
static int relocate(GuestMemory *m, const GuestImage *im, uint32_t rva, uint32_t size) {
    if (im->base==im->preferred_base) return 0;
    if (!rva || !size || !within(rva,size,im->size)) return -1;
    uint8_t *table=guest_ptr(m,im->base+rva,size);
    if (!table) return -1;
    uint32_t delta=im->base-im->preferred_base, p=0;
    while (p<size) {
        if (size-p<8) return -1;
        uint32_t page=rd32(table+p), span=rd32(table+p+4);
        if (span<8 || span>size-p || (span&1)) return -1;
        for (uint32_t k=8;k<span;k+=2) {
            uint16_t e=rd16(table+p+k);
            unsigned type=e>>12;
            if (!type) continue;
            uint64_t offset=(uint64_t)page+(e&0xfff);
            if (type!=3 || offset>UINT32_MAX || !within((uint32_t)offset,4,im->size)) return -1;
            /* Relocation metadata must remain unchanged during iteration. */
            if (offset<(uint64_t)rva+size && rva<offset+4) return -1;
            uint32_t addr=im->base+(uint32_t)offset, value;
            if (guest_read32(m,addr,&value) || guest_write32(m,addr,value+delta)) return -1;
        }
        p+=span;
    }
    return 0;
}

int pe_image_load(GuestMemory *m, const char *path, uint32_t want_base, GuestImage *out) {
    FILE *f=fopen(path,"rb");
    if (!f) return PE_IO;
    uint8_t dos[64], coff[24], opt[224], sec[40];
    int rc=PE_FORMAT, mapped=0;
    GuestImage im={0};
    if (fseek(f,0,SEEK_END)) { rc=PE_IO; goto done; }
    long end=ftell(f);
    if (end<64 || (uint64_t)end>INT32_MAX) goto done;
    uint32_t file_size=(uint32_t)end;
    if (at(f,0,dos,sizeof dos) || rd16(dos)!=0x5a4d) goto done;
    uint32_t pe=rd32(dos+60);
    if (pe<64 || !within(pe,sizeof coff,file_size) || at(f,pe,coff,sizeof coff) ||
        memcmp(coff,"PE\0\0",4) || rd16(coff+4)!=0x14c) goto done;
    im.sections=rd16(coff+6);
    uint32_t opt_size=rd16(coff+20), opt_at=pe+24;
    if (!im.sections || im.sections>96 || opt_size<96 || !within(opt_at,opt_size,file_size)) goto done;
    memset(opt,0,sizeof opt);
    size_t read_size=opt_size<sizeof opt?opt_size:sizeof opt;
    if (at(f,opt_at,opt,read_size) || rd16(opt)!=0x10b) goto done;
    uint32_t nrva=rd32(opt+92);
    if (nrva>16) nrva=16;
    if (96+nrva*8>opt_size) goto done;
    im.preferred_base=rd32(opt+28); im.base=want_base?want_base:im.preferred_base;
    im.size=rd32(opt+56);
    uint32_t hdr=rd32(opt+60), entry=rd32(opt+16), table=opt_at+opt_size;
    if (!im.size || !hdr || hdr>im.size || hdr>file_size || !within(table,40u*im.sections,hdr) ||
        entry>=im.size || (uint64_t)im.base+im.size>(UINT64_C(1)<<32)) goto done;
    im.entry=entry?im.base+entry:0;
    if (nrva>0) {im.export_rva=rd32(opt+96);im.export_size=rd32(opt+100);}
    if (nrva>1) { im.import_rva=rd32(opt+104); im.import_size=rd32(opt+108); }
    if (nrva>9) { im.tls_rva=rd32(opt+168); im.tls_size=rd32(opt+172); }
    if (!within(im.import_rva,im.import_size,im.size) || !within(im.export_rva,im.export_size,im.size) ||
        !within(im.tls_rva,im.tls_size,im.size)) goto done;
    /* Reserve a page-aligned slab; nothing is copied, pages come from the file
     * through the provider below. */
    if ((im.base & (GUEST_PAGE_SIZE-1u)) ||
        !guest_slab_reserve(m, im.base,
                            (im.size + GUEST_PAGE_SIZE - 1u) & ~(GUEST_PAGE_SIZE - 1u))) {
        rc=PE_MEMORY; goto done;
    }
    mapped=1;
    PeProvider *prov=(PeProvider *)calloc(1,sizeof *prov);
    if (!prov) { rc=PE_MEMORY; goto done; }
    prov->f=f; prov->hdr=hdr; prov->nsections=im.sections; prov->base=im.base;
    /* Reject overlapping section footprints and build the page provider. */
    uint32_t starts[96], sizes[96];
    for (unsigned i=0;i<im.sections;++i) {
        if (at(f,table+40*i,sec,sizeof sec)) { rc=PE_IO; free(prov); goto done; }
        uint32_t vs=rd32(sec+8), va=rd32(sec+12), raw=rd32(sec+16), off=rd32(sec+20);
        uint32_t extent=vs?vs:raw, copy=raw<extent?raw:extent;
        if (va<hdr || !within(va,extent,im.size) || (copy && !within(off,copy,file_size))) {
            rc=PE_FORMAT; free(prov); goto done;
        }
        for(unsigned j=0;j<i;++j) if (extent && sizes[j] && va<starts[j]+sizes[j] && starts[j]<va+extent) {
            rc=PE_FORMAT; free(prov); goto done;
        }
        starts[i]=va; sizes[i]=extent;
        prov->sec[i].va=va; prov->sec[i].bytes=copy; prov->sec[i].off=off;
        prov->sec[i].extent=extent;
        prov->sec[i].writable=rd32(sec+36)&0x80000000u?1:0;
        if (!g_touch.active && !guest_touch_active() && i < 96) {
            memcpy(g_touch.sec[i].name, sec, 8);
            g_touch.sec[i].name[8] = 0;
            g_touch.sec[i].va = va;
            g_touch.sec[i].size = extent;
            g_touch.count = i + 1;
        }
    }
    if (guest_slab_set_provider(m,im.base,0x04,0,pe_fill,NULL,prov,0)) {
        f=prov->f; free(prov); rc=PE_MEMORY; goto done;
    }
    f=NULL;   /* the provider owns the FILE now */
    guest_slab_set_provider_close(m,im.base,pe_provider_close);
    guest_slab_set_type(m,im.base,0x20000);   /* same MEM_* as a mapped region */
    guest_slab_set_window(m,im.base,pe_window_pages());
    for (unsigned i=0;i<im.sections;++i)
        if (prov->sec[i].writable)
            guest_slab_pin_range(m,im.base,im.base+prov->sec[i].va,prov->sec[i].extent);
    if (relocate(m,&im,nrva>5?rd32(opt+136):0,nrva>5?rd32(opt+140):0)) { rc=PE_RELOCATION; goto done; }
    if (!g_touch.active && !guest_touch_active() && g_touch.count == im.sections) {
        g_touch.active = 1; g_touch.base = im.base; g_touch.size = im.size;
        g_touch.hdr = hdr;
        guest_touch_begin(im.base, im.size);
    }
    *out=im; rc=PE_OK;
done:
    if (mapped && rc) pe_image_unload(m,im.base);
    if (f) fclose(f);
    return rc;
}
void pe_image_unload(GuestMemory *m, uint32_t base) {
    if (!m) return;
    if (guest_slab_find(m, base, 1)) guest_slab_release(m, base);
    else guest_unmap(m, base);
}
const char *pe_error(int code) {
    switch(code) {case PE_OK:return "OK";case PE_IO:return "file I/O";case PE_FORMAT:return "invalid PE32 image";
    case PE_MEMORY:return "guest memory budget/overlap";case PE_RELOCATION:return "unsupported relocations";default:return "unknown";}
}
