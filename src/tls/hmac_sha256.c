#include "hmac_sha256.h"

void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t out[HMAC_SHA256_SIZE])
{
	uint8_t kopad[SHA256_BLOCK_SIZE];
	uint8_t kipad[SHA256_BLOCK_SIZE];
	uint8_t khash[SHA256_DIGEST_SIZE];

	if (key_len > SHA256_BLOCK_SIZE) {
		sha256(key, key_len, khash);
		key = khash;
		key_len = SHA256_DIGEST_SIZE;
	}

	crypto_memset(kopad, 0x5c, sizeof(kopad));
	crypto_memset(kipad, 0x36, sizeof(kipad));
	for (size_t i = 0; i < key_len; i++) {
		kopad[i] ^= key[i];
		kipad[i] ^= key[i];
	}

	struct sha256_ctx inner;
	sha256_init(&inner);
	sha256_update(&inner, kipad, sizeof(kipad));
	sha256_update(&inner, msg, msg_len);
	sha256_final(&inner, khash);

	struct sha256_ctx outer;
	sha256_init(&outer);
	sha256_update(&outer, kopad, sizeof(kopad));
	sha256_update(&outer, khash, sizeof(khash));
	sha256_final(&outer, out);

	crypto_memset(khash, 0, sizeof(khash));
	crypto_memset(kopad, 0, sizeof(kopad));
	crypto_memset(kipad, 0, sizeof(kipad));
}
