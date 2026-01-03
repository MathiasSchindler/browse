#include "../core/text.h"
#include "fb_shm.h"
#include "net_dns.h"
#include "net_tcp.h"
#include "url.h"

#include "../tls/selftest.h"
#include "tls13_client.h"
#include "html_text.h"
#include "text_layout.h"

#include "image/jpeg.h"
#include "image/jpeg_decode.h"
#include "image/png.h"
#include "image/png_decode.h"
#include "image/gif.h"
#include "image/gif_decode.h"

#define FB_W 1920u
#define FB_H 1080u

#define UI_TOPBAR_H 24u
#define UI_CONTENT_PAD_Y 2u
#define UI_CONTENT_Y0 (UI_TOPBAR_H + UI_CONTENT_PAD_Y)

enum {
	HOST_BUF_LEN = 128,
	PATH_BUF_LEN = 2048,
	URL_BUF_LEN = 2304,
};

static char g_visible[512 * 1024];
static char g_status_bar[128];
static char g_url_bar[URL_BUF_LEN];
static char g_active_host[HOST_BUF_LEN];
static struct html_links g_links;
static struct html_spans g_spans;
static struct html_inline_imgs g_inline_imgs;
static uint32_t g_scroll_rows;
static int g_have_page;

static uint8_t g_body[512 * 1024];
static size_t g_body_len;

static uint32_t g_img_use_tick;

enum img_fmt {
	IMG_FMT_UNKNOWN = 0,
	IMG_FMT_PNG,
	IMG_FMT_JPG,
	IMG_FMT_GIF,
	IMG_FMT_WEBP,
	IMG_FMT_SVG,
};

struct img_sniff_cache_entry {
	uint8_t used;
	uint8_t state; /* 0=empty, 1=pending, 2=done */
	uint8_t inflight; /* dispatched to a worker */
	uint8_t want_pixels;
	enum img_fmt fmt;
	uint8_t has_dims;
	uint16_t w;
	uint16_t h;
	uint8_t has_pixels;
	uint32_t pix_off; /* offset into g_img_pixel_pool */
	uint16_t pix_w;
	uint16_t pix_h;
	uint32_t pix_len; /* pixels allocated */
	uint32_t hash;
	uint32_t last_use;
	char key[256]; /* usually "host|/path" (truncated) */
};

static struct img_sniff_cache_entry g_img_sniff_cache[256];

static uint8_t g_img_fetch_buf[1024 * 1024];

/* Extremely small pixel pool for the first real decoder (GIF).
 * Bump-allocated; reset on page navigation/reload.
 */
static uint32_t g_img_pixel_pool[1024 * 1024]; /* 1M px = 4 MiB */
static uint32_t g_img_pixel_pool_used;

struct img_pix_free_block {
	uint32_t off;
	uint32_t len;
};

static struct img_pix_free_block g_img_pix_free[128];
static uint32_t g_img_pix_free_n;

static void img_pixel_free(uint32_t off, uint32_t n_pixels)
{
	if (n_pixels == 0) return;
	if (off > (uint32_t)(sizeof(g_img_pixel_pool) / sizeof(g_img_pixel_pool[0]))) return;
	uint32_t cap = (uint32_t)(sizeof(g_img_pixel_pool) / sizeof(g_img_pixel_pool[0]));
	if (off + n_pixels > cap) return;

	if (g_img_pix_free_n < (uint32_t)(sizeof(g_img_pix_free) / sizeof(g_img_pix_free[0]))) {
		g_img_pix_free[g_img_pix_free_n].off = off;
		g_img_pix_free[g_img_pix_free_n].len = n_pixels;
		g_img_pix_free_n++;
	}

	/* Best-effort merge of adjacent free blocks (small N, simple O(n^2)). */
	for (uint32_t i = 0; i < g_img_pix_free_n; i++) {
		for (uint32_t j = 0; j < g_img_pix_free_n; j++) {
			if (i == j) continue;
			struct img_pix_free_block *a = &g_img_pix_free[i];
			struct img_pix_free_block *b = &g_img_pix_free[j];
			if (a->len == 0 || b->len == 0) continue;
			if (a->off + a->len == b->off) {
				a->len += b->len;
				b->len = 0;
			}
		}
	}
	/* Compact: remove zero-len entries. */
	uint32_t w = 0;
	for (uint32_t i = 0; i < g_img_pix_free_n; i++) {
		if (g_img_pix_free[i].len == 0) continue;
		g_img_pix_free[w++] = g_img_pix_free[i];
	}
	g_img_pix_free_n = w;
}

static int img_pixel_alloc(uint32_t n_pixels, uint32_t *out_off)
{
	if (!out_off) return -1;
	*out_off = 0;
	if (n_pixels == 0) return -1;
	if (n_pixels > (uint32_t)(sizeof(g_img_pixel_pool) / sizeof(g_img_pixel_pool[0]))) return -1;
	uint32_t cap = (uint32_t)(sizeof(g_img_pixel_pool) / sizeof(g_img_pixel_pool[0]));
	if (g_img_pixel_pool_used > cap) g_img_pixel_pool_used = cap;

	/* First-fit from free list. */
	for (uint32_t i = 0; i < g_img_pix_free_n; i++) {
		struct img_pix_free_block *b = &g_img_pix_free[i];
		if (b->len < n_pixels) continue;
		*out_off = b->off;
		b->off += n_pixels;
		b->len -= n_pixels;
		if (b->len == 0) {
			g_img_pix_free[i] = g_img_pix_free[g_img_pix_free_n - 1u];
			g_img_pix_free_n--;
		}
		return 0;
	}

	if (cap - g_img_pixel_pool_used < n_pixels) return -1;
	*out_off = g_img_pixel_pool_used;
	g_img_pixel_pool_used += n_pixels;
	return 0;
}

static int img_cache_evict_one(void)
{
	uint32_t best_i = 0xffffffffu;
	uint32_t best_use = 0xffffffffu;
	for (uint32_t i = 0; i < (uint32_t)(sizeof(g_img_sniff_cache) / sizeof(g_img_sniff_cache[0])); i++) {
		struct img_sniff_cache_entry *e = &g_img_sniff_cache[i];
		if (!e->used) continue;
		if (e->state != 2) continue;
		if (e->inflight) continue;
		if (e->last_use < best_use) {
			best_use = e->last_use;
			best_i = i;
		}
	}
	if (best_i == 0xffffffffu) return -1;
	struct img_sniff_cache_entry *e = &g_img_sniff_cache[best_i];
	if (e->has_pixels && e->pix_len != 0) {
		img_pixel_free(e->pix_off, e->pix_len);
	}
	e->used = 0;
	e->state = 0;
	e->inflight = 0;
	e->want_pixels = 0;
	e->fmt = IMG_FMT_UNKNOWN;
	e->has_dims = 0;
	e->w = 0;
	e->h = 0;
	e->has_pixels = 0;
	e->pix_off = 0;
	e->pix_w = 0;
	e->pix_h = 0;
	e->pix_len = 0;
	e->hash = 0;
	e->last_use = 0;
	e->key[0] = 0;
	return 0;
}

static int img_cache_drop_pending_one(void)
{
	uint32_t best_i = 0xffffffffu;
	uint32_t best_use = 0xffffffffu;
	for (uint32_t i = 0; i < (uint32_t)(sizeof(g_img_sniff_cache) / sizeof(g_img_sniff_cache[0])); i++) {
		struct img_sniff_cache_entry *e = &g_img_sniff_cache[i];
		if (!e->used) continue;
		if (e->state != 1) continue;
		if (e->inflight) continue;
		/* Prefer dropping entries that are no longer wanted. */
		uint32_t score = e->want_pixels ? (e->last_use | 0x80000000u) : e->last_use;
		if (score < best_use) {
			best_use = score;
			best_i = i;
		}
	}
	if (best_i == 0xffffffffu) return -1;
	struct img_sniff_cache_entry *e = &g_img_sniff_cache[best_i];
	/* Pending entries have no pixels yet; just drop the slot. */
	e->used = 0;
	e->state = 0;
	e->inflight = 0;
	e->want_pixels = 0;
	e->fmt = IMG_FMT_UNKNOWN;
	e->has_dims = 0;
	e->w = 0;
	e->h = 0;
	e->has_pixels = 0;
	e->pix_off = 0;
	e->pix_w = 0;
	e->pix_h = 0;
	e->pix_len = 0;
	e->hash = 0;
	e->last_use = 0;
	e->key[0] = 0;
	return 0;
}

static void blit_xrgb_clipped(struct shm_fb *fb,
			     uint32_t dst_x,
			     uint32_t dst_y,
			     uint32_t dst_w,
			     uint32_t dst_h,
			     const uint32_t *src,
			     uint32_t src_w,
			     uint32_t src_h)
{
	if (!fb || !src) return;
	if (dst_w == 0 || dst_h == 0 || src_w == 0 || src_h == 0) return;
	if (dst_x >= fb->width || dst_y >= fb->height) return;

	uint32_t max_w = fb->width - dst_x;
	uint32_t max_h = fb->height - dst_y;
	uint32_t w = dst_w;
	uint32_t h = dst_h;
	if (w > max_w) w = max_w;
	if (h > max_h) h = max_h;
	if (w > src_w) w = src_w;
	if (h > src_h) h = src_h;

	for (uint32_t y = 0; y < h; y++) {
		uint32_t *row = pixel_ptr(fb->pixels, fb->stride, dst_x, dst_y + y);
		const uint32_t *srow = src + (size_t)y * (size_t)src_w;
		for (uint32_t x = 0; x < w; x++) {
			row[x] = srow[x];
		}
	}
}

static uint32_t hash32_fnv1a(const char *s)
{
	uint32_t h = 2166136261u;
	if (!s) return h;
	for (size_t i = 0; s[i] != 0; i++) {
		h ^= (uint8_t)s[i];
		h *= 16777619u;
	}
	return h;
}

struct img_dim_ctx {
	const char *active_host;
};

static struct img_sniff_cache_entry *img_cache_find_done(const char *active_host, const char *url);

static int html_img_dim_lookup(void *ctx, const char *url, uint32_t *out_w, uint32_t *out_h)
{
	if (!ctx || !url || !out_w || !out_h) return -1;
	struct img_dim_ctx *c = (struct img_dim_ctx *)ctx;
	struct img_sniff_cache_entry *e = img_cache_find_done(c->active_host ? c->active_host : "", url);
	if (!e || !e->has_dims) return -1;
	*out_w = (uint32_t)e->w;
	*out_h = (uint32_t)e->h;
	return 0;
}

static int streq(const char *a, const char *b)
{
	if (a == b) return 1;
	if (!a || !b) return 0;
	for (size_t i = 0;; i++) {
		if (a[i] != b[i]) return 0;
		if (a[i] == 0) return 1;
	}
}

