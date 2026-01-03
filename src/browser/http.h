#pragma once

#include "../core/syscall.h"

/* User-Agent must point at the project repo. */
#define BROWSE_USER_AGENT "browse (+https://github.com/MathiasSchindler/browse)"

/* Formats a minimal HTTP/1.1 GET request.
 *
 * Returns number of bytes written (excluding the trailing NUL), or -1 on error
 * (e.g. buffer too small).
 */
int http_format_get(char *out, size_t out_len, const char *host, const char *path);
