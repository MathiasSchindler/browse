#include "../core/text.h"
#include "fb_shm.h"
#include "net_dns.h"
#include "net_tcp.h"
#include "url.h"

#include "../tls/selftest.h"
#include "tls13_client.h"

#define FB_W 1920u
#define FB_H 1080u

enum {
	HOST_BUF_LEN = 128,
	PATH_BUF_LEN = 512,
	URL_BUF_LEN = 640,
};

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

static void draw_ui(struct shm_fb *fb, const char *active_host, const char *url_bar, const char *line1, const char *line2, const char *line3)
{
	/* Background */
	fill_rect_u32(fb->pixels, fb->stride, 0, 0, fb->width, fb->height, 0xff101014u);

	/* Title bar */
	fill_rect_u32(fb->pixels, fb->stride, 0, 0, fb->width, 24, 0xff202028u);
	struct text_color tc = { .fg = 0xffffffffu, .bg = 0, .opaque_bg = 0 };
	struct text_color dim = { .fg = 0xffb0b0b0u, .bg = 0, .opaque_bg = 0 };

	draw_text_u32(fb->pixels, fb->stride, 8, 8, "BROWSER (SYS) - NET/TLS WIP", tc);

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

	/* Current/final URL (clipped to available space). */
	uint32_t url_x = 8 + 8 * 24; /* after title */
	uint32_t url_right = (b.en_x > 8) ? (b.en_x - 8) : 0;
	uint32_t url_w = (url_right > url_x) ? (url_right - url_x) : 0;
	draw_text_clipped_u32(fb->pixels, fb->stride, url_x, 8, url_w, url_bar, dim);

	/* Body */
	fill_rect_u32(fb->pixels, fb->stride, 0, 24, fb->width, fb->height - 24, 0xff101014u);

	draw_text_u32(fb->pixels, fb->stride, 8, 40, line1, tc);
	draw_text_u32(fb->pixels, fb->stride, 8, 56, line2, tc);
	draw_text_u32(fb->pixels, fb->stride, 8, 72, line3, dim);
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
	for (int step = 0; step < 6; step++) {
		compose_url_bar(url_bar, URL_BUF_LEN, host, path);
		draw_ui(fb, host, url_bar, "Resolving AAAA via Google DNS v6 ...", host, path);
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
		draw_ui(fb, host, url_bar, line1, "Connecting IPv6 to :443 ...", path);
		fb->hdr->frame_counter++;

		int sock = -1;
		if (dns_ok == 0) {
			sock = tcp6_connect(ip6, 443);
		}

		const char *line2 = (sock >= 0) ? "TCP connect OK (TLS 1.3 handshake ...)" : "TCP connect FAILED";
		compose_url_bar(url_bar, URL_BUF_LEN, host, path);
		draw_ui(fb, host, url_bar, line1, line2, "Sending ClientHello + SNI + HTTP/1.1 GET");
		fb->hdr->frame_counter++;

		if (sock < 0) {
			compose_url_bar(url_bar, URL_BUF_LEN, host, path);
			draw_ui(fb, host, url_bar, line1, "TLS+HTTP FAILED", "(connect)");
			fb->hdr->frame_counter++;
			break;
		}

		char status[128];
		char location[512];
		int status_code = -1;
		int rc = tls13_https_get_status_and_location(sock, host, path, status, sizeof(status), &status_code, location, sizeof(location));
		sys_close(sock);

		if (rc != 0) {
			compose_url_bar(url_bar, URL_BUF_LEN, host, path);
			draw_ui(fb, host, url_bar, line1, "TLS+HTTP FAILED", "(no cert validation yet; handshake bring-up)");
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
		draw_ui(fb, host, url_bar, line1, status, (location[0] ? location : path));
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
}

int main(void)
{
	struct shm_fb fb;
	if (shm_fb_open(&fb, FB_W, FB_H) < 0) {
		return 1;
	}

	int crypto_ok = tls_crypto_selftest();
	if (!crypto_ok) {
		draw_ui(&fb, "", "", "CRYPTO SELFTEST: FAIL", "Refusing to continue.", "Run: make test");
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
	draw_ui(&fb, host, url_bar, "CRYPTO SELFTEST: OK", "Click EN / DE / Reload", host);
	fb.hdr->frame_counter++;

	do_https_status(&fb, host, path, url_bar);

	/* Idle loop: poll shm click events so UI is interactive. */
	struct timespec req;
	req.tv_sec = 0;
	req.tv_nsec = 10 * 1000 * 1000;
	uint64_t last_mouse_event = fb.hdr->mouse_event_counter;
	for (;;) {
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
						compose_url_bar(url_bar, sizeof(url_bar), host, path);
						draw_ui(&fb, host, url_bar, "EN clicked", host, "Fetching ...");
						fb.hdr->frame_counter++;
						do_https_status(&fb, host, path, url_bar);
					} else if (a == UI_GO_DE) {
						(void)c_strlcpy_s(host, sizeof(host), "de.wikipedia.org");
						(void)c_strlcpy_s(path, sizeof(path), "/");
						compose_url_bar(url_bar, sizeof(url_bar), host, path);
						draw_ui(&fb, host, url_bar, "DE clicked", host, "Fetching ...");
						fb.hdr->frame_counter++;
						do_https_status(&fb, host, path, url_bar);
					} else if (a == UI_RELOAD) {
						compose_url_bar(url_bar, sizeof(url_bar), host, path);
						draw_ui(&fb, host, url_bar, "Reload clicked", host, "Fetching ...");
						fb.hdr->frame_counter++;
						do_https_status(&fb, host, path, url_bar);
					}
				}
			}
		}
		sys_nanosleep(&req, 0);
	}
	return 0;
}