static enum img_fmt img_fmt_from_sniff(const uint8_t *b, size_t n)
{
	if (!b || n == 0) return IMG_FMT_UNKNOWN;
	if (n >= 8 &&
	    b[0] == 0x89 && b[1] == 0x50 && b[2] == 0x4e && b[3] == 0x47 &&
	    b[4] == 0x0d && b[5] == 0x0a && b[6] == 0x1a && b[7] == 0x0a) {
		return IMG_FMT_PNG;
	}
	if (n >= 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff) {
		return IMG_FMT_JPG;
	}
	if (n >= 6 && b[0] == 'G' && b[1] == 'I' && b[2] == 'F' && b[3] == '8' &&
	    (b[4] == '7' || b[4] == '9') && b[5] == 'a') {
		return IMG_FMT_GIF;
	}
	if (n >= 12 &&
	    b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F' &&
	    b[8] == 'W' && b[9] == 'E' && b[10] == 'B' && b[11] == 'P') {
		return IMG_FMT_WEBP;
	}
	/* Best-effort SVG sniff: look for "<svg" near the beginning. */
	if (n >= 5) {
		size_t i = 0;
		while (i < n && (b[i] == ' ' || b[i] == '\n' || b[i] == '\r' || b[i] == '\t')) i++;
		for (size_t j = i; j + 4 < n && j < 96; j++) {
			if (b[j] == '<') {
				uint8_t s0 = b[j + 1];
				uint8_t s1 = b[j + 2];
				uint8_t s2 = b[j + 3];
				uint8_t s3 = b[j + 4];
				if (s0 >= 'A' && s0 <= 'Z') s0 = (uint8_t)(s0 + ('a' - 'A'));
				if (s1 >= 'A' && s1 <= 'Z') s1 = (uint8_t)(s1 + ('a' - 'A'));
				if (s2 >= 'A' && s2 <= 'Z') s2 = (uint8_t)(s2 + ('a' - 'A'));
				if (s3 >= 'A' && s3 <= 'Z') s3 = (uint8_t)(s3 + ('a' - 'A'));
				if (s0 == 's' && s1 == 'v' && s2 == 'g' && (s3 == ' ' || s3 == '>' || s3 == '\t' || s3 == '\n' || s3 == '\r')) {
					return IMG_FMT_SVG;
				}
			}
		}
	}
	return IMG_FMT_UNKNOWN;
}

static const char *img_fmt_token(enum img_fmt fmt)
{
	switch (fmt) {
		case IMG_FMT_PNG: return "PNG";
		case IMG_FMT_JPG: return "JPG";
		case IMG_FMT_GIF: return "GIF";
		case IMG_FMT_WEBP: return "WEBP";
		case IMG_FMT_SVG: return "SVG";
		default: return "?";
	}
}

static struct img_sniff_cache_entry *img_cache_find_done(const char *active_host, const char *url)
{
	if (!active_host || !active_host[0] || !url || !url[0]) return 0;
	char host[HOST_BUF_LEN];
	char path[PATH_BUF_LEN];
	if (url_apply_location(active_host, url, host, sizeof(host), path, sizeof(path)) != 0) return 0;

	char key[256];
	key[0] = 0;
	(void)c_strlcpy_s(key, sizeof(key), host);
	{
		size_t o = c_strnlen_s(key, sizeof(key));
		if (o + 1 < sizeof(key)) key[o++] = '|';
		for (size_t i = 0; path[i] && o + 1 < sizeof(key); i++) key[o++] = path[i];
		key[o] = 0;
	}
	uint32_t h = hash32_fnv1a(key);
	for (size_t i = 0; i < sizeof(g_img_sniff_cache) / sizeof(g_img_sniff_cache[0]); i++) {
		struct img_sniff_cache_entry *e = &g_img_sniff_cache[i];
		if (!e->used || e->state != 2) continue;
		if (e->hash == h && streq(e->key, key)) {
			e->last_use = ++g_img_use_tick;
			return e;
		}
	}
	return 0;
}

static int https_get_prefix_follow_redirects(const char *host_in, const char *path_in, uint8_t *out, size_t out_cap, size_t *out_len)
{
	if (!out || out_cap == 0 || !out_len) return -1;
	*out_len = 0;
	char host[HOST_BUF_LEN];
	char path[PATH_BUF_LEN];
	(void)c_strlcpy_s(host, sizeof(host), host_in ? host_in : "");
	(void)c_strlcpy_s(path, sizeof(path), path_in ? path_in : "/");

	for (int step = 0; step < 4; step++) {
		uint8_t ip6[16];
		c_memset(ip6, 0, sizeof(ip6));
		if (dns_resolve_aaaa_google(host, ip6) != 0) return -1;
		int sock = tcp6_connect(ip6, 443);
		if (sock < 0) return -1;

		char status[128];
		char location[512];
		int status_code = -1;
		size_t body_len = 0;
		uint64_t content_len = 0;
		int rc = tls13_https_get_status_location_and_body(sock,
							 host,
							 path,
							 status,
							 sizeof(status),
							 &status_code,
							 location,
							 sizeof(location),
							 out,
							 out_cap,
							 &body_len,
							 &content_len);
		sys_close(sock);
		if (rc != 0) return -1;

		int is_redirect = (status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 || status_code == 308);
		if (!is_redirect || location[0] == 0) {
			*out_len = (body_len > out_cap) ? out_cap : (size_t)body_len;
			return 0;
		}
		char new_host[HOST_BUF_LEN];
		char new_path[PATH_BUF_LEN];
		if (url_apply_location(host, location, new_host, sizeof(new_host), new_path, sizeof(new_path)) != 0) return -1;
		(void)c_strlcpy_s(host, sizeof(host), new_host);
		(void)c_strlcpy_s(path, sizeof(path), new_path);
	}
	return -1;
}

static int https_conn_open_host(struct tls13_https_conn *c, const char *host)
{
	if (!c || !host || !host[0]) return -1;
	tls13_https_conn_close(c);

	uint8_t ip6[16];
	c_memset(ip6, 0, sizeof(ip6));
	if (dns_resolve_aaaa_google(host, ip6) != 0) return -1;

	int sock = tcp6_connect(ip6, 443);
	if (sock < 0) return -1;

	if (tls13_https_conn_open(c, sock, host) != 0) {
		sys_close(sock);
		c->sock = -1;
		c->alive = 0;
		return -1;
	}
	return 0;
}

static int https_get_prefix_follow_redirects_keepalive(struct tls13_https_conn *c,
						const char *host_in,
						const char *path_in,
						uint8_t *out,
						size_t out_cap,
						size_t *out_len)
{
	if (!c || !out || out_cap == 0 || !out_len) return -1;
	*out_len = 0;

	char host[HOST_BUF_LEN];
	char path[PATH_BUF_LEN];
	(void)c_strlcpy_s(host, sizeof(host), host_in ? host_in : "");
	(void)c_strlcpy_s(path, sizeof(path), path_in ? path_in : "/");

	for (int step = 0; step < 4; step++) {
		if (!c->alive || c->sock < 0 || !streq(c->host, host)) {
			if (https_conn_open_host(c, host) != 0) return -1;
		}

		char status[128];
		char location[512];
		int status_code = -1;
		size_t body_len = 0;
		int peer_close = 0;

		int rc = -1;
		for (int attempt = 0; attempt < 2; attempt++) {
			peer_close = 0;
			rc = tls13_https_conn_get_status_location_and_body(c,
								 path,
								 status,
								 sizeof(status),
								 &status_code,
								 location,
								 sizeof(location),
								 out,
								 out_cap,
								 &body_len,
								 0,
								 1,
								 &peer_close);
			if (rc == 0) break;
			/* Retry once with a fresh connection. */
			tls13_https_conn_close(c);
			if (https_conn_open_host(c, host) != 0) return -1;
		}

		if (peer_close) tls13_https_conn_close(c);
		if (rc != 0) return -1;

		int is_redirect = (status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 || status_code == 308);
		if (!is_redirect || location[0] == 0) {
			*out_len = (body_len > out_cap) ? out_cap : (size_t)body_len;
			return 0;
		}

		char new_host[HOST_BUF_LEN];
		char new_path[PATH_BUF_LEN];
		if (url_apply_location(host, location, new_host, sizeof(new_host), new_path, sizeof(new_path)) != 0) return -1;
		(void)c_strlcpy_s(host, sizeof(host), new_host);
		(void)c_strlcpy_s(path, sizeof(path), new_path);
	}
	return -1;
}

static int split_host_path_from_key(const char *key, char *host_out, size_t host_out_len, char *path_out, size_t path_out_len)
{
	if (!key || !host_out || !path_out || host_out_len == 0 || path_out_len == 0) return -1;
	/* key is "host|/path" */
	size_t bar = 0;
	for (; key[bar] != 0; bar++) {
		if (key[bar] == '|') break;
	}
	if (key[bar] != '|') return -1;
	/* host */
	{
		size_t o = 0;
		for (size_t i = 0; i < bar && o + 1 < host_out_len; i++) host_out[o++] = key[i];
		host_out[o] = 0;
	}
	/* path */
	{
		size_t o = 0;
		for (size_t i = bar + 1; key[i] && o + 1 < path_out_len; i++) path_out[o++] = key[i];
		path_out[o] = 0;
	}
	if (host_out[0] == 0 || path_out[0] == 0) return -1;
	return 0;
}

static enum img_fmt img_cache_get_or_mark_pending(const char *active_host, const char *url)
{
	if (!active_host || !active_host[0] || !url || !url[0]) return IMG_FMT_UNKNOWN;

	/* We only sniff HTTPS via our TLS stack. */
	if (url[0] == 'd' && url[1] == 'a' && url[2] == 't' && url[3] == 'a' && url[4] == ':') return IMG_FMT_UNKNOWN;
	if (url[0] == 'h' && url[1] == 't' && url[2] == 't' && url[3] == 'p' && url[4] == ':' && url[5] == '/' && url[6] == '/') return IMG_FMT_UNKNOWN;

	char host[HOST_BUF_LEN];
	char path[PATH_BUF_LEN];
	if (url_apply_location(active_host, url, host, sizeof(host), path, sizeof(path)) != 0) {
		return IMG_FMT_UNKNOWN;
	}

	char key[256];
	key[0] = 0;
	(void)c_strlcpy_s(key, sizeof(key), host);
	/* Append '|' + path (best-effort). */
	{
		size_t o = c_strnlen_s(key, sizeof(key));
		if (o + 1 < sizeof(key)) key[o++] = '|';
		for (size_t i = 0; path[i] && o + 1 < sizeof(key); i++) key[o++] = path[i];
		key[o] = 0;
	}

	uint32_t h = hash32_fnv1a(key);
	for (size_t i = 0; i < sizeof(g_img_sniff_cache) / sizeof(g_img_sniff_cache[0]); i++) {
		struct img_sniff_cache_entry *e = &g_img_sniff_cache[i];
		if (!e->used) continue;
		if (e->hash == h && streq(e->key, key)) {
			e->last_use = ++g_img_use_tick;
			e->want_pixels = 1;
			return (e->state == 2) ? e->fmt : IMG_FMT_UNKNOWN;
		}
	}

	/* Not cached yet: mark pending. */
	struct img_sniff_cache_entry *slot = 0;
	for (size_t i = 0; i < sizeof(g_img_sniff_cache) / sizeof(g_img_sniff_cache[0]); i++) {
		struct img_sniff_cache_entry *e = &g_img_sniff_cache[i];
		if (!e->used) {
			slot = e;
			break;
		}
	}
	if (!slot) {
		/* If we over-prefetch, allow dropping least-recently-used pending entries. */
		if (img_cache_drop_pending_one() == 0 || img_cache_evict_one() == 0) {
			for (size_t i = 0; i < sizeof(g_img_sniff_cache) / sizeof(g_img_sniff_cache[0]); i++) {
				struct img_sniff_cache_entry *e = &g_img_sniff_cache[i];
				if (!e->used) {
					slot = e;
					break;
				}
			}
		}
	}
	if (slot) {
		slot->used = 1;
		slot->state = 1;
		slot->inflight = 0;
		slot->want_pixels = 1;
		slot->fmt = IMG_FMT_UNKNOWN;
		slot->has_dims = 0;
		slot->w = 0;
		slot->h = 0;
		slot->has_pixels = 0;
		slot->pix_off = 0;
		slot->pix_w = 0;
		slot->pix_h = 0;
		slot->pix_len = 0;
		slot->hash = h;
		slot->last_use = ++g_img_use_tick;
		(void)c_strlcpy_s(slot->key, sizeof(slot->key), key);
	}
	return IMG_FMT_UNKNOWN;
}

enum {
	IMG_WORKERS = 4,
	IMG_WORKER_MAX_W = 128,
	IMG_WORKER_MAX_H = 128,
	IMG_WORKER_MAX_PX = IMG_WORKER_MAX_W * IMG_WORKER_MAX_H,
};

