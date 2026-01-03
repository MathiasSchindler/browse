#pragma once

#include "util.h"

struct style_attr {
	uint8_t has_color;
	uint8_t has_bg;
	uint8_t bold;
	uint32_t color_xrgb;
	uint32_t bg_xrgb;
};

/* Parses a very small subset of inline CSS declarations:
 * - color: #RRGGBB
 * - background-color: #RRGGBB
 * - font-weight: bold
 *
 * Returns 0 on success (even if nothing recognized), -1 on invalid args.
 */
int style_attr_parse_inline(const uint8_t *s, size_t n, struct style_attr *out);
