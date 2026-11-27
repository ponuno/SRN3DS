#define _CRT_SECURE_NO_WARNINGS
#include "bink_decode.h"
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32) && !defined(__3DS__)
#define BINK_HOST_DECODER 1
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#ifdef BINK_HOST_DECODER
static unsigned g_store_seq;

/* Locate the host FFmpeg: explicit env, then PATH, then the common per-user
 * essentials install.  Returns 1 and fills out, or 0. */
static int find_ffmpeg(char *out, size_t cap) {
    const char *env = getenv("TESV_BINK_FFMPEG");
    char local[MAX_PATH], pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    if (env && *env) { snprintf(out, cap, "%s", env); return 1; }
    if (SearchPathA(NULL, "ffmpeg.exe", NULL, (DWORD)cap, out, NULL)) return 1;
    if (GetEnvironmentVariableA("LOCALAPPDATA", local, (DWORD)sizeof local)) {
        snprintf(pat, sizeof pat, "%s\\ffmpeg\\*", local);
        h = FindFirstFileA(pat, &fd);
        while (h != INVALID_HANDLE_VALUE) {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) {
                snprintf(out, cap, "%s\\ffmpeg\\%s\\bin\\ffmpeg.exe", local, fd.cFileName);
                if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) {
                    FindClose(h);
                    return 1;
                }
            }
            if (!FindNextFileA(h, &fd)) break;
        }
        if (h != INVALID_HANDLE_VALUE) FindClose(h);
    }
    return 0;
}

static int run_ffmpeg(const char *exe, const char *cmdline) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char *line = (char *)malloc(strlen(cmdline) + 1);
    DWORD code = 1;
    BOOL ok;
    if (!line) return -1;
    strcpy(line, cmdline);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    memset(&pi, 0, sizeof pi);
    ok = CreateProcessA(exe, line, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi);
    free(line);
    if (!ok) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

static void cache_dir(char *out, size_t cap) {
    const char *env = getenv("TESV_BINK_CACHE");
    if (env && *env) {
        snprintf(out, cap, "%s", env);
        return;
    }
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA((DWORD)sizeof tmp, tmp);
    if (!n || n >= sizeof tmp) snprintf(tmp, sizeof tmp, ".");
    snprintf(out, cap, "%s%ssrn3ds_bink_%lu", tmp,
             (n && (tmp[n - 1] == '\\' || tmp[n - 1] == '/')) ? "" : "\\",
             (unsigned long)GetCurrentProcessId());
}
#endif

int bink_decode_open(const char *host_path, BinkDecoded *out) {
    unsigned char hdr[64];
    FILE *f;
    if (!host_path || !*host_path || !out) return BINK_DECODE_BAD_ARG;
    memset(out, 0, sizeof *out);
    f = fopen(host_path, "rb");
    if (!f) return BINK_DECODE_BAD_ARG;
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) { fclose(f); return BINK_DECODE_BAD_FORMAT; }
    fclose(f);
    /* Bink 1: "BIK" + version char, frame count at 8, width at 0x14,
     * height at 0x18, frame-rate numerator/divisor at 0x1c/0x20. */
    if (memcmp(hdr, "BIK", 3) != 0) return BINK_DECODE_BAD_FORMAT;
    out->frames   = rd32(hdr + 0x08);
    out->width    = (int32_t)rd32(hdr + 0x14);
    out->height   = (int32_t)rd32(hdr + 0x18);
    out->fps_num  = rd32(hdr + 0x1c);
    out->fps_den  = rd32(hdr + 0x20);
    if (out->width <= 0 || out->height <= 0 ||
        out->width > 8192 || out->height > 8192 ||
        out->frames == 0 || out->frames > 1000000) return BINK_DECODE_BAD_FORMAT;
    out->frame_bytes = (size_t)out->width * (size_t)out->height * 4u;

#ifndef BINK_HOST_DECODER
    (void)host_path;
    return BINK_DECODE_NO_DECODER;
#else
    {
        char dir[512], cmd[1800], ff[MAX_PATH];
        FILE *probe;
        long stored;
        if (!find_ffmpeg(ff, sizeof ff)) {
            fprintf(stderr, "[bink] no ffmpeg.exe on this host; cannot decode %s\n", host_path);
            return BINK_DECODE_NO_DECODER;
        }
        cache_dir(dir, sizeof dir);
        CreateDirectoryA(dir, NULL);
        snprintf(out->raw_path, sizeof out->raw_path, "%s\\bink_%lu_%u.bgra",
                 dir, (unsigned long)GetCurrentProcessId(), g_store_seq++);
        snprintf(cmd, sizeof cmd,
                 "\"%s\" -v error -y -i \"%s\" -f rawvideo -pix_fmt bgra \"%s\"",
                 ff, host_path, out->raw_path);
        if (run_ffmpeg(ff, cmd) != 0) {
            fprintf(stderr, "[bink] ffmpeg decode failed for %s\n  cmd=%s\n", host_path, cmd);
            remove(out->raw_path);
            return BINK_DECODE_FAILED;
        }
        probe = fopen(out->raw_path, "rb");
        if (!probe) return BINK_DECODE_FAILED;
        if (fseek(probe, 0, SEEK_END) != 0) { fclose(probe); return BINK_DECODE_FAILED; }
        stored = ftell(probe);
        fclose(probe);
        if (stored <= 0 || (size_t)stored % out->frame_bytes != 0) {
            fprintf(stderr, "[bink] decoded store %ld bytes is not %d x %d x 4 x frames\n",
                    stored, out->width, out->height);
            remove(out->raw_path);
            return BINK_DECODE_FAILED;
        }
        {
            uint32_t actual = (uint32_t)((size_t)stored / out->frame_bytes);
            if (actual != out->frames) {
                fprintf(stderr, "[bink] header says %u frames, decoder produced %u\n",
                        out->frames, actual);
                out->frames = actual;
            }
        }
        out->fp = fopen(out->raw_path, "rb");
        if (!out->fp) return BINK_DECODE_FAILED;
        return BINK_DECODE_OK;
    }
