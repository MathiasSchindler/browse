#include "png_decode.h"

#include "../util.h"

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int is_png_sig(const uint8_t *d, size_t n)
{
	static const uint8_t sig[8] = {0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a};
	if (!d || n < 8) return 0;
	for (size_t i = 0; i < 8; i++) if (d[i] != sig[i]) return 0;
	return 1;
}

static inline uint32_t png_alpha_bg(uint32_t x, uint32_t y)
{
	/* Light checkerboard to make transparency visible regardless of UI theme.
	 * 8x8 squares.
	 */
	uint32_t a = ((x >> 3) ^ (y >> 3)) & 1u;
	return a ? 0xfff0f0f0u : 0xffd0d0d0u;
}

static inline uint8_t png_idx_packed(const uint8_t *row, uint32_t x, uint8_t bit_depth)
{
	/* Palette/grayscale packed samples are stored MSB-first within each byte. */
	uint32_t bitpos = x * (uint32_t)bit_depth;
	uint32_t byte_i = bitpos >> 3;
	uint32_t in_byte = bitpos & 7u;
	uint32_t shift = 8u - (uint32_t)bit_depth - in_byte;
	uint8_t mask = (uint8_t)((1u << bit_depth) - 1u);
	return (uint8_t)((row[byte_i] >> shift) & mask);
}

/* --- zlib/deflate (minimal, no dictionary) --- */
struct seg_reader {
	const uint8_t *const *segs;
	const size_t *lens;
	size_t nseg;
	size_t si;
	size_t off;
};

static int seg_get_byte(struct seg_reader *r, uint8_t *out)
{
	if (!r || !out) return -1;
	while (r->si < r->nseg) {
		const uint8_t *p = r->segs[r->si];
		size_t n = r->lens[r->si];
		if (r->off < n) {
			*out = p[r->off++];
			return 0;
		}
		r->si++;
		r->off = 0;
	}
	return -1;
}

struct bitr {
	struct seg_reader *r;
	uint32_t bitbuf;
	uint32_t bitcount;
};

static int br_need(struct bitr *b, uint32_t n)
{
	while (b->bitcount < n) {
		uint8_t by = 0;
		if (seg_get_byte(b->r, &by) != 0) return -1;
		b->bitbuf |= ((uint32_t)by) << b->bitcount;
		b->bitcount += 8;
	}
	return 0;
}

static int br_get_bits(struct bitr *b, uint32_t n, uint32_t *out)
{
	if (!out) return -1;
	if (n == 0) { *out = 0; return 0; }
	if (n > 24) return -1;
	if (br_need(b, n) != 0) return -1;
	uint32_t v = b->bitbuf & ((1u << n) - 1u);
	b->bitbuf >>= n;
	b->bitcount -= n;
	*out = v;
	return 0;
}

static int br_get_byte(struct bitr *b, uint8_t *out)
{
	uint32_t v = 0;
	if (br_get_bits(b, 8, &v) != 0) return -1;
	*out = (uint8_t)v;
	return 0;
}

static int br_align_byte(struct bitr *b)
{
	uint32_t drop = b->bitcount & 7u;
	if (drop) {
		b->bitbuf >>= drop;
		b->bitcount -= drop;
	}
	return 0;
}

struct htab {
	/* Canonical Huffman decoding via fast table for up to 9 bits + slow fallback */
	uint16_t sym[1u << 9];
	uint8_t  len[1u << 9];
	uint16_t first_code[16 + 1];
	uint16_t first_sym[16 + 1];
	uint16_t count[16 + 1];
	uint16_t syms[288];
	uint8_t maxbits;
	uint8_t valid;
};

