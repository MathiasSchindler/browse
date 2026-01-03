#include "gcm.h"

static inline void xor16(uint8_t out[16], const uint8_t a[16], const uint8_t b[16])
{
	for (int i = 0; i < 16; i++) out[i] = (uint8_t)(a[i] ^ b[i]);
}

static inline void inc32(uint8_t c[16])
{
	uint32_t n = ((uint32_t)c[12] << 24) | ((uint32_t)c[13] << 16) | ((uint32_t)c[14] << 8) | (uint32_t)c[15];
	n++;
	c[12] = (uint8_t)(n >> 24);
	c[13] = (uint8_t)(n >> 16);
	c[14] = (uint8_t)(n >> 8);
	c[15] = (uint8_t)n;
}

static void gf128_mul(uint8_t x[16], const uint8_t y[16])
{
	/* Bitwise shift/xor multiplication in GF(2^128) with polynomial
	 * x^128 + x^7 + x^2 + x + 1 (R = 0xe1...).
	 */
	uint8_t z[16];
	uint8_t v[16];
	crypto_memset(z, 0, 16);
	crypto_memcpy(v, y, 16);

	for (int i = 0; i < 128; i++) {
		uint8_t xi = (uint8_t)((x[i / 8] >> (7 - (i % 8))) & 1u);
		if (xi) {
			for (int j = 0; j < 16; j++) z[j] ^= v[j];
		}
		/* v = v >> 1; if lsb was 1, v[0] ^= 0xe1 */
		uint8_t lsb = (uint8_t)(v[15] & 1u);
		for (int j = 15; j > 0; j--) v[j] = (uint8_t)((v[j] >> 1) | ((v[j - 1] & 1u) << 7));
		v[0] >>= 1;
		if (lsb) v[0] ^= 0xe1u;
	}

	crypto_memcpy(x, z, 16);
	crypto_memset(z, 0, 16);
	crypto_memset(v, 0, 16);
}

static void ghash(const uint8_t h[16], const uint8_t *aad, size_t aad_len, const uint8_t *c, size_t c_len, uint8_t out[16])
{
	uint8_t y[16];
	crypto_memset(y, 0, 16);

	/* AAD blocks */
	for (size_t off = 0; off < aad_len; off += 16) {
		uint8_t blk[16];
		crypto_memset(blk, 0, 16);
		size_t take = aad_len - off;
		if (take > 16) take = 16;
		crypto_memcpy(blk, &aad[off], take);
		xor16(y, y, blk);
		gf128_mul(y, h);
	}

	/* ciphertext blocks */
	for (size_t off = 0; off < c_len; off += 16) {
		uint8_t blk[16];
		crypto_memset(blk, 0, 16);
		size_t take = c_len - off;
		if (take > 16) take = 16;
		crypto_memcpy(blk, &c[off], take);
		xor16(y, y, blk);
		gf128_mul(y, h);
	}

	/* lengths (bits) */
	uint8_t lens[16];
	crypto_memset(lens, 0, 16);
	uint64_t a_bits = (uint64_t)aad_len * 8ull;
	uint64_t c_bits = (uint64_t)c_len * 8ull;
	lens[0] = (uint8_t)(a_bits >> 56);
	lens[1] = (uint8_t)(a_bits >> 48);
	lens[2] = (uint8_t)(a_bits >> 40);
	lens[3] = (uint8_t)(a_bits >> 32);
	lens[4] = (uint8_t)(a_bits >> 24);
	lens[5] = (uint8_t)(a_bits >> 16);
	lens[6] = (uint8_t)(a_bits >> 8);
	lens[7] = (uint8_t)(a_bits);
	lens[8] = (uint8_t)(c_bits >> 56);
	lens[9] = (uint8_t)(c_bits >> 48);
	lens[10] = (uint8_t)(c_bits >> 40);
	lens[11] = (uint8_t)(c_bits >> 32);
	lens[12] = (uint8_t)(c_bits >> 24);
	lens[13] = (uint8_t)(c_bits >> 16);
	lens[14] = (uint8_t)(c_bits >> 8);
	lens[15] = (uint8_t)(c_bits);

	xor16(y, y, lens);
	gf128_mul(y, h);

	crypto_memcpy(out, y, 16);
	crypto_memset(y, 0, 16);
	crypto_memset(lens, 0, 16);
}

