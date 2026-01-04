#include "browser_img.h"

#include "net_dns.h"
#include "net_tcp.h"
#include "url.h"

#include "tls13_client.h"

#include "image/jpeg.h"
#include "image/jpeg_decode.h"
#include "image/png.h"
#include "image/png_decode.h"
#include "image/gif.h"
#include "image/gif_decode.h"

#include "../core/text.h"
#include "../core/log.h"

static int split_host_path_from_key(const char *key, char *host_out, size_t host_out_len, char *path_out, size_t path_out_len);

static inline void img__msg_append(char *buf, size_t cap, size_t *o, const char *s)
{
	if (!buf || cap == 0 || !o || !s) return;
	while (*s && *o + 1 < cap) {
		buf[(*o)++] = *s++;
	}
}

static inline void img__msg_append_u32_dec(char *buf, size_t cap, size_t *o, uint32_t v)
{
	char tmp[11];
	uint32_t n = 0;
	if (!buf || cap == 0 || !o) return;
	if (v == 0) {
		if (*o + 1 < cap) buf[(*o)++] = '0';
		return;
	}
	while (v > 0 && n < 10) {
		tmp[n++] = (char)('0' + (v % 10u));
		v /= 10u;
	}
	while (n > 0 && *o + 1 < cap) {
		buf[(*o)++] = tmp[--n];
	}
}

static inline void img__format_https_from_key(char *out, size_t out_cap, const char *key)
{
	if (!out || out_cap == 0) return;
	out[0] = 0;
	if (!key || !key[0]) return;
	char host[HOST_BUF_LEN];
	char path[PATH_BUF_LEN];
	if (split_host_path_from_key(key, host, sizeof(host), path, sizeof(path)) != 0) {
		/* Fallback: keep raw key if it isn't parseable. */
		(void)c_strlcpy_s(out, out_cap, key);
		return;
	}
	size_t o = 0;
	img__msg_append(out, out_cap, &o, "https://");
	img__msg_append(out, out_cap, &o, host);
	img__msg_append(out, out_cap, &o, path);
	if (o < out_cap) out[o] = 0;
}

static inline void img__format_https_from_host_path(char *out, size_t out_cap, const char *host, const char *path)
{
	if (!out || out_cap == 0) return;
	out[0] = 0;
	if (!host || !host[0] || !path || !path[0]) return;
	size_t o = 0;
	img__msg_append(out, out_cap, &o, "https://");
	img__msg_append(out, out_cap, &o, host);
	img__msg_append(out, out_cap, &o, path);
	if (o < out_cap) out[o] = 0;
}

static inline void img__log_key(enum log_level lvl, const char *what, const char *key)
{
	char url[768];
	img__format_https_from_key(url, sizeof(url), key);
	char msg[768];
	size_t o = 0;
	img__msg_append(msg, sizeof(msg), &o, what ? what : "img");
	img__msg_append(msg, sizeof(msg), &o, ": ");
	img__msg_append(msg, sizeof(msg), &o, url[0] ? url : (key ? key : "(null)"));
	if (o + 1 < sizeof(msg)) msg[o++] = '\n';
	if (lvl == LOG_LVL_ERROR) LOGE_BUF("img", msg, o);
	else if (lvl == LOG_LVL_WARN) LOGW_BUF("img", msg, o);
	else if (lvl == LOG_LVL_DEBUG) LOGD_BUF("img", msg, o);
	else LOGI_BUF("img", msg, o);
}

static inline void img__log_url(enum log_level lvl, const char *what, const char *url)
{
	char msg[768];
	size_t o = 0;
	img__msg_append(msg, sizeof(msg), &o, what ? what : "img");
	img__msg_append(msg, sizeof(msg), &o, ": ");
	img__msg_append(msg, sizeof(msg), &o, url ? url : "(null)");
	if (o + 1 < sizeof(msg)) msg[o++] = '\n';
	if (lvl == LOG_LVL_ERROR) LOGE_BUF("img", msg, o);
	else if (lvl == LOG_LVL_WARN) LOGW_BUF("img", msg, o);
	else if (lvl == LOG_LVL_DEBUG) LOGD_BUF("img", msg, o);
	else LOGI_BUF("img", msg, o);
}

static uint32_t g_img_use_tick;
static uint32_t g_img_generation;

