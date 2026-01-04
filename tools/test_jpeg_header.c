#include <stdio.h>

#include "browser/image/jpeg.h"

static int expect_dims(const uint8_t *data, size_t len, uint32_t ew, uint32_t eh)
{
	uint32_t w = 0, h = 0;
	int rc = jpeg_get_dimensions(data, len, &w, &h);
	if (rc != 0) {
		fprintf(stderr, "jpeg_get_dimensions failed\n");
		return 1;
	}
	if (w != ew || h != eh) {
		fprintf(stderr, "dims mismatch: got %ux%u expected %ux%u\n", w, h, ew, eh);
		return 1;
	}
	return 0;
}

int main(void)
{
	/* Minimal synthetic JPEG:
	 * SOI, APP0(JFIF), SOF0(32x16), EOI.
	 */
	static const uint8_t jpg[] = {
		0xff, 0xd8,
		0xff, 0xe0, 0x00, 0x10,
		'J','F','I','F',0x00, 0x01,0x01, 0x00, 0x00,0x01, 0x00,0x01, 0x00,0x00,
		0xff, 0xc0, 0x00, 0x11,
		0x08, 0x00,0x10, 0x00,0x20, 0x03,
		0x01,0x11,0x00,
		0x02,0x11,0x00,
		0x03,0x11,0x00,
		0xff, 0xd9,
	};

	/* Regression: jpeg_get_dimensions must not scan into entropy-coded bytes after SOS.
	 * This intentionally-invalid stream has no SOF marker at all, but contains bytes
	 * that look like an SOF0 marker after an SOS. The correct behavior is to fail.
	 */
	static const uint8_t jpg_sos_trick[] = {
		0xff, 0xd8, /* SOI */
		0xff, 0xda, 0x00, 0x02, /* SOS with empty payload */
		/* Fake SOF0 marker in what would be entropy-coded data: */
		0xff, 0xc0, 0x00, 0x11,
		0x08, 0x00,0x10, 0x00,0x20, 0x03,
		0x01,0x11,0x00,
		0x02,0x11,0x00,
		0x03,0x11,0x00,
		0xff, 0xd9, /* EOI */
	};

	if (expect_dims(jpg, sizeof(jpg), 32, 16)) return 1;
	{
		uint32_t w = 0, h = 0;
		int rc = jpeg_get_dimensions(jpg_sos_trick, sizeof(jpg_sos_trick), &w, &h);
		if (rc == 0) {
			fprintf(stderr, "expected jpeg_get_dimensions to fail after SOS, got %ux%u\n", w, h);
			return 1;
		}
	}
	printf("jpeg header selftest: OK\n");
	return 0;
}
