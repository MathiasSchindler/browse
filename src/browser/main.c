#include "../core/text.h"
#include "fb_shm.h"
#include "net_dns.h"
#include "net_tcp.h"
#include "url.h"

#include "../tls/selftest.h"
#include "tls13_client.h"
#include "html_text.h"
#include "text_layout.h"

#define FB_W 1920u
#define FB_H 1080u

#define UI_TOPBAR_H 24u
#define UI_CONTENT_PAD_Y 2u
#define UI_CONTENT_Y0 (UI_TOPBAR_H + UI_CONTENT_PAD_Y)

enum {
	HOST_BUF_LEN = 128,
	PATH_BUF_LEN = 512,
	URL_BUF_LEN = 640,
};

static char g_visible[512 * 1024];
static char g_status_bar[128];
static char g_url_bar[URL_BUF_LEN];
static char g_active_host[HOST_BUF_LEN];
static struct html_links g_links;
static struct html_spans g_spans;
static uint32_t g_scroll_rows;
static int g_have_page;

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
	enum img_fmt fmt;
	uint32_t hash;
	char key[256]; /* usually "host|/path" (truncated) */
};

static struct img_sniff_cache_entry g_img_sniff_cache[32];

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
			return (e->state == 2) ? e->fmt : IMG_FMT_UNKNOWN;
		}
	}

	/* Not cached yet: mark pending. */
	for (size_t i = 0; i < sizeof(g_img_sniff_cache) / sizeof(g_img_sniff_cache[0]); i++) {
		struct img_sniff_cache_entry *e = &g_img_sniff_cache[i];
		if (!e->used) {
			e->used = 1;
			e->state = 1;
			e->fmt = IMG_FMT_UNKNOWN;
			e->hash = h;
			(void)c_strlcpy_s(e->key, sizeof(e->key), key);
			break;
		}
	}
	return IMG_FMT_UNKNOWN;
}

