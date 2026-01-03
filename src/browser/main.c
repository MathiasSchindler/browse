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

static char g_visible[64 * 1024];
static char g_status_bar[128];
static char g_url_bar[URL_BUF_LEN];
static char g_active_host[HOST_BUF_LEN];
static struct html_links g_links;
static struct html_spans g_spans;
static uint32_t g_scroll_rows;
static int g_have_page;

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
	uint32_t bg_xrgb;
	uint8_t has_bg;
	uint8_t bold;
};

static struct link_style spans_style_at_index(const struct html_spans *spans, uint32_t idx, uint32_t default_fg_xrgb)
{
	struct link_style st;
	st.fg_xrgb = default_fg_xrgb;
	st.bg_xrgb = 0;
	st.has_bg = 0;
	st.bold = 0;
	if (!spans) return st;
	for (uint32_t i = 0; i < spans->n && i < HTML_MAX_SPANS; i++) {
		const struct html_span *sp = &spans->spans[i];
		if (idx >= sp->start && idx < sp->end) {
			if (sp->has_fg) st.fg_xrgb = sp->fg_xrgb;
			if (sp->has_bg) { st.has_bg = 1; st.bg_xrgb = sp->bg_xrgb; }
			st.bold = sp->bold;
			return st;
		}
	}
	return st;
}

static struct link_style links_style_at_index(const struct html_links *links, uint32_t idx, uint32_t default_fg_xrgb)
{
	struct link_style st;
	st.fg_xrgb = default_fg_xrgb;
	st.bg_xrgb = 0;
	st.has_bg = 0;
	st.bold = 0;
	if (!links) return st;
	for (uint32_t i = 0; i < links->n && i < HTML_MAX_LINKS; i++) {
		const struct html_link *l = &links->links[i];
		if (idx >= l->start && idx < l->end) {
			if (l->has_fg) st.fg_xrgb = l->fg_xrgb;
			if (l->has_bg) { st.has_bg = 1; st.bg_xrgb = l->bg_xrgb; }
			st.bold = l->bold;
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
	seg_st.bg_xrgb = 0;
	seg_st.has_bg = 0;
	seg_st.bold = 0;
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
		    (is_link && (st.fg_xrgb != seg_st.fg_xrgb || st.has_bg != seg_st.has_bg || st.bg_xrgb != seg_st.bg_xrgb || st.bold != seg_st.bold)) ||
		    si + 2 >= sizeof(seg)) {
			seg[si] = 0;
			struct text_color c = seg_is_link ? linkc : normal;
			c.fg = seg_st.fg_xrgb;
			if (seg_st.has_bg) {
				fill_rect_u32(fb->pixels, fb->stride, seg_x, y, (uint32_t)si * 8u, 16u, seg_st.bg_xrgb);
			}
			draw_text_u32(fb->pixels, fb->stride, seg_x, y, seg, c);
			if (seg_st.bold) {
				draw_text_u32(fb->pixels, fb->stride, seg_x + 1, y, seg, c);
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
		if (seg_st.has_bg) {
			fill_rect_u32(fb->pixels, fb->stride, seg_x, y, (uint32_t)si * 8u, 16u, seg_st.bg_xrgb);
		}
		draw_text_u32(fb->pixels, fb->stride, seg_x, y, seg, c);
		if (seg_st.bold) {
			draw_text_u32(fb->pixels, fb->stride, seg_x + 1, y, seg, c);
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
				     uint32_t scroll_rows)
{
	if (!fb || !text) return;
	uint32_t max_cols = (w_px / 8u);
	uint32_t max_rows = (h_px / 16u);
	if (max_cols == 0 || max_rows == 0) return;
	if (max_cols > 255) max_cols = 255;

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
		draw_line_with_links(fb, x, y + row * 16u, line, start, links, spans, tc, linkc);
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
	draw_body_wrapped(fb, 8, y0, w_px, h_px, visible_text, links, &g_spans, dim, linkc, scroll_rows);
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
	static uint8_t body[96 * 1024];
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
	for (;;) {
		/* Mouse wheel scroll (shared header reserved fields). */
		static uint64_t last_wheel_counter = 0;
		uint64_t wc = cfb_wheel_counter(fb.hdr);
		if (wc != last_wheel_counter) {
			last_wheel_counter = wc;
			int32_t dy = cfb_wheel_delta_y(fb.hdr);
			uint32_t step = 3u;
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
		sys_nanosleep(&req, 0);
	}
	return 0;
}