/* Keep enough entries for Wikipedia-scale pages (lots of icons/thumbs). */
static struct img_sniff_cache_entry g_img_sniff_cache[1024];

static uint8_t g_img_fetch_buf[1024 * 1024];

/* Extremely small pixel pool for the first real decoders.
 * Bump-allocated; blocks can be freed best-effort.
 */
/* Pixel pool is the main limiter for “image-heavy” pages (Wikipedia).
 * Keep it modest but large enough to avoid constant evictions/fragmentation.
 */
static uint32_t g_img_pixel_pool[2 * 1024 * 1024]; /* 2M px = 8 MiB */
static uint32_t g_img_pixel_pool_used;

struct img_pix_free_block {
	uint32_t off;
	uint32_t len;
};

static struct img_pix_free_block g_img_pix_free[128];
static uint32_t g_img_pix_free_n;

static void img_pixel_try_shrink_tail(void)
{
	/* If we freed a block at the end of the bump-allocated pool, shrink the
	 * high-water mark. This reduces fragmentation and makes large allocations
	 * more likely to succeed.
	 */
	uint32_t changed;
	do {
		changed = 0;
		for (uint32_t i = 0; i < g_img_pix_free_n; i++) {
			struct img_pix_free_block *b = &g_img_pix_free[i];
			if (b->len == 0) continue;
			if (b->off + b->len == g_img_pixel_pool_used) {
				g_img_pixel_pool_used -= b->len;
				g_img_pix_free[i] = g_img_pix_free[g_img_pix_free_n - 1u];
				g_img_pix_free_n--;
				changed = 1;
				break;
			}
		}
	} while (changed);
}

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

	img_pixel_try_shrink_tail();
}

