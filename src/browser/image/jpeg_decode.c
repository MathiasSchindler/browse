#include "jpeg_decode.h"

#include "../util.h"

#ifdef JPEG_DECODE_DEBUG
#include <stdarg.h>

static void jdlog_append(char *dst, uint32_t *io_n, uint32_t cap, const char *s)
{
	if (!dst || !io_n || cap == 0 || !s) return;
	uint32_t n = *io_n;
	for (uint32_t i = 0; s[i] && n + 1 < cap; i++) dst[n++] = s[i];
	*io_n = n;
}

static void jdlog_append_u32_dec(char *dst, uint32_t *io_n, uint32_t cap, uint32_t v)
{
	if (!dst || !io_n || cap == 0) return;
	char tmp[11];
	uint32_t t = 0;
	if (v == 0) {
		if (*io_n + 1 < cap) dst[(*io_n)++] = '0';
		return;
	}
	while (v > 0 && t < 10) {
		tmp[t++] = (char)('0' + (v % 10u));
		v /= 10u;
	}
	while (t > 0) {
		if (*io_n + 1 >= cap) break;
		dst[(*io_n)++] = tmp[--t];
	}
}

static void jdlog_append_u32_hex(char *dst, uint32_t *io_n, uint32_t cap, uint32_t v, uint32_t width)
{
	if (!dst || !io_n || cap == 0) return;
	static const char hex[16] = {
		'0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f',
	};
	char tmp[8];
	uint32_t t = 0;
	if (width > 8) width = 8;
	for (uint32_t i = 0; i < width && t < sizeof(tmp); i++) {
		tmp[t++] = hex[v & 0xFu];
		v >>= 4;
	}
	while (t > 0) {
		if (*io_n + 1 >= cap) break;
		dst[(*io_n)++] = tmp[--t];
	}
}

static void jdlogf(const char *fmt, ...)
{
	if (!fmt) return;
	char buf[256];
	uint32_t n = 0;

	va_list ap;
	va_start(ap, fmt);
	for (uint32_t i = 0; fmt[i] && n + 1 < sizeof(buf); i++) {
		char c = fmt[i];
		if (c != '%') {
			buf[n++] = c;
			continue;
		}
		char c1 = fmt[++i];
		if (c1 == 0) break;
		if (c1 == '%') {
			buf[n++] = '%';
			continue;
		}
		uint32_t width = 0;
		if (c1 >= '0' && c1 <= '9') {
			while (c1 >= '0' && c1 <= '9') {
				width = width * 10u + (uint32_t)(c1 - '0');
				c1 = fmt[++i];
				if (c1 == 0) break;
			}
		}
		if (c1 == 'u') {
			uint32_t v = (uint32_t)va_arg(ap, unsigned);
			(void)width;
			jdlog_append_u32_dec(buf, &n, (uint32_t)sizeof(buf), v);
			continue;
		}
		if (c1 == 'x') {
			uint32_t v = (uint32_t)va_arg(ap, unsigned);
			if (width == 0) width = 1;
			jdlog_append_u32_hex(buf, &n, (uint32_t)sizeof(buf), v, width);
			continue;
		}
		if (c1 == 's') {
			const char *s = va_arg(ap, const char *);
			jdlog_append(buf, &n, (uint32_t)sizeof(buf), s ? s : "(null)");
			continue;
		}
		/* Unknown specifier: emit literally. */
		buf[n++] = '%';
		if (n + 1 < sizeof(buf)) buf[n++] = c1;
	}
	va_end(ap);

	if (n) sys_write(2, buf, n);
}

#define JDLOG(...) do { jdlogf(__VA_ARGS__); } while (0)
#else
#define JDLOG(...) do { } while (0)
#endif

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static int clamp_u8(int v)
{
	if (v < 0) return 0;
	if (v > 255) return 255;
	return v;
}

static int is_jpeg_sig(const uint8_t *d, size_t n)
{
	return (d && n >= 2 && d[0] == 0xff && d[1] == 0xd8);
}

