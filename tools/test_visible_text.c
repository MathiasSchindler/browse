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
	if (expect_eq("utf8_punct", "foo\xE2\x80\x93" "bar" "\xE2\x80\xA6", "foo-bar...")) return 1;
	if (expect_eq("img_placeholder", "A<img alt='Example image' src='https://upload.wikimedia.org/wikipedia/commons/a/a9/X.png'>B",
	              "A\n\x1e" "IMG 8 ? Example image\x1fhttps://upload.wikimedia.org/wikipedia/commons/a/a9/X.png\n\n\n\n\n\n\n\n\nB")) return 1;
	if (expect_eq("img_src_newline", "A<img alt='Example image' src='//upload.wikimedia.org/wikipedi\na/commons/a/a9/X.png'>B",
	              "A\n\x1e" "IMG 8 ? Example image\x1f//upload.wikimedia.org/wikipedia/commons/a/a9/X.png\n\n\n\n\n\n\n\n\nB")) return 1;
	if (expect_eq("img_srcset_pick", "A<img srcset='https://x/y/a.webp 1x, https://x/y/b.png 2x'>B",
	              "A\n\x1e" "IMG 8 ? b.png\x1fhttps://x/y/b.png\n\n\n\n\n\n\n\n\nB")) return 1;
	if (expect_eq("img_srcset_newline", "A<img srcset='https://x/y/a.webp 1x, https://x/y/b.\npng 2x'>B",
	              "A\n\x1e" "IMG 8 ? b.png\x1fhttps://x/y/b.png\n\n\n\n\n\n\n\n\nB")) return 1;
	if (expect_eq("img_inline_icon", "A<img width='24' height='24' alt='icon' src='x.png'>B", "A [img]\xA0" "B")) return 1;
	if (expect_eq("breadcrumb_nav_inline_li",
	              "A<ul class='breadcrumb-nav-container'><li><img width='30' height='27' src='x.png'>Geographie</li><li><img width='30' height='27' src='y.png'>Geschichte</li></ul>Z",
	              "A\n[img]\xA0" "Geographie [img]\xA0" "Geschichte\nZ")) return 1;
	if (expect_eq("inline_display_none", "<html><body>foo <span style=\"display:none\">HIDE</span> bar</body></html>", "foo bar")) return 1;
	if (expect_eq("infobox_table_2col",
	              "<html><body>"
	              "<table class=\"infobox\">"
	              "<tr><th colspan=\"2\" style=\"background:#F0F0F0\">Kidd-Klasse</th></tr>"
	              "<tr><th>Land</th><td>Vereinigte Staaten</td></tr>"
	              "<tr><th>Typ</th><td>U-Boot</td></tr>"
	              "</table>"
	              "</body></html>",
	              "Kidd-Klasse\nLand: Vereinigte Staaten\nTyp: U-Boot")) return 1;

	puts("visible-text selftest: OK");
	return 0;
}
