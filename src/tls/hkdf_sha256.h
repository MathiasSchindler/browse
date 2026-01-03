#pragma once

#include "hmac_sha256.h"

/* HKDF per RFC 5869 with SHA-256. */

void hkdf_extract_sha256(const uint8_t *salt, size_t salt_len,
			const uint8_t *ikm, size_t ikm_len,
			uint8_t prk[HMAC_SHA256_SIZE]);

int hkdf_expand_sha256(const uint8_t prk[HMAC_SHA256_SIZE],
			const uint8_t *info, size_t info_len,
			uint8_t *okm, size_t okm_len);
