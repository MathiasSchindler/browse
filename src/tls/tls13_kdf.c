#include "tls13_kdf.h"

#include "types.h"

static size_t c_strlen(const char *s)
{
	size_t n = 0;
	while (s && s[n]) n++;
	return n;
}

int tls13_hkdf_expand_label_sha256(const uint8_t secret[TLS13_HASH_SIZE],
				  const char *label,
				  const uint8_t *context, size_t context_len,
				  uint8_t *out, size_t out_len)
{
	/* struct {
	 *   uint16 length;
	 *   opaque label<7..255>;  // "tls13 " + label
	 *   opaque context<0..255>;
	 * } HkdfLabel;
	 */

	if (out_len > 0xffffu) return -1;
	if (context_len > 255u) return -1;

	static const char prefix[] = "tls13 ";
	const size_t prefix_len = sizeof(prefix) - 1;
	const size_t label_len = c_strlen(label);
	const size_t full_label_len = prefix_len + label_len;
	if (full_label_len > 255u) return -1;

	uint8_t info[2 + 1 + 255 + 1 + 255];
	size_t n = 0;

	/* length */
	info[n++] = (uint8_t)((out_len >> 8) & 0xffu);
	info[n++] = (uint8_t)(out_len & 0xffu);

	/* label */
	info[n++] = (uint8_t)full_label_len;
	for (size_t i = 0; i < prefix_len; i++) info[n++] = (uint8_t)prefix[i];
	for (size_t i = 0; i < label_len; i++) info[n++] = (uint8_t)label[i];

	/* context */
	info[n++] = (uint8_t)context_len;
	for (size_t i = 0; i < context_len; i++) info[n++] = context[i];

	int rc = hkdf_expand_sha256(secret, info, n, out, out_len);
	crypto_memset(info, 0, sizeof(info));
	return rc;
}

int tls13_derive_secret_sha256(const uint8_t secret[TLS13_HASH_SIZE],
			       const char *label,
			       const uint8_t transcript_hash[TLS13_HASH_SIZE],
			       uint8_t out[TLS13_HASH_SIZE])
{
	return tls13_hkdf_expand_label_sha256(secret, label, transcript_hash, TLS13_HASH_SIZE, out, TLS13_HASH_SIZE);
}
