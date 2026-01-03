#include <stdio.h>
#include <string.h>

#include "../src/browser/text_layout.h"

static int expect_line(const char *name, const char *text, uint32_t cols, const char **expect_lines, size_t expect_n)
{
	size_t pos = 0;
	char line[256];
	size_t got = 0;
	while (1) {
		int r = text_layout_next_line(text, &pos, cols, line, sizeof(line));
		if (r != 0) break;
		if (got >= expect_n) {
			printf("text-layout %s: FAIL (too many lines; got '%s')\n", name, line);
			return 1;
		}
		if (strlen(line) > cols) {
			printf("text-layout %s: FAIL (line too long: %zu > %u)\n", name, strlen(line), cols);
			return 1;
		}
		if (strcmp(line, expect_lines[got]) != 0) {
			printf("text-layout %s: FAIL (line %zu)\n", name, got);
			printf("  got:    '%s'\n", line);
			printf("  expect: '%s'\n", expect_lines[got]);
			return 1;
		}
		got++;
	}
	if (got != expect_n) {
		printf("text-layout %s: FAIL (line count %zu != %zu)\n", name, got, expect_n);
		return 1;
	}
	return 0;
}

int main(void)
{
	{
		const char *exp[] = {"Hello", "world!"};
		if (expect_line("wrap_space", "Hello world!", 6, exp, 2)) return 1;
	}
	{
		const char *exp[] = {"Hello", "", "World"};
		if (expect_line("hard_newline", "Hello\n\nWorld", 80, exp, 3)) return 1;
	}
	{
		const char *exp[] = {"ABCDEF", "GHI"};
		if (expect_line("hard_split", "ABCDEFGHI", 6, exp, 2)) return 1;
	}
	{
		const char *exp[] = {"A B", "C"};
		if (expect_line("trim_leading", "   A B C", 3, exp, 2)) return 1;
	}
	{
		const char *exp[] = {"A", "B"};
		if (expect_line("skip_cr", "A\r\nB", 80, exp, 2)) return 1;
	}

	puts("text-layout selftest: OK");
	return 0;
}