static int img_cache_evict_one_min_len(uint32_t min_pix_len)
{
	uint32_t best_i = 0xffffffffu;
	uint32_t best_use = 0xffffffffu;
	for (uint32_t i = 0; i < (uint32_t)(sizeof(g_img_sniff_cache) / sizeof(g_img_sniff_cache[0])); i++) {
		struct img_sniff_cache_entry *e = &g_img_sniff_cache[i];
		if (!e->used) continue;
		if (e->state != 2) continue;
		if (e->inflight) continue;
		if (!e->has_pixels || e->pix_len == 0) continue;
		if (e->pix_len < min_pix_len) continue;
		if (e->last_use < best_use) {
			best_use = e->last_use;
			best_i = i;
		}
	}
	if (best_i == 0xffffffffu) return -1;
	struct img_sniff_cache_entry *e = &g_img_sniff_cache[best_i];
	img_pixel_free(e->pix_off, e->pix_len);
	e->used = 0;
	e->state = 0;
	e->inflight = 0;
	e->want_pixels = 0;
	e->gen = 0;
	e->fetch_failures = 0;
	e->pix_failures = 0;
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

static int img_pixel_alloc(uint32_t n_pixels, uint32_t *out_off)
{
	if (!out_off) return -1;
	*out_off = 0;
	if (n_pixels == 0) return -1;
	if (n_pixels > (uint32_t)(sizeof(g_img_pixel_pool) / sizeof(g_img_pixel_pool[0]))) return -1;
	uint32_t cap = (uint32_t)(sizeof(g_img_pixel_pool) / sizeof(g_img_pixel_pool[0]));
	if (g_img_pixel_pool_used > cap) g_img_pixel_pool_used = cap;

	/* Best-fit from free list (minimizes fragmentation). */
	uint32_t best_i = 0xffffffffu;
	uint32_t best_len = 0xffffffffu;
	for (uint32_t i = 0; i < g_img_pix_free_n; i++) {
		struct img_pix_free_block *b = &g_img_pix_free[i];
		if (b->len < n_pixels) continue;
		if (b->len < best_len) {
			best_len = b->len;
			best_i = i;
		}
	}
	if (best_i != 0xffffffffu) {
		struct img_pix_free_block *b = &g_img_pix_free[best_i];
		*out_off = b->off;
		b->off += n_pixels;
		b->len -= n_pixels;
		if (b->len == 0) {
			g_img_pix_free[best_i] = g_img_pix_free[g_img_pix_free_n - 1u];
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
	e->gen = 0;
	e->fetch_failures = 0;
	e->pix_failures = 0;
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
	e->gen = 0;
	e->fetch_failures = 0;
	e->pix_failures = 0;
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

const char *img_fmt_token(enum img_fmt fmt)
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

struct img_sniff_cache_entry *img_cache_find_done(const char *active_host, const char *url)
{
	if (!active_host || !active_host[0] || !url || !url[0]) return 0;
	char host[HOST_BUF_LEN];
	char path[PATH_BUF_LEN];
	if (url_apply_location(active_host, url, host, sizeof(host), path, sizeof(path)) != 0) return 0;

	char key[512];
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

int browser_html_img_dim_lookup(void *ctx, const char *url, uint32_t *out_w, uint32_t *out_h)
{
	if (!ctx || !url || !out_w || !out_h) return -1;
	struct img_dim_ctx *c = (struct img_dim_ctx *)ctx;
	struct img_sniff_cache_entry *e = img_cache_find_done(c->active_host ? c->active_host : "", url);
	if (!e || !e->has_dims) return -1;
	*out_w = (uint32_t)e->w;
	*out_h = (uint32_t)e->h;
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
		if (dns_resolve_aaaa_google(host, ip6) != 0) {
			char url[768];
			img__format_https_from_host_path(url, sizeof(url), host, path);
			img__log_url(LOG_LVL_WARN, "dns AAAA failed", url);
			return -1;
		}
		int sock = tcp6_connect(ip6, 443);
		if (sock < 0) {
			char url[768];
			img__format_https_from_host_path(url, sizeof(url), host, path);
			img__log_url(LOG_LVL_WARN, "tcp connect failed", url);
			return -1;
		}

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
						 0,
						 0,
						 0,
						 0,
						 out,
						 out_cap,
						 &body_len,
						 &content_len);
		sys_close(sock);
		if (rc != 0) {
			char url[768];
			img__format_https_from_host_path(url, sizeof(url), host, path);
			img__log_url(LOG_LVL_WARN, "https get failed", url);
			return -1;
		}

		int is_redirect = (status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 || status_code == 308);
		if (!is_redirect || location[0] == 0) {
			*out_len = (body_len > out_cap) ? out_cap : (size_t)body_len;
			return 0;
		}
		if (LOG_LEVEL >= 3) {
			char url[768];
			img__format_https_from_host_path(url, sizeof(url), host, path);
			img__log_url(LOG_LVL_DEBUG, "redirect", url);
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

	int sock = -1;

	/* Prefer IPv6, but fall back to IPv4 (many networks are IPv4-only). */
	{
		uint8_t ip6[16];
		c_memset(ip6, 0, sizeof(ip6));
		if (dns_resolve_aaaa_google(host, ip6) == 0) {
			sock = tcp6_connect(ip6, 443);
		}
	}
	if (sock < 0) {
		uint8_t ip4[4];
		c_memset(ip4, 0, sizeof(ip4));
		if (dns_resolve_a_google4(host, ip4) != 0) return -1;
		sock = tcp4_connect(ip4, 443);
		if (sock < 0) return -1;
	}

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
			if (https_conn_open_host(c, host) != 0) {
				char url[768];
				img__format_https_from_host_path(url, sizeof(url), host, path);
				img__log_url(LOG_LVL_WARN, "conn open failed", url);
				return -1;
			}
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
								 0,
								 0,
								 0,
								 0,
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
		if (rc != 0) {
			char url[768];
			img__format_https_from_host_path(url, sizeof(url), host, path);
			img__log_url(LOG_LVL_WARN, "https get failed", url);
			return -1;
		}

		int is_redirect = (status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 || status_code == 308);
		if (!is_redirect || location[0] == 0) {
			*out_len = (body_len > out_cap) ? out_cap : (size_t)body_len;
			return 0;
		}
		if (LOG_LEVEL >= 3) {
			char url[768];
			img__format_https_from_host_path(url, sizeof(url), host, path);
			img__log_url(LOG_LVL_DEBUG, "redirect", url);
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

enum img_fmt img_cache_get_or_mark_pending(const char *active_host, const char *url)
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

	char key[512];
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
			e->gen = g_img_generation;
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
		slot->gen = g_img_generation;
		slot->fetch_failures = 0;
		slot->pix_failures = 0;
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
	char key[512];
	uint32_t gen;
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
static int32_t g_img_worker_pids[IMG_WORKERS];

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
			img__log_key(LOG_LVL_ERROR, "bad key", w->key);
			w->rc = -1;
			w->state = 2u;
			continue;
		}

		uint8_t sniff_buf[4096];
		size_t got = 0;
		if (https_get_prefix_follow_redirects_keepalive(&conn, host, path, sniff_buf, sizeof(sniff_buf), &got) != 0 || got == 0) {
			img__log_key(LOG_LVL_WARN, "sniff fetch failed", w->key);
			w->rc = -1;
			w->state = 2u;
			continue;
		}
		/* Sniff succeeded. */
		w->rc = 0;
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
					/* Defensive: ensure any partially-written decode can't leak old pixels (e.g. from a previous PNG
					 * with a checkerboard transparency background).
					 */
					for (uint32_t i = 0; i < (uint32_t)IMG_WORKER_MAX_PX; i++) w->pixels[i] = 0xff000000u;
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
						/* Decode failure of tiny image: parent will decide on retries. */
						w->rc = -2;
					}
				} else {
					w->rc = -1;
				}
			}
		}
		w->state = 2u;
	}
}

