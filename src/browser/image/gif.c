#include "gif.h"

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

int gif_get_dimensions(const uint8_t *data, size_t len, uint32_t *out_w, uint32_t *out_h)
{
	if (!data || len < 10 || !out_w || !out_h) return -1;
	*out_w = 0;
	*out_h = 0;

	/* Header: GIF87a or GIF89a */
	if (!(data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8' && (data[4] == '7' || data[4] == '9') && data[5] == 'a')) {
		return -1;
	}

	uint16_t w = le16(&data[6]);
	uint16_t h = le16(&data[8]);
	if (w == 0 || h == 0) return -1;
	*out_w = (uint32_t)w;
	*out_h = (uint32_t)h;
	return 0;
}
