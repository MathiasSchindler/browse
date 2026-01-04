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
	/* Wikipedia-scale pages can easily exceed 512 anchors.
	 * Keep this bounded but large enough for long articles.
	 */
	HTML_MAX_LINKS = 2048,
	HTML_HREF_MAX = 512,
};

struct html_link {
	uint32_t start;
	uint32_t end;
	uint32_t fg_xrgb;
	uint8_t has_fg;
	uint32_t bg_xrgb;
	uint8_t has_bg;
	uint8_t bold;
	uint8_t underline;
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
	uint8_t underline;
};

struct html_spans {
	uint32_t n;
	struct html_span spans[HTML_MAX_SPANS];
};

enum {
	HTML_MAX_INLINE_IMGS = 512,
	HTML_INLINE_IMG_URL_MAX = 512,
};

/* Experimental: float-right image placeholders.
 * Disabled by default because pages with many consecutive thumbnails can
 * visually overlap with the current single-float renderer.
 */
#ifndef HTML_ENABLE_FLOAT_RIGHT
#define HTML_ENABLE_FLOAT_RIGHT 0
#endif

struct html_inline_img {
	uint32_t start;
	uint32_t end;
	char url[HTML_INLINE_IMG_URL_MAX];
};

struct html_inline_imgs {
	uint32_t n;
	struct html_inline_img imgs[HTML_MAX_INLINE_IMGS];
};

typedef int (*html_img_dim_lookup_fn)(void *ctx,
				      const char *url,
				      uint32_t *out_w,
				      uint32_t *out_h);

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

/* Like html_visible_text_extract_links_and_spans(), but allows the caller to
 * provide a best-effort image dimension lookup by URL.
 *
 * This is used to reserve enough rows for <img> placeholders once image
 * dimensions are known (e.g. after sniffing/decoding).
 */
int html_visible_text_extract_links_and_spans_ex(const uint8_t *html,
				     size_t html_len,
				     char *out,
				     size_t out_len,
				     struct html_links *out_links,
				     struct html_spans *out_spans,
				     html_img_dim_lookup_fn img_dim_lookup,
				     void *img_dim_lookup_ctx);

/* Like html_visible_text_extract_links_and_spans_ex(), but also returns a list
 * of inline-icon <img> placeholders (currently rendered as literal "[img]")
 * along with their source URLs.
 */
int html_visible_text_extract_links_spans_and_inline_imgs_ex(const uint8_t *html,
					    size_t html_len,
					    char *out,
					    size_t out_len,
					    struct html_links *out_links,
					    struct html_spans *out_spans,
					    struct html_inline_imgs *out_inline_imgs,
					    html_img_dim_lookup_fn img_dim_lookup,
					    void *img_dim_lookup_ctx);
