#include "../core/text.h"
#include "fb_shm.h"
#include "net_dns.h"
#include "net_tcp.h"

#include "../tls/selftest.h"
#include "tls13_client.h"

#define FB_W 1920u
#define FB_H 1080u

static void draw_ui(struct shm_fb *fb, const char *line1, const char *line2, const char *line3)
{
	/* Background */
	fill_rect_u32(fb->pixels, fb->stride, 0, 0, fb->width, fb->height, 0xff101014u);

	/* Title bar */
	fill_rect_u32(fb->pixels, fb->stride, 0, 0, fb->width, 24, 0xff202028u);
	struct text_color tc = { .fg = 0xffffffffu, .bg = 0, .opaque_bg = 0 };
	struct text_color dim = { .fg = 0xffb0b0b0u, .bg = 0, .opaque_bg = 0 };

	draw_text_u32(fb->pixels, fb->stride, 8, 8, "BROWSER (SYS) - NET/TLS WIP", tc);

	/* Body */
	fill_rect_u32(fb->pixels, fb->stride, 0, 24, fb->width, fb->height - 24, 0xff101014u);

	draw_text_u32(fb->pixels, fb->stride, 8, 40, line1, tc);
	draw_text_u32(fb->pixels, fb->stride, 8, 56, line2, tc);
	draw_text_u32(fb->pixels, fb->stride, 8, 72, line3, dim);
}

int main(void)
{
	struct shm_fb fb;
	if (shm_fb_open(&fb, FB_W, FB_H) < 0) {
		return 1;
	}

	int crypto_ok = tls_crypto_selftest();
	if (!crypto_ok) {
		draw_ui(&fb, "CRYPTO SELFTEST: FAIL", "Refusing to continue.", "Run: make test");
		fb.hdr->frame_counter++;
		for (;;) {
			struct timespec req;
			req.tv_sec = 0;
			req.tv_nsec = 250 * 1000 * 1000;
			sys_nanosleep(&req, 0);
		}
	}

	const char *host = "de.wikipedia.org";
	draw_ui(&fb, "CRYPTO SELFTEST: OK", "Resolving de.wikipedia.org (AAAA via Google DNS v6) ...", "TLS 1.3 next");
	fb.hdr->frame_counter++;

	uint8_t ip6[16];
	c_memset(ip6, 0, sizeof(ip6));
	int dns_ok = dns_resolve_aaaa_google(host, ip6);
	char ip_str[48];
	if (dns_ok == 0) {
		ip6_to_str(ip_str, ip6);
	} else {
		c_memcpy(ip_str, "<dns6 fail>", 12);
	}

	char line1[96];
	/* Compose a simple status line without snprintf. */
	c_memset(line1, 0, sizeof(line1));
	const char *pfx = "DNS AAAA de.wikipedia.org = ";
	size_t o = 0;
	for (size_t i = 0; pfx[i] && o + 1 < sizeof(line1); i++) line1[o++] = pfx[i];
	for (size_t i = 0; ip_str[i] && o + 1 < sizeof(line1); i++) line1[o++] = ip_str[i];
	line1[o] = 0;

	draw_ui(&fb, line1, "Connecting IPv6 to :443 ...", "");
	fb.hdr->frame_counter++;

	int sock = -1;
	if (dns_ok == 0) {
		sock = tcp6_connect(ip6, 443);
	}

	const char *line2 = (sock >= 0) ? "TCP connect OK (TLS 1.3 handshake ...)" : "TCP connect FAILED";
	draw_ui(&fb, line1, line2, "Sending ClientHello + SNI + HTTP/1.1 GET");
	fb.hdr->frame_counter++;

	if (sock >= 0) {
		char status[128];
		int rc = tls13_https_get_status_line(sock, host, "/", status, sizeof(status));
		if (rc == 0) {
			draw_ui(&fb, line1, "TLS+HTTP OK", status);
		} else {
			draw_ui(&fb, line1, "TLS+HTTP FAILED", "(no cert validation yet; handshake bring-up) ");
		}
		fb.hdr->frame_counter++;
		sys_close(sock);
	}

	/* Idle loop so the UI stays visible while we develop. */
	struct timespec req;
	req.tv_sec = 0;
	req.tv_nsec = 100 * 1000 * 1000;
	for (;;) {
		sys_nanosleep(&req, 0);
	}
	return 0;
}
