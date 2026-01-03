#include "png.h"

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int png_get_dimensions(const uint8_t *data, size_t len, uint32_t *out_w, uint32_t *out_h)
{
	if (!data || len < 24 || !out_w || !out_h) return -1;
	*out_w = 0;
	*out_h = 0;

	static const uint8_t sig[8] = { 0x89,'P','N','G', 0x0d,0x0a,0x1a,0x0a };
	for (size_t i = 0; i < 8; i++) {
		if (data[i] != sig[i]) return -1;
	}

	/* First chunk should be IHDR with length 13.
	 * Layout: len(4) type(4) data(13) crc(4)
	 */
	uint32_t chunk_len = be32(&data[8]);
	if (chunk_len != 13) return -1;
	if (!(data[12] == 'I' && data[13] == 'H' && data[14] == 'D' && data[15] == 'R')) return -1;

	uint32_t w = be32(&data[16]);
	uint32_t h = be32(&data[20]);
	if (w == 0 || h == 0) return -1;
	*out_w = w;
	*out_h = h;
	return 0;
}
