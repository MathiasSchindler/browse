#pragma once

#include "browser_defs.h"

#include "html_text.h"

enum img_fmt {
	IMG_FMT_UNKNOWN = 0,
	IMG_FMT_PNG,
	IMG_FMT_JPG,
	IMG_FMT_GIF,
	IMG_FMT_WEBP,
	IMG_FMT_SVG,
	IMG_FMT_AVIF,
};

struct img_sniff_cache_entry {
	uint8_t used;
	uint8_t state; /* 0=empty, 1=pending, 2=done */
	uint8_t inflight; /* dispatched to a worker */
	uint8_t want_pixels;
	uint32_t gen; /* page-generation that last requested this entry */
	uint8_t fetch_failures;
	uint8_t pix_failures;
	enum img_fmt fmt;
	uint8_t has_dims;
	uint16_t w;
	uint16_t h;
	uint8_t has_pixels;
	uint32_t pix_off; /* offset into pixel pool */
	uint16_t pix_w;
	uint16_t pix_h;
	uint32_t pix_len; /* pixels allocated */
	uint32_t hash;
	uint32_t last_use;
	char key[512]; /* usually "host|/path" (truncated) */
};

struct img_dim_ctx {
	const char *active_host;
};

const char *img_fmt_token(enum img_fmt fmt);

struct img_sniff_cache_entry *img_cache_find_done(const char *active_host, const char *url);

enum img_fmt img_cache_get_or_mark_pending(const char *active_host, const char *url);

/* Callback for html_visible_text_extract_* to provide best-effort dimensions. */
int browser_html_img_dim_lookup(void *ctx, const char *url, uint32_t *out_w, uint32_t *out_h);

/* Returns a pointer into the internal pixel pool for an entry, or NULL. */
const uint32_t *img_entry_pixels(const struct img_sniff_cache_entry *e);

void img_workers_init(void);
/* Cancels any in-flight worker fetches by killing and respawning workers. */
void img_workers_cancel_all(void);

/* Pumps completed workers and dispatches new work.
 * Returns 1 if something relevant to the current page changed (dims or pixels).
 */
int img_workers_pump(int *out_any_dims_changed, int *out_any_pixels_changed);
int img_decode_large_pump_one(void);

/* Keeps cached entries, but stops background pixel work for old pages. */
void img_cache_clear_want_pixels(void);

/* Begin loading a new page: increments internal generation and clears wants.
 * Call this before prefetching images for the new page.
 */
void img_cache_begin_new_page(void);

void prefetch_page_images(const char *active_host, const char *visible_text, const struct html_inline_imgs *inline_imgs);