static void gcm_j0(const struct aes128_ctx *aes, const uint8_t *iv, size_t iv_len, uint8_t j0[16])
{
	if (iv_len == 12) {
		crypto_memcpy(j0, iv, 12);
		j0[12] = 0;
		j0[13] = 0;
		j0[14] = 0;
		j0[15] = 1;
		return;
	}
	/* general: J0 = GHASH_H(IV || pad || [len(IV)]_64) */
	uint8_t h[16];
	uint8_t zero[16];
	crypto_memset(zero, 0, 16);
	aes128_encrypt_block(aes, zero, h);
	ghash(h, NULL, 0, iv, iv_len, j0);
	crypto_memset(h, 0, 16);
}

void aes128_gcm_encrypt(const uint8_t key[AES128_KEY_SIZE],
			 const uint8_t *iv, size_t iv_len,
			 const uint8_t *aad, size_t aad_len,
			 const uint8_t *pt, size_t pt_len,
			 uint8_t *ct,
			 uint8_t tag[GCM_TAG_SIZE])
{
	struct aes128_ctx aes;
	aes128_init(&aes, key);

	uint8_t h[16];
	uint8_t zero[16];
	crypto_memset(zero, 0, 16);
	aes128_encrypt_block(&aes, zero, h);

	uint8_t j0[16];
	gcm_j0(&aes, iv, iv_len, j0);

	/* CTR encrypt */
	uint8_t ctr[16];
	crypto_memcpy(ctr, j0, 16);
	inc32(ctr);
	for (size_t off = 0; off < pt_len; off += 16) {
		uint8_t stream[16];
		aes128_encrypt_block(&aes, ctr, stream);
		inc32(ctr);
		size_t take = pt_len - off;
		if (take > 16) take = 16;
		for (size_t i = 0; i < take; i++) ct[off + i] = (uint8_t)(pt[off + i] ^ stream[i]);
		crypto_memset(stream, 0, 16);
	}

	uint8_t s[16];
	ghash(h, aad, aad_len, ct, pt_len, s);

	uint8_t e0[16];
	aes128_encrypt_block(&aes, j0, e0);
	xor16(tag, s, e0);

	crypto_memset(&aes, 0, sizeof(aes));
	crypto_memset(h, 0, 16);
	crypto_memset(j0, 0, 16);
	crypto_memset(ctr, 0, 16);
	crypto_memset(s, 0, 16);
	crypto_memset(e0, 0, 16);
	crypto_memset(zero, 0, 16);
}

int aes128_gcm_decrypt(const uint8_t key[AES128_KEY_SIZE],
			 const uint8_t *iv, size_t iv_len,
			 const uint8_t *aad, size_t aad_len,
			 const uint8_t *ct, size_t ct_len,
			 uint8_t *pt,
			 const uint8_t tag[GCM_TAG_SIZE])
{
	struct aes128_ctx aes;
	aes128_init(&aes, key);

	uint8_t h[16];
	uint8_t zero[16];
	crypto_memset(zero, 0, 16);
	aes128_encrypt_block(&aes, zero, h);

	uint8_t j0[16];
	gcm_j0(&aes, iv, iv_len, j0);

	uint8_t s[16];
	ghash(h, aad, aad_len, ct, ct_len, s);

	uint8_t e0[16];
	aes128_encrypt_block(&aes, j0, e0);
	uint8_t want[16];
	xor16(want, s, e0);

	int ok = crypto_memeq(want, tag, 16);
	if (!ok) {
		crypto_memset(&aes, 0, sizeof(aes));
		crypto_memset(h, 0, 16);
		crypto_memset(j0, 0, 16);
		crypto_memset(s, 0, 16);
		crypto_memset(e0, 0, 16);
		crypto_memset(want, 0, 16);
		crypto_memset(zero, 0, 16);
		return 0;
	}

	/* CTR decrypt */
	uint8_t ctr[16];
	crypto_memcpy(ctr, j0, 16);
	inc32(ctr);
	for (size_t off = 0; off < ct_len; off += 16) {
		uint8_t stream[16];
		aes128_encrypt_block(&aes, ctr, stream);
		inc32(ctr);
		size_t take = ct_len - off;
		if (take > 16) take = 16;
		for (size_t i = 0; i < take; i++) pt[off + i] = (uint8_t)(ct[off + i] ^ stream[i]);
		crypto_memset(stream, 0, 16);
	}

	crypto_memset(&aes, 0, sizeof(aes));
	crypto_memset(h, 0, 16);
	crypto_memset(j0, 0, 16);
	crypto_memset(s, 0, 16);
	crypto_memset(e0, 0, 16);
	crypto_memset(want, 0, 16);
	crypto_memset(ctr, 0, 16);
	crypto_memset(zero, 0, 16);
	return 1;
}
