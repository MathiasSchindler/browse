#pragma once

#include "util.h"

/* Extracts visible-ish text from an HTML document.
 * - Strips tags.
 * - Drops script/style/noscript content.
 * - Decodes a small set of entities and numeric entities.
 * - Collapses whitespace.
 * Output is always NUL-terminated if out_len > 0.
 * Returns 0 on success, -1 on error.
 */
int html_visible_text_extract(const uint8_t *html, size_t html_len, char *out, size_t out_len);

enum {
	HTML_MAX_LINKS = 512,
	HTML_HREF_MAX = 256,
};

struct html_link {
	uint32_t start;
	uint32_t end;
	uint32_t fg_xrgb;
	uint8_t has_fg;
	uint32_t bg_xrgb;
	uint8_t has_bg;
	uint8_t bold;
	char href[HTML_HREF_MAX];
};

struct html_links {
	uint32_t n;
	struct html_link links[HTML_MAX_LINKS];
};

enum {
	HTML_MAX_SPANS = 1024,
};

struct html_span {
	uint32_t start;
	uint32_t end;
	uint32_t fg_xrgb;
	uint8_t has_fg;
	uint32_t bg_xrgb;
	uint8_t has_bg;
	uint8_t bold;
};

struct html_spans {
	uint32_t n;
	struct html_span spans[HTML_MAX_SPANS];
};

/* Like html_visible_text_extract(), but also captures <a href="..."> link ranges
 * in the produced visible-text stream. Ranges are [start,end) indices into out.
 */
int html_visible_text_extract_links(const uint8_t *html, size_t html_len, char *out, size_t out_len, struct html_links *out_links);

/* Like html_visible_text_extract_links(), but also captures inline style attributes
 * (subset) as ranges in the produced visible-text stream.
 *
 * Precedence is expected to be handled by the caller (e.g. links overriding spans).
 */
int html_visible_text_extract_links_and_spans(const uint8_t *html,
					  size_t html_len,
					  char *out,
					  size_t out_len,
					  struct html_links *out_links,
					  struct html_spans *out_spans);
