#include "selftest.h"

#include "aes128.h"
#include "gcm.h"
#include "x25519.h"

/* Test vectors from RFC 4231 (HMAC-SHA-256), RFC 5869 (HKDF-SHA-256), and
 * common SHA-256 vectors.
 */

static int test_sha256_empty(void)
{
	static const uint8_t expect[32] = {
		0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
		0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55,
	};
	uint8_t out[32];
	sha256(NULL, 0, out);
	return crypto_memeq(out, expect, 32);
}

static int test_sha256_abc(void)
{
	static const uint8_t msg[] = {'a','b','c'};
	static const uint8_t expect[32] = {
		0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
		0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad,
	};
	uint8_t out[32];
	sha256(msg, sizeof(msg), out);
	return crypto_memeq(out, expect, 32);
}

static int test_hmac_rfc4231_case1(void)
{
	/* key = 0x0b*20, msg="Hi There" */
	uint8_t key[20];
	for (size_t i = 0; i < 20; i++) key[i] = 0x0b;
	static const uint8_t msg[] = {'H','i',' ','T','h','e','r','e'};
	static const uint8_t expect[32] = {
		0xb0,0x34,0x4c,0x61,0xd8,0xdb,0x38,0x53,0x5c,0xa8,0xaf,0xce,0xaf,0x0b,0xf1,0x2b,
		0x88,0x1d,0xc2,0x00,0xc9,0x83,0x3d,0xa7,0x26,0xe9,0x37,0x6c,0x2e,0x32,0xcf,0xf7,
	};
	uint8_t out[32];
	hmac_sha256(key, sizeof(key), msg, sizeof(msg), out);
	return crypto_memeq(out, expect, 32);
}

static int test_hkdf_rfc5869_case1(void)
{
	/* RFC 5869 test case 1 */
	static const uint8_t ikm[22] = {
		0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
		0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
	};
	static const uint8_t salt[13] = {
		0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,
	};
	static const uint8_t info[10] = {
		0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,
	};
	static const uint8_t prk_expect[32] = {
		0x07,0x77,0x09,0x36,0x2c,0x2e,0x32,0xdf,0x0d,0xdc,0x3f,0x0d,0xc4,0x7b,0xba,0x63,
		0x90,0xb6,0xc7,0x3b,0xb5,0x0f,0x9c,0x31,0x22,0xec,0x84,0x4a,0xd7,0xc2,0xb3,0xe5,
	};
	static const uint8_t okm_expect[42] = {
		0x3c,0xb2,0x5f,0x25,0xfa,0xac,0xd5,0x7a,0x90,0x43,0x4f,0x64,0xd0,0x36,0x2f,0x2a,
		0x2d,0x2d,0x0a,0x90,0xcf,0x1a,0x5a,0x4c,0x5d,0xb0,0x2d,0x56,0xec,0xc4,0xc5,0xbf,
		0x34,0x00,0x72,0x08,0xd5,0xb8,0x87,0x18,0x58,0x65,
	};

	uint8_t prk[32];
	hkdf_extract_sha256(salt, sizeof(salt), ikm, sizeof(ikm), prk);
	if (!crypto_memeq(prk, prk_expect, 32)) return 0;

	uint8_t okm[42];
	if (hkdf_expand_sha256(prk, info, sizeof(info), okm, sizeof(okm)) != 0) return 0;
	if (!crypto_memeq(okm, okm_expect, sizeof(okm))) return 0;

	return 1;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
	return -1;
}