struct img_worker_shm {
	volatile uint32_t state; /* 0=idle, 1=req, 2=done */
	char key[256];
	uint32_t fmt;
	uint32_t has_dims;
	uint16_t w;
	uint16_t h;
	uint32_t has_pixels;
	uint16_t pix_w;
	uint16_t pix_h;
	uint32_t pix_len;
	int32_t rc;
	uint32_t pixels[IMG_WORKER_MAX_PX];
};

static struct img_worker_shm *g_img_workers;

static void img_worker_loop(uint32_t wi)
{
	if (!g_img_workers || wi >= IMG_WORKERS) sys_exit(1);
	struct img_worker_shm *w = &g_img_workers[wi];
	struct tls13_https_conn conn;
	c_memset(&conn, 0, sizeof(conn));
	conn.sock = -1;
	struct timespec req;
	req.tv_sec = 0;
	req.tv_nsec = 1 * 1000 * 1000;

	for (;;) {
		if (w->state != 1u) {
			sys_nanosleep(&req, 0);
			continue;
		}
		w->rc = -1;
		w->fmt = IMG_FMT_UNKNOWN;
		w->has_dims = 0;
		w->w = 0;
		w->h = 0;
		w->has_pixels = 0;
		w->pix_w = 0;
		w->pix_h = 0;
		w->pix_len = 0;

		char host[HOST_BUF_LEN];
		char path[PATH_BUF_LEN];
		if (split_host_path_from_key(w->key, host, sizeof(host), path, sizeof(path)) != 0) {
			w->rc = -1;
			w->state = 2u;
			continue;
		}

		uint8_t sniff_buf[4096];
		size_t got = 0;
		if (https_get_prefix_follow_redirects_keepalive(&conn, host, path, sniff_buf, sizeof(sniff_buf), &got) != 0 || got == 0) {
			w->rc = -1;
			w->state = 2u;
			continue;
		}
		w->fmt = (uint32_t)img_fmt_from_sniff(sniff_buf, got);
		uint32_t dim_w = 0, dim_h = 0;
		if ((enum img_fmt)w->fmt == IMG_FMT_JPG) {
			if (jpeg_get_dimensions(sniff_buf, got, &dim_w, &dim_h) == 0) {
				w->has_dims = 1;
				w->w = (dim_w > 0xffffu) ? 0xffffu : (uint16_t)dim_w;
				w->h = (dim_h > 0xffffu) ? 0xffffu : (uint16_t)dim_h;
			}
		} else if ((enum img_fmt)w->fmt == IMG_FMT_PNG) {
			if (png_get_dimensions(sniff_buf, got, &dim_w, &dim_h) == 0) {
				w->has_dims = 1;
				w->w = (dim_w > 0xffffu) ? 0xffffu : (uint16_t)dim_w;
				w->h = (dim_h > 0xffffu) ? 0xffffu : (uint16_t)dim_h;
			}
		} else if ((enum img_fmt)w->fmt == IMG_FMT_GIF) {
			if (gif_get_dimensions(sniff_buf, got, &dim_w, &dim_h) == 0) {
				w->has_dims = 1;
				w->w = (dim_w > 0xffffu) ? 0xffffu : (uint16_t)dim_w;
				w->h = (dim_h > 0xffffu) ? 0xffffu : (uint16_t)dim_h;
			}
		}

		/* Only decode tiny images in workers (icons) to keep IPC small. */
		if (w->has_dims) {
			uint32_t pw = (uint32_t)w->w;
			uint32_t ph = (uint32_t)w->h;
			uint32_t px = pw * ph;
			if (pw > 0 && ph > 0 && pw <= IMG_WORKER_MAX_W && ph <= IMG_WORKER_MAX_H && px <= IMG_WORKER_MAX_PX) {
				uint8_t fetch_buf[512 * 1024];
				size_t got_full = 0;
				if (https_get_prefix_follow_redirects_keepalive(&conn, host, path, fetch_buf, sizeof(fetch_buf), &got_full) == 0 && got_full > 0) {
					uint32_t dw = 0, dh = 0;
					int dec_ok = -1;
					if ((enum img_fmt)w->fmt == IMG_FMT_JPG) {
						dec_ok = jpeg_decode_baseline_xrgb(fetch_buf, got_full, w->pixels, (size_t)IMG_WORKER_MAX_PX, &dw, &dh);
					} else if ((enum img_fmt)w->fmt == IMG_FMT_PNG) {
						uint8_t *scratch = &fetch_buf[got_full];
						size_t scratch_cap = sizeof(fetch_buf) - got_full;
						dec_ok = png_decode_xrgb(fetch_buf, got_full, scratch, scratch_cap, w->pixels, (size_t)IMG_WORKER_MAX_PX, &dw, &dh);
					} else if ((enum img_fmt)w->fmt == IMG_FMT_GIF) {
						dec_ok = gif_decode_first_frame_xrgb(fetch_buf, got_full, w->pixels, (size_t)IMG_WORKER_MAX_PX, &dw, &dh);
					}
					if (dec_ok == 0 && dw <= IMG_WORKER_MAX_W && dh <= IMG_WORKER_MAX_H && dw * dh <= IMG_WORKER_MAX_PX) {
						w->has_pixels = 1;
						w->pix_w = (uint16_t)dw;
						w->pix_h = (uint16_t)dh;
						w->pix_len = (uint32_t)(dw * dh);
						w->rc = 0;
					} else {
						w->rc = -1;
					}
				}
			}
		}

		if (w->rc != 0) {
			/* Even if pixel decode failed, keep sniff results (fmt/dims) best-effort. */
			w->rc = 0;
		}
		w->state = 2u;
	}
}

static void img_workers_init(void)
{
	if (g_img_workers) return;
	void *p = sys_mmap(0,
			    sizeof(struct img_worker_shm) * (size_t)IMG_WORKERS,
			    PROT_READ | PROT_WRITE,
			    MAP_SHARED | MAP_ANONYMOUS,
			    -1,
			    0);
	if (p == MAP_FAILED) return;
	g_img_workers = (struct img_worker_shm *)p;
	for (uint32_t i = 0; i < IMG_WORKERS; i++) {
		g_img_workers[i].state = 0;
		g_img_workers[i].key[0] = 0;
	}
	for (uint32_t i = 0; i < IMG_WORKERS; i++) {
		int pid = sys_fork();
		if (pid == 0) {
			img_worker_loop(i);
			sys_exit(0);
		}
		/* Parent: if fork failed, just leave fewer workers. */
		if (pid < 0) break;
	}
}

static struct img_sniff_cache_entry *img_cache_find_by_key(const char *key)
{
	if (!key || !key[0]) return 0;
	uint32_t h = hash32_fnv1a(key);
	for (size_t i = 0; i < sizeof(g_img_sniff_cache) / sizeof(g_img_sniff_cache[0]); i++) {
		struct img_sniff_cache_entry *e = &g_img_sniff_cache[i];
		if (!e->used) continue;
		if (e->hash == h && streq(e->key, key)) return e;
	}
	return 0;
}

static int img_workers_pump(int *out_any_dims_changed)
{
	if (out_any_dims_changed) *out_any_dims_changed = 0;
	if (!g_img_workers) return 0;
	int did_complete = 0;

	/* Collect completed workers. */
	for (uint32_t wi = 0; wi < IMG_WORKERS; wi++) {
		struct img_worker_shm *w = &g_img_workers[wi];
		if (w->state != 2u) continue;
		did_complete = 1;
		struct img_sniff_cache_entry *e = img_cache_find_by_key(w->key);
		if (e) {
			uint8_t had_dims = e->has_dims;
			e->fmt = (enum img_fmt)w->fmt;
			e->has_dims = (uint8_t)(w->has_dims != 0);
			e->w = w->w;
			e->h = w->h;
			if (out_any_dims_changed && e->has_dims && !had_dims) {
				*out_any_dims_changed = 1;
			}

			if (w->has_pixels && w->pix_len != 0) {
				/* Replace existing pixels if any. */
				if (e->has_pixels && e->pix_len != 0) {
					img_pixel_free(e->pix_off, e->pix_len);
					e->has_pixels = 0;
					e->pix_off = 0;
					e->pix_w = 0;
					e->pix_h = 0;
					e->pix_len = 0;
				}

				uint32_t off = 0;
				int alloc_ok = img_pixel_alloc(w->pix_len, &off);
				for (int tries = 0; alloc_ok != 0 && tries < 8; tries++) {
					if (img_cache_evict_one() != 0) break;
					alloc_ok = img_pixel_alloc(w->pix_len, &off);
				}
				if (alloc_ok == 0) {
					/* Copy pixels into shared pool. */
					for (uint32_t i = 0; i < w->pix_len; i++) {
						g_img_pixel_pool[off + i] = w->pixels[i];
					}
					e->has_pixels = 1;
					e->pix_off = off;
					e->pix_w = w->pix_w;
					e->pix_h = w->pix_h;
					e->pix_len = w->pix_len;
				}
			}
			e->state = 2;
			e->inflight = 0;
			e->last_use = ++g_img_use_tick;
		}

		w->state = 0u;
	}

	/* Dispatch new work to idle workers. */
	for (uint32_t wi = 0; wi < IMG_WORKERS; wi++) {
		struct img_worker_shm *w = &g_img_workers[wi];
		if (w->state != 0u) continue;
		struct img_sniff_cache_entry *pick = 0;
		uint32_t best_use = 0;
		for (uint32_t i = 0; i < (uint32_t)(sizeof(g_img_sniff_cache) / sizeof(g_img_sniff_cache[0])); i++) {
			struct img_sniff_cache_entry *e = &g_img_sniff_cache[i];
			if (!e->used) continue;
			if (e->state != 1) continue;
			if (e->inflight) continue;
			if (!e->want_pixels) continue;
			if (!pick || e->last_use > best_use) {
				pick = e;
				best_use = e->last_use;
			}
		}
		if (!pick) break;
		pick->inflight = 1;
		(void)c_strlcpy_s(w->key, sizeof(w->key), pick->key);
		w->state = 1u;
		w->rc = 0;
	}

	return did_complete;
}

static int img_decode_large_pump_one(void)
{
	for (size_t i = 0; i < sizeof(g_img_sniff_cache) / sizeof(g_img_sniff_cache[0]); i++) {
		struct img_sniff_cache_entry *e = &g_img_sniff_cache[i];
		if (!e->used) continue;
		if (e->state != 2) continue;
		if (!e->want_pixels) continue;
		if (e->inflight) continue;
		if (e->has_pixels) continue;
		if (!e->has_dims) continue;
		if (e->w <= IMG_WORKER_MAX_W && e->h <= IMG_WORKER_MAX_H) continue;
		if (e->w > 512u || e->h > 512u) continue;
		if (!(e->fmt == IMG_FMT_JPG || e->fmt == IMG_FMT_PNG || e->fmt == IMG_FMT_GIF)) continue;

		char host[HOST_BUF_LEN];
		char path[PATH_BUF_LEN];
		if (split_host_path_from_key(e->key, host, sizeof(host), path, sizeof(path)) != 0) {
			e->want_pixels = 0;
			return 0;
		}

		uint32_t px = (uint32_t)e->w * (uint32_t)e->h;
		if (px == 0 || px > (uint32_t)(sizeof(g_img_pixel_pool) / sizeof(g_img_pixel_pool[0]))) {
			e->want_pixels = 0;
			return 0;
		}

		size_t got_full = 0;
		if (https_get_prefix_follow_redirects(host, path, g_img_fetch_buf, sizeof(g_img_fetch_buf), &got_full) != 0 || got_full == 0) {
			e->want_pixels = 0;
			return 0;
		}

		uint32_t old_used = g_img_pixel_pool_used;
		uint32_t off = 0;
		if (img_pixel_alloc(px, &off) != 0) return 0;
		uint32_t dw = 0, dh = 0;
		int ok = -1;
		if (e->fmt == IMG_FMT_JPG) {
			ok = jpeg_decode_baseline_xrgb(g_img_fetch_buf, got_full, &g_img_pixel_pool[off], (size_t)px, &dw, &dh);
		} else if (e->fmt == IMG_FMT_PNG) {
			uint8_t *scratch = &g_img_fetch_buf[got_full];
			size_t scratch_cap = sizeof(g_img_fetch_buf) - got_full;
			ok = png_decode_xrgb(g_img_fetch_buf, got_full, scratch, scratch_cap, &g_img_pixel_pool[off], (size_t)px, &dw, &dh);
		} else if (e->fmt == IMG_FMT_GIF) {
			ok = gif_decode_first_frame_xrgb(g_img_fetch_buf, got_full, &g_img_pixel_pool[off], (size_t)px, &dw, &dh);
		}
		if (ok == 0) {
			e->has_pixels = 1;
			e->pix_off = off;
			e->pix_w = (dw > 0xffffu) ? 0xffffu : (uint16_t)dw;
			e->pix_h = (dh > 0xffffu) ? 0xffffu : (uint16_t)dh;
			e->pix_len = px;
			e->last_use = ++g_img_use_tick;
			return 1;
		} else {
			g_img_pixel_pool_used = old_used;
			e->want_pixels = 0;
		}
		return 0;
	}
	return 0;
}

