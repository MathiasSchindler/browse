#pragma once

#include "../../core/syscall.h"

/* Minimal JPEG header parser.
 *
 * This does NOT decode pixels. It only extracts width/height from the first
 * Start-Of-Frame marker found.
 *
 * Returns 0 on success and writes out_w/out_h.
 * Returns -1 on failure.
 */
int jpeg_get_dimensions(const uint8_t *data, size_t len, uint32_t *out_w, uint32_t *out_h);
