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
	if (expect_eq("heading_spacing", "<h1>Title</h1><p>Body</p>", "Title\n\nBody")) return 1;
	if (expect_eq("list_bullets", "<ul><li>A</li><li>B</li></ul>", "- A\n- B")) return 1;
	if (expect_eq("skip_script", "X<script>alert(1)</script>Y", "X Y")) return 1;
	if (expect_eq("comment", "A<!-- hidden -->B", "A B")) return 1;
	if (expect_eq("numeric", "A&#32;B&#x20;C", "A B C")) return 1;
	if (expect_eq("umlaut", "WIKIPEDIA DIE FREIE ENZYKLOP\xC3\x84" "DIE", "WIKIPEDIA DIE FREIE ENZYKLOP\xC4" "DIE")) return 1;
	if (expect_eq("img_placeholder", "A<img alt='Example image' src='https://upload.wikimedia.org/wikipedia/commons/a/a9/X.png'>B",
	              "A\n\x1e" "IMG 8 ? Example image\x1fhttps://upload.wikimedia.org/wikipedia/commons/a/a9/X.png\n\n\n\n\n\n\n\n\nB")) return 1;
	if (expect_eq("img_srcset_pick", "A<img srcset='https://x/y/a.webp 1x, https://x/y/b.png 2x'>B",
	              "A\n\x1e" "IMG 8 ? b.png\x1fhttps://x/y/b.png\n\n\n\n\n\n\n\n\nB")) return 1;
	if (expect_eq("img_inline_icon", "A<img width='24' height='24' alt='icon' src='x.png'>B", "A [img] B")) return 1;

	puts("visible-text selftest: OK");
	return 0;
}