struct ui_buttons {
	uint32_t en_x, en_y, en_w, en_h;
	uint32_t de_x, de_y, de_w, de_h;
	uint32_t reload_x, reload_y, reload_w, reload_h;
};

static struct ui_buttons ui_layout_buttons(const struct shm_fb *fb)
{
	struct ui_buttons b;
	b.en_w = 32;
	b.de_w = 32;
	b.reload_w = 64;
	b.en_h = 18;
	b.de_h = 18;
	b.reload_h = 18;
	b.en_y = 3;
	b.de_y = 3;
	b.reload_y = 3;

	uint32_t pad = 6;
	uint32_t right = (fb->width > 8) ? (fb->width - 8) : 0;

	b.reload_x = (right > b.reload_w) ? (right - b.reload_w) : 0;
	right = (b.reload_x > pad) ? (b.reload_x - pad) : 0;
	b.de_x = (right > b.de_w) ? (right - b.de_w) : 0;
	right = (b.de_x > pad) ? (b.de_x - pad) : 0;
	b.en_x = (right > b.en_w) ? (right - b.en_w) : 0;

	return b;
}

static void draw_text_clipped_u32(void *base, uint32_t stride_bytes, uint32_t x, uint32_t y, uint32_t max_w_px, const char *s, struct text_color color)
{
	if (!s) return;
	if (max_w_px < 8) return;
	uint32_t max_chars = max_w_px / 8u;
	if (max_chars == 0) return;
	char tmp[256];
	uint32_t o = 0;
	for (; s[o] && o + 1 < sizeof(tmp) && o < max_chars; o++) {
		tmp[o] = s[o];
	}
	tmp[o] = 0;
	draw_text_u32(base, stride_bytes, x, y, tmp, color);
}

static void draw_ui(struct shm_fb *fb, const char *active_host, const char *url_bar, const char *status_bar, const char *line1, const char *line2, const char *line3)
{
	/* Background */
	fill_rect_u32(fb->pixels, fb->stride, 0, 0, fb->width, fb->height, 0xff101014u);

	/* Title bar */
	fill_rect_u32(fb->pixels, fb->stride, 0, 0, fb->width, UI_TOPBAR_H, 0xff202028u);
	struct text_color tc = { .fg = 0xffffffffu, .bg = 0, .opaque_bg = 0 };
	struct text_color dim = { .fg = 0xffb0b0b0u, .bg = 0, .opaque_bg = 0 };

	/* Clickable buttons in the top bar */
	struct ui_buttons b = ui_layout_buttons(fb);
	uint32_t bg_active = 0xff405060u;
	uint32_t bg_idle = 0xff303840u;
	uint32_t bg_reload = 0xff303840u;

	int active_en = (active_host && active_host[0] == 'e');
	int active_de = (active_host && active_host[0] == 'd');

	fill_rect_u32(fb->pixels, fb->stride, b.en_x, b.en_y, b.en_w, b.en_h, active_en ? bg_active : bg_idle);
	fill_rect_u32(fb->pixels, fb->stride, b.de_x, b.de_y, b.de_w, b.de_h, active_de ? bg_active : bg_idle);
	fill_rect_u32(fb->pixels, fb->stride, b.reload_x, b.reload_y, b.reload_w, b.reload_h, bg_reload);

	draw_text_u32(fb->pixels, fb->stride, b.en_x + 8, b.en_y + 7, "EN", dim);
	draw_text_u32(fb->pixels, fb->stride, b.de_x + 8, b.de_y + 7, "DE", dim);
	draw_text_u32(fb->pixels, fb->stride, b.reload_x + 8, b.reload_y + 7, "Reload", dim);

	/* URL + HTTP status (clipped to available space). */
	uint32_t url_x = 8;
	uint32_t url_right = (b.en_x > 8) ? (b.en_x - 8) : 0;
	uint32_t url_w = (url_right > url_x) ? (url_right - url_x) : 0;
	char top[512];
	top[0] = 0;
	if (url_bar && url_bar[0]) {
		(void)c_strlcpy_s(top, sizeof(top), url_bar);
	}
	if (status_bar && status_bar[0]) {
		/* Append a separator + status. */
		size_t o = c_strnlen_s(top, sizeof(top));
		if (o + 1 < sizeof(top)) top[o++] = ' ';
		if (o + 1 < sizeof(top)) top[o++] = ' ';
		for (size_t i = 0; status_bar[i] && o + 1 < sizeof(top); i++) {
			top[o++] = status_bar[i];
		}
		top[o] = 0;
	}
	draw_text_clipped_u32(fb->pixels, fb->stride, url_x, 8, url_w, top, dim);

	/* Body */
	fill_rect_u32(fb->pixels, fb->stride, 0, UI_TOPBAR_H, fb->width, fb->height - UI_TOPBAR_H, 0xff101014u);

	draw_text_u32(fb->pixels, fb->stride, 8, 40, line1, tc);
	draw_text_u32(fb->pixels, fb->stride, 8, 56, line2, tc);
	draw_text_u32(fb->pixels, fb->stride, 8, 72, line3, dim);
}

static int links_contains_index(const struct html_links *links, uint32_t idx)
{
	if (!links) return 0;
	for (uint32_t i = 0; i < links->n && i < HTML_MAX_LINKS; i++) {
		const struct html_link *l = &links->links[i];
		if (idx >= l->start && idx < l->end) return 1;
	}
	return 0;
}

struct link_style {
	uint32_t fg_xrgb;
	uint8_t has_fg;
	uint32_t bg_xrgb;
	uint8_t has_bg;
	uint8_t bold;
	uint8_t underline;
};

static struct link_style spans_style_at_index(const struct html_spans *spans, uint32_t idx, uint32_t default_fg_xrgb)
{
	struct link_style st;
	st.fg_xrgb = default_fg_xrgb;
	st.has_fg = 0;
	st.bg_xrgb = 0;
	st.has_bg = 0;
	st.bold = 0;
	st.underline = 0;
	if (!spans) return st;
	for (uint32_t i = 0; i < spans->n && i < HTML_MAX_SPANS; i++) {
		const struct html_span *sp = &spans->spans[i];
		if (idx >= sp->start && idx < sp->end) {
			if (sp->has_fg) { st.has_fg = 1; st.fg_xrgb = sp->fg_xrgb; }
			if (sp->has_bg) { st.has_bg = 1; st.bg_xrgb = sp->bg_xrgb; }
			st.bold = sp->bold;
			st.underline = sp->underline;
			return st;
		}
	}
	return st;
}

static struct link_style links_style_at_index(const struct html_links *links, uint32_t idx, uint32_t default_fg_xrgb)
{
	struct link_style st;
	st.fg_xrgb = default_fg_xrgb;
	st.has_fg = 0;
	st.bg_xrgb = 0;
	st.has_bg = 0;
	st.bold = 0;
	st.underline = 0;
	if (!links) return st;
	for (uint32_t i = 0; i < links->n && i < HTML_MAX_LINKS; i++) {
		const struct html_link *l = &links->links[i];
		if (idx >= l->start && idx < l->end) {
			if (l->has_fg) { st.has_fg = 1; st.fg_xrgb = l->fg_xrgb; }
			if (l->has_bg) { st.has_bg = 1; st.bg_xrgb = l->bg_xrgb; }
			st.bold = l->bold;
			st.underline = l->underline;
			return st;
		}
	}
	return st;
}

static const char *links_href_at_index(const struct html_links *links, uint32_t idx)
{
	if (!links) return 0;
	for (uint32_t i = 0; i < links->n && i < HTML_MAX_LINKS; i++) {
		const struct html_link *l = &links->links[i];
		if (idx >= l->start && idx < l->end) return l->href;
	}
	return 0;
}

static void draw_line_with_links(struct shm_fb *fb,
				 uint32_t x,
				 uint32_t y,
				 const char *line,
				 size_t base_index,
				 const struct html_links *links,
				 const struct html_spans *spans,
				 struct text_color normal,
				 struct text_color linkc)
{
	if (!fb || !line) return;
	/* Build and draw segments where link/non-link state is constant. */
	char seg[256];
	uint32_t seg_x = x;
	int seg_is_link = 0;
	struct link_style seg_st;
	seg_st.fg_xrgb = linkc.fg;
	seg_st.has_fg = 0;
	seg_st.bg_xrgb = 0;
	seg_st.has_bg = 0;
	seg_st.bold = 0;
	seg_st.underline = 0;
	uint32_t si = 0;
	for (uint32_t i = 0; line[i] != 0; i++) {
		uint32_t idx = (base_index > 0xffffffffu) ? 0xffffffffu : (uint32_t)(base_index + (size_t)i);
		int is_link = links_contains_index(links, idx);
		struct link_style st = spans_style_at_index(spans, idx, normal.fg);
		if (is_link) {
			/* Links override spans. */
			st = links_style_at_index(links, idx, linkc.fg);
		}
		if (i == 0) { seg_is_link = is_link; seg_st = st; }
		if (is_link != seg_is_link ||
		    (is_link && (st.fg_xrgb != seg_st.fg_xrgb || st.has_bg != seg_st.has_bg || st.bg_xrgb != seg_st.bg_xrgb || st.bold != seg_st.bold || st.underline != seg_st.underline)) ||
		    (!is_link && (st.fg_xrgb != seg_st.fg_xrgb || st.has_bg != seg_st.has_bg || st.bg_xrgb != seg_st.bg_xrgb || st.bold != seg_st.bold || st.underline != seg_st.underline)) ||
		    si + 2 >= sizeof(seg)) {
			seg[si] = 0;
			struct text_color c = seg_is_link ? linkc : normal;
			c.fg = seg_st.fg_xrgb;
			if (seg_st.bold && !seg_is_link && !seg_st.has_fg) {
				/* Make bold stand out even when the default text color is dim. */
				c.fg = 0xffffffffu;
			}
			if (seg_st.has_bg) {
				fill_rect_u32(fb->pixels, fb->stride, seg_x, y, (uint32_t)si * 8u, 16u, seg_st.bg_xrgb);
			}
			draw_text_u32(fb->pixels, fb->stride, seg_x, y, seg, c);
			if (seg_st.bold) {
				draw_text_u32(fb->pixels, fb->stride, seg_x + 1, y, seg, c);
			}
			if (seg_st.underline) {
				fill_rect_u32(fb->pixels, fb->stride, seg_x, y + 15u, (uint32_t)si * 8u, 1u, c.fg);
			}
			seg_x += (uint32_t)si * 8u;
			si = 0;
			seg_is_link = is_link;
			seg_st = st;
		}
		seg[si++] = line[i];
	}
	if (si > 0) {
		seg[si] = 0;
		struct text_color c = seg_is_link ? linkc : normal;
		c.fg = seg_st.fg_xrgb;
		if (seg_st.bold && !seg_is_link && !seg_st.has_fg) {
			/* Make bold stand out even when the default text color is dim. */
			c.fg = 0xffffffffu;
		}
		if (seg_st.has_bg) {
			fill_rect_u32(fb->pixels, fb->stride, seg_x, y, (uint32_t)si * 8u, 16u, seg_st.bg_xrgb);
		}
		draw_text_u32(fb->pixels, fb->stride, seg_x, y, seg, c);
		if (seg_st.bold) {
			draw_text_u32(fb->pixels, fb->stride, seg_x + 1, y, seg, c);
		}
		if (seg_st.underline) {
			fill_rect_u32(fb->pixels, fb->stride, seg_x, y + 15u, (uint32_t)si * 8u, 1u, c.fg);
		}
	}
}

