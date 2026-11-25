/* Test-only shader blob declaration.  This probe never opens the backend, and
 * --gc-sections discards d9_citro_open(), so no shader image is linked. */
#pragma once
#include <stddef.h>
#include <stdint.h>
extern const uint8_t d9shader_shbin[];
extern const uint8_t d9shader_shbin_end[];
static const size_t d9shader_shbin_size = 0;