struct br {
	const uint8_t *p;
	const uint8_t *end;
	uint32_t bitbuf;
	uint32_t bitcount;
	uint16_t marker; /* 0 if none; else 0xFFxx */
};

static int br_init(struct br *b, const uint8_t *p, const uint8_t *end)
{
	if (!b || !p || !end || p > end) return -1;
	b->p = p;
	b->end = end;
	b->bitbuf = 0;
	b->bitcount = 0;
	b->marker = 0;
	return 0;
}

static int br_next_byte_entropy(struct br *b, uint8_t *out)
{
	if (!b || !out) return -1;
	if (b->marker) return -1;
	if (b->p >= b->end) return -1;
	uint8_t v = *b->p++;
	if (v != 0xff) {
		*out = v;
		return 0;
	}
	/* 0xFF: stuffed 0x00 => literal 0xFF, otherwise marker */
	if (b->p >= b->end) return -1;
	uint8_t n = *b->p++;
	if (n == 0x00) {
		*out = 0xff;
		return 0;
	}
	b->marker = (uint16_t)(0xff00u | n);
	return -1;
}

static int br_ensure_bits(struct br *b, uint32_t need)
{
	if (!b) return -1;
	if (need > 24) return -1;
	while (b->bitcount < need) {
		uint8_t byte = 0;
		if (br_next_byte_entropy(b, &byte) != 0) return -1;
		b->bitbuf = (b->bitbuf << 8) | (uint32_t)byte;
		b->bitcount += 8;
	}
	return 0;
}

static int br_get_bit(struct br *b, uint32_t *out_bit)
{
	uint32_t v = 0;
	if (br_ensure_bits(b, 1) != 0) return -1;
	uint32_t shift = b->bitcount - 1u;
	v = (b->bitbuf >> shift) & 1u;
	b->bitcount -= 1u;
	if (b->bitcount == 0) {
		b->bitbuf = 0;
	} else {
		b->bitbuf &= (1u << b->bitcount) - 1u;
	}
	*out_bit = v;
	return 0;
}

static int br_get_bits(struct br *b, uint32_t nbits, uint32_t *out)
{
	if (!b || !out) return -1;
	if (nbits == 0) {
		*out = 0;
		return 0;
	}
	if (nbits > 24) return -1;
	if (br_ensure_bits(b, nbits) != 0) return -1;
	uint32_t shift = b->bitcount - nbits;
	uint32_t mask = (nbits == 32) ? 0xffffffffu : ((1u << nbits) - 1u);
	uint32_t v = (b->bitbuf >> shift) & mask;
	b->bitcount -= nbits;
	if (b->bitcount == 0) {
		b->bitbuf = 0;
	} else {
		b->bitbuf &= (1u << b->bitcount) - 1u;
	}
	*out = v;
	return 0;
}

static int32_t extend_sign(uint32_t v, uint32_t nbits)
{
	if (nbits == 0) return 0;
	uint32_t vt = 1u << (nbits - 1u);
	if (v >= vt) return (int32_t)v;
	return (int32_t)v - (int32_t)((1u << nbits) - 1u);
}

struct huff {
	uint8_t bits[17];
	uint8_t hval[256];
	int32_t mincode[17];
	int32_t maxcode[17];
	int32_t valptr[17];
	uint16_t nvals;
	uint8_t valid;
};

static int huff_build(struct huff *h)
{
	if (!h) return -1;
	int32_t code = 0;
	int32_t k = 0;
	for (int i = 1; i <= 16; i++) {
		if (h->bits[i] == 0) {
			h->mincode[i] = -1;
			h->maxcode[i] = -1;
			h->valptr[i] = -1;
			code <<= 1;
			continue;
		}
		h->valptr[i] = k;
		h->mincode[i] = code;
		code += (int32_t)h->bits[i] - 1;
		h->maxcode[i] = code;
		k += (int32_t)h->bits[i];
		code++;
		code <<= 1;
	}
	h->nvals = (uint16_t)k;
	h->valid = 1;
	return 0;
}

