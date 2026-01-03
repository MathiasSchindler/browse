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
	CSS_MAX_ID_LEN = 63,
	CSS_MAX_CLASS_LEN = 31,
	CSS_MAX_CLASSES_PER_NODE = 8,
	CSS_MAX_UA_RULES = 32,
};

struct css_simple_selector {
	uint8_t has_tag;
	uint8_t has_id;
	uint8_t has_class;
	char tag[CSS_MAX_TAG_LEN + 1];
	char id[CSS_MAX_ID_LEN + 1];
	char klass[CSS_MAX_CLASS_LEN + 1];
};

struct css_selector {
	uint8_t has_ancestor;
	struct css_simple_selector ancestor;
	struct css_simple_selector self;
};

struct css_rule {
	struct css_selector sel;
	struct style_attr style;
	enum css_display display;
	uint8_t has_display;
};

struct css_sheet {
	uint32_t n;
	struct css_rule rules[CSS_MAX_RULES];
	uint32_t ua_n;
	struct css_rule ua_rules[CSS_MAX_UA_RULES];
};

struct css_computed {
	struct style_attr style;
	enum css_display display;
	uint8_t has_display;
};

struct css_node {
	const uint8_t *tag_lc;
	size_t tag_len;
	const char *id_lc;
	const char (*classes)[CSS_MAX_CLASS_LEN + 1];
	uint32_t class_count;
};

void css_sheet_init(struct css_sheet *sheet);

/* Parse a <style> block into the sheet.
 * - selectors: tag, .class, tag.class (ignores pseudo-classes like :hover)
 * - declarations: color, background-color, font-weight, text-decoration, display (block/inline/none)
 * - later rules override earlier rules
 * Best-effort: ignores unknown tokens and never fails hard.
 */
int css_parse_style_block(const uint8_t *css, size_t css_len, struct css_sheet *sheet);

/* Compute style/display for a given node based on its selector inputs.
 * Supports a tiny subset:
 * - selectors: tag, .class, #id, tag.class, tag#id, .class#id
 * - one-level descendant: A B (A matches any ancestor, B matches the node)
 * - later rules override earlier rules; UA rules are applied last.
 * The returned fields include only what rules set; callers decide inheritance/precedence.
 */
void css_sheet_compute(const struct css_sheet *sheet,
		       const uint8_t *tag_lc,
		       size_t tag_len,
		       const char *id_lc,
		       const char classes[CSS_MAX_CLASSES_PER_NODE][CSS_MAX_CLASS_LEN + 1],
		       uint32_t class_count,
		       const struct css_node *ancestors,
		       uint32_t ancestor_count,
		       struct css_computed *out);
