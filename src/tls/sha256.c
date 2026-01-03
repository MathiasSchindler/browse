#include "sha256.h"

static inline uint32_t rotr32(uint32_t x, uint32_t n) { return (x >> n) | (x << (32u - n)); }
static inline uint32_t shr32(uint32_t x, uint32_t n) { return x >> n; }

static inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
static inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }

static inline uint32_t big0(uint32_t x) { return rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22); }
static inline uint32_t big1(uint32_t x) { return rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25); }
static inline uint32_t sml0(uint32_t x) { return rotr32(x, 7) ^ rotr32(x, 18) ^ shr32(x, 3); }
static inline uint32_t sml1(uint32_t x) { return rotr32(x, 17) ^ rotr32(x, 19) ^ shr32(x, 10); }

static const uint32_t K[64] = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
	0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
	0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
	0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
	0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
	0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
	0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
	0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
	0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
	0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
	0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
	0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
	0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static inline uint32_t load_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | ((uint32_t)p[3]);
}

static inline void store_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)(v);
}

static inline void store_be64(uint8_t *p, uint64_t v)
{
	p[0] = (uint8_t)(v >> 56);
	p[1] = (uint8_t)(v >> 48);
	p[2] = (uint8_t)(v >> 40);
	p[3] = (uint8_t)(v >> 32);
	p[4] = (uint8_t)(v >> 24);
	p[5] = (uint8_t)(v >> 16);
	p[6] = (uint8_t)(v >> 8);
	p[7] = (uint8_t)(v);
}

static void sha256_compress(struct sha256_ctx *ctx, const uint8_t block[64])
{
	uint32_t w[64];
	for (uint32_t i = 0; i < 16; i++) {
		w[i] = load_be32(&block[i * 4u]);
	}
	for (uint32_t i = 16; i < 64; i++) {
		w[i] = sml1(w[i - 2]) + w[i - 7] + sml0(w[i - 15]) + w[i - 16];
	}

	uint32_t a = ctx->h[0];
	uint32_t b = ctx->h[1];
	uint32_t c = ctx->h[2];
	uint32_t d = ctx->h[3];
	uint32_t e = ctx->h[4];
	uint32_t f = ctx->h[5];
	uint32_t g = ctx->h[6];
	uint32_t h = ctx->h[7];

	for (uint32_t i = 0; i < 64; i++) {
		uint32_t t1 = h + big1(e) + ch(e, f, g) + K[i] + w[i];
		uint32_t t2 = big0(a) + maj(a, b, c);
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}

	ctx->h[0] += a;
	ctx->h[1] += b;
	ctx->h[2] += c;
	ctx->h[3] += d;
	ctx->h[4] += e;
	ctx->h[5] += f;
	ctx->h[6] += g;
	ctx->h[7] += h;
}

void sha256_init(struct sha256_ctx *ctx)
{
	ctx->h[0] = 0x6a09e667u;
	ctx->h[1] = 0xbb67ae85u;
	ctx->h[2] = 0x3c6ef372u;
	ctx->h[3] = 0xa54ff53au;
	ctx->h[4] = 0x510e527fu;
	ctx->h[5] = 0x9b05688cu;
	ctx->h[6] = 0x1f83d9abu;
	ctx->h[7] = 0x5be0cd19u;
	ctx->total_len = 0;
	ctx->buf_len = 0;
}

void sha256_update(struct sha256_ctx *ctx, const uint8_t *data, size_t len)
{
	ctx->total_len += (uint64_t)len;

	while (len > 0) {
		uint32_t space = (uint32_t)(SHA256_BLOCK_SIZE - ctx->buf_len);
		uint32_t take = (len < (size_t)space) ? (uint32_t)len : space;
		crypto_memcpy(&ctx->buf[ctx->buf_len], data, take);
		ctx->buf_len += take;
		data += take;
		len -= take;

		if (ctx->buf_len == SHA256_BLOCK_SIZE) {
			sha256_compress(ctx, ctx->buf);
			ctx->buf_len = 0;
		}
	}
}

void sha256_final(struct sha256_ctx *ctx, uint8_t out[SHA256_DIGEST_SIZE])
{
	uint64_t bit_len = ctx->total_len * 8ull;

	/* append 0x80 */
	ctx->buf[ctx->buf_len++] = 0x80u;

	/* pad with zeros until length field fits */
	if (ctx->buf_len > 56) {
		while (ctx->buf_len < 64) ctx->buf[ctx->buf_len++] = 0;
		sha256_compress(ctx, ctx->buf);
		ctx->buf_len = 0;
	}
	while (ctx->buf_len < 56) ctx->buf[ctx->buf_len++] = 0;

	store_be64(&ctx->buf[56], bit_len);
	sha256_compress(ctx, ctx->buf);

	for (uint32_t i = 0; i < 8; i++) {
		store_be32(&out[i * 4u], ctx->h[i]);
	}

	crypto_memset(ctx, 0, sizeof(*ctx));
}

void sha256(const uint8_t *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE])
{
	struct sha256_ctx ctx;
	sha256_init(&ctx);
	sha256_update(&ctx, data, len);
	sha256_final(&ctx, out);
}
