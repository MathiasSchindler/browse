#include <stdio.h>
#include <stddef.h>

/* Use our freestanding TLS code (which defines its own types). */
#include "../src/tls/hkdf_sha256.h"
#include "../src/tls/tls13_kdf.h"

static void print_hex(const char *label, const unsigned char *p, size_t n)
{
	printf("%s (%zu): ", label, n);
	for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
	printf("\n");
}

int main(void)
{
	static const unsigned char sha256_empty[32] = {
		0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
		0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55,
	};
	unsigned char early[32];
	hkdf_extract_sha256(NULL, 0, NULL, 0, early);
	print_hex("early_secret", early, 32);

	unsigned char derived[32];
	tls13_derive_secret_sha256(early, "derived", sha256_empty, derived);
	print_hex("derived_secret", derived, 32);
	return 0;
}