static int htab_build(struct htab *h, const uint8_t *lens, uint32_t nsyms)
{
	if (!h || !lens || nsyms == 0 || nsyms > 288) return -1;
	c_memset(h, 0, sizeof(*h));
	uint16_t cnt[16 + 1];
	c_memset(cnt, 0, sizeof(cnt));
	uint8_t maxb = 0;
	for (uint32_t i = 0; i < nsyms; i++) {
		uint8_t L = lens[i];
		if (L > 15) return -1;
		if (L) { cnt[L]++; if (L > maxb) maxb = L; }
	}
	h->maxbits = maxb;
	for (int i = 0; i <= 15; i++) h->count[i] = cnt[i];
	uint16_t code = 0;
	uint16_t sym = 0;
	for (int bits = 1; bits <= 15; bits++) {
		code = (uint16_t)((code + cnt[bits - 1]) << 1);
		h->first_code[bits] = code;
		h->first_sym[bits] = sym;
		sym = (uint16_t)(sym + cnt[bits]);
		if (sym > nsyms) return -1;
	}
	/* Build symbol list ordered by (len, code). */
	uint16_t next[16 + 1];
	for (int bits = 1; bits <= 15; bits++) next[bits] = h->first_sym[bits];
	for (uint32_t s = 0; s < nsyms; s++) {
		uint8_t L = lens[s];
		if (!L) continue;
		uint16_t idx = next[L]++;
		if (idx >= nsyms) return -1;
		h->syms[idx] = (uint16_t)s;
	}
	/* Fast table for first 9 bits */
	for (uint32_t i = 0; i < (1u << 9); i++) {
		h->len[i] = 0;
		h->sym[i] = 0;
	}
	for (int bits = 1; bits <= 9; bits++) {
		uint16_t fc = h->first_code[bits];
		uint16_t fs = h->first_sym[bits];
		uint16_t c = h->count[bits];
		for (uint16_t j = 0; j < c; j++) {
			uint16_t codev = (uint16_t)(fc + j);
			uint16_t symv = h->syms[fs + j];
			/* reverse bits for LSB-first deflate */
			uint16_t r = 0;
			for (int k = 0; k < bits; k++) r = (uint16_t)((r << 1) | ((codev >> k) & 1u));
			uint16_t fill = (uint16_t)(1u << (9 - bits));
			for (uint16_t t = 0; t < fill; t++) {
				/* bitbuf stores bits LSB-first in the low bits, so table index is r in low bits */
				uint16_t idx = (uint16_t)(((uint16_t)t << bits) | r);
				h->len[idx] = (uint8_t)bits;
				h->sym[idx] = symv;
			}
		}
	}
	h->valid = 1;
	return 0;
}

static int br_peek9(struct bitr *b, uint32_t *out)
{
	if (br_need(b, 9) != 0) return -1;
	*out = b->bitbuf & 0x1ffu;
	return 0;
}

static int br_drop(struct bitr *b, uint32_t n)
{
	if (n > b->bitcount) return -1;
	b->bitbuf >>= n;
	b->bitcount -= n;
	return 0;
}

static int htab_decode(struct bitr *b, const struct htab *h, uint32_t *out_sym)
{
	if (!b || !h || !out_sym || !h->valid) return -1;
	uint32_t peek = 0;
	if (br_peek9(b, &peek) != 0) return -1;
	uint8_t L = h->len[peek];
	if (L) {
		uint16_t s = h->sym[peek];
		if (br_drop(b, L) != 0) return -1;
		*out_sym = s;
		return 0;
	}
	/* Slow path for >9 bits: accumulate bits and search canonical ranges.
	 * Note: we must reverse the code bits because deflate is LSB-first.
	 */
	uint32_t code = 0;
	for (uint32_t bits = 1; bits <= h->maxbits; bits++) {
		uint32_t bit = 0;
		if (br_get_bits(b, 1, &bit) != 0) return -1;
		code |= (bit << (bits - 1u));
		/* reverse 'bits' bits */
		uint32_t r = 0;
		for (uint32_t k = 0; k < bits; k++) r = (r << 1) | ((code >> k) & 1u);
		uint16_t fc = h->first_code[bits];
		uint16_t cnt = h->count[bits];
		if (cnt) {
			int32_t diff = (int32_t)r - (int32_t)fc;
			if (diff >= 0 && (uint32_t)diff < cnt) {
				uint16_t idx = (uint16_t)(h->first_sym[bits] + (uint16_t)diff);
				if (idx >= 288) return -1;
				*out_sym = h->syms[idx];
				return 0;
			}
		}
	}
	return -1;
}

