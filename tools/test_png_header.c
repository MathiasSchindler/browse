#include <stdio.h>

#include "browser/image/png.h"

static int expect_dims(const uint8_t *data, size_t len, uint32_t ew, uint32_t eh)
{
	uint32_t w = 0, h = 0;
	int rc = png_get_dimensions(data, len, &w, &h);
	if (rc != 0) {
		fprintf(stderr, "png_get_dimensions failed\n");
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
	/* Minimal PNG with IHDR only (CRC not validated by parser).
	 * Signature + IHDR chunk (len=13).
	 */
	static const uint8_t png[] = {
		0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a,
		0x00,0x00,0x00,0x0d, 'I','H','D','R',
		0x00,0x00,0x00,0x20, /* w=32 */
		0x00,0x00,0x00,0x10, /* h=16 */
		0x08, /* bit depth */
		0x02, /* color type */
		0x00, /* compression */
		0x00, /* filter */
		0x00, /* interlace */
		0x00,0x00,0x00,0x00, /* crc dummy */
	};

	if (expect_dims(png, sizeof(png), 32, 16)) return 1;
	printf("png header selftest: OK\n");
	return 0;
}
