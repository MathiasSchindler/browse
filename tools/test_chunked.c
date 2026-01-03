#include <stdio.h>
#include <string.h>

#include "../src/browser/http_parse.h"

static int run_case(const char *name, const char *in, const char *expect, size_t cap, int feed_one_by_one)
{
	struct http_chunked_dec d;
	http_chunked_init(&d);

	uint8_t out[256];
	memset(out, 0, sizeof(out));
	size_t out_len = 0;

	size_t in_len = strlen(in);
	size_t off = 0;
	while (off < in_len) {
		size_t step = feed_one_by_one ? 1 : (in_len - off);
		if (step > in_len - off) step = in_len - off;

		size_t used = 0;
		size_t wrote = 0;
		int r = http_chunked_feed(&d,
					 (const uint8_t *)in + off,
					 step,
					 &used,
					 out + out_len,
					 (cap > out_len) ? (cap - out_len) : 0,
					 &wrote);
		out_len += wrote;
		off += used;

		if (r < 0) {
			printf("chunked selftest %s: FAIL (decoder error)\n", name);
			return 1;
		}
		if (r == 1) break;
		if (used == 0) {
			printf("chunked selftest %s: FAIL (no progress)\n", name);
			return 1;
		}
		if (out_len >= cap) {
			break;
		}
	}

	out[out_len] = 0;
	if (strcmp((const char *)out, expect) != 0) {
		printf("chunked selftest %s: FAIL\n", name);
		printf("  got:    '%s'\n", out);
		printf("  expect: '%s'\n", expect);
		return 1;
	}
	return 0;
}

int main(void)
{
	if (run_case("basic", "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n", "Wikipedia", 255, 0)) return 1;
	if (run_case("ext", "4;ext=1\r\nWiki\r\n0\r\n\r\n", "Wiki", 255, 0)) return 1;
	if (run_case("trailers", "1\r\na\r\n0\r\nX: y\r\n\r\n", "a", 255, 0)) return 1;
	if (run_case("split", "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n", "Wikipedia", 255, 1)) return 1;
	if (run_case("cap", "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n", "Wikip", 5, 1)) return 1;

	puts("chunked selftest: OK");
	return 0;
}