static void draw_body_wrapped(struct shm_fb *fb,
				     uint32_t x,
				     uint32_t y,
				     uint32_t w_px,
				     uint32_t h_px,
				     const char *text,
				     const struct html_links *links,
				     const struct html_spans *spans,
				     const struct html_inline_imgs *inline_imgs,
				     struct text_color tc,
				     struct text_color linkc,
			     uint32_t scroll_rows,
			     const char *active_host)
{
	if (!fb || !text) return;
	uint32_t max_cols = (w_px / 8u);
	uint32_t max_rows = (h_px / 16u);
	if (max_cols == 0 || max_rows == 0) return;
	if (max_cols > 255) max_cols = 255;

	struct {
		uint8_t active;
		uint32_t rows_total;
		uint32_t row_in_box; /* 0=label row, 1..=pixel rows */
		struct img_sniff_cache_entry *entry;
	} img_box = {0};

	struct {
		uint8_t active;
		uint32_t rows_total;
		uint32_t row_in_box; /* 1..=pixel rows (label row is rendered on marker row) */
		uint32_t cols_total; /* total box width in text columns */
		struct img_sniff_cache_entry *entry;
	} float_box = {0};

	/* Skip scroll_rows lines by running the layout forward.
	 * Track whether we are currently inside an image placeholder so that
	 * scrolling into the middle of an image still renders the correct slice.
	 */
	size_t pos = 0;
	char line[1024];
	for (uint32_t s = 0; s < scroll_rows; s++) {
		uint32_t use_cols = max_cols;
		if (float_box.active) {
			uint32_t fcols = float_box.cols_total;
			if (max_cols > 12u) {
				if (fcols > max_cols - 10u) fcols = max_cols - 10u;
			} else {
				if (fcols > max_cols - 1u) fcols = max_cols - 1u;
			}
			use_cols = (max_cols > (fcols + 1u)) ? (max_cols - fcols - 1u) : 1u;
			if (use_cols < 1u) use_cols = 1u;
		}
		int r = text_layout_next_line(text, &pos, use_cols, line, sizeof(line));
		if (r != 0) break;
		if (img_box.active) {
			img_box.row_in_box++;
			if (img_box.row_in_box >= img_box.rows_total) {
				img_box.active = 0;
			}
			continue;
		}
		if (line[0] == (char)0x1e && line[1] == 'I' && line[2] == 'M' && line[3] == 'G' && line[4] == ' ') {
			uint32_t rows = 0;
			size_t p = 5;
			while (line[p] >= '0' && line[p] <= '9') {
				rows = rows * 10u + (uint32_t)(line[p] - '0');
				p++;
			}
			while (line[p] == ' ') p++;
			char *tok = &line[p];
			while (line[p] && line[p] != ' ' && (uint8_t)line[p] != 0x1f) p++;
			char tok_end = line[p];
			line[p] = 0;
			int is_float = (tok[0] == 'F' && tok[1] == 'R');
			uint32_t fcols = 0;
			if (is_float) {
				for (size_t k = 2; tok[k] >= '0' && tok[k] <= '9'; k++) {
					fcols = fcols * 10u + (uint32_t)(tok[k] - '0');
				}
			}
			line[p] = tok_end;
			if (rows < 2u) rows = 2u;
			if (rows > 200u) rows = 200u;
			/* Find URL payload to get cache entry (best-effort). */
			char *url = 0;
			for (size_t k = p; line[k] != 0; k++) {
				if ((uint8_t)line[k] == 0x1f) {
					url = &line[k + 1];
					break;
				}
			}
			if (is_float && fcols > 0) {
				float_box.entry = img_cache_find_done(active_host, url ? url : "");
				float_box.active = 1;
				float_box.rows_total = rows;
				if (fcols < 6u) fcols = 6u;
				if (fcols > 80u) fcols = 80u;
				float_box.cols_total = fcols;
				float_box.row_in_box = 0;

				/* The renderer draws the float label row and also consumes the next
				 * wrapped text line in the *same* visual row. Mirror that here so
				 * scrolling keeps pos aligned with what is actually displayed.
				 */
				uint32_t use_cols2 = max_cols;
				{
					uint32_t f2 = float_box.cols_total;
					if (max_cols > 12u) {
						if (f2 > max_cols - 10u) f2 = max_cols - 10u;
					} else {
						if (f2 > max_cols - 1u) f2 = max_cols - 1u;
					}
					use_cols2 = (max_cols > (f2 + 1u)) ? (max_cols - f2 - 1u) : 1u;
					if (use_cols2 < 1u) use_cols2 = 1u;
				}
				(void)text_layout_next_line(text, &pos, use_cols2, line, sizeof(line));
			} else {
				img_box.entry = img_cache_find_done(active_host, url ? url : "");
				img_box.active = 1;
				img_box.rows_total = rows;
				img_box.row_in_box = 1; /* marker row was skipped */
			}
		}
		if (float_box.active) {
			float_box.row_in_box++;
			if (float_box.row_in_box >= float_box.rows_total) {
				float_box.active = 0;
			}
		}
	}

	for (uint32_t row = 0; row < max_rows; row++) {
		size_t start = 0;
		uint32_t use_cols = max_cols;
		if (float_box.active) {
			uint32_t fcols = float_box.cols_total;
			if (max_cols > 12u) {
				if (fcols > max_cols - 10u) fcols = max_cols - 10u;
			} else {
				if (fcols > max_cols - 1u) fcols = max_cols - 1u;
			}
			use_cols = (max_cols > (fcols + 1u)) ? (max_cols - fcols - 1u) : 1u;
			if (use_cols < 1u) use_cols = 1u;
		}
		int r = text_layout_next_line_ex(text, &pos, use_cols, line, sizeof(line), &start);
		if (r != 0) return;
		uint32_t row_y = y + row * 16u;

		/* If we are inside an image placeholder (but not on the marker row), draw
		 * the corresponding 16px slice for this row.
		 */
		if (img_box.active) {
			uint32_t rows_total = img_box.rows_total;
			uint32_t rbox = img_box.row_in_box;
			/* Geometry */
			uint32_t content_bottom = y + h_px;
			if (fb->height < content_bottom) content_bottom = fb->height;
			uint32_t remaining_h = (content_bottom > row_y) ? (content_bottom - row_y) : 0u;
			uint32_t row_h = (remaining_h > 16u) ? 16u : remaining_h;
			uint32_t max_w = (fb->width > x) ? (fb->width - x) : 0u;
			if (row_h == 0u || max_w == 0u) {
				img_box.row_in_box++;
				if (img_box.row_in_box >= rows_total) img_box.active = 0;
				continue;
			}
			uint32_t box_w = w_px;
			if (box_w > max_w) box_w = max_w;
			/* Optional width shrink based on known dimensions. */
			if (img_box.entry && img_box.entry->has_dims) {
				uint32_t want_w = (uint32_t)img_box.entry->w + 2u;
				if (want_w < 32u) want_w = 32u;
				if (want_w < box_w) box_w = want_w;
			}
			if (box_w >= 2u) {
				uint32_t col = 0xff505058u;
				/* Left + right border for this row */
				fill_rect_u32(fb->pixels, fb->stride, x, row_y, 1u, row_h, col);
				fill_rect_u32(fb->pixels, fb->stride, x + box_w - 1u, row_y, 1u, row_h, col);
				/* Bottom border on last row */
				if (rbox + 1u == rows_total && row_h > 0u) {
					fill_rect_u32(fb->pixels, fb->stride, x, row_y + row_h - 1u, box_w, 1u, col);
				}
				/* Pixel slice (skip label row). */
				if (img_box.entry && img_box.entry->has_pixels && rbox >= 1u) {
					uint32_t inner_x = x + 1u;
					uint32_t inner_w = (box_w > 2u) ? (box_w - 2u) : 0u;
					uint32_t dst_h = row_h;
					if (rbox + 1u == rows_total && dst_h > 0u) {
						/* Keep bottom border visible. */
						dst_h = (dst_h > 1u) ? (dst_h - 1u) : 0u;
					}
					uint32_t src_y0 = (rbox - 1u) * 16u;
					if (dst_h != 0u && src_y0 < (uint32_t)img_box.entry->pix_h) {
						const uint32_t *src = &g_img_pixel_pool[img_box.entry->pix_off] + (size_t)src_y0 * (size_t)img_box.entry->pix_w;
						uint32_t src_h = (uint32_t)img_box.entry->pix_h - src_y0;
						blit_xrgb_clipped(fb, inner_x, row_y, inner_w, dst_h, src, img_box.entry->pix_w, src_h);
					}
				}
			}
			img_box.row_in_box++;
			if (img_box.row_in_box >= rows_total) {
				img_box.active = 0;
			}
			continue;
		}

		/* If a float is active for this row, draw its pixel slice on the right.
		 * (The label row is drawn when we encounter the marker line.)
		 */
		if (float_box.active) {
			uint32_t rows_total = float_box.rows_total;
			uint32_t rbox = float_box.row_in_box;
			uint32_t content_bottom = y + h_px;
			if (fb->height < content_bottom) content_bottom = fb->height;
			uint32_t remaining_h = (content_bottom > row_y) ? (content_bottom - row_y) : 0u;
			uint32_t row_h = (remaining_h > 16u) ? 16u : remaining_h;
			uint32_t max_w = (fb->width > x) ? (fb->width - x) : 0u;
			if (row_h != 0u && max_w != 0u && rows_total >= 2u && rbox >= 1u) {
				uint32_t fcols = float_box.cols_total;
				if (max_cols > 12u) {
					if (fcols > max_cols - 10u) fcols = max_cols - 10u;
				} else if (fcols > max_cols - 1u) {
					fcols = max_cols - 1u;
				}
				uint32_t box_w = fcols * 8u;
				if (box_w > w_px) box_w = w_px;
				if (box_w > max_w) box_w = max_w;
				/* Optional width shrink based on known dimensions. */
				if (float_box.entry && float_box.entry->has_dims) {
					uint32_t want_w = (uint32_t)float_box.entry->w + 2u;
					if (want_w < 32u) want_w = 32u;
					if (want_w < box_w) box_w = want_w;
				}
				/* Cap floats to at most half the content width. */
				if (box_w > w_px / 2u) box_w = w_px / 2u;
				if (box_w >= 2u) {
					uint32_t fx = x + ((w_px > box_w) ? (w_px - box_w) : 0u);
					uint32_t col = 0xff505058u;
					fill_rect_u32(fb->pixels, fb->stride, fx, row_y, 1u, row_h, col);
					fill_rect_u32(fb->pixels, fb->stride, fx + box_w - 1u, row_y, 1u, row_h, col);
					if (rbox + 1u == rows_total && row_h > 0u) {
						fill_rect_u32(fb->pixels, fb->stride, fx, row_y + row_h - 1u, box_w, 1u, col);
					}
					if (float_box.entry && float_box.entry->has_pixels) {
						uint32_t inner_x = fx + 1u;
						uint32_t inner_w = (box_w > 2u) ? (box_w - 2u) : 0u;
						uint32_t dst_h = row_h;
						if (rbox + 1u == rows_total && dst_h > 0u) {
							dst_h = (dst_h > 1u) ? (dst_h - 1u) : 0u;
						}
						uint32_t src_y0 = (rbox - 1u) * 16u;
						if (dst_h != 0u && src_y0 < (uint32_t)float_box.entry->pix_h) {
							const uint32_t *src = &g_img_pixel_pool[float_box.entry->pix_off] + (size_t)src_y0 * (size_t)float_box.entry->pix_w;
							uint32_t src_h = (uint32_t)float_box.entry->pix_h - src_y0;
							blit_xrgb_clipped(fb, inner_x, row_y, inner_w, dst_h, src, float_box.entry->pix_w, src_h);
						}
					}
				}
			}
		}

		/* Image placeholder marker row: draw real lines in framebuffer.
		 * Marker format: 0x1e "IMG <rows> <token> <label>" 0x1f "<url>".
		 * Token is either "?" (block placeholder) or "FR<cols>" (float-right).
		 * Format is sniffed from the first bytes of the URL.
		 */
		if (line[0] == (char)0x1e && line[1] == 'I' && line[2] == 'M' && line[3] == 'G' && line[4] == ' ') {
			/* Parse rows */
			uint32_t rows = 0;
			size_t p = 5;
			while (line[p] >= '0' && line[p] <= '9') {
				uint32_t d = (uint32_t)(line[p] - '0');
				rows = rows * 10u + d;
				p++;
			}
			while (line[p] == ' ') p++;
			/* Parse placeholder token (renderer still sniffs actual image format). */
			char *tok = &line[p];
			while (line[p] && line[p] != ' ' && (uint8_t)line[p] != 0x1f) p++;
			char tok_end = line[p];
			line[p] = 0;
			int is_float = (tok[0] == 'F' && tok[1] == 'R');
			uint32_t fcols = 0;
			if (is_float) {
				for (size_t k = 2; tok[k] >= '0' && tok[k] <= '9'; k++) {
					fcols = fcols * 10u + (uint32_t)(tok[k] - '0');
				}
			}
			line[p] = tok_end;
			while (line[p] == ' ') p++;
			char *label = &line[p];
			char *url = 0;
			for (size_t k = p; line[k] != 0; k++) {
				if ((uint8_t)line[k] == 0x1f) {
					line[k] = 0;
					url = &line[k + 1];
					break;
				}
			}
			enum img_fmt sniffed = img_cache_get_or_mark_pending(active_host, url ? url : "");
			struct img_sniff_cache_entry *entry = img_cache_find_done(active_host, url ? url : "");
			const char *fmt = img_fmt_token(sniffed);
			if (rows < 2u) rows = 2u;
			if (rows > 200u) rows = 200u;
			if (is_float && fcols > 0) {
				/* Float-right marker row: draw only the label row on the right,
				 * and render the next text line alongside it in the same visual row.
				 */
				uint32_t content_bottom = y + h_px;
				if (fb->height < content_bottom) content_bottom = fb->height;
				uint32_t remaining_h = (content_bottom > row_y) ? (content_bottom - row_y) : 0u;
				uint32_t row_h = (remaining_h > 16u) ? 16u : remaining_h;
				uint32_t max_w = (fb->width > x) ? (fb->width - x) : 0u;
				if (fcols < 6u) fcols = 6u;
				if (fcols > 80u) fcols = 80u;
				uint32_t box_w = fcols * 8u;
				if (box_w > w_px) box_w = w_px;
				if (box_w > max_w) box_w = max_w;
				if (entry && entry->has_dims) {
					uint32_t want_w = (uint32_t)entry->w + 2u;
					if (want_w < 32u) want_w = 32u;
					if (want_w < box_w) box_w = want_w;
				}
				if (box_w > w_px / 2u) box_w = w_px / 2u;
				uint32_t fx = x + ((w_px > box_w) ? (w_px - box_w) : 0u);
				uint32_t col = 0xff505058u;
				if (row_h != 0u && box_w != 0u) {
					fill_rect_u32(fb->pixels, fb->stride, fx, row_y, box_w, 1u, col);
					if (box_w >= 2u) {
						fill_rect_u32(fb->pixels, fb->stride, fx, row_y, 1u, row_h, col);
						fill_rect_u32(fb->pixels, fb->stride, fx + box_w - 1u, row_y, 1u, row_h, col);
					}
				}
				struct text_color fmtc = tc;
				fmtc.fg = 0xffffa000u;
				uint32_t label_x = fx + 8u;
				uint32_t label_w = (box_w > 16u) ? (box_w - 16u) : 0u;
				if (fmt[0] != 0 && label_w >= 8u * 4u) {
					draw_text_u32(fb->pixels, fb->stride, label_x, row_y + 6u, fmt, fmtc);
					label_x += 8u * 5u;
					label_w = (label_w > 8u * 5u) ? (label_w - 8u * 5u) : 0u;
				}
				if (entry && entry->has_dims && label_w >= 8u * 6u) {
					char wdec[11];
					char hdec[11];
					u32_to_dec(wdec, (uint32_t)entry->w);
					u32_to_dec(hdec, (uint32_t)entry->h);
					char dims[32];
					size_t di = 0;
					for (size_t ii = 0; wdec[ii] && di + 1 < sizeof(dims); ii++) dims[di++] = wdec[ii];
					if (di + 1 < sizeof(dims)) dims[di++] = 'x';
					for (size_t ii = 0; hdec[ii] && di + 1 < sizeof(dims); ii++) dims[di++] = hdec[ii];
					if (di + 1 < sizeof(dims)) dims[di++] = ' ';
					dims[di] = 0;
					draw_text_u32(fb->pixels, fb->stride, label_x, row_y + 6u, dims, fmtc);
					uint32_t adv = (uint32_t)di * 8u;
					label_x += adv;
					label_w = (label_w > adv) ? (label_w - adv) : 0u;
				}
				draw_text_clipped_u32(fb->pixels, fb->stride, label_x, row_y + 6u, label_w, label, tc);

				/* Activate float; pixel rows start on the next visual row. */
				float_box.active = 1;
				float_box.rows_total = rows;
				float_box.row_in_box = 0;
				float_box.cols_total = fcols;
				float_box.entry = entry;

				/* Replace this marker line with the next text line, rendered with
				 * reduced width so it flows beside the float.
				 */
				use_cols = max_cols;
				{
					uint32_t f2 = float_box.cols_total;
					if (max_cols > 12u) {
						if (f2 > max_cols - 10u) f2 = max_cols - 10u;
					} else {
						if (f2 > max_cols - 1u) f2 = max_cols - 1u;
					}
					use_cols = (max_cols > (f2 + 1u)) ? (max_cols - f2 - 1u) : 1u;
					if (use_cols < 1u) use_cols = 1u;
				}
				start = 0;
				line[0] = 0;
				(void)text_layout_next_line_ex(text, &pos, use_cols, line, sizeof(line), &start);
				/* Fall through to normal text rendering for this row (do NOT also render a block placeholder). */
			} else {
				img_box.entry = entry;

				/* Draw only the marker row (top border + label). The following reserved
				 * blank lines will be rendered row-by-row so scrolling into the middle
				 * of an image still shows pixels.
				 */
				uint32_t content_bottom = y + h_px;
				if (fb->height < content_bottom) content_bottom = fb->height;
				uint32_t remaining_h = (content_bottom > row_y) ? (content_bottom - row_y) : 0u;
				uint32_t row_h = (remaining_h > 16u) ? 16u : remaining_h;
				uint32_t max_w = (fb->width > x) ? (fb->width - x) : 0u;
				uint32_t box_w = w_px;
				if (box_w > max_w) box_w = max_w;
				if (img_box.entry && img_box.entry->has_dims) {
					uint32_t want_w = (uint32_t)img_box.entry->w + 2u;
					if (want_w < 32u) want_w = 32u;
					if (want_w < box_w) box_w = want_w;
				}
				uint32_t col = 0xff505058u;
				if (row_h != 0u && box_w != 0u) {
					/* Top border for this row */
					fill_rect_u32(fb->pixels, fb->stride, x, row_y, box_w, 1u, col);
					/* Left + right borders for this row */
					if (box_w >= 2u) {
						fill_rect_u32(fb->pixels, fb->stride, x, row_y, 1u, row_h, col);
						fill_rect_u32(fb->pixels, fb->stride, x + box_w - 1u, row_y, 1u, row_h, col);
					}
				}
				/* Label: show format in orange, then the rest in normal. */
				struct text_color fmtc = tc;
				fmtc.fg = 0xffffa000u;
				uint32_t label_x = x + 8u;
				uint32_t label_w = (box_w > 16u) ? (box_w - 16u) : 0u;
				if (fmt[0] != 0 && label_w >= 8u * 4u) {
					draw_text_u32(fb->pixels, fb->stride, label_x, row_y + 6u, fmt, fmtc);
					label_x += 8u * 5u; /* fmt + space */
					label_w = (label_w > 8u * 5u) ? (label_w - 8u * 5u) : 0u;
				}
				if (img_box.entry && img_box.entry->has_dims && label_w >= 8u * 6u) {
					char wdec[11];
					char hdec[11];
					u32_to_dec(wdec, (uint32_t)img_box.entry->w);
					u32_to_dec(hdec, (uint32_t)img_box.entry->h);
					char dims[32];
					size_t di = 0;
					for (size_t ii = 0; wdec[ii] && di + 1 < sizeof(dims); ii++) dims[di++] = wdec[ii];
					if (di + 1 < sizeof(dims)) dims[di++] = 'x';
					for (size_t ii = 0; hdec[ii] && di + 1 < sizeof(dims); ii++) dims[di++] = hdec[ii];
					if (di + 1 < sizeof(dims)) dims[di++] = ' ';
					dims[di] = 0;
					draw_text_u32(fb->pixels, fb->stride, label_x, row_y + 6u, dims, fmtc);
					uint32_t adv = (uint32_t)di * 8u;
					label_x += adv;
					label_w = (label_w > adv) ? (label_w - adv) : 0u;
				}
				draw_text_clipped_u32(fb->pixels, fb->stride, label_x, row_y + 6u, label_w, label, tc);

				/* Activate per-row rendering for the reserved blank lines that follow. */
				img_box.active = 1;
				img_box.rows_total = rows;
				img_box.row_in_box = 1;
				continue;
			}
		}

		char drawline[1024];
		(void)c_strlcpy_s(drawline, sizeof(drawline), line);
		struct {
			uint32_t col;
			struct img_sniff_cache_entry *entry;
		} to_draw[32];
		uint32_t n_to_draw = 0;
		if (inline_imgs && inline_imgs->n > 0) {
			size_t line_len = c_strlen(line);
			for (uint32_t ii = 0; ii < inline_imgs->n; ii++) {
				const struct html_inline_img *im = &inline_imgs->imgs[ii];
				if (im->url[0] == 0) continue;
				if (im->start < start) continue;
				size_t rel = (size_t)(im->start - (uint32_t)start);
				if (rel + 5u > line_len) continue;
				/* Best-effort: ensure the placeholder token is present. */
				if (!(line[rel + 0] == '[' && line[rel + 1] == 'i' && line[rel + 2] == 'm' && line[rel + 3] == 'g' && line[rel + 4] == ']')) continue;
				(void)img_cache_get_or_mark_pending(active_host, im->url);
				struct img_sniff_cache_entry *e = img_cache_find_done(active_host, im->url);
				if (!e || !e->has_pixels) continue;
				/* Hide the textual token once pixels are available, preserving width. */
				for (size_t k = 0; k < 5u; k++) drawline[rel + k] = ' ';
				if (n_to_draw < (uint32_t)(sizeof(to_draw) / sizeof(to_draw[0]))) {
					to_draw[n_to_draw].col = (uint32_t)rel;
					to_draw[n_to_draw].entry = e;
					n_to_draw++;
				}
			}
		}

		draw_line_with_links(fb, x, row_y, drawline, start, links, spans, tc, linkc);

		/* Draw inline icons after text so they are on top. */
		for (uint32_t di = 0; di < n_to_draw; di++) {
			struct img_sniff_cache_entry *e = to_draw[di].entry;
			if (!e || !e->has_pixels) continue;
			uint32_t token_x = x + to_draw[di].col * 8u;
			uint32_t token_w = 5u * 8u;
			uint32_t w = (uint32_t)e->pix_w;
			uint32_t h = (uint32_t)e->pix_h;
			/* Fit into a single text row (16px). Clip rather than scale for now. */
			if (w > 16u) w = 16u;
			if (h > 16u) h = 16u;
			uint32_t dx = token_x + ((token_w > w) ? ((token_w - w) / 2u) : 0u);
			uint32_t dy = row_y + ((16u > h) ? ((16u - h) / 2u) : 0u);
			const uint32_t *src = &g_img_pixel_pool[e->pix_off];
			blit_xrgb_clipped(fb, dx, dy, w, h, src, e->pix_w, e->pix_h);
		}

		if (float_box.active) {
			float_box.row_in_box++;
			if (float_box.row_in_box >= float_box.rows_total) {
				float_box.active = 0;
			}
		}
	}
}