void img_workers_init(void)
{
	if (g_img_workers) return;
	for (uint32_t i = 0; i < IMG_WORKERS; i++) g_img_worker_pids[i] = -1;
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
		g_img_workers[i].gen = 0;
	}
	for (uint32_t i = 0; i < IMG_WORKERS; i++) {
		int pid = sys_fork();
		if (pid == 0) {
			img_worker_loop(i);
			sys_exit(0);
		}
		/* Parent: if fork failed, just leave fewer workers. */
		if (pid > 0) g_img_worker_pids[i] = (int32_t)pid;
		if (pid < 0) break;
	}
}

void img_workers_cancel_all(void)
{
	if (!g_img_workers) return;
	for (uint32_t i = 0; i < IMG_WORKERS; i++) {
		int32_t pid = g_img_worker_pids[i];
		if (pid > 0) (void)sys_kill((int)pid, SIGKILL);
	}
	/* Best-effort reap to avoid zombies. */
	for (uint32_t i = 0; i < IMG_WORKERS; i++) {
		int32_t pid = g_img_worker_pids[i];
		if (pid <= 0) continue;
		for (int tries = 0; tries < 64; tries++) {
			int st = 0;
			int r = sys_wait4((int)pid, &st, WNOHANG, 0);
			if (r == pid) break;
			struct timespec req;
			req.tv_sec = 0;
			req.tv_nsec = 1 * 1000 * 1000;
			(void)sys_nanosleep(&req, 0);
		}
		g_img_worker_pids[i] = -1;
	}

	(void)sys_munmap((void *)g_img_workers, sizeof(struct img_worker_shm) * (size_t)IMG_WORKERS);
	g_img_workers = 0;

	/* Ensure cache doesn't wedge on stale inflight flags. */
	for (size_t i = 0; i < sizeof(g_img_sniff_cache) / sizeof(g_img_sniff_cache[0]); i++) {
		g_img_sniff_cache[i].inflight = 0;
	}
}

