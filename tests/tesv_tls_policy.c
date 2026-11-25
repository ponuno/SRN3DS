#include "tesv_tls_policy.h"
#include <stdio.h>

int main(void) {
    if (TESV_TLS_BOOTSTRAP_COUNT != 1u ||
        tesv_tls_bootstrap_targets[0] != 0x00f794abu) {
        fprintf(stderr, "unexpected TESV TLS bootstrap policy\n");
        return 1;
    }
    for (unsigned i = 0; i < TESV_TLS_BOOTSTRAP_COUNT; ++i) {
        if (tesv_tls_bootstrap_targets[i] == 0x00f5b303u) {
            fprintf(stderr, "CRT terminator 0xf5b303 must not be a TLS callback\n");
            return 1;
        }
    }

    /* Windows stops at the first NULL.  Entries after it are unrelated C
     * initializer/terminator table pointers and must never be invoked. */
    const uint32_t callbacks[] = {0u, 0x00f794abu, 0x00f5b303u};
    unsigned ran = 0;
    for (unsigned i = 0; i < sizeof callbacks / sizeof callbacks[0]; ++i) {
        if (!callbacks[i]) break;
        ++ran;
    }
    if (ran != 0u) {
        fprintf(stderr, "loader callback walk did not stop at leading NULL\n");
        return 1;
    }

    puts("PASS TESV TLS bootstrap policy");
    return 0;
}
