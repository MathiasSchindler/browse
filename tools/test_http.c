#include <stdio.h>
#include <string.h>

#include "../src/browser/http.h"

static int must_contain(const char *hay, const char *needle)
{
	return strstr(hay, needle) != NULL;
}

int main(void)
{
	char req[512];
	int n = http_format_get(req, sizeof(req), "example.com", "/");
	if (n < 0) {
		puts("http selftest: FAIL (format)");
		return 1;
	}
	if ((size_t)n != strlen(req)) {
		puts("http selftest: FAIL (len)");
		return 1;
	}
	if (strncmp(req, "GET / HTTP/1.1\r\n", 16) != 0) {
		puts("http selftest: FAIL (request line)");
		return 1;
	}
	if (!must_contain(req, "Host: example.com\r\n")) {
		puts("http selftest: FAIL (host)");
		return 1;
	}
	if (!must_contain(req, "User-Agent: " BROWSE_USER_AGENT "\r\n")) {
		puts("http selftest: FAIL (user-agent)");
		return 1;
	}
	if (!must_contain(req, "\r\n\r\n")) {
		puts("http selftest: FAIL (terminator)");
		return 1;
	}

	puts("http selftest: OK");
	return 0;
}
