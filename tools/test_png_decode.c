#include <stdio.h>

#include "browser/image/png_decode.h"

static int test_1x1_red_rgba(void)
{
	/* Minimal PNG (RGBA, 8bpc, non-interlaced) with a stored (uncompressed) deflate block.
	 *
	 * Pixel: (255,0,0,255)
	 * Scanline: filter=0, then RGBA.
	 * CRCs are set to 0; decoder does not validate them.
	 */
	static const uint8_t png[] = {
		/* signature */
		0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a,
		/* IHDR */
		0x00,0x00,0x00,0x0d,
		'I','H','D','R',
		0x00,0x00,0x00,0x01, /* w=1 */
		0x00,0x00,0x00,0x01, /* h=1 */
		0x08, /* bit depth */
		0x06, /* color type RGBA */
		0x00, /* compression */
		0x00, /* filter */
		0x00, /* interlace */
		0x00,0x00,0x00,0x00, /* CRC */
		/* IDAT: zlib stream */
		0x00,0x00,0x00,0x10,
		'I','D','A','T',
		0x78,0x01, /* zlib header */
		0x01,       /* BFINAL=1, BTYPE=00 (stored) */
		0x05,0x00,  /* LEN=5 */
		0xFA,0xFF,  /* NLEN=~5 */
		0x00,0xFF,0x00,0x00,0xFF, /* scanline: filter 0 + RGBA */
		0x05,0x00,0x01,0xFF, /* Adler32 */
		0x00,0x00,0x00,0x00, /* CRC */
		/* IEND */
		0x00,0x00,0x00,0x00,
		'I','E','N','D',
		0x00,0x00,0x00,0x00, /* CRC */
	};

	uint8_t scratch[64];
	for (size_t i = 0; i < sizeof(scratch); i++) scratch[i] = 0;

	uint32_t px[1] = {0};
	uint32_t w = 0, h = 0;
	if (png_decode_xrgb(png, sizeof(png), scratch, sizeof(scratch), px, 1, &w, &h) != 0) {
		fprintf(stderr, "png_decode_xrgb failed\n");
		return 1;
	}
	if (w != 1 || h != 1) {
		fprintf(stderr, "dims mismatch: got %ux%u expected 1x1\n", w, h);
		return 1;
	}
	if (px[0] != 0xffff0000u) {
		fprintf(stderr, "pixel mismatch: got 0x%08x expected 0xffff0000\n", px[0]);
		return 1;
	}
	return 0;
}

static int test_2x2_gray_alpha(void)
{
	/* Minimal PNG (grayscale+alpha, 8bpc, non-interlaced) with a stored (uncompressed)
	 * deflate block.
	 *
	 * Pixels (g,a):
	 *  (255,255) (0,255)
	 *  (255,128) (0,0)
	 */
	static const uint8_t png[] = {
		0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a,
		/* IHDR */
		0x00,0x00,0x00,0x0d,
		'I','H','D','R',
		0x00,0x00,0x00,0x02, /* w=2 */
		0x00,0x00,0x00,0x02, /* h=2 */
		0x08, /* bit depth */
		0x04, /* color type grayscale+alpha */
		0x00, /* compression */
		0x00, /* filter */
		0x00, /* interlace */
		0x00,0x00,0x00,0x00, /* CRC */
		/* IDAT */
		0x00,0x00,0x00,0x15,
		'I','D','A','T',
		0x78,0x01, /* zlib header */
		0x01,       /* BFINAL=1, BTYPE=00 (stored) */
		0x0a,0x00,  /* LEN=10 */
		0xf5,0xff,  /* NLEN=~10 */
		/* scanlines */
		0x00,0xff,0xff,0x00,0xff,
		0x00,0xff,0x80,0x00,0x00,
		/* Adler32 */
		0x1c,0x6f,0x04,0x7d,
		0x00,0x00,0x00,0x00, /* CRC */
		/* IEND */
		0x00,0x00,0x00,0x00,
		'I','E','N','D',
		0x00,0x00,0x00,0x00,
	};

	uint8_t scratch[128];
	for (size_t i = 0; i < sizeof(scratch); i++) scratch[i] = 0;

	uint32_t px[4] = {0};
	uint32_t w = 0, h = 0;
	if (png_decode_xrgb(png, sizeof(png), scratch, sizeof(scratch), px, 4, &w, &h) != 0) {
		fprintf(stderr, "png_decode_xrgb (gray+alpha) failed\n");
		return 1;
	}
	if (w != 2 || h != 2) {
		fprintf(stderr, "dims mismatch: got %ux%u expected 2x2\n", w, h);
		return 1;
	}

	/* Decoder composites grayscale+alpha over a light checkerboard background.
	 * For this 2x2 image, all pixels land in the same 8x8 tile.
	 */
	if (px[0] != 0xffffffffu) {
		fprintf(stderr, "p0 mismatch: got 0x%08x expected 0xffffffff\n", px[0]);
		return 1;
	}
	if (px[1] != 0xff000000u) {
		fprintf(stderr, "p1 mismatch: got 0x%08x expected 0xff000000\n", px[1]);
		return 1;
	}
	if (px[3] != 0xffd0d0d0u) {
		fprintf(stderr, "p3 mismatch: got 0x%08x expected 0xffd0d0d0\n", px[3]);
		return 1;
	}
	return 0;
}

