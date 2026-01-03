#pragma once

#include "types.h"

#define AES128_KEY_SIZE 16u
#define AES_BLOCK_SIZE 16u

struct aes128_ctx {
	uint32_t rk[44]; /* 11 round keys * 4 words */
};

void aes128_init(struct aes128_ctx *ctx, const uint8_t key[AES128_KEY_SIZE]);
void aes128_encrypt_block(const struct aes128_ctx *ctx, const uint8_t in[AES_BLOCK_SIZE], uint8_t out[AES_BLOCK_SIZE]);
