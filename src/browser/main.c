#include "../core/text.h"
#include "fb_shm.h"
#include "url.h"

#include "../tls/selftest.h"
#include "html_text.h"
#include "text_layout.h"

#include "image/jpeg.h"
#include "image/jpeg_decode.h"
#include "image/png.h"
#include "image/png_decode.h"
#include "image/gif.h"
#include "image/gif_decode.h"

#include "browser_defs.h"
#include "browser_img.h"

#include "browser_nav.h"
#include "browser_ui.h"

static uint8_t g_body[512 * 1024];
static size_t g_body_len;

static char g_visible[512 * 1024];
static char g_status_bar[128];
static char g_url_bar[URL_BUF_LEN];
static char g_active_host[HOST_BUF_LEN];
static struct html_links g_links;
static struct html_spans g_spans;
static struct html_inline_imgs g_inline_imgs;
static uint32_t g_scroll_rows;
static int g_have_page;

static int g_url_edit_active;
static char g_url_edit[URL_BUF_LEN];
static uint32_t g_url_edit_len;
static uint32_t g_url_edit_cursor;

static void url_edit_begin(void)
{
	g_url_edit_active = 1;
	(void)c_strlcpy_s(g_url_edit, sizeof(g_url_edit), g_url_bar);
	g_url_edit_len = (uint32_t)c_strnlen_s(g_url_edit, sizeof(g_url_edit));
	g_url_edit_cursor = g_url_edit_len;
}

static void url_edit_cancel(void)
{
	g_url_edit_active = 0;
}

static const char *url_bar_display(char *tmp, size_t tmp_len)
{
	if (!tmp || tmp_len == 0) return g_url_bar;
	if (!g_url_edit_active) return g_url_bar;

	uint32_t cursor = g_url_edit_cursor;
	uint32_t len = g_url_edit_len;
	if (cursor > len) cursor = len;
	if (len + 2u > (uint32_t)tmp_len) len = (uint32_t)tmp_len - 2u;
	if (cursor > len) cursor = len;

	uint32_t o = 0;
	for (; o < cursor && o < len && o + 1u < (uint32_t)tmp_len; o++) tmp[o] = g_url_edit[o];
	if (o + 1u < (uint32_t)tmp_len) tmp[o++] = '|';
	for (uint32_t i = cursor; i < len && o + 1u < (uint32_t)tmp_len; i++) tmp[o++] = g_url_edit[i];
	tmp[o] = 0;
	return tmp;
}

static int url_parse_user_input(const char *in,
				 const char *current_host,
				 char *out_host, size_t out_host_len,
				 char *out_path, size_t out_path_len)
{
	if (!in || !out_host || !out_path) return -1;

	/* Trim leading spaces */
	while (*in == ' ' || *in == '\t' || *in == '\n' || *in == '\r') in++;
	if (*in == 0) return -1;

	/* Reuse existing URL logic for absolute forms. */
	if (c_starts_with(in, "https://") || c_starts_with(in, "http://") || c_starts_with(in, "//") || in[0] == '/') {
		return url_apply_location(current_host, in, out_host, out_host_len, out_path, out_path_len);
	}

	/* Accept: host[/path] */
	char host_tmp[HOST_BUF_LEN];
	char path_tmp[PATH_BUF_LEN];
	uint32_t hi = 0;
	while (in[hi] && in[hi] != '/' && in[hi] != ' ' && in[hi] != '\t' && in[hi] != '\n' && in[hi] != '\r' && hi + 1u < (uint32_t)sizeof(host_tmp)) {
		host_tmp[hi] = in[hi];
		hi++;
	}
	host_tmp[hi] = 0;
	if (hi == 0) return -1;

	const char *p = in + hi;
	while (*p == ' ' || *p == '\t') p++;
	if (*p == 0) {
		(void)c_strlcpy_s(path_tmp, sizeof(path_tmp), "/");
	} else if (*p == '/') {
		(void)c_strlcpy_s(path_tmp, sizeof(path_tmp), p);
	} else {
		/* No explicit '/', treat remaining as path and force leading '/'. */
		path_tmp[0] = '/';
		(void)c_strlcpy_s(path_tmp + 1, sizeof(path_tmp) - 1u, p);
	}

	if (c_strlcpy_s(out_host, out_host_len, host_tmp) != 0) return -1;
	if (c_strlcpy_s(out_path, out_path_len, path_tmp) != 0) return -1;
	return 0;
}