static int test_2x1_palette_4bpc(void)
{
	/* Paletted PNG with 4-bit indices (common for Wikimedia SVG->PNG thumbs).
	 * Two pixels: idx0=0 (red), idx1=1 (green).
	 */
	static const uint8_t png[] = {
		0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a,
		/* IHDR */
		0x00,0x00,0x00,0x0d,
		'I','H','D','R',
		0x00,0x00,0x00,0x02, /* w=2 */
		0x00,0x00,0x00,0x01, /* h=1 */
		0x04, /* bit depth = 4 */
		0x03, /* color type = palette */
		0x00, /* compression */
		0x00, /* filter */
		0x00, /* interlace */
		0x00,0x00,0x00,0x00,
		/* PLTE: two entries */
		0x00,0x00,0x00,0x06,
		'P','L','T','E',
		0xff,0x00,0x00, /* red */
		0x00,0xff,0x00, /* green */
		0x00,0x00,0x00,0x00,
		/* IDAT: zlib stream w/ stored block: [filter=0][packed idx: 0x01] */
		0x00,0x00,0x00,0x0d,
		'I','D','A','T',
		0x78,0x01,
		0x01,
		0x02,0x00,
		0xfd,0xff,
		0x00,0x01,
		0x00,0x03,0x00,0x02, /* Adler32 */
		0x00,0x00,0x00,0x00,
		/* IEND */
		0x00,0x00,0x00,0x00,
		'I','E','N','D',
		0x00,0x00,0x00,0x00,
	};

	uint8_t scratch[128];
	for (size_t i = 0; i < sizeof(scratch); i++) scratch[i] = 0;

	uint32_t px[2] = {0};
	uint32_t w = 0, h = 0;
	if (png_decode_xrgb(png, sizeof(png), scratch, sizeof(scratch), px, 2, &w, &h) != 0) {
		fprintf(stderr, "png_decode_xrgb (pal4) failed\n");
		return 1;
	}
	if (w != 2 || h != 1) {
		fprintf(stderr, "dims mismatch: got %ux%u expected 2x1\n", w, h);
		return 1;
	}
	if (px[0] != 0xffff0000u) {
		fprintf(stderr, "p0 mismatch: got 0x%08x expected 0xffff0000\n", px[0]);
		return 1;
	}
	if (px[1] != 0xff00ff00u) {
		fprintf(stderr, "p1 mismatch: got 0x%08x expected 0xff00ff00\n", px[1]);
		return 1;
	}
	return 0;
}

int main(void)
{
	if (test_1x1_red_rgba()) return 1;
	if (test_2x2_gray_alpha()) return 1;
	if (test_2x1_palette_4bpc()) return 1;
	printf("png decode selftest: OK\n");
	return 0;
}
