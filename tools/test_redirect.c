#include "../src/browser/url.h"

static int fail(const char *msg)
{
	dbg_write(msg);
	return 1;
}

int main(void)
{
	/* Status code parsing */
	if (http_parse_status_code("HTTP/1.1 301 Moved Permanently") != 301) return fail("redirect test: status 301 parse fail\n");
	if (http_parse_status_code("HTTP/2 200") != 200) return fail("redirect test: status 200 parse fail\n");

	/* Location header extraction */
	{
		char out[256];
		if (http_header_extract_value("Location: https://en.wikipedia.org/wiki/Main_Page\r\n", "Location", out, sizeof(out)) != 0)
			return fail("redirect test: Location extract fail\n");
		if (!c_starts_with(out, "https://en.wikipedia.org/")) return fail("redirect test: Location value wrong\n");
	}
	{
		char out[256];
		if (http_header_extract_value("location: /wiki/Hauptseite\r", "Location", out, sizeof(out)) != 0)
			return fail("redirect test: location (lowercase) extract fail\n");
		if (!c_starts_with(out, "/wiki/")) return fail("redirect test: location value wrong\n");
	}

	/* URL application */
	{
		char host[128];
		char path[512];
		if (url_apply_location("de.wikipedia.org", "/wiki/Wikipedia:Hauptseite", host, sizeof(host), path, sizeof(path)) != 0)
			return fail("redirect test: apply absolute path fail\n");
		if (!c_starts_with(host, "de.wikipedia.org")) return fail("redirect test: host changed unexpectedly\n");
		if (!c_starts_with(path, "/wiki/")) return fail("redirect test: path wrong\n");
	}
	{
		char host[128];
		char path[512];
		if (url_apply_location("de.wikipedia.org", "https://en.wikipedia.org/wiki/Main_Page", host, sizeof(host), path, sizeof(path)) != 0)
			return fail("redirect test: apply https url fail\n");
		if (!c_starts_with(host, "en.wikipedia.org")) return fail("redirect test: host not updated\n");
		if (!c_starts_with(path, "/wiki/")) return fail("redirect test: path wrong (https)\n");
	}

	dbg_write("redirect selftest: OK\n");
	return 0;
}
