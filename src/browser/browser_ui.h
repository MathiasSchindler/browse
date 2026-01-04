#pragma once

#include "browser_defs.h"
#include "fb_shm.h"
#include "html_text.h"

enum ui_action {
	UI_NONE = 0,
	UI_GO_EN = 1,
	UI_GO_DE = 2,
	UI_RELOAD = 3,
	UI_FOCUS_URLBAR = 4,
};

void browser_draw_ui(struct shm_fb *fb,
			    const char *active_host,
			    const char *url_bar,
			    const char *status_bar,
			    const char *line1,
			    const char *line2,
			    const char *line3);

void browser_render_page(struct shm_fb *fb,
				const char *active_host,
				const char *url_bar,
				const char *status_bar,
				const char *visible_text,
				const struct html_links *links,
				const struct html_spans *spans,
				const struct html_inline_imgs *inline_imgs,
				uint32_t scroll_rows);

enum ui_action browser_ui_action_from_click(const struct shm_fb *fb, uint32_t x, uint32_t y);

int browser_ui_try_link_click(uint32_t x,
			     uint32_t y,
			     uint32_t width,
			     const char *visible_text,
			     const struct html_links *links,
			     uint32_t scroll_rows,
			     char *out_href,
			     size_t out_href_len);