int img_workers_pump(int *out_any_dims_changed, int *out_any_pixels_changed)
{
	if (out_any_dims_changed) *out_any_dims_changed = 0;
	if (out_any_pixels_changed) *out_any_pixels_changed = 0;
	if (!g_img_workers) return 0;
	int did_relevant_change = 0;

	/* Collect completed workers. */
	for (uint32_t wi = 0; wi < IMG_WORKERS; wi++) {
		struct img_worker_shm *w = &g_img_workers[wi];
		if (w->state != 2u) continue;
		struct img_sniff_cache_entry *e = img_cache_find_by_key(w->key);
		if (e) {
			int is_current = (e->gen == g_img_generation) && e->want_pixels;
			if (w->rc != 0) {
				/* Transient failure: keep pending so we can retry a few times. */
				e->inflight = 0;
				if (e->fetch_failures < 0xffu) e->fetch_failures++;
				if (e->fetch_failures >= 3u) {
					img__log_key(LOG_LVL_WARN, "fetch failed (giving up)", e->key);
					e->want_pixels = 0;
					e->state = 2;
				} else {
					if (LOG_LEVEL >= 3) img__log_key(LOG_LVL_DEBUG, "fetch failed (retrying)", e->key);
					e->state = 1;
				}
				w->state = 0u;
				continue;
			}
			e->fetch_failures = 0;

			uint8_t had_dims = e->has_dims;
			e->fmt = (enum img_fmt)w->fmt;
			e->has_dims = (uint8_t)(w->has_dims != 0);
			e->w = w->w;
			e->h = w->h;
			if (is_current && out_any_dims_changed && e->has_dims && !had_dims) *out_any_dims_changed = 1;

			if (is_current && w->has_pixels && w->pix_len != 0) {
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
				for (int tries = 0; alloc_ok != 0 && tries < 32; tries++) {
					/* First try to evict something that can satisfy this allocation.
					 * This avoids evicting many tiny entries and still failing due to fragmentation.
					 */
					if (img_cache_evict_one_min_len(w->pix_len) != 0) {
						if (img_cache_evict_one() != 0) break;
					}
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
					e->pix_failures = 0;
					if (out_any_pixels_changed) *out_any_pixels_changed = 1;
				}
			} else {
				/* If this was a small image we tried to decode but got no pixels, allow a few retries. */
				if (is_current && e->has_dims && !e->has_pixels && e->want_pixels &&
				    e->w > 0 && e->h > 0 && e->w <= IMG_WORKER_MAX_W && e->h <= IMG_WORKER_MAX_H &&
				    (e->fmt == IMG_FMT_PNG || e->fmt == IMG_FMT_JPG || e->fmt == IMG_FMT_GIF)) {
					if (e->pix_failures < 0xffu) e->pix_failures++;
					if (e->pix_failures >= 3u) {
						img__log_key(LOG_LVL_WARN, "decode failed (giving up)", e->key);
						e->want_pixels = 0;
					} else {
						if (LOG_LEVEL >= 3) img__log_key(LOG_LVL_DEBUG, "decode failed (retrying)", e->key);
					}
				}
				/* Unsupported formats: don't keep burning worker cycles. */
				if (e->fmt == IMG_FMT_WEBP || e->fmt == IMG_FMT_SVG) {
					img__log_key(LOG_LVL_WARN, "unsupported format (skipping)", e->key);
					e->want_pixels = 0;
				}
			}
			e->state = 2;
			e->inflight = 0;
			e->last_use = ++g_img_use_tick;
			if (is_current) did_relevant_change = 1;
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
			if (!e->want_pixels) continue;
			if (e->gen != g_img_generation) continue;
			int eligible = 0;
			if (e->state == 1) {
				eligible = 1;
			} else if (e->state == 2 && e->want_pixels && !e->has_pixels && e->has_dims &&
					   e->w > 0 && e->h > 0 && e->w <= IMG_WORKER_MAX_W && e->h <= IMG_WORKER_MAX_H &&
					   (e->fmt == IMG_FMT_PNG || e->fmt == IMG_FMT_JPG || e->fmt == IMG_FMT_GIF) &&
					   e->pix_failures < 3u) {
				eligible = 1;
			}
			if (!eligible) continue;
			if (e->inflight) continue;
			if (!pick || e->last_use > best_use) {
				pick = e;
				best_use = e->last_use;
			}
		}
		if (!pick) break;
		pick->inflight = 1;
		(void)c_strlcpy_s(w->key, sizeof(w->key), pick->key);
		w->gen = pick->gen;
		w->state = 1u;
		w->rc = 0;
	}

	return did_relevant_change;
}

