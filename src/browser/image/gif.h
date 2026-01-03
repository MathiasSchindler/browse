#pragma once

#include "../../core/syscall.h"

/* Minimal GIF header parser.
 *
 * Does NOT decode pixels. Only extracts logical screen width/height.
 * Returns 0 on success, -1 on failure.
 */
int gif_get_dimensions(const uint8_t *data, size_t len, uint32_t *out_w, uint32_t *out_h);
