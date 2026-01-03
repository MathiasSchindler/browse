#include <stdio.h>

#include "browser/image/gif_decode.h"

static int test_1x1_white(void)
{
	/* Minimal 1x1 GIF with a 2-entry global palette and a single white pixel.
	 * LZW min code size = 2, codes: clear(4), index(1), end(5).
	 */
	static const uint8_t gif[] = {
		'G','I','F','8','9','a',
		0x01,0x00, /* w=1 */
		0x01,0x00, /* h=1 */
		0x80, /* GCT flag=1, size=2 */
		0x00, /* bg */
		0x00, /* aspect */
		/* GCT: black, white */
		0x00,0x00,0x00,
		0xff,0xff,0xff,
		/* Image descriptor */
		0x2c,
		0x00,0x00,0x00,0x00, /* left/top */
		0x01,0x00,0x01,0x00, /* w/h */
		0x00, /* no LCT, not interlaced */
		0x02, /* LZW min */
		0x02, /* sub-block length */
		0x4c,0x01, /* data */
		0x00, /* end sub-blocks */
		0x3b, /* trailer */
	};

	uint32_t px[1] = {0};
	uint32_t w = 0, h = 0;
	if (gif_decode_first_frame_xrgb(gif, sizeof(gif), px, 1, &w, &h) != 0) {
		fprintf(stderr, "gif_decode_first_frame_xrgb failed\n");
		return 1;
	}
	if (w != 1 || h != 1) {
		fprintf(stderr, "dims mismatch: got %ux%u expected 1x1\n", w, h);
		return 1;
	}
	if (px[0] != 0xffffffffu) {
		fprintf(stderr, "pixel mismatch: got 0x%08x expected 0xffffffff\n", px[0]);
		return 1;
	}
	return 0;
}

int main(void)
{
	if (test_1x1_white()) return 1;
	printf("gif decode selftest: OK\n");
	return 0;
}
