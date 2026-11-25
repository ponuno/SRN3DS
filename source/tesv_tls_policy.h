#pragma once
#include <stdint.h>

/* TESV's PE TLS callback array starts with NULL, so the Windows loader invokes
 * no executable TLS callbacks.  The static CRT still needs its thread-data
 * initializer before C++ constructors.  Keep that manual bootstrap policy in
 * one testable place: 0xf5b303 is a CRT terminator, not a TLS callback. */
#define TESV_TLS_BOOTSTRAP_COUNT 1u
static const uint32_t tesv_tls_bootstrap_targets[TESV_TLS_BOOTSTRAP_COUNT] = {
    0x00f794abu
};