static void render_page(struct shm_fb *fb,
				const char *active_host,
				const char *url_bar,
				const char *status_bar,
				const char *visible_text,
				const struct html_links *links,
				uint32_t scroll_rows)
{
	struct text_color dim = { .fg = 0xffb0b0b0u, .bg = 0, .opaque_bg = 0 };
	struct text_color linkc = { .fg = 0xff6aa8ffu, .bg = 0, .opaque_bg = 0 };
	draw_ui(fb, active_host, url_bar, status_bar, "", "", "");
	fill_rect_u32(fb->pixels, fb->stride, 0, UI_TOPBAR_H, fb->width, (fb->height > UI_TOPBAR_H) ? (fb->height - UI_TOPBAR_H) : 0, 0xff101014u);
	uint32_t w_px = (fb->width > 16) ? (fb->width - 16) : 0;
	uint32_t y0 = UI_CONTENT_Y0;
	uint32_t h_px = (fb->height > y0) ? (fb->height - y0) : 0;
	if (!visible_text) visible_text = "";
	draw_body_wrapped(fb, 8, y0, w_px, h_px, visible_text, links, &g_spans, &g_inline_imgs, dim, linkc, scroll_rows, active_host);
	fb->hdr->frame_counter++;
}

static int text_layout_index_for_row_col_with_float_right(const char *text,
						  uint32_t max_cols,
						  uint32_t target_row,
						  uint32_t target_col,
						  size_t *out_index)
{
	if (!text || !out_index || max_cols == 0) return -1;
	struct {
		uint8_t active;
		uint32_t rows_total;
		uint32_t row_in_box;
		uint32_t cols_total;
	} float_box = {0};

	size_t pos = 0;
	char line[256];
	for (uint32_t row = 0; ; row++) {
		uint32_t use_cols = max_cols;
		uint32_t text_cols = max_cols;
		if (float_box.active) {
			uint32_t fcols = float_box.cols_total;
			if (max_cols > 12u) {
				if (fcols > max_cols - 10u) fcols = max_cols - 10u;
			} else {
				if (fcols > max_cols - 1u) fcols = max_cols - 1u;
			}
			text_cols = (max_cols > (fcols + 1u)) ? (max_cols - fcols - 1u) : 1u;
			if (text_cols < 1u) text_cols = 1u;
			use_cols = text_cols;
		}
		size_t start = 0;
		int r = text_layout_next_line_ex(text, &pos, use_cols, line, sizeof(line), &start);
		if (r != 0) return -1;

		/* Update float state from marker rows.
		 * For float markers, treat the next text line as the clickable content
		 * for this same visual row (matching the renderer).
		 */
		if (line[0] == (char)0x1e && line[1] == 'I' && line[2] == 'M' && line[3] == 'G' && line[4] == ' ') {
			uint32_t rows_total = 0;
			size_t p = 5;
			while (line[p] >= '0' && line[p] <= '9') {
				rows_total = rows_total * 10u + (uint32_t)(line[p] - '0');
				p++;
			}
			while (line[p] == ' ') p++;
			char *tok = &line[p];
			while (line[p] && line[p] != ' ' && (uint8_t)line[p] != 0x1f) p++;
			char tok_end = line[p];
			line[p] = 0;
			int is_float = (tok[0] == 'F' && tok[1] == 'R');
			uint32_t fcols = 0;
			if (is_float) {
				for (size_t k = 2; tok[k] >= '0' && tok[k] <= '9'; k++) {
					fcols = fcols * 10u + (uint32_t)(tok[k] - '0');
				}
			}
			line[p] = tok_end;
			if (is_float && fcols > 0 && rows_total >= 2u) {
				if (rows_total > 200u) rows_total = 200u;
				if (fcols < 6u) fcols = 6u;
				if (fcols > 80u) fcols = 80u;
				float_box.active = 1;
				float_box.rows_total = rows_total;
				float_box.row_in_box = 0;
				float_box.cols_total = fcols;

				/* Replace marker row with next text line for click mapping on this row. */
				uint32_t f2 = float_box.cols_total;
				uint32_t use_cols2 = max_cols;
				uint32_t text_cols2 = max_cols;
				if (max_cols > 12u) {
					if (f2 > max_cols - 10u) f2 = max_cols - 10u;
				} else {
					if (f2 > max_cols - 1u) f2 = max_cols - 1u;
				}
				text_cols2 = (max_cols > (f2 + 1u)) ? (max_cols - f2 - 1u) : 1u;
				if (text_cols2 < 1u) text_cols2 = 1u;
				use_cols2 = text_cols2;

				size_t start2 = 0;
				char line2[256];
				line2[0] = 0;
				int r2 = text_layout_next_line_ex(text, &pos, use_cols2, line2, sizeof(line2), &start2);
				if (row == target_row) {
					if (target_col >= text_cols2) return -1;
					if (r2 != 0) return -1;
					if (line2[0] == (char)0x1e) return -1;
					size_t len2 = c_strlen(line2);
					if (target_col >= len2) return -1;
					*out_index = start2 + (size_t)target_col;
					return 0;
				}

				/* Non-target row: treat it as rendered; proceed to float row counter update below. */
				line[0] = 0;
			}
		}

		if (row == target_row) {
			if (float_box.active && target_col >= text_cols) return -1;
			if (line[0] == (char)0x1e) return -1;
			size_t len = c_strlen(line);
			if (target_col >= len) return -1;
			*out_index = start + (size_t)target_col;
			return 0;
		}

		if (float_box.active) {
			float_box.row_in_box++;
			if (float_box.row_in_box >= float_box.rows_total) {
				float_box.active = 0;
			}
		}
	}
}

