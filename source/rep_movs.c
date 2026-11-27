/* Forward guest string copies, shared by AOT code and the IR tier.
 *
 * A host pointer returned by n3ds_guest_ptr()/guest_ptr() is only valid until
 * the next guest-memory call.  Two different backing operations can free the
 * block behind it:
 *
 *   1. slab_prepare() bridges a multi-page slab range into one contiguous
 *      allocation.  page_drop_preserve() frees every allocation that covered
 *      the bridged pages, and pages of the same allocation that lie outside
 *      the new bridge are reallocated (their old addresses die too).
 *   2. page_back() at the memory limit calls slab_evict_one(), which flushes
 *      and frees another file-backed page (page_drop()).
 *
 * `rep movsd` used to obtain src and dst host pointers and then run one
 * memmove()/dword loop over them.  Obtaining the second pointer can perform
 * either operation, so the first pointer may already point into freed host
 * memory: ASan reports a heap-use-after-free and a non-ASan run can copy page
 * contents that the allocator already reused.  This is a host-pointer
 * lifetime rule, not an x86 semantic: the guest still sees a forward copy
 * with DF=0 and dword-granular overlap propagation.
 *
 * The copies below never hold two raw guest pointers across a potential
 * commit:
 *   - non-overlapping copies move one page-bounded chunk through a stack
 *     buffer, fetching each pointer immediately before using it;
 *   - overlapping copies read a dword and then write it, which is the exact
 *     x86 forward `rep movsd` result.
 *
 * The implementation deliberately lives in its own translation unit that only
 * needs n3ds_guest_ptr() (defined by the runtime or by a test harness), so the
 * standalone container tools can link the executors without guest.c,
 * win32_shims.c or the full runtime.
 */
#include <stdint.h>
#include <string.h>

#include "guest.h" /* GUEST_PAGE_SIZE */
#include "x86.h"   /* n3ds_guest_ptr, n3ds_rep_movsd/n3ds_movs* prototypes */

void n3ds_rep_movsd(uint32_t n, uint32_t dst, uint32_t src) {
    if (!n) return;
    size_t total = (size_t)n * 4u;
    int overlap = dst > src && (uint64_t)dst < (uint64_t)src + total;
    if (overlap) {
        for (uint32_t k = 0; k < n; ++k) {
            uint32_t v;
            memcpy(&v, n3ds_guest_ptr(src + 4u * k, 4), sizeof v);
            memcpy(n3ds_guest_ptr(dst + 4u * k, 4), &v, sizeof v);
        }
        return;
    }
    uint8_t buf[GUEST_PAGE_SIZE];
    size_t done = 0;
    while (done < total) {
        uint32_t sa = src + (uint32_t)done, da = dst + (uint32_t)done;
        size_t sroom = GUEST_PAGE_SIZE - (sa & (GUEST_PAGE_SIZE - 1u));
        size_t droom = GUEST_PAGE_SIZE - (da & (GUEST_PAGE_SIZE - 1u));
        size_t chunk = total - done;
        if (chunk > sroom) chunk = sroom;
        if (chunk > droom) chunk = droom;
        memcpy(buf, n3ds_guest_ptr(sa, chunk), chunk);
        memcpy(n3ds_guest_ptr(da, chunk), buf, chunk);
        done += chunk;
    }
}

/* One guest `movs` element (rep or plain, either DF direction).  The caller
 * steps esi/edi; only the read of the value is ordered before the write, so a
 * page commit for src cannot invalidate a destination pointer or vice versa. */
static void movs_elem(uint32_t dst, uint32_t src, unsigned bytes) {
    uint8_t v[4];
    memcpy(v, n3ds_guest_ptr(src, bytes), bytes);
    memcpy(n3ds_guest_ptr(dst, bytes), v, bytes);
}
void n3ds_movs8(uint32_t dst, uint32_t src) { movs_elem(dst, src, 1); }
void n3ds_movs16(uint32_t dst, uint32_t src) { movs_elem(dst, src, 2); }
void n3ds_movs32(uint32_t dst, uint32_t src) { movs_elem(dst, src, 4); }