static int huff_decode(struct br *b, const struct huff *h, uint32_t *out_sym)
{
	if (!b || !h || !out_sym || !h->valid) return -1;
	int32_t code = 0;
	for (int i = 1; i <= 16; i++) {
		uint32_t bit = 0;
		if (br_get_bit(b, &bit) != 0) return -1;
		code = (code << 1) | (int32_t)bit;
		int32_t maxc = h->maxcode[i];
		if (maxc >= 0 && code <= maxc) {
			int32_t idx = h->valptr[i] + (code - h->mincode[i]);
			if (idx < 0 || (uint32_t)idx >= h->nvals) return -1;
			*out_sym = (uint32_t)h->hval[idx];
			return 0;
		}
	}
	return -1;
}

static const uint8_t zigzag[64] = {
	0, 1, 8, 16, 9, 2, 3, 10,
	17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34,
	27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36,
	29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46,
	53, 60, 61, 54, 47, 55, 62, 63,
};

/* Fixed-point IDCT via separable matrix multiply.
 * Scale chosen to keep precision while avoiding overflow with int64 intermediates.
 */
#define IDCT_SCALE 16384
static const int32_t idct_m[8][8] = {
	{11585, 16069, 15137, 13623, 11585,  9102,  6270,  3196},
	{11585, 13623,  6270, -3196,-11585,-16069,-15137, -9102},
	{11585,  9102, -6270,-16069,-11585,  3196, 15137, 13623},
	{11585,  3196,-15137, -9102, 11585, 13623, -6270,-16069},
	{11585, -3196,-15137,  9102, 11585,-13623, -6270, 16069},
	{11585, -9102, -6270, 16069,-11585, -3196, 15137,-13623},
	{11585,-13623,  6270,  3196,-11585, 16069,-15137,  9102},
	{11585,-16069, 15137,-13623, 11585, -9102,  6270, -3196},
};

static void idct8x8(const int32_t *in, uint8_t *out, size_t out_stride)
{
	int32_t tmp[64];
	const int64_t denom = (int64_t)IDCT_SCALE * 2;
	for (int y = 0; y < 8; y++) {
		for (int x = 0; x < 8; x++) {
			int64_t s = 0;
			for (int u = 0; u < 8; u++) {
				s += (int64_t)idct_m[x][u] * (int64_t)in[y * 8 + u];
			}
			/* Back to coefficient domain scale.
			 * The basis matrix is scaled but does not include the 1/2 factor for the
			 * 1D IDCT, so we apply it here by dividing by (2*IDCT_SCALE).
			 */
			tmp[y * 8 + x] = (int32_t)((s + (denom / 2)) / denom);
		}
	}
	for (int y = 0; y < 8; y++) {
		for (int x = 0; x < 8; x++) {
			int64_t s = 0;
			for (int v = 0; v < 8; v++) {
				s += (int64_t)idct_m[y][v] * (int64_t)tmp[v * 8 + x];
			}
			int32_t v0 = (int32_t)((s + (denom / 2)) / denom);
			v0 += 128;
			out[(size_t)y * out_stride + (size_t)x] = (uint8_t)clamp_u8(v0);
		}
	}
}

struct comp {
	uint8_t id;
	uint8_t hs;
	uint8_t vs;
	uint8_t tq;
	uint8_t td;
	uint8_t ta;
	int32_t dc_pred;
	uint8_t mcu_buf[16 * 16];
};

static struct comp *comp_find_by_id(struct comp *comps, uint32_t ncomp, uint8_t id)
{
	if (!comps) return 0;
	for (uint32_t i = 0; i < ncomp; i++) {
		if (comps[i].id == id) return &comps[i];
	}
	return 0;
}

