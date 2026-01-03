#pragma once

#include "../../core/syscall.h"

/* Decodes a non-interlaced PNG (bit depth 8) into XRGB8888.
 *
 * Supported (initial scope):
 * - Color types: 0 (grayscale), 2 (RGB), 3 (indexed + PLTE), 6 (RGBA)
 * - Bit depth: 8 only
 * - Interlace: none (Adam7 not supported yet)
 * - Transparency: alpha in RGBA is ignored (composited over black)
 *
 * Caller provides a scratch buffer used to hold the inflated scanlines.
 * Required scratch size is roughly: height * (1 + rowbytes).
 */
int png_decode_xrgb(const uint8_t *data,
		    size_t len,
		    uint8_t *scratch,
		    size_t scratch_cap,
		    uint32_t *out_pixels,
		    size_t out_cap_pixels,
		    uint32_t *out_w,
		    uint32_t *out_h);