static void url_edit_insert_char(uint32_t ch)
{
	if (!g_url_edit_active) return;
	if (ch == 0) return;
	if (ch == ' ') return;
	if (ch < 0x20u || ch >= 0x7fu) return;
	if (g_url_edit_len + 1u >= (uint32_t)sizeof(g_url_edit)) return;

	uint32_t cursor = g_url_edit_cursor;
	if (cursor > g_url_edit_len) cursor = g_url_edit_len;
	for (uint32_t i = g_url_edit_len + 1u; i > cursor; i--) {
		g_url_edit[i] = g_url_edit[i - 1u];
	}
	g_url_edit[cursor] = (char)ch;
	g_url_edit_len++;
	g_url_edit_cursor = cursor + 1u;
}

static void url_edit_backspace(void)
{
	if (!g_url_edit_active) return;
	uint32_t cursor = g_url_edit_cursor;
	if (cursor == 0) return;
	if (cursor > g_url_edit_len) cursor = g_url_edit_len;
	for (uint32_t i = cursor - 1u; i < g_url_edit_len; i++) {
		g_url_edit[i] = g_url_edit[i + 1u];
	}
	g_url_edit_len--;
	g_url_edit_cursor = cursor - 1u;
}

static void redraw_now(struct shm_fb *fb,
		       const char *active_host,
		       const char *status_bar,
		       const char *visible,
		       const struct html_links *links,
		       const struct html_spans *spans,
		       const struct html_inline_imgs *inline_imgs,
		       uint32_t scroll_rows,
		       int have_page)
{
	if (!fb) return;
	char url_tmp[URL_BUF_LEN + 2u];
	const char *disp_url = url_bar_display(url_tmp, sizeof(url_tmp));
	const char *disp_status = g_url_edit_active ? "" : (status_bar ? status_bar : "");
	if (have_page && visible && links && spans && inline_imgs) {
		browser_render_page(fb, active_host, disp_url, disp_status, visible, links, spans, inline_imgs, scroll_rows);
	} else {
		browser_draw_ui(fb, active_host, disp_url, disp_status, "", "", "");
		fb->hdr->frame_counter++;
	}
}

static struct browser_page make_page(void)
{
	struct browser_page page;
	page.body = g_body;
	page.body_cap = sizeof(g_body);
	page.body_len = &g_body_len;

	page.visible = g_visible;
	page.visible_cap = sizeof(g_visible);

	page.status_bar = g_status_bar;
	page.status_bar_cap = sizeof(g_status_bar);

	page.active_host = g_active_host;
	page.active_host_cap = sizeof(g_active_host);

	page.url_bar = g_url_bar;
	page.url_bar_cap = sizeof(g_url_bar);

	page.links = &g_links;
	page.spans = &g_spans;
	page.inline_imgs = &g_inline_imgs;

	page.scroll_rows = &g_scroll_rows;
	page.have_page = &g_have_page;
	return page;
}

