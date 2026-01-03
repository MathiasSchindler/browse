#include "gif_decode.h"

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

struct gif_subblock_stream {
	const uint8_t *p;
	const uint8_t *end;
	uint32_t bitbuf;
	uint32_t bitcount;
	uint8_t block_left;
};

static int gss_init(struct gif_subblock_stream *s, const uint8_t *p, const uint8_t *end)
{
	if (!s || !p || !end || p > end) return -1;
	s->p = p;
	s->end = end;
	s->bitbuf = 0;
	s->bitcount = 0;
	s->block_left = 0;
	return 0;
}

static int gss_next_data_byte(struct gif_subblock_stream *s, uint8_t *out)
{
	if (!s || !out) return -1;
	for (;;) {
		if (s->block_left != 0) {
			if (s->p >= s->end) return -1;
			*out = *s->p++;
			s->block_left--;
			return 0;
		}
		if (s->p >= s->end) return -1;
		uint8_t n = *s->p++;
		if (n == 0) return 1; /* end of image data */
		s->block_left = n;
	}
}

static int gss_read_code(struct gif_subblock_stream *s, uint32_t code_size, uint32_t *out_code)
{
	if (!s || !out_code || code_size == 0 || code_size > 12) return -1;
	while (s->bitcount < code_size) {
		uint8_t b = 0;
		int r = gss_next_data_byte(s, &b);
		if (r != 0) return -1;
		s->bitbuf |= ((uint32_t)b) << s->bitcount;
		s->bitcount += 8;
	}
	uint32_t mask = (code_size == 32) ? 0xffffffffu : ((1u << code_size) - 1u);
	*out_code = s->bitbuf & mask;
	s->bitbuf >>= code_size;
	s->bitcount -= code_size;
	return 0;
}

static int is_gif_sig(const uint8_t *d, size_t n)
{
	if (!d || n < 6) return 0;
	if (!(d[0] == 'G' && d[1] == 'I' && d[2] == 'F' && d[3] == '8')) return 0;
	if (!(d[4] == '7' || d[4] == '9')) return 0;
	if (d[5] != 'a') return 0;
	return 1;
}

static int gif_interlace_next_row(uint32_t h, uint32_t *io_pass, uint32_t *io_y)
{
	/* Passes: (start,step) = (0,8),(4,8),(2,4),(1,2) */
	static const uint8_t starts[4] = {0,4,2,1};
	static const uint8_t steps[4]  = {8,8,4,2};
	uint32_t pass = *io_pass;
	uint32_t y = *io_y;
	for (;;) {
		y += (uint32_t)steps[pass];
		if (y < h) {
			*io_y = y;
			return 0;
		}
		pass++;
		if (pass >= 4) return 1;
		y = (uint32_t)starts[pass];
		if (y < h) {
			*io_pass = pass;
			*io_y = y;
			return 0;
		}
	}
}

