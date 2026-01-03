#include "hkdf_sha256.h"

void hkdf_extract_sha256(const uint8_t *salt, size_t salt_len,
			const uint8_t *ikm, size_t ikm_len,
			uint8_t prk[HMAC_SHA256_SIZE])
{
	uint8_t zeros[HMAC_SHA256_SIZE];
	if (salt == NULL || salt_len == 0) {
		crypto_memset(zeros, 0, sizeof(zeros));
		salt = zeros;
		salt_len = sizeof(zeros);
	}
	hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
	crypto_memset(zeros, 0, sizeof(zeros));
}

int hkdf_expand_sha256(const uint8_t prk[HMAC_SHA256_SIZE],
			const uint8_t *info, size_t info_len,
			uint8_t *okm, size_t okm_len)
{
	if (okm_len == 0) return 0;
	if (okm_len > 255u * HMAC_SHA256_SIZE) return -1;

	uint8_t t[HMAC_SHA256_SIZE];
	uint8_t inbuf[HMAC_SHA256_SIZE + 1024 + 1];
	size_t in_len;

	size_t produced = 0;
	uint8_t counter = 1;
	size_t t_len = 0;

	while (produced < okm_len) {
		in_len = 0;
		if (t_len) {
			crypto_memcpy(&inbuf[in_len], t, t_len);
			in_len += t_len;
		}
		if (info && info_len) {
			if (info_len > 1024) return -1;
			crypto_memcpy(&inbuf[in_len], info, info_len);
			in_len += info_len;
		}
		inbuf[in_len++] = counter;

		hmac_sha256(prk, HMAC_SHA256_SIZE, inbuf, in_len, t);
		t_len = HMAC_SHA256_SIZE;

		size_t take = okm_len - produced;
		if (take > HMAC_SHA256_SIZE) take = HMAC_SHA256_SIZE;
		crypto_memcpy(&okm[produced], t, take);
		produced += take;
		counter++;
	}

	crypto_memset(t, 0, sizeof(t));
	crypto_memset(inbuf, 0, sizeof(inbuf));
	return 0;
}
