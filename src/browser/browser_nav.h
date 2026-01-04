#pragma once

#include "browser_defs.h"
#include "fb_shm.h"
#include "html_text.h"

struct browser_page {
	uint8_t *body;
	size_t body_cap;
	size_t *body_len;

	char *visible;
	size_t visible_cap;

	char *status_bar;
	size_t status_bar_cap;

	char *active_host;
	size_t active_host_cap;

	char *url_bar;
	size_t url_bar_cap;

	struct html_links *links;
	struct html_spans *spans;
	struct html_inline_imgs *inline_imgs;

	uint32_t *scroll_rows;
	int *have_page;
};

void browser_compose_url_bar(char *out, size_t out_len, const char *host, const char *path);

void browser_do_https_status(struct shm_fb *fb,
				 char host[HOST_BUF_LEN],
				 char path[PATH_BUF_LEN],
				 char url_bar[URL_BUF_LEN],
				 struct browser_page page);
