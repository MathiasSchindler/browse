#pragma once

#include "style_attr.h"

enum css_display {
	CSS_DISPLAY_UNSET = 0,
	CSS_DISPLAY_INLINE = 1,
	CSS_DISPLAY_BLOCK = 2,
};

enum {
	CSS_MAX_TAG_RULES = 128,
	CSS_MAX_TAG_LEN = 15,
};

struct css_tag_rule {
	char tag[CSS_MAX_TAG_LEN + 1];
	struct style_attr style;
	enum css_display display;
	uint8_t has_display;
};

struct css_sheet {
	uint32_t n;
	struct css_tag_rule rules[CSS_MAX_TAG_RULES];
};

void css_sheet_init(struct css_sheet *sheet);

/* Parse a <style> block into the sheet.
 * - selectors: tag selectors only (e.g. p, a, h1)
 * - declarations: color, background-color, font-weight, display (block/inline)
 * - later rules override earlier rules
 * Best-effort: ignores unknown tokens and never fails hard.
 */
int css_parse_style_block(const uint8_t *css, size_t css_len, struct css_sheet *sheet);

/* Lookup computed (latest) rule for tag. Returns 1 if found, else 0.
 * The returned struct contains only the fields set by that tag's rules.
 */
int css_sheet_get_tag_rule(const struct css_sheet *sheet, const uint8_t *tag_lc, size_t tag_len, struct css_tag_rule *out);
