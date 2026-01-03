#pragma once

#include "types.h"

#define SHA256_DIGEST_SIZE 32u
#define SHA256_BLOCK_SIZE 64u

struct sha256_ctx {
	uint32_t h[8];
	uint64_t total_len; /* bytes */
	uint8_t buf[SHA256_BLOCK_SIZE];
	uint32_t buf_len;
};

void sha256_init(struct sha256_ctx *ctx);
void sha256_update(struct sha256_ctx *ctx, const uint8_t *data, size_t len);
void sha256_final(struct sha256_ctx *ctx, uint8_t out[SHA256_DIGEST_SIZE]);

void sha256(const uint8_t *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);
