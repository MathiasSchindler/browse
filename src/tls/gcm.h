#pragma once

#include "aes128.h"

#define GCM_TAG_SIZE 16u

/* AES-GCM for TLS_AES_128_GCM_SHA256.
 * Supports arbitrary IV length, but TLS uses 12-byte nonces.
 */

void aes128_gcm_encrypt(const uint8_t key[AES128_KEY_SIZE],
			 const uint8_t *iv, size_t iv_len,
			 const uint8_t *aad, size_t aad_len,
			 const uint8_t *pt, size_t pt_len,
			 uint8_t *ct,
			 uint8_t tag[GCM_TAG_SIZE]);

int aes128_gcm_decrypt(const uint8_t key[AES128_KEY_SIZE],
			 const uint8_t *iv, size_t iv_len,
			 const uint8_t *aad, size_t aad_len,
			 const uint8_t *ct, size_t ct_len,
			 uint8_t *pt,
			 const uint8_t tag[GCM_TAG_SIZE]);
