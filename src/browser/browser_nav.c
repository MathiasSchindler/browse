#include "browser_nav.h"

#include "browser_img.h"
#include "browser_ui.h"

#include "net_dns.h"
#include "net_tcp.h"
#include "tls13_client.h"
#include "url.h"

#include "../core/text.h"
#include "../core/log.h"

void browser_compose_url_bar(char *out, size_t out_len, const char *host, const char *path)
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

void browser_do_https_status(struct shm_fb *fb,
				 char host[HOST_BUF_LEN],
				 char path[PATH_BUF_LEN],
				 char url_bar[URL_BUF_LEN],
				 struct browser_page page)
{
	if (!fb) return;
	if (!page.body || !page.body_len || page.body_cap == 0) return;
	if (!page.visible || page.visible_cap == 0) return;
	if (!page.links || !page.spans || !page.inline_imgs) return;
	if (!page.scroll_rows || !page.have_page) return;

	*page.body_len = 0;
	uint64_t content_len = 0;
	char final_status[128];
	final_status[0] = 0;
	page.visible[0] = 0;
	page.body[0] = 0;
	if (page.status_bar && page.status_bar_cap) page.status_bar[0] = 0;
	page.links->n = 0;
	page.spans->n = 0;
	page.inline_imgs->n = 0;
	*page.scroll_rows = 0;
	*page.have_page = 0;

	/* Navigation: stop any in-flight image fetch/decode from the previous page,
	 * and start a new generation so only current-page images are eligible.
	 */
	img_workers_cancel_all();
	img_workers_init();
	img_cache_begin_new_page();

	for (int step = 0; step < 6; step++) {
		browser_compose_url_bar(url_bar, URL_BUF_LEN, host, path);
		browser_draw_ui(fb, host, url_bar, "", "Resolving AAAA via Google DNS v6 ...", host, path);
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

		browser_compose_url_bar(url_bar, URL_BUF_LEN, host, path);
		browser_draw_ui(fb, host, url_bar, "", line1, "Connecting IPv6 to :443 ...", path);
		fb->hdr->frame_counter++;

		int sock = -1;
		if (dns_ok == 0) {
			sock = tcp6_connect(ip6, 443);
		}

		const char *line2 = (sock >= 0) ? "TCP connect OK (TLS 1.3 handshake ...)" : "TCP connect FAILED";
		browser_compose_url_bar(url_bar, URL_BUF_LEN, host, path);
		browser_draw_ui(fb, host, url_bar, "", line1, line2, "Sending ClientHello + SNI + HTTP/1.1 GET");
		fb->hdr->frame_counter++;

		if (sock < 0) {
			browser_compose_url_bar(url_bar, URL_BUF_LEN, host, path);
			browser_draw_ui(fb, host, url_bar, "", line1, "TLS+HTTP FAILED", "(connect)");
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
							 page.body,
							 page.body_cap,
							 page.body_len,
							 &content_len);
		sys_close(sock);

		if (rc != 0) {
			browser_compose_url_bar(url_bar, URL_BUF_LEN, host, path);
			browser_draw_ui(fb, host, url_bar, "", line1, "TLS+HTTP FAILED", "(no cert validation yet; handshake bring-up)");
			fb->hdr->frame_counter++;
			break;
		}

		/* Log HTTP status/location (best-effort first line only). */
		{
			char msg[256];
			size_t omsg = 0;
			const char *pfx = "status: ";
			for (size_t i = 0; pfx[i] && omsg + 1 < sizeof(msg); i++) msg[omsg++] = pfx[i];
			for (size_t i = 0; status[i] && status[i] != '\n' && status[i] != '\r' && omsg + 1 < sizeof(msg); i++) {
				msg[omsg++] = status[i];
			}
			if (omsg + 1 < sizeof(msg)) msg[omsg++] = '\n';
			LOGI_BUF("http", msg, omsg);
		}
		if (location[0]) {
			char msg[640];
			size_t omsg = 0;
			const char *pfx = "location: ";
			for (size_t i = 0; pfx[i] && omsg + 1 < sizeof(msg); i++) msg[omsg++] = pfx[i];
			for (size_t i = 0; location[i] && location[i] != '\n' && location[i] != '\r' && omsg + 1 < sizeof(msg); i++) {
				msg[omsg++] = location[i];
			}
			if (omsg + 1 < sizeof(msg)) msg[omsg++] = '\n';
			LOGI_BUF("http", msg, omsg);
		}

		/* Keep only first line of status in status bar (best effort) */
		if (page.status_bar && page.status_bar_cap) {
			size_t so = 0;
			for (size_t i = 0; status[i] && status[i] != '\n' && so + 1 < page.status_bar_cap; i++) {
				page.status_bar[so++] = status[i];
			}
			page.status_bar[so] = 0;
		}

		int is_redirect = (status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 || status_code == 308);
		browser_compose_url_bar(url_bar, URL_BUF_LEN, host, path);
		browser_draw_ui(fb, host, url_bar, status, line1, (location[0] ? "Redirecting..." : ""), (location[0] ? location : path));
		fb->hdr->frame_counter++;

		if (!is_redirect || location[0] == 0) {
			(void)c_strlcpy_s(final_status, sizeof(final_status), status);
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
	browser_compose_url_bar(url_bar, URL_BUF_LEN, host, path);

	/* Render extracted visible text (best-effort). */
	if (*page.body_len > 0) {
		struct img_dim_ctx ctx = { .active_host = host };
		(void)html_visible_text_extract_links_spans_and_inline_imgs_ex(page.body,
							      *page.body_len,
							      page.visible,
							      page.visible_cap,
							      page.links,
							      page.spans,
							      page.inline_imgs,
							      browser_html_img_dim_lookup,
							      &ctx);
	}

	if (page.url_bar && page.url_bar_cap) {
		(void)c_strlcpy_s(page.url_bar, page.url_bar_cap, url_bar);
	}
	if (page.active_host && page.active_host_cap) {
		(void)c_strlcpy_s(page.active_host, page.active_host_cap, host);
	}
	*page.have_page = 1;

	prefetch_page_images(host, page.visible, page.inline_imgs);
	(void)img_workers_pump(0, 0);

	browser_render_page(fb,
			      host,
			      url_bar,
			      (page.status_bar ? page.status_bar : ""),
			      page.visible,
			      page.links,
			      page.spans,
			      page.inline_imgs,
			      *page.scroll_rows);
}