static int ui_try_link_click(uint32_t x,
			    uint32_t y,
			    uint32_t width,
			    const char *visible_text,
			    const struct html_links *links,
			    uint32_t scroll_rows,
			    char *out_href,
			    size_t out_href_len)
{
	if (!visible_text || !links || !out_href || out_href_len == 0) return 0;
	if (y < UI_CONTENT_Y0) return 0;
	if (x < 8) return 0;
	uint32_t w_px = (width > 16) ? (width - 16) : 0;
	uint32_t max_cols = (w_px / 8u);
	if (max_cols == 0) max_cols = 1;
	if (max_cols > 255) max_cols = 255;
	uint32_t row = (y - UI_CONTENT_Y0) / 16u;
	uint32_t col = (x - 8) / 8u;
	row += scroll_rows;
	size_t idx = 0;
	if (text_layout_index_for_row_col_with_float_right(visible_text, max_cols, row, col, &idx) != 0) return 0;
	if (idx > 0xffffffffu) return 0;
	const char *href = links_href_at_index(links, (uint32_t)idx);
	if (!href || href[0] == 0) return 0;
	/* copy */
	size_t o = 0;
	for (; href[o] && o + 1 < out_href_len; o++) out_href[o] = href[o];
	out_href[o] = 0;
	return 1;
}

enum ui_action {
	UI_NONE = 0,
	UI_GO_EN = 1,
	UI_GO_DE = 2,
	UI_RELOAD = 3,
};

static int ui_hit_rect(uint32_t x, uint32_t y, uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh)
{
	if (x < rx || x >= (rx + rw)) return 0;
	if (y < ry || y >= (ry + rh)) return 0;
	return 1;
}

static enum ui_action ui_action_from_click(const struct shm_fb *fb, uint32_t x, uint32_t y)
{
	struct ui_buttons b = ui_layout_buttons(fb);
	if (ui_hit_rect(x, y, b.en_x, b.en_y, b.en_w, b.en_h)) return UI_GO_EN;
	if (ui_hit_rect(x, y, b.de_x, b.de_y, b.de_w, b.de_h)) return UI_GO_DE;
	if (ui_hit_rect(x, y, b.reload_x, b.reload_y, b.reload_w, b.reload_h)) return UI_RELOAD;
	return UI_NONE;
}

static void compose_url_bar(char *out, size_t out_len, const char *host, const char *path)
{
	if (!out || out_len == 0) return;
	out[0] = 0;
	const char *pfx = "https://";
	size_t o = 0;
	for (size_t i = 0; pfx[i] && o + 1 < out_len; i++) out[o++] = pfx[i];
	for (size_t i = 0; host && host[i] && o + 1 < out_len; i++) out[o++] = host[i];
	if (!path || path[0] != '/') {
		if (o + 1 < out_len) out[o++] = '/';
		out[o] = 0;
		return;
	}
	for (size_t i = 0; path[i] && o + 1 < out_len; i++) out[o++] = path[i];
	out[o] = 0;
}

static void prefetch_page_images(const char *active_host, const char *visible_text, const struct html_inline_imgs *inline_imgs)
{
	if (!active_host || !active_host[0]) return;
	if (inline_imgs) {
		for (uint32_t i = 0; i < inline_imgs->n; i++) {
			const struct html_inline_img *im = &inline_imgs->imgs[i];
			if (!im->url[0]) continue;
			(void)img_cache_get_or_mark_pending(active_host, im->url);
		}
	}
	/* Best-effort: prefetch block images referenced by marker lines too. */
	if (!visible_text) return;
	for (size_t i = 0; visible_text[i] != 0; ) {
		size_t line_start = i;
		while (visible_text[i] != 0 && visible_text[i] != '\n') i++;
		size_t line_end = i;
		if (visible_text[i] == '\n') i++;
		if (line_end <= line_start) continue;
		if ((uint8_t)visible_text[line_start] != 0x1eu) continue;
		if (line_end - line_start < 8) continue;
		if (!(visible_text[line_start + 1] == 'I' && visible_text[line_start + 2] == 'M' && visible_text[line_start + 3] == 'G')) continue;
		for (size_t k = line_start; k < line_end; k++) {
			if ((uint8_t)visible_text[k] == 0x1fu) {
				const char *url = &visible_text[k + 1];
				if (url[0]) (void)img_cache_get_or_mark_pending(active_host, url);
				break;
			}
		}
	}
}

