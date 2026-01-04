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

int main(void)
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
				browser_render_page(&fb, g_active_host, g_url_bar, g_status_bar, g_visible, &g_links, &g_spans, &g_inline_imgs, g_scroll_rows);
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
					if (a == UI_GO_EN) {
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
