#pragma once

#if defined(__STDC_HOSTED__) && __STDC_HOSTED__ == 0
#include "../tls/types.h"
#else
#include <stddef.h>
#include <stdint.h>
#endif

/*
 * Tiny, allocation-free line layout for our fixed-width bitmap font.
 *
 * Input is a NUL-terminated UTF-8-ish byte stream, but this function treats
 * bytes as characters. It:
 * - skips '\r'
 * - breaks on '\n'
 * - trims leading spaces/tabs per produced line
 * - word-wraps on spaces when possible
 */
int text_layout_next_line(const char *text,
				 size_t *io_pos,
				 uint32_t max_cols,
				 char *out_line,
				 size_t out_cap);

/* Like text_layout_next_line(), but also returns the source start index within
 * `text` for the returned line (after trimming leading spaces).
 */
int text_layout_next_line_ex(const char *text,
				    size_t *io_pos,
				    uint32_t max_cols,
				    char *out_line,
				    size_t out_cap,
				    size_t *out_src_start);

/* Maps a rendered (row,col) back to an index into `text`, using the same
 * layout rules as text_layout_next_line(). Returns 0 on success.
 */
int text_layout_index_for_row_col(const char *text,
					 uint32_t max_cols,
					 uint32_t target_row,
					 uint32_t target_col,
					 size_t *out_index);
