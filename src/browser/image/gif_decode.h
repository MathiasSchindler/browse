#pragma once

#include "../../core/syscall.h"

/* Decodes the first GIF image into XRGB8888.
 *
 * Scope: first image only, no animation timing, no disposal, no alpha blending.
 * Supports global/local color tables; supports interlaced images.
 *
 * Returns 0 on success and fills out_w/out_h.
 * Returns -1 on failure.
 *
 * Output buffer must have capacity for at least out_w*out_h pixels.
 */
int gif_decode_first_frame_xrgb(const uint8_t *data,
			       size_t len,
			       uint32_t *out_pixels,
			       size_t out_cap_pixels,
			       uint32_t *out_w,
			       uint32_t *out_h);
