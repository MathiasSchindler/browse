#pragma once

#include "../core/syscall.h"

/* User-Agent must point at the project repo. */
#define BROWSE_USER_AGENT "browse/0.1 (+https://github.com/MathiasSchindler/browse; contact: https://github.com/MathiasSchindler)"

/* Formats a minimal HTTP/1.1 GET request.
 *
 * Returns number of bytes written (excluding the trailing NUL), or -1 on error
 * (e.g. buffer too small).
 */
int http_format_get(char *out, size_t out_len, const char *host, const char *path);

/* Like http_format_get, but allows choosing Connection: keep-alive vs close.
 * keep_alive=1 emits "Connection: keep-alive".
 */
int http_format_get_ex(char *out, size_t out_len, const char *host, const char *path, int keep_alive);
