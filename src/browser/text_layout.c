#include "text_layout.h"

static int is_space(char c)
{
	return (c == ' ' || c == '\t');
}

int text_layout_next_line_ex(const char *text,
				    size_t *io_pos,
				    uint32_t max_cols,
				    char *out_line,
				    size_t out_cap,
				    size_t *out_src_start)
{
	if (!text || !io_pos || !out_line || out_cap == 0 || max_cols == 0) return 1;
	out_line[0] = 0;

	size_t pos = *io_pos;
	/* Skip CRs globally. */
	while (text[pos] == '\r') pos++;
	if (text[pos] == 0) {
		*io_pos = pos;
		return 1;
	}

	/* A hard newline produces an empty line. */
	if (text[pos] == '\n') {
		*io_pos = pos + 1;
		if (out_src_start) *out_src_start = pos;
		out_line[0] = 0;
		return 0;
	}

	/* Trim leading whitespace for this produced line. */
	while (is_space(text[pos])) pos++;
	while (text[pos] == '\r') pos++;
	if (out_src_start) *out_src_start = pos;

	uint32_t cols = 0;
	size_t src_i = pos;
	size_t last_space_src = (size_t)-1;
	uint32_t last_space_out = 0;

	/* Copy until newline, end, or max_cols. */
	while (text[src_i] != 0 && text[src_i] != '\n') {
		char c = text[src_i];
		if (c == '\r') {
			src_i++;
			continue;
		}

		if (is_space(c)) {
			last_space_src = src_i;
			last_space_out = cols;
		}

		if (cols + 1 >= out_cap) break;
		out_line[cols++] = c;
		src_i++;
		if (cols >= max_cols) break;
	}

	/* If we hit max_cols and there's more text on this visual line, prefer wrapping at last space. */
	if (cols >= max_cols && text[src_i] != 0 && text[src_i] != '\n') {
		/* If the next byte is whitespace, we ended cleanly on a word boundary. */
		if (is_space(text[src_i]) || text[src_i] == '\r') {
			while (cols > 0 && is_space(out_line[cols - 1])) cols--;
			out_line[cols] = 0;
			pos = src_i;
			while (is_space(text[pos])) pos++;
			while (text[pos] == '\r') pos++;
			*io_pos = pos;
			return 0;
		}
		if (last_space_src != (size_t)-1 && last_space_out > 0) {
			cols = last_space_out;
			out_line[cols] = 0;
			pos = last_space_src + 1;
			while (is_space(text[pos])) pos++;
			while (text[pos] == '\r') pos++;
			*io_pos = pos;
			return 0;
		}
		/* No space: hard split at max_cols. */
		out_line[cols] = 0;
		*io_pos = src_i;
		return 0;
	}

	/* Trim trailing spaces/tabs (nice for rendering). */
	while (cols > 0 && is_space(out_line[cols - 1])) cols--;
	out_line[cols] = 0;

	/* Advance past newline if present. */
	if (text[src_i] == '\n') src_i++;
	*io_pos = src_i;
	return 0;
}

int text_layout_next_line(const char *text,
				 size_t *io_pos,
				 uint32_t max_cols,
				 char *out_line,
				 size_t out_cap)
{
	return text_layout_next_line_ex(text, io_pos, max_cols, out_line, out_cap, NULL);
}

int text_layout_index_for_row_col(const char *text,
					 uint32_t max_cols,
					 uint32_t target_row,
					 uint32_t target_col,
					 size_t *out_index)
{
	if (!text || !out_index || max_cols == 0) return -1;
	size_t pos = 0;
	char line[256];
	for (uint32_t row = 0; ; row++) {
		size_t start = 0;
		int r = text_layout_next_line_ex(text, &pos, max_cols, line, sizeof(line), &start);
		if (r != 0) return -1;
		if (row == target_row) {
			size_t len = 0;
			while (line[len] != 0) len++;
			if (target_col >= len) return -1;
			*out_index = start + (size_t)target_col;
			return 0;
		}
	}
}