#endif
}

void bink_decode_close(BinkDecoded *out) {
    if (!out) return;
    if (out->fp) { fclose(out->fp); out->fp = NULL; }
    /* Keep the decoded store: a later open in the same run can reuse temp space;
     * the OS temp cleaner owns eventual removal. */
}

static int put_pixel(unsigned char *dst, const unsigned char *src, uint32_t fmt) {
    unsigned char b = src[0], g = src[1], r = src[2], a = src[3];
    switch (fmt & 7u) {
    case BINK_COPY_BGRA: dst[0]=b; dst[1]=g; dst[2]=r; dst[3]=a; return 4;
    case BINK_COPY_RGBA: dst[0]=r; dst[1]=g; dst[2]=b; dst[3]=a; return 4;
    case BINK_COPY_ABGR: dst[0]=a; dst[1]=b; dst[2]=g; dst[3]=r; return 4;
    case BINK_COPY_ARGB: dst[0]=a; dst[1]=r; dst[2]=g; dst[3]=b; return 4;
    case BINK_COPY_RGB:  dst[0]=r; dst[1]=g; dst[2]=b; return 3;
    case BINK_COPY_BGR:  dst[0]=b; dst[1]=g; dst[2]=r; return 3;
    case BINK_COPY_565: {
        unsigned v = ((unsigned)(r >> 3) << 11) | ((unsigned)(g >> 2) << 5) | (unsigned)(b >> 3);
        dst[0] = (unsigned char)(v & 0xffu);
        dst[1] = (unsigned char)(v >> 8);
        return 2;
    }
    default: return 0;
    }
}

int bink_decode_copy_rect(BinkDecoded *d, uint32_t frame,
                          void *dst, uint32_t dst_pitch, uint32_t dst_height,
                          uint32_t dst_x, uint32_t dst_y,
                          uint32_t src_x, uint32_t src_y,
                          uint32_t src_w, uint32_t src_h, uint32_t flags) {
    unsigned char row[8192 * 4];
    unsigned bpp;
    uint32_t r;
    int flip = (flags & BINK_COPY_FLIP) != 0;
    if (!d || !d->raw_path[0] || !dst || src_w == 0 || src_h == 0) return BINK_DECODE_BAD_ARG;
    if (frame >= d->frames) return BINK_DECODE_BAD_RECT;
    if (src_x + src_w > (uint32_t)d->width || src_y + src_h > (uint32_t)d->height)
        return BINK_DECODE_BAD_RECT;
    if (src_w > 8192) return BINK_DECODE_BAD_RECT;
    switch (flags & 7u) {
    case BINK_COPY_BGRA: case BINK_COPY_RGBA:
    case BINK_COPY_ABGR: case BINK_COPY_ARGB: bpp = 4; break;
    case BINK_COPY_RGB: case BINK_COPY_BGR: bpp = 3; break;
    case BINK_COPY_565: bpp = 2; break;
    default: return BINK_DECODE_BAD_RECT;
    }
    if (dst_x + src_w > dst_pitch / bpp || dst_y + src_h > dst_height)
        return BINK_DECODE_BAD_RECT;
    if (!d->fp) d->fp = fopen(d->raw_path, "rb");
    if (!d->fp) return BINK_DECODE_FAILED;
    for (r = 0; r < src_h; ++r) {
        size_t off = (((size_t)frame * (size_t)d->height) + src_y + r) *
                     (size_t)d->width * 4u + (size_t)src_x * 4u;
        uint32_t dr = flip ? (src_h - 1u - r) : r;
        unsigned char *drow = (unsigned char *)dst +
            (size_t)(dst_y + dr) * dst_pitch + (size_t)dst_x * bpp;
        uint32_t x;
        if (fseek(d->fp, (long)off, SEEK_SET) != 0) return BINK_DECODE_FAILED;
        if (fread(row, 4, src_w, d->fp) != src_w) return BINK_DECODE_FAILED;
        for (x = 0; x < src_w; ++x)
            put_pixel(drow + (size_t)x * bpp, row + (size_t)x * 4u, flags);
    }
    return BINK_DECODE_OK;
}
