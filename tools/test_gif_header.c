#include <stdio.h>

#include "browser/image/gif.h"

static int expect_dims(const uint8_t *data, size_t len, uint32_t ew, uint32_t eh)
{
	uint32_t w = 0, h = 0;
	int rc = gif_get_dimensions(data, len, &w, &h);
	if (rc != 0) {
		fprintf(stderr, "gif_get_dimensions failed\n");
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
	/* Minimal GIF header + logical screen descriptor.
	 * GIF89a + width/height.
	 */
	static const uint8_t gif[] = {
		'G','I','F','8','9','a',
		0x20,0x00, /* w=32 */
		0x10,0x00, /* h=16 */
		0x00, /* packed */
		0x00, /* bg */
		0x00, /* aspect */
	};

	if (expect_dims(gif, sizeof(gif), 32, 16)) return 1;
	printf("gif header selftest: OK\n");
	return 0;
}
