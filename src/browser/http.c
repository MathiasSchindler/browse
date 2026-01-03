#include "http.h"

static int append_bytes(char *out, size_t out_len, size_t *off, const char *src, size_t n)
{
	if (*off > out_len) return -1;
	if (n > out_len - *off) return -1;
	for (size_t i = 0; i < n; i++) {
		out[*off + i] = src[i];
	}
	*off += n;
	return 0;
}

static int append_cstr(char *out, size_t out_len, size_t *off, const char *s)
{
	for (size_t i = 0; s[i] != 0; i++) {
		if (append_bytes(out, out_len, off, &s[i], 1) != 0) return -1;
	}
	return 0;
}

int http_format_get_ex(char *out, size_t out_len, const char *host, const char *path, int keep_alive)
{
	if (!out || out_len == 0 || !host || !path) return -1;

	size_t off = 0;
	out[0] = 0;

	/* Normalize path: must start with '/'. */
	const char *p = path;
	if (p[0] != '/') {
		p = "/";
	}

	if (append_cstr(out, out_len, &off, "GET ") != 0) return -1;
	if (append_cstr(out, out_len, &off, p) != 0) return -1;
	if (append_cstr(out, out_len, &off, " HTTP/1.1\r\n") != 0) return -1;

	if (append_cstr(out, out_len, &off, "Host: ") != 0) return -1;
	if (append_cstr(out, out_len, &off, host) != 0) return -1;
	if (append_cstr(out, out_len, &off, "\r\n") != 0) return -1;

	if (append_cstr(out, out_len, &off, "User-Agent: ") != 0) return -1;
	if (append_cstr(out, out_len, &off, BROWSE_USER_AGENT) != 0) return -1;
	if (append_cstr(out, out_len, &off, "\r\n") != 0) return -1;

	if (append_cstr(out, out_len, &off, "Accept: */*\r\n") != 0) return -1;
	if (append_cstr(out, out_len, &off, "Accept-Language: en,de;q=0.9\r\n") != 0) return -1;
	if (append_cstr(out, out_len, &off, keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n") != 0) return -1;
	if (append_cstr(out, out_len, &off, "\r\n") != 0) return -1;

	/* NUL-terminate for convenience/debugging, but length excludes it. */
	if (off >= out_len) return -1;
	out[off] = 0;
	return (int)off;
}

int http_format_get(char *out, size_t out_len, const char *host, const char *path)
{
	return http_format_get_ex(out, out_len, host, path, 0);
}
