#pragma once

#include "util.h"

/* Extracts visible-ish text from an HTML document.
 * - Strips tags.
 * - Drops script/style/noscript content.
 * - Decodes a small set of entities and numeric entities.
 * - Collapses whitespace.
 * Output is always NUL-terminated if out_len > 0.
 * Returns 0 on success, -1 on error.
 */
int html_visible_text_extract(const uint8_t *html, size_t html_len, char *out, size_t out_len);
