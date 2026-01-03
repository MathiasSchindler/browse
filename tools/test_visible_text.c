#include <stdio.h>
#include <string.h>

#include "../src/browser/html_text.h"

static int expect_eq(const char *name, const char *html, const char *expect)
{
	char out[512];
	memset(out, 0, sizeof(out));
	if (html_visible_text_extract((const uint8_t *)html, strlen(html), out, sizeof(out)) != 0) {
		printf("visible-text %s: FAIL (extract)\n", name);
		return 1;
	}
	if (strcmp(out, expect) != 0) {
		printf("visible-text %s: FAIL\n", name);
		printf("  got:    '%s'\n", out);
		printf("  expect: '%s'\n", expect);
		return 1;
	}
	return 0;
}

int main(void)
{
	if (expect_eq("basic", "<html><body>Hello <b>world</b>!</body></html>", "Hello world!")) return 1;
	if (expect_eq("entities", "A&amp;B &lt; C &nbsp; D", "A&B < C D")) return 1;
	if (expect_eq("blocks", "A<p>B<br/>C</p>D", "A\nB\nC\nD")) return 1;
	if (expect_eq("skip_script", "X<script>alert(1)</script>Y", "X Y")) return 1;
	if (expect_eq("comment", "A<!-- hidden -->B", "A B")) return 1;
	if (expect_eq("numeric", "A&#32;B&#x20;C", "A B C")) return 1;
	if (expect_eq("umlaut", "WIKIPEDIA DIE FREIE ENZYKLOP\xC3\x84" "DIE", "WIKIPEDIA DIE FREIE ENZYKLOPAEDIE")) return 1;

	puts("visible-text selftest: OK");
	return 0;
}
