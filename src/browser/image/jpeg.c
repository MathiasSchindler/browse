#include "jpeg.h"

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static int is_sof_marker(uint8_t m)
{
	/* SOF0..SOF3, SOF5..SOF7, SOF9..SOF11, SOF13..SOF15 */
	if (m >= 0xC0 && m <= 0xC3) return 1;
	if (m >= 0xC5 && m <= 0xC7) return 1;
	if (m >= 0xC9 && m <= 0xCB) return 1;
	if (m >= 0xCD && m <= 0xCF) return 1;
	return 0;
}

int jpeg_get_dimensions(const uint8_t *data, size_t len, uint32_t *out_w, uint32_t *out_h)
{
	if (!data || len < 4 || !out_w || !out_h) return -1;
	*out_w = 0;
	*out_h = 0;

	/* SOI */
	if (!(data[0] == 0xFF && data[1] == 0xD8)) return -1;

	size_t i = 2;
	while (i + 1 < len) {
		/* Find marker prefix 0xFF (skip any non-0xFF bytes defensively). */
		while (i < len && data[i] != 0xFF) i++;
		if (i + 1 >= len) break;
		/* Skip fill bytes 0xFF 0xFF ... */
		while (i < len && data[i] == 0xFF) i++;
		if (i >= len) break;
		uint8_t marker = data[i++];
		if (marker == 0x00) {
			/* Stuffed 0xFF00 inside entropy data; ignore. */
			continue;
		}

		/* Standalone markers without length. */
		if (marker == 0xD8 || marker == 0xD9) {
			/* SOI / EOI */
			continue;
		}
		if (marker >= 0xD0 && marker <= 0xD7) {
			/* RST0..RST7 */
			continue;
		}
		if (marker == 0x01) {
			/* TEM */
			continue;
		}

		/* Segment must have a length. */
		if (i + 1 >= len) return -1;
		uint16_t seg_len = be16(&data[i]);
		i += 2;
		if (seg_len < 2) return -1;
		size_t payload_len = (size_t)seg_len - 2u;
		if (i + payload_len > len) return -1;

		if (is_sof_marker(marker)) {
			/* SOF payload: P(1), Y(2), X(2), Nf(1), ... */
			if (payload_len < 6) return -1;
			uint16_t h = be16(&data[i + 1]);
			uint16_t w = be16(&data[i + 3]);
			if (w == 0 || h == 0) return -1;
			*out_w = (uint32_t)w;
			*out_h = (uint32_t)h;
			return 0;
		}

		/* Skip payload. */
		i += payload_len;
	}

	return -1;
}
