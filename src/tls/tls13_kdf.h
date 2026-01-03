#pragma once

#include "hkdf_sha256.h"

/* TLS 1.3 HKDF labels and secret derivation helpers (SHA-256).
 *
 * Implements HKDF-Expand-Label and Derive-Secret from RFC 8446.
 */

#define TLS13_HASH_SIZE SHA256_DIGEST_SIZE

/* HKDF-Expand-Label(secret, label, context, length)
 *
 * label is the unprefixed label string (e.g. "derived", "c hs traffic").
 * The "tls13 " prefix is added internally.
 */
int tls13_hkdf_expand_label_sha256(const uint8_t secret[TLS13_HASH_SIZE],
				  const char *label,
				  const uint8_t *context, size_t context_len,
				  uint8_t *out, size_t out_len);

/* Derive-Secret(secret, label, transcript_hash) producing a 32-byte secret. */
int tls13_derive_secret_sha256(const uint8_t secret[TLS13_HASH_SIZE],
			       const char *label,
			       const uint8_t transcript_hash[TLS13_HASH_SIZE],
			       uint8_t out[TLS13_HASH_SIZE]);
