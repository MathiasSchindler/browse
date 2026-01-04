#include "browser_nav.h"

#include "browser_img.h"
#include "browser_ui.h"

#include "net_dns.h"
#include "net_tcp.h"
#include "tls13_client.h"

#include "http_parse.h"
#include "url.h"

#include "../core/text.h"
#include "../core/log.h"

static void nav_log_resolve(const char *host, const char *path)
{
	if (!host) host = "";
	if (!path) path = "";
	char msg[256];
	size_t o = 0;
	const char *pfx = "GET https://";
	for (size_t i = 0; pfx[i] && o + 1 < sizeof(msg); i++) msg[o++] = pfx[i];
	for (size_t i = 0; host[i] && o + 1 < sizeof(msg); i++) msg[o++] = host[i];
	for (size_t i = 0; path[i] && o + 1 < sizeof(msg); i++) msg[o++] = path[i];
	msg[o] = 0;
	LOGI("nav", msg);
}

static void nav_log_dns6_ok(const uint8_t ip6[16])
{
	char ip_str6[48];
	ip6_to_str(ip_str6, ip6);
	char msg[96];
	size_t o = 0;
	const char *pfx = "DNS AAAA ok: ";
	for (size_t i = 0; pfx[i] && o + 1 < sizeof(msg); i++) msg[o++] = pfx[i];
	for (size_t i = 0; ip_str6[i] && o + 1 < sizeof(msg); i++) msg[o++] = ip_str6[i];
	msg[o] = 0;
	LOGI("nav", msg);
}