static int decode_block(struct br *b,
			const struct huff *hdc,
			const struct huff *hac,
			const uint16_t *qt,
			int32_t *io_dc_pred,
			uint8_t *out8,
			size_t out_stride)
{
	int32_t coef[64];
	for (int i = 0; i < 64; i++) coef[i] = 0;

	uint32_t sym = 0;
	if (huff_decode(b, hdc, &sym) != 0) {
		JDLOG("DC huff_decode failed (marker=%04x bitcount=%u)\n", (unsigned)b->marker, (unsigned)b->bitcount);
		return -1;
	}
	uint32_t s = sym & 0x0fu;
	uint32_t bits = 0;
	if (br_get_bits(b, s, &bits) != 0) {
		JDLOG("DC br_get_bits failed s=%u (marker=%04x)\n", (unsigned)s, (unsigned)b->marker);
		return -1;
	}
	int32_t diff = extend_sign(bits, s);
	int32_t dc = *io_dc_pred + diff;
	*io_dc_pred = dc;
	coef[0] = dc;

	int k = 1;
	while (k < 64) {
		if (huff_decode(b, hac, &sym) != 0) {
			JDLOG("AC huff_decode failed (marker=%04x bitcount=%u)\n", (unsigned)b->marker, (unsigned)b->bitcount);
			return -1;
		}
		if (sym == 0) break; /* EOB */
		uint32_t run = (sym >> 4) & 0x0fu;
		uint32_t ss = sym & 0x0fu;
		if (sym == 0xf0) {
			k += 16;
			continue;
		}
		k += (int)run;
		if (k >= 64) return -1;
		uint32_t ab = 0;
		if (br_get_bits(b, ss, &ab) != 0) {
			JDLOG("AC br_get_bits failed ss=%u (marker=%04x)\n", (unsigned)ss, (unsigned)b->marker);
			return -1;
		}
		int32_t ac = extend_sign(ab, ss);
		coef[zigzag[k]] = ac;
		k++;
	}

	int32_t deq[64];
	for (int i = 0; i < 64; i++) {
		deq[i] = coef[i] * (int32_t)qt[i];
	}
	idct8x8(deq, out8, out_stride);
	return 0;
}

static void ycbcr_to_xrgb(uint32_t *dst, uint32_t y, uint32_t cb, uint32_t cr)
{
	int32_t Y = (int32_t)y;
	int32_t Cb = (int32_t)cb - 128;
	int32_t Cr = (int32_t)cr - 128;
	int32_t R = Y + (int32_t)((91881 * Cr + 32768) >> 16);
	int32_t G = Y - (int32_t)((22554 * Cb + 46802 * Cr + 32768) >> 16);
	int32_t B = Y + (int32_t)((116130 * Cb + 32768) >> 16);
	uint32_t r = (uint32_t)clamp_u8(R);
	uint32_t g = (uint32_t)clamp_u8(G);
	uint32_t b = (uint32_t)clamp_u8(B);
	*dst = 0xff000000u | (r << 16) | (g << 8) | b;
}