int gif_decode_first_frame_xrgb(const uint8_t *data,
			       size_t len,
			       uint32_t *out_pixels,
			       size_t out_cap_pixels,
			       uint32_t *out_w,
			       uint32_t *out_h)
{
	if (!data || !out_pixels || !out_w || !out_h) return -1;
	*out_w = 0;
	*out_h = 0;
	if (!is_gif_sig(data, len)) return -1;
	if (len < 13) return -1;

	uint32_t screen_w = (uint32_t)le16(&data[6]);
	uint32_t screen_h = (uint32_t)le16(&data[8]);
	if (screen_w == 0 || screen_h == 0) return -1;

	uint8_t packed = data[10];
	uint8_t gct_flag = (packed & 0x80u) ? 1u : 0u;
	uint8_t gct_pow = (packed & 0x07u);
	uint32_t gct_size = gct_flag ? (uint32_t)(1u << (gct_pow + 1u)) : 0u;

	size_t p = 13;
	const uint8_t *gct = 0;
	if (gct_flag) {
		size_t need = (size_t)gct_size * 3u;
		if (p + need > len) return -1;
		gct = &data[p];
		p += need;
	}

	/* Walk blocks until first image descriptor. */
	for (;;) {
		if (p >= len) return -1;
		uint8_t sep = data[p++];
		if (sep == 0x3B) {
			/* Trailer */
			return -1;
		}
		if (sep == 0x21) {
			/* Extension: skip label + sub-blocks */
			if (p >= len) return -1;
			p++; /* label */
			for (;;) {
				if (p >= len) return -1;
				uint8_t n = data[p++];
				if (n == 0) break;
				if (p + (size_t)n > len) return -1;
				p += (size_t)n;
			}
			continue;
		}
		if (sep != 0x2C) {
			/* Unknown block */
			return -1;
		}

		/* Image descriptor */
		if (p + 9 > len) return -1;
		/* left/top ignored */
		uint32_t img_w = (uint32_t)le16(&data[p + 4]);
		uint32_t img_h = (uint32_t)le16(&data[p + 6]);
		uint8_t ipacked = data[p + 8];
		p += 9;
		if (img_w == 0 || img_h == 0) return -1;
		if ((uint64_t)img_w * (uint64_t)img_h > (uint64_t)out_cap_pixels) return -1;

		uint8_t lct_flag = (ipacked & 0x80u) ? 1u : 0u;
		uint8_t interlace = (ipacked & 0x40u) ? 1u : 0u;
		uint8_t lct_pow = (ipacked & 0x07u);
		uint32_t lct_size = lct_flag ? (uint32_t)(1u << (lct_pow + 1u)) : 0u;
		const uint8_t *pal = gct;
		uint32_t pal_size = gct_size;
		if (lct_flag) {
			size_t need = (size_t)lct_size * 3u;
			if (p + need > len) return -1;
			pal = &data[p];
			pal_size = lct_size;
			p += need;
		}
		if (!pal || pal_size == 0) return -1;

		if (p >= len) return -1;
		uint8_t lzw_min = data[p++];
		if (lzw_min < 2 || lzw_min > 11) return -1;

		struct gif_subblock_stream s;
		if (gss_init(&s, &data[p], &data[len]) != 0) return -1;

		uint16_t prefix[4096];
		uint8_t suffix[4096];
		uint8_t stack[4096];

		uint32_t clear_code = 1u << lzw_min;
		uint32_t end_code = clear_code + 1u;
		uint32_t next_code = end_code + 1u;
		uint32_t code_size = (uint32_t)lzw_min + 1u;
		uint32_t code_limit = 1u << code_size;

		for (uint32_t i = 0; i < clear_code; i++) {
			prefix[i] = 0xffffu;
			suffix[i] = (uint8_t)i;
		}

		uint32_t out_count = 0;
		uint32_t x = 0;
		uint32_t y = 0;
		uint32_t pass = 0;
		if (interlace) {
			y = 0;
			pass = 0;
		}

		uint32_t prev_code = 0xffffffffu;
		uint8_t prev_first = 0;

		for (;;) {
			uint32_t code = 0;
			if (gss_read_code(&s, code_size, &code) != 0) return -1;

			if (code == clear_code) {
				/* Reset dictionary */
				code_size = (uint32_t)lzw_min + 1u;
				code_limit = 1u << code_size;
				next_code = end_code + 1u;
				prev_code = 0xffffffffu;
				continue;
			}
			if (code == end_code) {
				break;
			}

			uint32_t cur = code;
			uint32_t sp = 0;
			uint8_t first = 0;

			if (cur >= next_code) {
				/* KwKwK case */
				if (prev_code == 0xffffffffu) return -1;
				cur = prev_code;
				stack[sp++] = prev_first;
			}

			while (cur != 0xffffu) {
				if (cur >= 4096) return -1;
				uint8_t sfx = suffix[cur];
				stack[sp++] = sfx;
				first = sfx;
				uint16_t pre = prefix[cur];
				if (pre == 0xffffu) break;
				cur = (uint32_t)pre;
				if (sp >= sizeof(stack)) return -1;
			}

			prev_first = first;

			/* Add new dictionary entry */
			if (prev_code != 0xffffffffu && next_code < 4096) {
				prefix[next_code] = (uint16_t)prev_code;
				suffix[next_code] = first;
				next_code++;
				if (next_code >= code_limit && code_size < 12) {
					code_size++;
					code_limit <<= 1;
				}
			}

			prev_code = code;

			/* Pop stack to output */
			while (sp > 0) {
				uint8_t idx = stack[--sp];
				uint32_t rgb = 0xff000000u;
				if (idx < pal_size) {
					const uint8_t *c = pal + (size_t)idx * 3u;
					rgb |= ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | (uint32_t)c[2];
				}

				uint32_t oy = y;
				uint32_t ox = x;
				if (ox < img_w && oy < img_h) {
					out_pixels[(size_t)oy * (size_t)img_w + (size_t)ox] = rgb;
				}
				x++;
				out_count++;
				if (x >= img_w) {
					x = 0;
					if (!interlace) {
						y++;
					} else {
						if (gif_interlace_next_row(img_h, &pass, &y) != 0) {
							y = img_h; /* done */
						}
					}
				}
				if (out_count >= img_w * img_h) {
					break;
				}
			}
			if (out_count >= img_w * img_h) break;
		}

		*out_w = img_w;
		*out_h = img_h;
		return 0;
	}
}
