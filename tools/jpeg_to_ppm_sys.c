#include "../src/browser/image/jpeg_decode.h"

#include "../src/browser/util.h"

static void write_all(int fd, const void *buf, size_t n)
{
	const uint8_t *p = (const uint8_t *)buf;
	while (n) {
		ssize_t r = sys_write(fd, p, n);
		if (r <= 0) sys_exit(1);
		p += (size_t)r;
		n -= (size_t)r;
	}
}

static size_t u32_to_dec_local(char out[11], uint32_t v)
{
	char tmp[11];
	size_t n = 0;
	if (v == 0) {
		out[0] = '0';
		out[1] = 0;
		return 1;
	}
	while (v && n + 1 < sizeof(tmp)) {
		tmp[n++] = (char)('0' + (v % 10u));
		v /= 10u;
	}
	for (size_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
	out[n] = 0;
	return n;
}

/* Reads a JPEG from stdin and writes a binary PPM (P6) to stdout.
 * Intended for debugging the decoder without libc.
 */
int main(void)
{
	static uint8_t in[512 * 1024];
	static uint32_t px[2 * 1024 * 1024];

	size_t used = 0;
	for (;;) {
		if (used >= sizeof(in)) {
			dbg_write("jpeg_to_ppm_sys: input too large\n");
			return 1;
		}
		ssize_t r = sys_read(0, in + used, sizeof(in) - used);
		if (r < 0) {
			dbg_write("jpeg_to_ppm_sys: read failed\n");
			return 1;
		}
		if (r == 0) break;
		used += (size_t)r;
	}

	uint32_t w = 0, h = 0;
	if (jpeg_decode_baseline_xrgb(in, used, px, (size_t)(sizeof(px) / sizeof(px[0])), &w, &h) != 0) {
		dbg_write("jpeg_to_ppm_sys: decode failed\n");
		return 1;
	}

	/* Header */
	char wdec[11];
	char hdec[11];
	u32_to_dec_local(wdec, w);
	u32_to_dec_local(hdec, h);
	write_all(1, "P6\n", 3);
	write_all(1, wdec, c_strlen(wdec));
	write_all(1, " ", 1);
	write_all(1, hdec, c_strlen(hdec));
	write_all(1, "\n255\n", 5);

	/* Body, streamed */
	uint8_t outbuf[4096];
	size_t o = 0;
	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++) {
			uint32_t v = px[(size_t)y * (size_t)w + (size_t)x];
			uint8_t r = (uint8_t)((v >> 16) & 0xffu);
			uint8_t g = (uint8_t)((v >> 8) & 0xffu);
			uint8_t b = (uint8_t)(v & 0xffu);
			if (o + 3 > sizeof(outbuf)) {
				write_all(1, outbuf, o);
				o = 0;
			}
			outbuf[o++] = r;
			outbuf[o++] = g;
			outbuf[o++] = b;
		}
	}
	if (o) write_all(1, outbuf, o);

	return 0;
}