static uint8_t sample_bilinear_16stride(const uint8_t *src,
					uint32_t src_w,
					uint32_t src_h,
					uint32_t dst_x,
					uint32_t dst_y,
					uint32_t dst_w,
					uint32_t dst_h)
{
	if (!src || src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) return 0;

	/* Map dst pixel center to src space and do bilinear interpolation.
	 * Using 16.16 fixed point.
	 */
	int32_t sx_fp = (int32_t)(((((uint64_t)dst_x << 16) + 32768u) * (uint64_t)src_w) / (uint64_t)dst_w);
	int32_t sy_fp = (int32_t)(((((uint64_t)dst_y << 16) + 32768u) * (uint64_t)src_h) / (uint64_t)dst_h);
	sx_fp -= 32768;
	sy_fp -= 32768;
	if (sx_fp < 0) sx_fp = 0;
	if (sy_fp < 0) sy_fp = 0;
	int32_t max_sx = (int32_t)((src_w - 1u) << 16);
	int32_t max_sy = (int32_t)((src_h - 1u) << 16);
	if (sx_fp > max_sx) sx_fp = max_sx;
	if (sy_fp > max_sy) sy_fp = max_sy;

	uint32_t sx0 = (uint32_t)(sx_fp >> 16);
	uint32_t sy0 = (uint32_t)(sy_fp >> 16);
	uint32_t fx = (uint32_t)(sx_fp & 0xffff);
	uint32_t fy = (uint32_t)(sy_fp & 0xffff);
	uint32_t sx1 = (sx0 + 1u < src_w) ? (sx0 + 1u) : sx0;
	uint32_t sy1 = (sy0 + 1u < src_h) ? (sy0 + 1u) : sy0;

	uint32_t s00 = src[sy0 * 16u + sx0];
	uint32_t s01 = src[sy0 * 16u + sx1];
	uint32_t s10 = src[sy1 * 16u + sx0];
	uint32_t s11 = src[sy1 * 16u + sx1];

	uint32_t w0 = 65536u - fx;
	uint32_t w1 = fx;
	uint32_t t0 = (s00 * w0 + s01 * w1 + 32768u) >> 16;
	uint32_t t1 = (s10 * w0 + s11 * w1 + 32768u) >> 16;
	uint32_t wy0 = 65536u - fy;
	uint32_t wy1 = fy;
	uint32_t v = (t0 * wy0 + t1 * wy1 + 32768u) >> 16;
	if (v > 255u) v = 255u;
	return (uint8_t)v;
}

static void upsample_to_mcu_16stride(uint8_t *dst,
				     const uint8_t *src,
				     uint32_t src_w,
				     uint32_t src_h,
				     uint32_t dst_w,
				     uint32_t dst_h)
{
	if (!dst || !src) return;
	if (src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) return;
	if (dst_w > 16u) dst_w = 16u;
	if (dst_h > 16u) dst_h = 16u;
	if (src_w > 16u) src_w = 16u;
	if (src_h > 16u) src_h = 16u;

	for (uint32_t y = 0; y < dst_h; y++) {
		for (uint32_t x = 0; x < dst_w; x++) {
			dst[y * 16u + x] = sample_bilinear_16stride(src, src_w, src_h, x, y, dst_w, dst_h);
		}
	}
}

