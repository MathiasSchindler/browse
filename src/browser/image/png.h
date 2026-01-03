#pragma once

#include "../../core/syscall.h"

/* Minimal PNG header parser.
 *
 * Does NOT decode pixels. Only extracts width/height from IHDR.
 * Returns 0 on success, -1 on failure.
 */
int png_get_dimensions(const uint8_t *data, size_t len, uint32_t *out_w, uint32_t *out_h);
