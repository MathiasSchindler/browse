#pragma once

#include "style_attr.h"

enum css_display {
	CSS_DISPLAY_UNSET = 0,
	CSS_DISPLAY_INLINE = 1,
	CSS_DISPLAY_BLOCK = 2,
	CSS_DISPLAY_NONE = 3,
};

enum {
	CSS_MAX_RULES = 128,
	CSS_MAX_TAG_LEN = 15,
	CSS_MAX_CLASS_LEN = 31,
	CSS_MAX_CLASSES_PER_NODE = 8,
};

enum css_selector_kind {
	CSS_SEL_TAG = 0,
	CSS_SEL_CLASS = 1,
	CSS_SEL_TAG_CLASS = 2,
};

struct css_rule {
	enum css_selector_kind kind;
	char tag[CSS_MAX_TAG_LEN + 1];
	char klass[CSS_MAX_CLASS_LEN + 1];
	struct style_attr style;
	enum css_display display;
	uint8_t has_display;
};

struct css_sheet {
	uint32_t n;
	struct css_rule rules[CSS_MAX_RULES];
};

struct css_computed {
	struct style_attr style;
	enum css_display display;
	uint8_t has_display;
};

void css_sheet_init(struct css_sheet *sheet);

/* Parse a <style> block into the sheet.
 * - selectors: tag, .class, tag.class (ignores pseudo-classes like :hover)
 * - declarations: color, background-color, font-weight, text-decoration, display (block/inline/none)
 * - later rules override earlier rules
 * Best-effort: ignores unknown tokens and never fails hard.
 */
int css_parse_style_block(const uint8_t *css, size_t css_len, struct css_sheet *sheet);

/* Compute style/display for a given node based on its tag and classes.
 * The returned fields include only what rules set; callers decide inheritance/precedence.
 */
void css_sheet_compute(const struct css_sheet *sheet,
		       const uint8_t *tag_lc,
		       size_t tag_len,
		       const char classes[CSS_MAX_CLASSES_PER_NODE][CSS_MAX_CLASS_LEN + 1],
		       uint32_t class_count,
		       struct css_computed *out);