static void do_https_status(struct shm_fb *fb, char host[HOST_BUF_LEN], char path[PATH_BUF_LEN], char url_bar[URL_BUF_LEN])
{
	g_body_len = 0;
	uint64_t content_len = 0;
	char final_status[128];
	final_status[0] = 0;
	g_visible[0] = 0;
	g_body[0] = 0;
	g_status_bar[0] = 0;
	g_links.n = 0;
	g_spans.n = 0;
	g_inline_imgs.n = 0;
	g_scroll_rows = 0;
	g_have_page = 0;
	/* Keep the image cache across navigations for faster icon-heavy pages. */
	for (size_t i = 0; i < sizeof(g_img_sniff_cache) / sizeof(g_img_sniff_cache[0]); i++) {
		g_img_sniff_cache[i].want_pixels = 0;
	}

	for (int step = 0; step < 6; step++) {
		compose_url_bar(url_bar, URL_BUF_LEN, host, path);
				draw_ui(fb, host, url_bar, "", "Resolving AAAA via Google DNS v6 ...", host, path);
		fb->hdr->frame_counter++;

		uint8_t ip6[16];
		c_memset(ip6, 0, sizeof(ip6));
		int dns_ok = dns_resolve_aaaa_google(host, ip6);
		char ip_str[48];
		if (dns_ok == 0) {
			ip6_to_str(ip_str, ip6);
		} else {
			c_memcpy(ip_str, "<dns6 fail>", 12);
		}

		char line1[128];
		c_memset(line1, 0, sizeof(line1));
		const char *pfx = "DNS AAAA = ";
		size_t o = 0;
		for (size_t i = 0; pfx[i] && o + 1 < sizeof(line1); i++) line1[o++] = pfx[i];
		for (size_t i = 0; ip_str[i] && o + 1 < sizeof(line1); i++) line1[o++] = ip_str[i];
		line1[o] = 0;

		compose_url_bar(url_bar, URL_BUF_LEN, host, path);
		draw_ui(fb, host, url_bar, "", line1, "Connecting IPv6 to :443 ...", path);
		fb->hdr->frame_counter++;

		int sock = -1;
		if (dns_ok == 0) {
			sock = tcp6_connect(ip6, 443);
		}

		const char *line2 = (sock >= 0) ? "TCP connect OK (TLS 1.3 handshake ...)" : "TCP connect FAILED";
		compose_url_bar(url_bar, URL_BUF_LEN, host, path);
		draw_ui(fb, host, url_bar, "", line1, line2, "Sending ClientHello + SNI + HTTP/1.1 GET");
		fb->hdr->frame_counter++;

		if (sock < 0) {
			compose_url_bar(url_bar, URL_BUF_LEN, host, path);
			draw_ui(fb, host, url_bar, "", line1, "TLS+HTTP FAILED", "(connect)");
			fb->hdr->frame_counter++;
			break;
		}

		char status[128];
		char location[512];
		int status_code = -1;
		int rc = tls13_https_get_status_location_and_body(sock,
									 host,
									 path,
									 status,
									 sizeof(status),
									 &status_code,
									 location,
									 sizeof(location),
									 g_body,
									 sizeof(g_body),
									 &g_body_len,
									 &content_len);
		sys_close(sock);

		if (rc != 0) {
			compose_url_bar(url_bar, URL_BUF_LEN, host, path);
			draw_ui(fb, host, url_bar, "", line1, "TLS+HTTP FAILED", "(no cert validation yet; handshake bring-up)");
			fb->hdr->frame_counter++;
			break;
		}

		dbg_write("http: ");
		dbg_write(status);
		dbg_write("\n");
		if (location[0]) {
			dbg_write("loc: ");
			dbg_write(location);
			dbg_write("\n");
		}

		compose_url_bar(url_bar, URL_BUF_LEN, host, path);
		/* During redirects, keep status in the top bar instead of body. */
		draw_ui(fb, host, url_bar, status, line1, (location[0] ? "Redirecting..." : ""), (location[0] ? location : path));
		/* If this is not a redirect, keep the body we just fetched. */
		(void)c_strlcpy_s(final_status, sizeof(final_status), status);
		(void)c_strlcpy_s(g_status_bar, sizeof(g_status_bar), status);
		fb->hdr->frame_counter++;

		int is_redirect = (status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 || status_code == 308);
		if (!is_redirect || location[0] == 0) {
			break;
		}

		char new_host[HOST_BUF_LEN];
		char new_path[PATH_BUF_LEN];
		if (url_apply_location(host, location, new_host, sizeof(new_host), new_path, sizeof(new_path)) != 0) {
			break;
		}
		(void)c_strlcpy_s(host, HOST_BUF_LEN, new_host);
		(void)c_strlcpy_s(path, PATH_BUF_LEN, new_path);
	}
	compose_url_bar(url_bar, URL_BUF_LEN, host, path);

	/* Render extracted visible text (best-effort). */
	if (g_body_len > 0) {
		struct img_dim_ctx ctx = { .active_host = host };
		(void)html_visible_text_extract_links_spans_and_inline_imgs_ex(g_body,
							  g_body_len,
							  g_visible,
							  sizeof(g_visible),
							  &g_links,
							  &g_spans,
						  &g_inline_imgs,
							  html_img_dim_lookup,
							  &ctx);
	}
	(void)c_strlcpy_s(g_url_bar, sizeof(g_url_bar), url_bar);
	(void)c_strlcpy_s(g_active_host, sizeof(g_active_host), host);
	g_have_page = 1;
	prefetch_page_images(g_active_host, g_visible, &g_inline_imgs);
	(void)img_workers_pump(0);
	render_page(fb, host, url_bar, g_status_bar, g_visible, &g_links, g_scroll_rows);
}

int main(void)
{
	struct shm_fb fb;
	if (shm_fb_open(&fb, FB_W, FB_H) < 0) {
		return 1;
	}

	int crypto_ok = tls_crypto_selftest();
	if (!crypto_ok) {
		draw_ui(&fb, "", "", "", "CRYPTO SELFTEST: FAIL", "Refusing to continue.", "Run: make test");
		fb.hdr->frame_counter++;
		for (;;) {
			struct timespec req;
			req.tv_sec = 0;
			req.tv_nsec = 250 * 1000 * 1000;
			sys_nanosleep(&req, 0);
		}
	}

	img_workers_init();

	char host[HOST_BUF_LEN];
	char path[PATH_BUF_LEN];
	char url_bar[URL_BUF_LEN];
	(void)c_strlcpy_s(host, sizeof(host), "de.wikipedia.org");
	(void)c_strlcpy_s(path, sizeof(path), "/");
	compose_url_bar(url_bar, sizeof(url_bar), host, path);
	draw_ui(&fb, host, url_bar, "", "CRYPTO SELFTEST: OK", "Click EN / DE / Reload", host);
	fb.hdr->frame_counter++;

	do_https_status(&fb, host, path, url_bar);

	/* Idle loop: poll shm click events so UI is interactive. */
	struct timespec req;
	req.tv_sec = 0;
	req.tv_nsec = 10 * 1000 * 1000;
	uint64_t last_mouse_event = fb.hdr->mouse_event_counter;
	uint32_t idle_ticks = 0;
	for (;;) {
		int did_interact = 0;
		/* Mouse wheel scroll (shared header reserved fields). */
		static uint64_t last_wheel_counter = 0;
		uint64_t wc = cfb_wheel_counter(fb.hdr);
		if (wc != last_wheel_counter) {
			last_wheel_counter = wc;
			int32_t dy = cfb_wheel_delta_y(fb.hdr);
			did_interact = 1;
			uint32_t step = 12u;
			if (dy > 0) {
				uint32_t dec = (uint32_t)dy * step;
				g_scroll_rows = (g_scroll_rows > dec) ? (g_scroll_rows - dec) : 0u;
			} else if (dy < 0) {
				uint32_t inc = (uint32_t)(-dy) * step;
				g_scroll_rows += inc;
			}
			if (g_have_page) {
				render_page(&fb, g_active_host, g_url_bar, g_status_bar, g_visible, &g_links, g_scroll_rows);
			}
		}

		if (fb.hdr->version >= 2) {
			uint64_t cur = fb.hdr->mouse_event_counter;
			if (cur != last_mouse_event) {
				last_mouse_event = cur;
				if (fb.hdr->mouse_last_button == 1u && fb.hdr->mouse_last_state == 1u) {
					did_interact = 1;
					uint32_t x = fb.hdr->mouse_last_x;
					uint32_t y = fb.hdr->mouse_last_y;
						enum ui_action a = ui_action_from_click(&fb, x, y);
					if (a == UI_GO_EN) {
						(void)c_strlcpy_s(host, sizeof(host), "en.wikipedia.org");
						(void)c_strlcpy_s(path, sizeof(path), "/");
						g_scroll_rows = 0;
						compose_url_bar(url_bar, sizeof(url_bar), host, path);
							draw_ui(&fb, host, url_bar, "", "EN clicked", host, "Fetching ...");
						fb.hdr->frame_counter++;
						do_https_status(&fb, host, path, url_bar);
					} else if (a == UI_GO_DE) {
						(void)c_strlcpy_s(host, sizeof(host), "de.wikipedia.org");
						(void)c_strlcpy_s(path, sizeof(path), "/");
						g_scroll_rows = 0;
						compose_url_bar(url_bar, sizeof(url_bar), host, path);
							draw_ui(&fb, host, url_bar, "", "DE clicked", host, "Fetching ...");
						fb.hdr->frame_counter++;
						do_https_status(&fb, host, path, url_bar);
					} else if (a == UI_RELOAD) {
						g_scroll_rows = 0;
						compose_url_bar(url_bar, sizeof(url_bar), host, path);
							draw_ui(&fb, host, url_bar, "", "Reload clicked", host, "Fetching ...");
						fb.hdr->frame_counter++;
						do_https_status(&fb, host, path, url_bar);
						} else {
							/* Body link click */
							char href[HTML_HREF_MAX];
							href[0] = 0;
							if (g_have_page && ui_try_link_click(x, y, fb.width, g_visible, &g_links, g_scroll_rows, href, sizeof(href))) {
								char new_host[HOST_BUF_LEN];
								char new_path[PATH_BUF_LEN];
								if (url_apply_location(host, href, new_host, sizeof(new_host), new_path, sizeof(new_path)) == 0) {
									(void)c_strlcpy_s(host, sizeof(host), new_host);
									(void)c_strlcpy_s(path, sizeof(path), new_path);
									g_scroll_rows = 0;
									compose_url_bar(url_bar, sizeof(url_bar), host, path);
									draw_ui(&fb, host, url_bar, "", "Link clicked", href, "Fetching ...");
									fb.hdr->frame_counter++;
									do_https_status(&fb, host, path, url_bar);
								}
							}
					}
				}
			}
		}

		if (did_interact) {
			idle_ticks = 0;
		} else {
			idle_ticks++;
		}

		if (g_have_page) {
			int dims_changed = 0;
			if (img_workers_pump(&dims_changed)) {
				if (dims_changed && g_body_len > 0) {
					struct img_dim_ctx ctx = { .active_host = g_active_host };
					(void)html_visible_text_extract_links_spans_and_inline_imgs_ex(g_body,
									  g_body_len,
									  g_visible,
									  sizeof(g_visible),
									  &g_links,
									  &g_spans,
								  &g_inline_imgs,
									  html_img_dim_lookup,
									  &ctx);
					prefetch_page_images(g_active_host, g_visible, &g_inline_imgs);
				}
				render_page(&fb, g_active_host, g_url_bar, g_status_bar, g_visible, &g_links, g_scroll_rows);
			}
		}

		/* Large-image fallback: keep it slow to avoid stutter. */
		if (!did_interact && g_have_page && idle_ticks > 100u && (idle_ticks % 100u) == 0u) {
			if (img_decode_large_pump_one()) {
				render_page(&fb, g_active_host, g_url_bar, g_status_bar, g_visible, &g_links, g_scroll_rows);
			}
		}
		sys_nanosleep(&req, 0);
	}
	return 0;
}
