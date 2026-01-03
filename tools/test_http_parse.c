#include <stdio.h>
#include <string.h>

#include "../src/browser/http_parse.h"

int main(void)
{
	/* header end */
	{
		const char *s = "HTTP/1.1 200 OK\r\nA: b\r\n\r\nBODY";
		size_t off = 0;
		if (http_find_header_end((const uint8_t *)s, strlen(s), &off) != 0) {
			puts("http-parse selftest: FAIL (find header end)");
			return 1;
		}
		if (off != strlen("HTTP/1.1 200 OK\r\nA: b\r\n\r\n")) {
			puts("http-parse selftest: FAIL (header end offset)");
			return 1;
		}
	}

	/* content-length line */
	{
		uint64_t v = 0;
		if (http_parse_content_length_line("Content-Length: 123\r\n", &v) != 0 || v != 123) {
			puts("http-parse selftest: FAIL (content-length 123)");
			return 1;
		}
		if (http_parse_content_length_line("content-length:\t42\n", &v) != 0 || v != 42) {
			puts("http-parse selftest: FAIL (content-length 42)");
			return 1;
		}
		if (http_parse_content_length_line("X: 1\r\n", &v) == 0) {
			puts("http-parse selftest: FAIL (false positive)");
			return 1;
		}
	}

	puts("http-parse selftest: OK");
	return 0;
}
