#include <stdio.h>
#include <string.h>

#include "../src/browser/net_dns.h"

static int test_qname(void)
{
	uint8_t out[64];
	memset(out, 0, sizeof(out));
	size_t n = dns_write_qname(out, sizeof(out), "wikipedia.org");
	/* 9 wikipedia 3 org 0 */
	static const uint8_t expect[] = {
		9,'w','i','k','i','p','e','d','i','a',
		3,'o','r','g',
		0,
	};
	if (n != sizeof(expect)) return 0;
	return memcmp(out, expect, sizeof(expect)) == 0;
}

static int test_ip6_to_str_no_compress(void)
{
	uint8_t ip[16] = {0x20,0x01,0x48,0x60,0x48,0x60,0,0,0,0,0,0,0,0,0x88,0x88};
	char s[48];
	ip6_to_str(s, ip);
	return strcmp(s, "2001:4860:4860:0:0:0:0:8888") == 0;
}

int main(void)
{
	int ok = 1;
	ok &= test_qname();
	ok &= test_ip6_to_str_no_compress();
	if (ok) {
		puts("net ipv6 selftest: OK");
		return 0;
	}
	puts("net ipv6 selftest: FAIL");
	return 1;
}