static int build_fixed(struct htab *litlen, struct htab *dist)
{
	uint8_t ll[288];
	uint8_t dl[32];
	for (uint32_t i = 0; i < 288; i++) {
		if (i <= 143) ll[i] = 8;
		else if (i <= 255) ll[i] = 9;
		else if (i <= 279) ll[i] = 7;
		else ll[i] = 8;
	}
	for (uint32_t i = 0; i < 32; i++) dl[i] = 5;
	if (htab_build(litlen, ll, 288) != 0) return -1;
	if (htab_build(dist, dl, 32) != 0) return -1;
	return 0;
}

static const uint16_t len_base[29] = {
	3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
	35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t len_extra[29] = {
	0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
	3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t dist_base[30] = {
	1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
	257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const uint8_t dist_extra[30] = {
	0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
	7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

static int build_dynamic(struct bitr *b, struct htab *litlen, struct htab *dist)
{
	uint32_t HLIT = 0, HDIST = 0, HCLEN = 0;
	if (br_get_bits(b, 5, &HLIT) != 0) return -1;
	if (br_get_bits(b, 5, &HDIST) != 0) return -1;
	if (br_get_bits(b, 4, &HCLEN) != 0) return -1;
	HLIT += 257;
	HDIST += 1;
	HCLEN += 4;
	if (HLIT > 286 || HDIST > 30) return -1;

	static const uint8_t order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
	uint8_t cl[19];
	for (int i = 0; i < 19; i++) cl[i] = 0;
	for (uint32_t i = 0; i < HCLEN; i++) {
		uint32_t v = 0;
		if (br_get_bits(b, 3, &v) != 0) return -1;
		cl[order[i]] = (uint8_t)v;
	}
	struct htab cht;
	if (htab_build(&cht, cl, 19) != 0) return -1;

	uint32_t total = HLIT + HDIST;
	uint8_t lens[288 + 32];
	for (uint32_t i = 0; i < total; i++) lens[i] = 0;

	uint32_t i = 0;
	uint8_t prev = 0;
	while (i < total) {
		uint32_t sym = 0;
		if (htab_decode(b, &cht, &sym) != 0) return -1;
		if (sym <= 15) {
			prev = (uint8_t)sym;
			lens[i++] = prev;
			continue;
		}
		if (sym == 16) {
			uint32_t extra = 0;
			if (br_get_bits(b, 2, &extra) != 0) return -1;
			uint32_t rep = 3 + extra;
			for (uint32_t k = 0; k < rep && i < total; k++) lens[i++] = prev;
			continue;
		}
		if (sym == 17) {
			uint32_t extra = 0;
			if (br_get_bits(b, 3, &extra) != 0) return -1;
			uint32_t rep = 3 + extra;
			prev = 0;
			for (uint32_t k = 0; k < rep && i < total; k++) lens[i++] = 0;
			continue;
		}
		if (sym == 18) {
			uint32_t extra = 0;
			if (br_get_bits(b, 7, &extra) != 0) return -1;
			uint32_t rep = 11 + extra;
			prev = 0;
			for (uint32_t k = 0; k < rep && i < total; k++) lens[i++] = 0;
			continue;
		}
		return -1;
	}

	if (htab_build(litlen, lens, HLIT) != 0) return -1;
	if (htab_build(dist, lens + HLIT, HDIST) != 0) return -1;
	return 0;
}

static int inflate_zlib_segments(const uint8_t *const *segs,
				const size_t *lens,
				size_t nseg,
				uint8_t *out,
				size_t out_cap,
				size_t *out_len)
{
	if (!out || out_cap == 0 || !out_len) return -1;
	*out_len = 0;
	if (!segs || !lens || nseg == 0) return -1;

	struct seg_reader rd;
	rd.segs = segs;
	rd.lens = lens;
	rd.nseg = nseg;
	rd.si = 0;
	rd.off = 0;

	/* zlib header: CMF, FLG */
	uint8_t cmf = 0, flg = 0;
	if (seg_get_byte(&rd, &cmf) != 0) return -1;
	if (seg_get_byte(&rd, &flg) != 0) return -1;
	if ((cmf & 0x0fu) != 8u) return -1; /* deflate */
	uint16_t chk = (uint16_t)(((uint16_t)cmf << 8) | (uint16_t)flg);
	if ((chk % 31u) != 0) return -1;
	if (flg & 0x20u) return -1; /* preset dictionary not supported */

	struct bitr b;
	b.r = &rd;
	b.bitbuf = 0;
	b.bitcount = 0;

	struct htab fixed_ll, fixed_d;
	int have_fixed = 0;

	int last = 0;
	while (!last) {
		uint32_t bfinal = 0, btype = 0;
		if (br_get_bits(&b, 1, &bfinal) != 0) return -1;
		if (br_get_bits(&b, 2, &btype) != 0) return -1;
		last = (bfinal != 0);

		if (btype == 0) {
			/* Stored */
			if (br_align_byte(&b) != 0) return -1;
			uint8_t lo1=0, hi1=0, lo2=0, hi2=0;
			if (br_get_byte(&b, &lo1) != 0) return -1;
			if (br_get_byte(&b, &hi1) != 0) return -1;
			if (br_get_byte(&b, &lo2) != 0) return -1;
			if (br_get_byte(&b, &hi2) != 0) return -1;
			uint16_t l = (uint16_t)((uint16_t)lo1 | ((uint16_t)hi1 << 8));
			uint16_t nl = (uint16_t)((uint16_t)lo2 | ((uint16_t)hi2 << 8));
			if ((uint16_t)(l ^ 0xffffu) != nl) return -1;
			if (*out_len + (size_t)l > out_cap) return -1;
			for (uint16_t i = 0; i < l; i++) {
				uint8_t by = 0;
				if (br_get_byte(&b, &by) != 0) return -1;
				out[(*out_len)++] = by;
			}
			continue;
		}

		struct htab litlen, dist;
		if (btype == 1) {
			if (!have_fixed) {
				if (build_fixed(&fixed_ll, &fixed_d) != 0) return -1;
				have_fixed = 1;
			}
			litlen = fixed_ll;
			dist = fixed_d;
		} else if (btype == 2) {
			if (build_dynamic(&b, &litlen, &dist) != 0) return -1;
		} else {
			return -1;
		}

		for (;;) {
			uint32_t sym = 0;
			if (htab_decode(&b, &litlen, &sym) != 0) return -1;
			if (sym < 256u) {
				if (*out_len + 1 > out_cap) return -1;
				out[(*out_len)++] = (uint8_t)sym;
				continue;
			}
			if (sym == 256u) break;
			if (sym < 257u || sym > 285u) return -1;
			uint32_t li = sym - 257u;
			size_t length = (size_t)len_base[li];
			uint32_t ex = (uint32_t)len_extra[li];
			uint32_t ev = 0;
			if (ex && br_get_bits(&b, ex, &ev) != 0) return -1;
			length += (size_t)ev;

			uint32_t ds = 0;
			if (htab_decode(&b, &dist, &ds) != 0) return -1;
			if (ds > 29u) return -1;
			size_t distv = (size_t)dist_base[ds];
			uint32_t dex = (uint32_t)dist_extra[ds];
			uint32_t dv = 0;
			if (dex && br_get_bits(&b, dex, &dv) != 0) return -1;
			distv += (size_t)dv;

			if (distv == 0 || distv > *out_len) return -1;
			if (*out_len + length > out_cap) return -1;
			size_t src = *out_len - distv;
			for (size_t i = 0; i < length; i++) {
				out[*out_len] = out[src + i];
				(*out_len)++;
			}
		}
	}

	/* Adler32 follows; we ignore/skip validation. */
	return 0;
}

/* --- PNG unfilter --- */
static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c)
{
	int p = (int)a + (int)b - (int)c;
	int pa = p - (int)a; if (pa < 0) pa = -pa;
	int pb = p - (int)b; if (pb < 0) pb = -pb;
	int pc = p - (int)c; if (pc < 0) pc = -pc;
	if (pa <= pb && pa <= pc) return a;
	if (pb <= pc) return b;
	return c;
}

static int unfilter(uint8_t *dst, const uint8_t *src, const uint8_t *prev, uint32_t rowbytes, uint32_t bpp)
{
	uint8_t f = src[0];
	const uint8_t *s = src + 1;
	if (f == 0) {
		for (uint32_t i = 0; i < rowbytes; i++) dst[i] = s[i];
		return 0;
	}
	if (f == 1) {
		for (uint32_t i = 0; i < rowbytes; i++) {
			uint8_t left = (i >= bpp) ? dst[i - bpp] : 0;
			dst[i] = (uint8_t)(s[i] + left);
		}
		return 0;
	}
	if (f == 2) {
		for (uint32_t i = 0; i < rowbytes; i++) {
			uint8_t up = prev ? prev[i] : 0;
			dst[i] = (uint8_t)(s[i] + up);
		}
		return 0;
	}
	if (f == 3) {
		for (uint32_t i = 0; i < rowbytes; i++) {
			uint8_t left = (i >= bpp) ? dst[i - bpp] : 0;
			uint8_t up = prev ? prev[i] : 0;
			dst[i] = (uint8_t)(s[i] + (uint8_t)(((uint32_t)left + (uint32_t)up) / 2u));
		}
		return 0;
	}
	if (f == 4) {
		for (uint32_t i = 0; i < rowbytes; i++) {
			uint8_t left = (i >= bpp) ? dst[i - bpp] : 0;
			uint8_t up = prev ? prev[i] : 0;
			uint8_t ul = (prev && i >= bpp) ? prev[i - bpp] : 0;
			dst[i] = (uint8_t)(s[i] + paeth(left, up, ul));
		}
		return 0;
	}
	return -1;
}

int png_decode_xrgb(const uint8_t *data,
		    size_t len,
		    uint8_t *scratch,
		    size_t scratch_cap,
		    uint32_t *out_pixels,
		    size_t out_cap_pixels,
		    uint32_t *out_w,
		    uint32_t *out_h)
{
	if (!out_w || !out_h) return -1;
	*out_w = 0;
	*out_h = 0;
	if (!data || !scratch || !out_pixels) return -1;
	if (!is_png_sig(data, len)) return -1;

	uint32_t w=0,h=0;
	uint8_t bit_depth=0, color_type=0, interlace=0;
	const uint8_t *plte = 0;
	size_t plte_len = 0;
	const uint8_t *trns = 0;
	size_t trns_len = 0;

	const uint8_t *idat_segs[64];
	size_t idat_lens[64];
	size_t idat_n = 0;

	size_t p = 8;
	int seen_ihdr = 0;
	while (p + 12 <= len) {
		uint32_t clen = be32(&data[p]);
		uint32_t ctyp = be32(&data[p+4]);
		const uint8_t *cdata = &data[p+8];
		if (p + 12u + (size_t)clen > len) return -1;
		if (ctyp == 0x49484452u) { /* IHDR */
			if (clen < 13) return -1;
			w = be32(&cdata[0]);
			h = be32(&cdata[4]);
			bit_depth = cdata[8];
			color_type = cdata[9];
			interlace = cdata[12];
			seen_ihdr = 1;
		} else if (ctyp == 0x504c5445u) { /* PLTE */
			plte = cdata;
			plte_len = clen;
		} else if (ctyp == 0x74524e53u) { /* tRNS */
			/* Transparency info: for palette, this is alpha values for palette entries. */
			trns = cdata;
			trns_len = clen;
		} else if (ctyp == 0x49444154u) { /* IDAT */
			if (idat_n >= 64) return -1;
			idat_segs[idat_n] = cdata;
			idat_lens[idat_n] = clen;
			idat_n++;
		} else if (ctyp == 0x49454e44u) { /* IEND */
			break;
		}
		p += 12u + (size_t)clen;
	}

	if (!seen_ihdr || w == 0 || h == 0) return -1;
	if (interlace != 0) return -2;
	if (!(color_type==0 || color_type==2 || color_type==3 || color_type==4 || color_type==6)) return -1;
	if (color_type == 3) {
		/* Palette: allow packed samples (1/2/4/8 bpc). */
		if (!(bit_depth == 1 || bit_depth == 2 || bit_depth == 4 || bit_depth == 8)) return -2;
	} else {
		/* Other color types: keep it simple for now (8bpc only). */
		if (bit_depth != 8) return -2;
	}
	if ((uint64_t)w * (uint64_t)h > (uint64_t)out_cap_pixels) return -1;
	if (idat_n == 0) return -1;

	uint32_t channels = (color_type==0) ? 1u : (color_type==2) ? 3u : (color_type==3) ? 1u : (color_type==4) ? 2u : 4u;
	uint32_t bits_per_px = channels * (uint32_t)bit_depth;
	uint32_t bpp = (bits_per_px + 7u) / 8u; /* bytes per pixel for filter */
	uint32_t rowbytes = (uint32_t)(((uint64_t)w * (uint64_t)bits_per_px + 7u) / 8u);
	uint64_t need = (uint64_t)h * (uint64_t)(1u + rowbytes);
	if (need > (uint64_t)scratch_cap) return -1;

	size_t out_len = 0;
	if (inflate_zlib_segments(idat_segs, idat_lens, idat_n, scratch, (size_t)need, &out_len) != 0) return -1;
	if (out_len < (size_t)need) {
		/* Some encoders may pad less; require at least full scanline data. */
		return -1;
	}

	/* Unfilter into a second view in-place: we can reuse scratch by writing rows forward. */
	uint8_t *rows = scratch;
	uint8_t *tmp = scratch; /* filtered source starts at scratch */
	for (uint32_t y = 0; y < h; y++) {
		uint8_t *dst = &rows[(size_t)y * (size_t)rowbytes];
		const uint8_t *src = &tmp[(size_t)y * (size_t)(rowbytes + 1u)];
		const uint8_t *prev = (y == 0) ? 0 : &rows[(size_t)(y - 1u) * (size_t)rowbytes];
		if (unfilter(dst, src, prev, rowbytes, bpp) != 0) return -1;
	}

	/* Expand to XRGB */
	if (color_type == 3) {
		if (!plte || (plte_len % 3u) != 0) return -1;
		uint32_t palsz = (uint32_t)(plte_len / 3u);
		for (uint32_t y = 0; y < h; y++) {
			const uint8_t *src = &rows[(size_t)y * (size_t)rowbytes];
			for (uint32_t x = 0; x < w; x++) {
				uint8_t idx = 0;
				if (bit_depth == 8) idx = src[x];
				else idx = png_idx_packed(src, x, bit_depth);

				uint32_t rgb = 0xff000000u;
				uint32_t a = 255u;
				if (trns && idx < trns_len) a = (uint32_t)trns[idx];
				if (idx < palsz) {
					const uint8_t *c = &plte[(size_t)idx * 3u];
					uint32_t r = (uint32_t)c[0];
					uint32_t g = (uint32_t)c[1];
					uint32_t b = (uint32_t)c[2];
					if (a < 255u) {
						uint32_t bg = png_alpha_bg(x, y);
						uint32_t bgr = (bg >> 16) & 0xffu;
						uint32_t bgg = (bg >> 8) & 0xffu;
						uint32_t bgb = (bg >> 0) & 0xffu;
						r = (bgr * (255u - a) + r * a + 127u) / 255u;
						g = (bgg * (255u - a) + g * a + 127u) / 255u;
						b = (bgb * (255u - a) + b * a + 127u) / 255u;
					}
					rgb |= (r << 16) | (g << 8) | b;
				}
				out_pixels[(size_t)y * (size_t)w + (size_t)x] = rgb;
			}
		}
	} else if (color_type == 0) {
		for (uint32_t y = 0; y < h; y++) {
			const uint8_t *src = &rows[(size_t)y * (size_t)rowbytes];
			for (uint32_t x = 0; x < w; x++) {
				uint32_t g = (uint32_t)src[x];
				out_pixels[(size_t)y * (size_t)w + (size_t)x] = 0xff000000u | (g << 16) | (g << 8) | g;
			}
		}
	} else if (color_type == 4) {
		/* Grayscale+alpha: composite over a light checkerboard so transparent
		 * pixels remain visible on dark backgrounds.
		 */
		for (uint32_t y = 0; y < h; y++) {
			const uint8_t *src = &rows[(size_t)y * (size_t)rowbytes];
			for (uint32_t x = 0; x < w; x++) {
				uint32_t g = (uint32_t)src[(size_t)x * 2u + 0u];
				uint32_t a = (uint32_t)src[(size_t)x * 2u + 1u];
				uint32_t bg = png_alpha_bg(x, y);
				uint32_t bgr = (bg >> 16) & 0xffu;
				uint32_t bgg = (bg >> 8) & 0xffu;
				uint32_t bgb = (bg >> 0) & 0xffu;
				uint32_t r = (bgr * (255u - a) + g * a + 127u) / 255u;
				uint32_t gg = (bgg * (255u - a) + g * a + 127u) / 255u;
				uint32_t b = (bgb * (255u - a) + g * a + 127u) / 255u;
				out_pixels[(size_t)y * (size_t)w + (size_t)x] = 0xff000000u | (r << 16) | (gg << 8) | b;
			}
		}
	} else if (color_type == 2) {
		for (uint32_t y = 0; y < h; y++) {
			const uint8_t *src = &rows[(size_t)y * (size_t)rowbytes];
			for (uint32_t x = 0; x < w; x++) {
				const uint8_t *c = &src[(size_t)x * 3u];
				out_pixels[(size_t)y * (size_t)w + (size_t)x] = 0xff000000u | ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | (uint32_t)c[2];
			}
		}
	} else { /* 6 */
		for (uint32_t y = 0; y < h; y++) {
			const uint8_t *src = &rows[(size_t)y * (size_t)rowbytes];
			for (uint32_t x = 0; x < w; x++) {
				const uint8_t *c = &src[(size_t)x * 4u];
				uint32_t r0 = (uint32_t)c[0];
				uint32_t g0 = (uint32_t)c[1];
				uint32_t b0 = (uint32_t)c[2];
				uint32_t a0 = (uint32_t)c[3];
				uint32_t bg = png_alpha_bg(x, y);
				uint32_t bgr = (bg >> 16) & 0xffu;
				uint32_t bgg = (bg >> 8) & 0xffu;
				uint32_t bgb = (bg >> 0) & 0xffu;
				uint32_t r = (bgr * (255u - a0) + r0 * a0 + 127u) / 255u;
				uint32_t gg = (bgg * (255u - a0) + g0 * a0 + 127u) / 255u;
				uint32_t b = (bgb * (255u - a0) + b0 * a0 + 127u) / 255u;
				out_pixels[(size_t)y * (size_t)w + (size_t)x] = 0xff000000u | (r << 16) | (gg << 8) | b;
			}
		}
	}

	*out_w = w;
	*out_h = h;
	return 0;
}
