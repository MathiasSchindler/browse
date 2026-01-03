#pragma once

#include "util.h"

static inline char c_tolower_ascii(char c)
{
	if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
	return c;
}

static inline int c_ieq_n(const char *a, const char *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		char ca = c_tolower_ascii(a[i]);
		char cb = c_tolower_ascii(b[i]);
		if (ca != cb) return 0;
	}
	return 1;
}

static inline size_t c_strnlen_s(const char *s, size_t maxn)
{
	if (!s) return 0;
	size_t n = 0;
	while (n < maxn && s[n] != 0) n++;
	return n;
}

static inline int c_strlcpy_s(char *dst, size_t dst_len, const char *src)
{
	if (!dst || dst_len == 0) return -1;
	size_t i = 0;
	for (; src && src[i] && i + 1 < dst_len; i++) {
		dst[i] = src[i];
	}
	dst[i] = 0;
	return (src && src[i] == 0) ? 0 : -1;
}

static inline int http_parse_status_code(const char *status_line)
{
	/* Expect: HTTP/1.1 301 ... */
	if (!status_line) return -1;
	if (!c_starts_with(status_line, "HTTP/")) return -1;

	/* Find first space */
	size_t i = 0;
	while (status_line[i] && status_line[i] != ' ') i++;
	while (status_line[i] == ' ') i++;
	if (status_line[i] < '0' || status_line[i] > '9') return -1;

	int code = 0;
	for (int k = 0; k < 3; k++) {
		char c = status_line[i + (size_t)k];
		if (c < '0' || c > '9') return -1;
		code = code * 10 + (c - '0');
	}
	return code;
}

static inline int http_header_extract_value(const char *line, const char *name, char *out, size_t out_len)
{
	if (!line || !name || !out || out_len == 0) return -1;
	out[0] = 0;

	size_t name_len = c_strlen(name);
	/* Match "Name:" case-insensitive */
	if (!c_ieq_n(line, name, name_len)) return -1;
	if (line[name_len] != ':') return -1;

	const char *p = line + name_len + 1;
	while (*p == ' ' || *p == '\t') p++;

	/* Copy until end-of-line, trimming trailing spaces and CR. */
	size_t n = 0;
	while (p[n] != 0) n++;
	while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' ' || p[n - 1] == '\t')) n--;

	size_t w = 0;
	for (; w < n && w + 1 < out_len; w++) out[w] = p[w];
	out[w] = 0;
	return (w == n) ? 0 : -1;
}

static inline int url_apply_location(const char *current_host,
			     const char *location,
			     char *out_host, size_t out_host_len,
			     char *out_path, size_t out_path_len)
{
	if (!current_host || !location || !out_host || !out_path) return -1;
	out_host[0] = 0;
	out_path[0] = 0;

	const char *p = location;
	if (c_starts_with(p, "https://")) {
		p += 8;
	} else if (c_starts_with(p, "http://")) {
		p += 7;
	} else if (c_starts_with(p, "//")) {
		p += 2;
	} else if (p[0] == '/') {
		/* Absolute path on same host */
		if (c_strlcpy_s(out_host, out_host_len, current_host) != 0) return -1;
		return c_strlcpy_s(out_path, out_path_len, p);
	} else {
		/* Unsupported relative form */
		return -1;
	}

	/* Parse host[:port]/path */
	char host_tmp[256];
	size_t hi = 0;
	while (p[hi] && p[hi] != '/' && p[hi] != ':' && hi + 1 < sizeof(host_tmp)) {
		host_tmp[hi] = p[hi];
		hi++;
	}
	host_tmp[hi] = 0;
	if (hi == 0) return -1;

	/* Ignore :port if present */
	while (p[hi] && p[hi] != '/') {
		hi++;
	}
	const char *path = p[hi] ? (p + hi) : "/";

	if (c_strlcpy_s(out_host, out_host_len, host_tmp) != 0) return -1;
	if (c_strlcpy_s(out_path, out_path_len, path) != 0) return -1;
	return 0;
}