static void nav_log_dns4_ok(const uint8_t ip4[4])
{
	char ip_str4[16];
	ip4_to_str(ip_str4, ip4);
	char msg[64];
	size_t o = 0;
	const char *pfx = "DNS A ok: ";
	for (size_t i = 0; pfx[i] && o + 1 < sizeof(msg); i++) msg[o++] = pfx[i];
	for (size_t i = 0; ip_str4[i] && o + 1 < sizeof(msg); i++) msg[o++] = ip_str4[i];
	msg[o] = 0;
	LOGI("nav", msg);
}

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
		nav_log_resolve(host, path);
		browser_compose_url_bar(url_bar, URL_BUF_LEN, host, path);
		browser_draw_ui(fb, host, url_bar, "", "Resolving (AAAA/A) via Google DNS (IPv6 preferred; IPv4 fallback) ...", host, path);
		fb->hdr->frame_counter++;

		uint8_t ip6[16];
		uint8_t ip4[4];
		c_memset(ip6, 0, sizeof(ip6));
		c_memset(ip4, 0, sizeof(ip4));
		int dns6_ok = (dns_resolve_aaaa_google(host, ip6) == 0);
		if (dns6_ok) {
			nav_log_dns6_ok(ip6);
		} else {
			LOGW("nav", "DNS AAAA failed (Google DNS over IPv6+IPv4)");
		}
		int dns4_ok = 0;
		int use_v4 = 0;
		int sock = -1;

		char line1[192];
		c_memset(line1, 0, sizeof(line1));
		size_t o = 0;
		if (dns6_ok) {
			char ip_str6[48];
			ip6_to_str(ip_str6, ip6);
			const char *pfx = "DNS AAAA = ";
			for (size_t i = 0; pfx[i] && o + 1 < sizeof(line1); i++) line1[o++] = pfx[i];
			for (size_t i = 0; ip_str6[i] && o + 1 < sizeof(line1); i++) line1[o++] = ip_str6[i];
			line1[o] = 0;
		} else {
			const char *pfx = "DNS AAAA failed; trying A ...";
			for (size_t i = 0; pfx[i] && o + 1 < sizeof(line1); i++) line1[o++] = pfx[i];
			line1[o] = 0;
		}

		browser_compose_url_bar(url_bar, URL_BUF_LEN, host, path);
		browser_draw_ui(fb, host, url_bar, "", line1, "Connecting ...", path);
		fb->hdr->frame_counter++;

		if (dns6_ok) {
			sock = tcp6_connect(ip6, 443);
			if (sock >= 0) {
				use_v4 = 0;
			} else {
				LOGW("nav", "TCP connect (IPv6) failed");
			}
		}
		if (sock < 0) {
			dns4_ok = (dns_resolve_a_google4(host, ip4) == 0);
			if (dns4_ok) {
				nav_log_dns4_ok(ip4);
			} else {
				LOGW("nav", "DNS A failed (Google DNS over IPv6+IPv4)");
			}
			if (dns4_ok) {
				sock = tcp4_connect(ip4, 443);
				if (sock >= 0) {
					use_v4 = 1;
				} else {
					LOGW("nav", "TCP connect (IPv4) failed");
				}
			}
		}
		/* If we ended up using IPv4, adjust the DNS line to avoid confusion.
		 * (Otherwise it may still say “trying A...” even when A succeeded.)
		 */
		if (dns4_ok) {
			char ip_str4[16];
			ip4_to_str(ip_str4, ip4);
			c_memset(line1, 0, sizeof(line1));
			o = 0;
			if (dns6_ok) {
				char ip_str6[48];
				ip6_to_str(ip_str6, ip6);
				const char *pfx = "DNS AAAA/A = ";
				for (size_t i = 0; pfx[i] && o + 1 < sizeof(line1); i++) line1[o++] = pfx[i];
				for (size_t i = 0; ip_str6[i] && o + 1 < sizeof(line1); i++) line1[o++] = ip_str6[i];
				if (o + 3 < sizeof(line1)) { line1[o++] = ' '; line1[o++] = '/'; line1[o++] = ' '; }
				for (size_t i = 0; ip_str4[i] && o + 1 < sizeof(line1); i++) line1[o++] = ip_str4[i];
				line1[o] = 0;
			} else {
				const char *pfx = "DNS A = ";
				for (size_t i = 0; pfx[i] && o + 1 < sizeof(line1); i++) line1[o++] = pfx[i];
				for (size_t i = 0; ip_str4[i] && o + 1 < sizeof(line1); i++) line1[o++] = ip_str4[i];
				line1[o] = 0;
			}
		}

		const char *line2 = (sock >= 0) ? (use_v4 ? "TCP connect OK (IPv4)" : "TCP connect OK (IPv6)") : "TCP connect FAILED";
		browser_compose_url_bar(url_bar, URL_BUF_LEN, host, path);
		browser_draw_ui(fb, host, url_bar, "", line1, line2, "TLS 1.3 handshake + HTTP/1.1 GET");
		fb->hdr->frame_counter++;

		if (sock < 0) {
			if (page.status_bar && page.status_bar_cap) {
				(void)c_strlcpy_s(page.status_bar, page.status_bar_cap, "CONNECT FAILED (IPv6+IPv4)");
			}
			if (page.visible && page.visible_cap) {
				const char *m = 0;
				if (!dns6_ok && !dns4_ok) {
					m = "DNS failed (Google DNS unreachable/blocked?)";
				} else if (dns6_ok && !dns4_ok) {
					m = "IPv6 connect failed; DNS A failed too.";
				} else if (!dns6_ok && dns4_ok) {
					m = "DNS A ok but connect failed (IPv4).";
				} else {
					m = "IPv6 connect failed; IPv4 connect failed too.";
				}
				(void)c_strlcpy_s(page.visible, page.visible_cap, m);
			}
			LOGW("nav", "CONNECT FAILED (IPv6+IPv4)");
			*page.have_page = 1;
			browser_render_page(fb,
					  host,
					  url_bar,
					  (page.status_bar ? page.status_bar : ""),
					  page.visible,
					  page.links,
					  page.spans,
					  page.inline_imgs,
					  *page.scroll_rows);
			return;
		}

		char status[128];
		char location[512];
		int status_code = -1;
		char content_type[128];
		char content_enc[64];
		content_type[0] = 0;
		content_enc[0] = 0;
		int rc = tls13_https_get_status_location_and_body(sock,
							 host,
							 path,
							 status,
							 sizeof(status),
							 &status_code,
							 location,
							 sizeof(location),
						 content_type,
						 sizeof(content_type),
						 content_enc,
						 sizeof(content_enc),
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

		/* If the server sends compressed body we can't decode yet, don't pretend it's a blank page. */
		if (content_enc[0] && !http_value_has_token_ci(content_enc, "identity")) {
			if (page.status_bar && page.status_bar_cap) {
				(void)c_strlcpy_s(page.status_bar, page.status_bar_cap, "UNSUPPORTED Content-Encoding");
			}
			if (page.visible && page.visible_cap) {
				char msg[256];
				size_t mo = 0;
				const char *pfx = "Unsupported Content-Encoding: ";
				for (size_t i = 0; pfx[i] && mo + 1 < sizeof(msg); i++) msg[mo++] = pfx[i];
				for (size_t i = 0; content_enc[i] && mo + 1 < sizeof(msg); i++) msg[mo++] = content_enc[i];
				msg[mo] = 0;
				(void)c_strlcpy_s(page.visible, page.visible_cap, msg);
			}
			*page.body_len = 0;
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

			/* Add lightweight response diagnostics without making the UI noisy. */
			if (content_type[0] && so + 3 < page.status_bar_cap) {
				page.status_bar[so++] = ' ';
				page.status_bar[so++] = '|';
				page.status_bar[so++] = ' ';
				for (size_t i = 0; content_type[i] && content_type[i] != ';' && content_type[i] != '\r' && content_type[i] != '\n' && so + 1 < page.status_bar_cap; i++) {
					page.status_bar[so++] = content_type[i];
				}
				page.status_bar[so] = 0;
			}
			if (content_enc[0] && so + 6 < page.status_bar_cap) {
				const char *pfx = " enc=";
				for (size_t i = 0; pfx[i] && so + 1 < page.status_bar_cap; i++) page.status_bar[so++] = pfx[i];
				for (size_t i = 0; content_enc[i] && content_enc[i] != ';' && content_enc[i] != '\r' && content_enc[i] != '\n' && so + 1 < page.status_bar_cap; i++) {
					page.status_bar[so++] = content_enc[i];
				}
				page.status_bar[so] = 0;
			}
			if (so + 4 < page.status_bar_cap) {
				const char *pfx = use_v4 ? " v4" : " v6";
				for (size_t i = 0; pfx[i] && so + 1 < page.status_bar_cap; i++) page.status_bar[so++] = pfx[i];
				page.status_bar[so] = 0;
			}
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
