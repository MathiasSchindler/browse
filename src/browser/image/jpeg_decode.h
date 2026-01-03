#pragma once

#include "../../core/syscall.h"

/* Decodes a baseline (SOF0) JPEG into XRGB8888.
 *
 * Scope:
 * - Baseline DCT (SOF0), 8-bit samples
 * - Huffman-coded, single-scan (no progressive)
 * - Grayscale (1 component) and YCbCr (3 components) with common subsampling
 *
 * Returns 0 on success and fills out_w/out_h.
 * Returns -1 on failure.
 */
int jpeg_decode_baseline_xrgb(const uint8_t *data,
			     size_t len,
			     uint32_t *out_pixels,
			     size_t out_cap_pixels,
			     uint32_t *out_w,
			     uint32_t *out_h);