static int img_sniff_pump_one(void)
{
	for (size_t i = 0; i < sizeof(g_img_sniff_cache) / sizeof(g_img_sniff_cache[0]); i++) {
		struct img_sniff_cache_entry *e = &g_img_sniff_cache[i];
		if (!e->used || e->state != 1) continue;

		char host[HOST_BUF_LEN];
		char path[PATH_BUF_LEN];
		if (split_host_path_from_key(e->key, host, sizeof(host), path, sizeof(path)) != 0) {
			e->fmt = IMG_FMT_UNKNOWN;
			e->state = 2;
			return 1;
		}

		uint8_t prefix[96];
		size_t got = 0;
		if (https_get_prefix_follow_redirects(host, path, prefix, sizeof(prefix), &got) != 0) {
			e->fmt = IMG_FMT_UNKNOWN;
			e->state = 2;
			return 1;
		}
		e->fmt = img_fmt_from_sniff(prefix, got);
		e->state = 2;
		return 1;
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
		uint32_t rows_left;
	} img_box = {0};

	/* Skip scroll_rows lines by running the layout forward. */
	size_t pos = 0;
	char line[256];
	for (uint32_t s = 0; s < scroll_rows; s++) {
		int r = text_layout_next_line(text, &pos, max_cols, line, sizeof(line));
		if (r != 0) break;
	}

	for (uint32_t row = 0; row < max_rows; row++) {
		size_t start = 0;
		int r = text_layout_next_line_ex(text, &pos, max_cols, line, sizeof(line), &start);
		if (r != 0) return;
		uint32_t row_y = y + row * 16u;

		/* Image placeholder marker row: draw real lines in framebuffer.
		 * Marker format: 0x1e "IMG <rows> ? <label>" 0x1f "<url>".
		 * Format is sniffed from the first bytes of the URL.
		 */
		if (img_box.rows_left != 0) {
			img_box.rows_left--;
			continue;
		}
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
			/* Skip placeholder format token (renderer sniffs actual format). */
			while (line[p] && line[p] != ' ' && (uint8_t)line[p] != 0x1f) p++;
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
			const char *fmt = img_fmt_token(sniffed);
			if (rows < 2u) rows = 2u;
			if (rows > 10u) rows = 10u;

			/* Clip to remaining visible content area and framebuffer bounds. */
			uint32_t content_bottom = y + h_px;
			uint32_t remaining_h = (content_bottom > row_y) ? (content_bottom - row_y) : 0u;
			if (fb->height < content_bottom) content_bottom = fb->height;
			remaining_h = (content_bottom > row_y) ? (content_bottom - row_y) : 0u;
			uint32_t max_w = (fb->width > x) ? (fb->width - x) : 0u;
			if (remaining_h == 0u || max_w == 0u) {
				img_box.rows_left = (rows > 0) ? (rows - 1u) : 0u;
				continue;
			}

			uint32_t box_h = rows * 16u;
			if (box_h > remaining_h) box_h = remaining_h;
			uint32_t box_w = w_px;
			if (box_w > max_w) box_w = max_w;
			uint32_t stroke = 1u;
			uint32_t col = 0xff505058u;
			/* Top + bottom */
			fill_rect_u32(fb->pixels, fb->stride, x, row_y, box_w, stroke, col);
			if (box_h > 1u) {
				fill_rect_u32(fb->pixels, fb->stride, x, row_y + box_h - 1u, box_w, stroke, col);
			}
			/* Left + right */
			fill_rect_u32(fb->pixels, fb->stride, x, row_y, stroke, box_h, col);
			if (box_w > 1u) {
				fill_rect_u32(fb->pixels, fb->stride, x + box_w - 1u, row_y, stroke, box_h, col);
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
			draw_text_clipped_u32(fb->pixels, fb->stride, label_x, row_y + 6u, label_w, label, tc);
			img_box.rows_left = (rows > 0) ? (rows - 1u) : 0u;
			continue;
		}

		draw_line_with_links(fb, x, row_y, line, start, links, spans, tc, linkc);
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
	draw_body_wrapped(fb, 8, y0, w_px, h_px, visible_text, links, &g_spans, dim, linkc, scroll_rows, active_host);
	fb->hdr->frame_counter++;
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
	if (text_layout_index_for_row_col(visible_text, max_cols, row, col, &idx) != 0) return 0;
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

static void do_https_status(struct shm_fb *fb, char host[HOST_BUF_LEN], char path[PATH_BUF_LEN], char url_bar[URL_BUF_LEN])
{
	static uint8_t body[512 * 1024];
	size_t body_len = 0;
	uint64_t content_len = 0;
	char final_status[128];
	final_status[0] = 0;
	g_visible[0] = 0;
	body[0] = 0;
	g_status_bar[0] = 0;
	g_links.n = 0;
	g_spans.n = 0;
	g_scroll_rows = 0;
	g_have_page = 0;

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
									 body,
									 sizeof(body),
									 &body_len,
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
	if (body_len > 0) {
		(void)html_visible_text_extract_links_and_spans(body, body_len, g_visible, sizeof(g_visible), &g_links, &g_spans);
	}
	(void)c_strlcpy_s(g_url_bar, sizeof(g_url_bar), url_bar);
	(void)c_strlcpy_s(g_active_host, sizeof(g_active_host), host);
	g_have_page = 1;
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

	char host[128];
	char path[512];
	char url_bar[640];
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
			/* Opportunistically sniff at most one pending image when idle.
			 * This avoids stuttering while the user scrolls.
			 */
			if (g_have_page && idle_ticks > 25u && (idle_ticks % 20u) == 0u) {
				if (img_sniff_pump_one()) {
					render_page(&fb, g_active_host, g_url_bar, g_status_bar, g_visible, &g_links, g_scroll_rows);
				}
			}
		}
		sys_nanosleep(&req, 0);
	}
	return 0;
}