int jpeg_decode_baseline_xrgb(const uint8_t *data,
			     size_t len,
			     uint32_t *out_pixels,
			     size_t out_cap_pixels,
			     uint32_t *out_w,
			     uint32_t *out_h)
{
	if (!out_w || !out_h) return -1;
	*out_w = 0;
	*out_h = 0;
	if (!data || !out_pixels) return -1;
	if (!is_jpeg_sig(data, len)) {
		JDLOG("not a jpeg\n");
		return -1;
	}

	uint16_t qt[4][64];
	uint8_t qt_valid[4] = {0,0,0,0};
	struct huff hdc[4];
	struct huff hac[4];
	for (int i = 0; i < 4; i++) {
		c_memset(&hdc[i], 0, sizeof(hdc[i]));
		c_memset(&hac[i], 0, sizeof(hac[i]));
	}

	struct comp comps[3];
	uint32_t ncomp = 0;
	c_memset(comps, 0, sizeof(comps));

	uint32_t width = 0, height = 0;
	uint16_t restart_interval = 0;

	size_t p = 2;
	while (p + 4 <= len) {
		if (data[p] != 0xff) return -1;
		while (p < len && data[p] == 0xff) p++;
		if (p >= len) return -1;
		uint8_t marker = data[p++];
		if (marker == 0xd9) break; /* EOI */
		if (marker == 0xda) {
			/* SOS */
			if (p + 2 > len) return -1;
			uint16_t seglen = be16(&data[p]);
			if (seglen < 2 || p + seglen > len) return -1;
			if (p + 2 + 1 > len) return -1;
			uint8_t ns = data[p + 2];
			if (ns != ncomp) {
				JDLOG("SOS ns=%u ncomp=%u\n", (unsigned)ns, (unsigned)ncomp);
				return -1;
			}
			/* Per-component huffman selectors */
			size_t sp = p + 3;
			for (uint32_t i = 0; i < ns; i++) {
				if (sp + 2 > p + seglen) return -1;
				uint8_t cid = data[sp++];
				uint8_t sel = data[sp++];
				uint8_t td = (sel >> 4) & 0x0f;
				uint8_t ta = sel & 0x0f;
				int found = 0;
				for (uint32_t k = 0; k < ncomp; k++) {
					if (comps[k].id == cid) {
						comps[k].td = td;
						comps[k].ta = ta;
						found = 1;
						break;
					}
				}
				if (!found) return -1;
			}
			/* skip Ss/Se/AhAl */
			p += seglen;
			/* entropy-coded segment starts at p */
			break;
		}

		if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) {
			continue; /* standalone */
		}
		if (p + 2 > len) return -1;
		uint16_t seglen = be16(&data[p]);
		if (seglen < 2 || p + seglen > len) return -1;
		size_t seg = p + 2;

		switch (marker) {
			case 0xdb: {
				/* DQT */
				size_t q = seg;
				while (q < p + seglen) {
					if (q >= p + seglen) return -1;
					uint8_t pq_tq = data[q++];
					uint8_t pq = (pq_tq >> 4) & 0x0f;
					uint8_t tq = pq_tq & 0x0f;
					if (tq >= 4) return -1;
					if (pq != 0) return -1; /* only 8-bit */
					if (q + 64 > p + seglen) return -1;
					for (int i = 0; i < 64; i++) {
						qt[tq][zigzag[i]] = (uint16_t)data[q++];
					}
					qt_valid[tq] = 1;
				}
				break;
			}
			case 0xc0: {
				/* SOF0 */
				if (seg + 6 > p + seglen) return -1;
				uint8_t prec = data[seg + 0];
				if (prec != 8) return -1;
				height = (uint32_t)be16(&data[seg + 1]);
				width = (uint32_t)be16(&data[seg + 3]);
				ncomp = (uint32_t)data[seg + 5];
				if (width == 0 || height == 0) return -1;
				if (!(ncomp == 1 || ncomp == 3)) return -1;
				if ((uint64_t)width * (uint64_t)height > (uint64_t)out_cap_pixels) return -1;
				if (seg + 6 + 3 * ncomp > p + seglen) return -1;
				size_t cp = seg + 6;
				for (uint32_t i = 0; i < ncomp; i++) {
					comps[i].id = data[cp++];
					uint8_t hv = data[cp++];
					comps[i].hs = (hv >> 4) & 0x0f;
					comps[i].vs = hv & 0x0f;
					comps[i].tq = data[cp++];
					comps[i].dc_pred = 0;
					if (comps[i].hs == 0 || comps[i].vs == 0) return -1;
					if (comps[i].hs > 2 || comps[i].vs > 2) return -1;
					if (comps[i].tq >= 4) return -1;
				}
				JDLOG("SOF0 %ux%u ncomp=%u\n", (unsigned)width, (unsigned)height, (unsigned)ncomp);
				break;
			}
			case 0xc4: {
				/* DHT */
				size_t q = seg;
				while (q < p + seglen) {
					if (q >= p + seglen) return -1;
					uint8_t tc_th = data[q++];
					uint8_t tc = (tc_th >> 4) & 0x0f;
					uint8_t th = tc_th & 0x0f;
					if (th >= 4) return -1;
					struct huff *h = (tc == 0) ? &hdc[th] : (tc == 1) ? &hac[th] : 0;
					if (!h) return -1;
					uint32_t total = 0;
					h->bits[0] = 0;
					for (int i = 1; i <= 16; i++) {
						if (q >= p + seglen) return -1;
						h->bits[i] = data[q++];
						total += h->bits[i];
					}
					if (total > 256) return -1;
					if (q + total > p + seglen) return -1;
					for (uint32_t i = 0; i < total; i++) {
						h->hval[i] = data[q++];
					}
					huff_build(h);
				}
				break;
			}
			case 0xdd: {
				/* DRI */
				if (seg + 2 > p + seglen) return -1;
				restart_interval = be16(&data[seg]);
				break;
			}
			default:
				break;
		}
		p += seglen;
	}

	if (width == 0 || height == 0 || ncomp == 0) return -1;
	for (uint32_t i = 0; i < ncomp; i++) {
		if (!qt_valid[comps[i].tq]) {
			JDLOG("missing qt %u\n", (unsigned)comps[i].tq);
			return -1;
		}
		if (comps[i].td >= 4 || comps[i].ta >= 4) {
			JDLOG("bad huff selectors td=%u ta=%u\n", (unsigned)comps[i].td, (unsigned)comps[i].ta);
			return -1;
		}
		if (!hdc[comps[i].td].valid || !hac[comps[i].ta].valid) {
			JDLOG("missing huff table dc=%u ac=%u\n", (unsigned)comps[i].td, (unsigned)comps[i].ta);
			return -1;
		}
	}
	if (p >= len) return -1;

	uint32_t max_h = 1, max_v = 1;
	for (uint32_t i = 0; i < ncomp; i++) {
		if (comps[i].hs > max_h) max_h = comps[i].hs;
		if (comps[i].vs > max_v) max_v = comps[i].vs;
	}
	uint32_t mcu_w = max_h * 8u;
	uint32_t mcu_h = max_v * 8u;
	uint32_t mcus_x = (width + mcu_w - 1u) / mcu_w;
	uint32_t mcus_y = (height + mcu_h - 1u) / mcu_h;

	struct br b;
	if (br_init(&b, &data[p], &data[len]) != 0) return -1;
	uint32_t rst_left = restart_interval ? restart_interval : 0xffffffffu;
	uint32_t rst_index = 0;

	for (uint32_t my = 0; my < mcus_y; my++) {
		for (uint32_t mx = 0; mx < mcus_x; mx++) {
			/* Restart handling */
			if (restart_interval) {
				if (rst_left == 0) {
					/* Consume pending marker if present; otherwise try to align by draining to marker. */
					b.bitbuf = 0;
					b.bitcount = 0;
					for (uint32_t ci = 0; ci < ncomp; ci++) comps[ci].dc_pred = 0;
					rst_left = restart_interval;
					rst_index = (rst_index + 1u) & 7u;
				}
				rst_left--;
			}

			/* Decode blocks into per-component MCU buffers. */
			for (uint32_t ci = 0; ci < ncomp; ci++) {
				struct comp *c = &comps[ci];
				uint32_t cw = c->hs * 8u;
				uint32_t ch = c->vs * 8u;
				/* Clear MCU buffer (only the used region). */
				for (uint32_t yy = 0; yy < ch; yy++) {
					for (uint32_t xx = 0; xx < cw; xx++) {
						c->mcu_buf[yy * 16u + xx] = 0;
					}
				}
				for (uint32_t by = 0; by < c->vs; by++) {
					for (uint32_t bx = 0; bx < c->hs; bx++) {
						uint8_t *dst = &c->mcu_buf[(by * 8u) * 16u + (bx * 8u)];
						if (decode_block(&b,
								&hdc[c->td],
								&hac[c->ta],
								qt[c->tq],
								&c->dc_pred,
								dst,
								16u) != 0) {
							JDLOG("decode_block failed at mcu (%u,%u) comp=%u block(%u,%u)\n",
							      (unsigned)mx, (unsigned)my, (unsigned)ci, (unsigned)bx, (unsigned)by);
							return -1;
						}
					}
				}
			}

			/* If components are subsampled relative to the MCU size, upsample them
			 * with bilinear filtering to avoid blocky/chunky chroma.
			 */
			uint8_t y_up[16u * 16u];
			uint8_t cb_up[16u * 16u];
			uint8_t cr_up[16u * 16u];
			int have_y_up = 0;
			int have_cb_up = 0;
			int have_cr_up = 0;
			struct comp *y = 0;
			struct comp *cb = 0;
			struct comp *cr = 0;
			if (ncomp >= 1) {
				y = comp_find_by_id(comps, ncomp, 1);
				if (!y) y = &comps[0];
				uint32_t yw = y->hs * 8u;
				uint32_t yh = y->vs * 8u;
				if (y->hs != max_h || y->vs != max_v) {
					upsample_to_mcu_16stride(y_up, y->mcu_buf, yw, yh, mcu_w, mcu_h);
					have_y_up = 1;
				}
			}
			if (ncomp > 1) {
				cb = comp_find_by_id(comps, ncomp, 2);
				cr = comp_find_by_id(comps, ncomp, 3);
				if (!cb) cb = (ncomp > 1) ? &comps[1] : 0;
				if (!cr) cr = (ncomp > 2) ? &comps[2] : cb;
				uint32_t cbw = cb->hs * 8u;
				uint32_t cbh = cb->vs * 8u;
				uint32_t crw = cr->hs * 8u;
				uint32_t crh = cr->vs * 8u;
				if (cb->hs != max_h || cb->vs != max_v) {
					upsample_to_mcu_16stride(cb_up, cb->mcu_buf, cbw, cbh, mcu_w, mcu_h);
					have_cb_up = 1;
				}
				if (cr->hs != max_h || cr->vs != max_v) {
					upsample_to_mcu_16stride(cr_up, cr->mcu_buf, crw, crh, mcu_w, mcu_h);
					have_cr_up = 1;
				}
			}

			/* Write pixels for this MCU. */
			for (uint32_t py = 0; py < mcu_h; py++) {
				uint32_t iy = my * mcu_h + py;
				if (iy >= height) break;
				for (uint32_t px = 0; px < mcu_w; px++) {
					uint32_t ix = mx * mcu_w + px;
					if (ix >= width) break;
					uint8_t Y = 0;
					uint8_t Cb = 128;
					uint8_t Cr = 128;
					if (ncomp == 1) {
						struct comp *c = &comps[0];
						if (have_y_up) {
							Y = y_up[py * 16u + px];
						} else {
							uint32_t cx = (px * c->hs) / max_h;
							uint32_t cy = (py * c->vs) / max_v;
							Y = c->mcu_buf[cy * 16u + cx];
						}
						uint32_t g = (uint32_t)Y;
						out_pixels[(size_t)iy * (size_t)width + (size_t)ix] = 0xff000000u | (g << 16) | (g << 8) | g;
					} else {
						/* Use the component-id mapping established above if present. */
						if (!y) { y = comp_find_by_id(comps, ncomp, 1); if (!y) y = &comps[0]; }
						if (!cb) { cb = comp_find_by_id(comps, ncomp, 2); if (!cb) cb = (ncomp > 1) ? &comps[1] : y; }
						if (!cr) { cr = comp_find_by_id(comps, ncomp, 3); if (!cr) cr = (ncomp > 2) ? &comps[2] : cb; }
						if (have_y_up) {
							Y = y_up[py * 16u + px];
						} else {
							uint32_t yx = (px * y->hs) / max_h;
							uint32_t yy = (py * y->vs) / max_v;
							Y = y->mcu_buf[yy * 16u + yx];
						}
						if (have_cb_up) {
							Cb = cb_up[py * 16u + px];
						} else {
							uint32_t cbx = (px * cb->hs) / max_h;
							uint32_t cby = (py * cb->vs) / max_v;
							Cb = cb->mcu_buf[cby * 16u + cbx];
						}
						if (have_cr_up) {
							Cr = cr_up[py * 16u + px];
						} else {
							uint32_t crx = (px * cr->hs) / max_h;
							uint32_t cry = (py * cr->vs) / max_v;
							Cr = cr->mcu_buf[cry * 16u + crx];
						}
						ycbcr_to_xrgb(&out_pixels[(size_t)iy * (size_t)width + (size_t)ix], Y, Cb, Cr);
					}
				}
			}
		}
	}

	*out_w = width;
	*out_h = height;
	return 0;
}