int main(int argc, char **argv)
{
	struct shm_fb fb;
	if (shm_fb_open(&fb, FB_W, FB_H) < 0) {
		return 1;
	}

	int crypto_ok = tls_crypto_selftest();
	if (!crypto_ok) {
		browser_draw_ui(&fb, "", "", "", "CRYPTO SELFTEST: FAIL", "Refusing to continue.", "Run: make test");
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
	if (argc > 1 && argv && argv[1] && argv[1][0]) {
		char new_host[HOST_BUF_LEN];
		char new_path[PATH_BUF_LEN];
		if (url_parse_user_input(argv[1], host, new_host, sizeof(new_host), new_path, sizeof(new_path)) == 0) {
			(void)c_strlcpy_s(host, sizeof(host), new_host);
			(void)c_strlcpy_s(path, sizeof(path), new_path);
		}
	}
	browser_compose_url_bar(url_bar, sizeof(url_bar), host, path);
	browser_draw_ui(&fb, host, url_bar, "", "CRYPTO SELFTEST: OK", "Click EN / DE / Reload", host);
	fb.hdr->frame_counter++;

	struct browser_page page = make_page();
	browser_do_https_status(&fb, host, path, url_bar, page);

	/* Idle loop: poll shm click events so UI is interactive. */
	struct timespec req;
	req.tv_sec = 0;
	req.tv_nsec = 10 * 1000 * 1000;
	uint64_t last_mouse_event = fb.hdr->mouse_event_counter;
	uint32_t last_keyq_wpos = fb.hdr->keyq_wpos;
	uint32_t idle_ticks = 0;
	for (;;) {
		int did_interact = 0;
		char url_tmp[URL_BUF_LEN + 2u];
		const char *disp_url = url_bar_display(url_tmp, sizeof(url_tmp));
		const char *disp_status = g_url_edit_active ? "" : g_status_bar;
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
				browser_render_page(&fb, g_active_host, disp_url, disp_status, g_visible, &g_links, &g_spans, &g_inline_imgs, g_scroll_rows);
			}
		}

		/* Keyboard/text events from dev viewer (CFB v3+). */
		if (fb.hdr->version >= 3) {
			uint32_t wpos = fb.hdr->keyq_wpos;
			while (last_keyq_wpos != wpos) {
				struct cfb_key_event ev = fb.hdr->keyq[last_keyq_wpos & (CFB_KEYQ_SIZE - 1u)];
				last_keyq_wpos++;
				did_interact = 1;

				if (g_url_edit_active) {
					if (ev.kind == CFB_KEY_TEXT) {
						url_edit_insert_char(ev.ch);
					} else if (ev.kind == CFB_KEY_BACKSPACE) {
						url_edit_backspace();
					} else if (ev.kind == CFB_KEY_ESCAPE) {
						url_edit_cancel();
					} else if (ev.kind == CFB_KEY_ENTER) {
						char new_host[HOST_BUF_LEN];
						char new_path[PATH_BUF_LEN];
						if (url_parse_user_input(g_url_edit, host, new_host, sizeof(new_host), new_path, sizeof(new_path)) == 0) {
							(void)c_strlcpy_s(host, sizeof(host), new_host);
							(void)c_strlcpy_s(path, sizeof(path), new_path);
							g_scroll_rows = 0;
							url_edit_cancel();
							browser_compose_url_bar(url_bar, sizeof(url_bar), host, path);
							browser_draw_ui(&fb, host, url_bar, "", "Fetching ...", host, path);
							fb.hdr->frame_counter++;
							browser_do_https_status(&fb, host, path, url_bar, page);
						}
					}
				} else {
					if (ev.kind == CFB_KEY_TEXT && (ev.ch == (uint32_t)'g' || ev.ch == (uint32_t)'G')) {
						url_edit_begin();
					}
				}

				if (g_have_page) {
					char url_tmp2[URL_BUF_LEN + 2u];
					const char *disp_url2 = url_bar_display(url_tmp2, sizeof(url_tmp2));
					const char *disp_status2 = g_url_edit_active ? "" : g_status_bar;
					browser_render_page(&fb, g_active_host, disp_url2, disp_status2, g_visible, &g_links, &g_spans, &g_inline_imgs, g_scroll_rows);
				}
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
					enum ui_action a = browser_ui_action_from_click(&fb, x, y);
					if (g_url_edit_active && a != UI_FOCUS_URLBAR) {
						/* Click outside the URL bar cancels editing. */
						url_edit_cancel();
						redraw_now(&fb, g_active_host, g_status_bar, g_visible, &g_links, &g_spans, &g_inline_imgs, g_scroll_rows, g_have_page);
					}
					if (a == UI_GO_SP) {
						(void)c_strlcpy_s(host, sizeof(host), "www.spiegel.de");
						(void)c_strlcpy_s(path, sizeof(path), "/");
						g_scroll_rows = 0;
						browser_compose_url_bar(url_bar, sizeof(url_bar), host, path);
						browser_draw_ui(&fb, host, url_bar, "", "SP clicked", host, "Fetching ...");
						fb.hdr->frame_counter++;
						browser_do_https_status(&fb, host, path, url_bar, page);
					} else if (a == UI_GO_EN) {
						(void)c_strlcpy_s(host, sizeof(host), "en.wikipedia.org");
						(void)c_strlcpy_s(path, sizeof(path), "/");
						g_scroll_rows = 0;
						browser_compose_url_bar(url_bar, sizeof(url_bar), host, path);
						browser_draw_ui(&fb, host, url_bar, "", "EN clicked", host, "Fetching ...");
						fb.hdr->frame_counter++;
						browser_do_https_status(&fb, host, path, url_bar, page);
					} else if (a == UI_GO_DE) {
						(void)c_strlcpy_s(host, sizeof(host), "de.wikipedia.org");
						(void)c_strlcpy_s(path, sizeof(path), "/");
						g_scroll_rows = 0;
						browser_compose_url_bar(url_bar, sizeof(url_bar), host, path);
						browser_draw_ui(&fb, host, url_bar, "", "DE clicked", host, "Fetching ...");
						fb.hdr->frame_counter++;
						browser_do_https_status(&fb, host, path, url_bar, page);
					} else if (a == UI_RELOAD) {
						g_scroll_rows = 0;
						browser_compose_url_bar(url_bar, sizeof(url_bar), host, path);
						browser_draw_ui(&fb, host, url_bar, "", "Reload clicked", host, "Fetching ...");
						fb.hdr->frame_counter++;
						browser_do_https_status(&fb, host, path, url_bar, page);
					} else if (a == UI_FOCUS_URLBAR) {
						url_edit_begin();
						redraw_now(&fb, g_active_host, g_status_bar, g_visible, &g_links, &g_spans, &g_inline_imgs, g_scroll_rows, g_have_page);
					} else {
						/* Body link click */
						char href[HTML_HREF_MAX];
						href[0] = 0;
						if (g_have_page && browser_ui_try_link_click(x, y, fb.width, g_visible, &g_links, g_scroll_rows, href, sizeof(href))) {
							char new_host[HOST_BUF_LEN];
							char new_path[PATH_BUF_LEN];
							if (url_apply_location(host, href, new_host, sizeof(new_host), new_path, sizeof(new_path)) == 0) {
								(void)c_strlcpy_s(host, sizeof(host), new_host);
								(void)c_strlcpy_s(path, sizeof(path), new_path);
								g_scroll_rows = 0;
								browser_compose_url_bar(url_bar, sizeof(url_bar), host, path);
								browser_draw_ui(&fb, host, url_bar, "", "Link clicked", href, "Fetching ...");
								fb.hdr->frame_counter++;
								browser_do_https_status(&fb, host, path, url_bar, page);
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
			int pixels_changed = 0;
			if (img_workers_pump(&dims_changed, &pixels_changed)) {
				if (dims_changed && g_body_len > 0) {
					struct img_dim_ctx ctx = { .active_host = g_active_host };
					(void)html_visible_text_extract_links_spans_and_inline_imgs_ex(g_body,
									      g_body_len,
									      g_visible,
									      sizeof(g_visible),
									      &g_links,
									      &g_spans,
									      &g_inline_imgs,
									      browser_html_img_dim_lookup,
									      &ctx);
					prefetch_page_images(g_active_host, g_visible, &g_inline_imgs);
				}
				if (dims_changed || pixels_changed) {
					browser_render_page(&fb, g_active_host, g_url_bar, g_status_bar, g_visible, &g_links, &g_spans, &g_inline_imgs, g_scroll_rows);
				}
			}
		}

		/* Large-image fallback: keep it slow to avoid stutter. */
		if (!did_interact && g_have_page && idle_ticks > 100u && (idle_ticks % 100u) == 0u) {
			if (img_decode_large_pump_one()) {
				browser_render_page(&fb, g_active_host, g_url_bar, g_status_bar, g_visible, &g_links, &g_spans, &g_inline_imgs, g_scroll_rows);
			}
		}
		sys_nanosleep(&req, 0);
	}
	return 0;
}