static int hex_to_bytes(uint8_t *out, size_t out_len, const char *hex)
{
	for (size_t i = 0; i < out_len; i++) {
		int hi = hexval(hex[i * 2]);
		int lo = hexval(hex[i * 2 + 1]);
		if (hi < 0 || lo < 0) return 0;
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	return 1;
}

static int test_x25519_rfc7748(void)
{
	/* RFC 7748 section 5.2 test vectors are specified as 32-byte strings.
	 * The hex below is the byte order (little-endian) used by X25519.
	 */
	static const char *a_hex = "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a";
	static const char *A_hex = "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
	static const char *b_hex = "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb";
	static const char *B_hex = "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";
	static const char *K_hex = "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742";

	uint8_t a[32], A[32], b[32], B[32], K[32];
	if (!hex_to_bytes(a, 32, a_hex)) return 0;
	if (!hex_to_bytes(A, 32, A_hex)) return 0;
	if (!hex_to_bytes(b, 32, b_hex)) return 0;
	if (!hex_to_bytes(B, 32, B_hex)) return 0;
	if (!hex_to_bytes(K, 32, K_hex)) return 0;

	uint8_t out[32];
	uint8_t base[32];
	crypto_memset(base, 0, 32);
	base[0] = 9;

	/* Public keys */
	x25519(out, a, base);
	if (!crypto_memeq(out, A, 32)) return 0;
	x25519(out, b, base);
	if (!crypto_memeq(out, B, 32)) return 0;

	/* Shared secrets */
	x25519(out, a, B);
	if (!crypto_memeq(out, K, 32)) return 0;
	x25519(out, b, A);
	if (!crypto_memeq(out, K, 32)) return 0;

	crypto_memset(a, 0, sizeof(a));
	crypto_memset(A, 0, sizeof(A));
	crypto_memset(b, 0, sizeof(b));
	crypto_memset(B, 0, sizeof(B));
	crypto_memset(K, 0, sizeof(K));
	crypto_memset(out, 0, sizeof(out));
	crypto_memset(base, 0, sizeof(base));
	return 1;
}

int tls_crypto_selftest(void)
{
	return tls_crypto_selftest_detail(NULL);
}

int tls_crypto_selftest_detail(int *failed_step)
{
	int step = 0;

	step++; if (!test_sha256_empty()) { if (failed_step) *failed_step = step; return 0; }
	step++; if (!test_sha256_abc()) { if (failed_step) *failed_step = step; return 0; }
	step++; if (!test_hmac_rfc4231_case1()) { if (failed_step) *failed_step = step; return 0; }
	step++; if (!test_hkdf_rfc5869_case1()) { if (failed_step) *failed_step = step; return 0; }

	/* AES-128 single-block test: key=0, pt=0 -> 66e94bd4ef8a2c3b884cfa59ca342b2e */
	step++;
	{
		uint8_t key[16];
		uint8_t pt[16];
		uint8_t ct[16];
		static const uint8_t expect[16] = {0x66,0xe9,0x4b,0xd4,0xef,0x8a,0x2c,0x3b,0x88,0x4c,0xfa,0x59,0xca,0x34,0x2b,0x2e};
		crypto_memset(key, 0, 16);
		crypto_memset(pt, 0, 16);
		struct aes128_ctx aes;
		aes128_init(&aes, key);
		aes128_encrypt_block(&aes, pt, ct);
		int ok = crypto_memeq(ct, expect, 16);
		crypto_memset(&aes, 0, sizeof(aes));
		crypto_memset(ct, 0, 16);
		if (!ok) { if (failed_step) *failed_step = step; return 0; }
	}

	/* AES-128-GCM test vector (NIST SP800-38D):
	 * key=0, iv=0 (12), pt=0 (16), aad=empty
	 */
	step++;
	{
		uint8_t key[16];
		uint8_t iv[12];
		uint8_t pt[16];
		uint8_t ct[16];
		uint8_t tag[16];
		static const uint8_t ct_expect[16] = {0x03,0x88,0xda,0xce,0x60,0xb6,0xa3,0x92,0xf3,0x28,0xc2,0xb9,0x71,0xb2,0xfe,0x78};
		static const uint8_t tag_expect[16] = {0xab,0x6e,0x47,0xd4,0x2c,0xec,0x13,0xbd,0xf5,0x3a,0x67,0xb2,0x12,0x57,0xbd,0xdf};
		crypto_memset(key, 0, 16);
		crypto_memset(iv, 0, 12);
		crypto_memset(pt, 0, 16);
		aes128_gcm_encrypt(key, iv, 12, NULL, 0, pt, 16, ct, tag);
		int ok = crypto_memeq(ct, ct_expect, 16) && crypto_memeq(tag, tag_expect, 16);
		crypto_memset(ct, 0, 16);
		crypto_memset(tag, 0, 16);
		if (!ok) { if (failed_step) *failed_step = step; return 0; }
	}

	/* X25519 (RFC 7748) */
	step++;
	if (!test_x25519_rfc7748()) { if (failed_step) *failed_step = step; return 0; }

	if (failed_step) *failed_step = 0;
	return 1;
}