int img_decode_large_pump_one(void)
{
	for (size_t i = 0; i < sizeof(g_img_sniff_cache) / sizeof(g_img_sniff_cache[0]); i++) {
		struct img_sniff_cache_entry *e = &g_img_sniff_cache[i];
		if (!e->used) continue;
		if (e->state != 2) continue;
		if (!e->want_pixels) continue;
		if (e->gen != g_img_generation) continue;
		if (e->inflight) continue;
		if (e->has_pixels) continue;
		if (!e->has_dims) continue;
		if (e->w <= IMG_WORKER_MAX_W && e->h <= IMG_WORKER_MAX_H) continue;
		if (e->w > 512u || e->h > 512u) continue;
		if (!(e->fmt == IMG_FMT_JPG || e->fmt == IMG_FMT_PNG || e->fmt == IMG_FMT_GIF)) continue;

		char host[HOST_BUF_LEN];
		char path[PATH_BUF_LEN];
		if (split_host_path_from_key(e->key, host, sizeof(host), path, sizeof(path)) != 0) {
			img__log_key(LOG_LVL_ERROR, "bad key", e->key);
			e->want_pixels = 0;
			return 0;
		}

		uint32_t px = (uint32_t)e->w * (uint32_t)e->h;
		if (px == 0 || px > (uint32_t)(sizeof(g_img_pixel_pool) / sizeof(g_img_pixel_pool[0]))) {
			img__log_key(LOG_LVL_WARN, "bad dimensions", e->key);
			e->want_pixels = 0;
			return 0;
		}

		size_t got_full = 0;
		if (https_get_prefix_follow_redirects(host, path, g_img_fetch_buf, sizeof(g_img_fetch_buf), &got_full) != 0 || got_full == 0) {
			img__log_key(LOG_LVL_WARN, "fetch failed", e->key);
			e->want_pixels = 0;
			return 0;
		}

		uint32_t old_used = g_img_pixel_pool_used;
		uint32_t off = 0;
		if (img_pixel_alloc(px, &off) != 0) {
			/* Try to free some space by evicting old decoded entries, similar to worker path. */
			int alloc_ok = -1;
			for (int tries = 0; tries < 64; tries++) {
				if (img_cache_evict_one_min_len(px) != 0) {
					if (img_cache_evict_one() != 0) break;
				}
				if (img_pixel_alloc(px, &off) == 0) { alloc_ok = 0; break; }
			}
			if (alloc_ok != 0) {
				if (e->pix_failures < 0xffu) e->pix_failures++;
				if (e->pix_failures == 1u) {
					char msg[256];
					size_t o = 0;
					img__msg_append(msg, sizeof(msg), &o, "pixel alloc failed (px=");
					img__msg_append_u32_dec(msg, sizeof(msg), &o, px);
					img__msg_append(msg, sizeof(msg), &o, ", pool_used=");
					img__msg_append_u32_dec(msg, sizeof(msg), &o, g_img_pixel_pool_used);
					img__msg_append(msg, sizeof(msg), &o, ", pool_cap=");
					img__msg_append_u32_dec(msg, sizeof(msg), &o, (uint32_t)(sizeof(g_img_pixel_pool) / sizeof(g_img_pixel_pool[0])));
					img__msg_append(msg, sizeof(msg), &o, ")");
					img__log_key(LOG_LVL_WARN, msg, e->key);
				}
				/* Stop burning cycles until the UI asks again for this image. */
				e->want_pixels = 0;
				return 0;
			}
		}
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
			if (e->fmt == IMG_FMT_JPG && ok == -2) img__log_key(LOG_LVL_WARN, "unsupported jpeg (non-baseline/progressive)", e->key);
			else if (e->fmt == IMG_FMT_PNG && ok == -2) img__log_key(LOG_LVL_WARN, "unsupported png (bit depth / interlace)", e->key);
			else img__log_key(LOG_LVL_WARN, "decode failed", e->key);
			e->want_pixels = 0;
		}
		return 0;
	}
	return 0;
}

const uint32_t *img_entry_pixels(const struct img_sniff_cache_entry *e)
{
	if (!e || !e->has_pixels || e->pix_len == 0) return 0;
	uint32_t cap = (uint32_t)(sizeof(g_img_pixel_pool) / sizeof(g_img_pixel_pool[0]));
	if (e->pix_off >= cap) return 0;
	if (e->pix_off + e->pix_len > cap) return 0;
	return &g_img_pixel_pool[e->pix_off];
}

void img_cache_clear_want_pixels(void)
{
	for (size_t i = 0; i < sizeof(g_img_sniff_cache) / sizeof(g_img_sniff_cache[0]); i++) {
		g_img_sniff_cache[i].want_pixels = 0;
	}
}

void img_cache_begin_new_page(void)
{
	/* Move to a new generation so stale in-flight work won't be dispatched/rendered. */
	g_img_generation++;
	img_cache_clear_want_pixels();
}

void prefetch_page_images(const char *active_host, const char *visible_text, const struct html_inline_imgs *inline_imgs)
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
