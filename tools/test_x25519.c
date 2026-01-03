#include <stdio.h>
#include <string.h>

#include "../src/tls/x25519.h"

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
	return -1;
}

static int hex_to_bytes(unsigned char *out, size_t out_len, const char *hex)
{
	for (size_t i = 0; i < out_len; i++) {
		int hi = hexval(hex[i * 2]);
		int lo = hexval(hex[i * 2 + 1]);
		if (hi < 0 || lo < 0) return 0;
		out[i] = (unsigned char)((hi << 4) | lo);
	}
	return 1;
}

static void dump_hex(const unsigned char *b, size_t n)
{
	for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
}

int main(void)
{
	const char *a_hex = "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a";
	const char *A_hex = "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
	const char *b_hex = "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb";
	const char *B_hex = "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";
	const char *K_hex = "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742";

	unsigned char a[32], A_exp[32], b[32], B_exp[32], K_exp[32];
	hex_to_bytes(a, 32, a_hex);
	hex_to_bytes(A_exp, 32, A_hex);
	hex_to_bytes(b, 32, b_hex);
	hex_to_bytes(B_exp, 32, B_hex);
	hex_to_bytes(K_exp, 32, K_hex);

	unsigned char base[32];
	memset(base, 0, 32);
	base[0] = 9;

	unsigned char out[32];
	x25519(out, a, base);
	printf("A calc: "); dump_hex(out, 32); printf("\n");
	printf("A exp : "); dump_hex(A_exp, 32); printf("\n");

	x25519(out, b, base);
	printf("B calc: "); dump_hex(out, 32); printf("\n");
	printf("B exp : "); dump_hex(B_exp, 32); printf("\n");

	x25519(out, a, B_exp);
	printf("K(a,B) calc: "); dump_hex(out, 32); printf("\n");
	printf("K exp      : "); dump_hex(K_exp, 32); printf("\n");

	x25519(out, b, A_exp);
	printf("K(b,A) calc: "); dump_hex(out, 32); printf("\n");
	printf("K exp      : "); dump_hex(K_exp, 32); printf("\n");

	return 0;
}
