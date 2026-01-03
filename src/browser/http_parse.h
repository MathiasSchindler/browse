#pragma once

#include "url.h"

/* Find end of HTTP headers (\r\n\r\n) in a byte buffer.
 * Returns 0 and sets *out_off to the index just after the delimiter.
 * Returns -1 if not found.
 */
static inline int http_find_header_end(const uint8_t *buf, size_t len, size_t *out_off)
{
	if (!buf || !out_off) return -1;
	if (len < 4) return -1;
	for (size_t i = 0; i + 3 < len; i++) {
		if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
			*out_off = i + 4;
			return 0;
		}
	}
	return -1;
}

static inline int http_parse_u64_dec(const char *s, uint64_t *out)
{
	if (!s || !out) return -1;
	uint64_t v = 0;
	size_t i = 0;
	if (s[0] < '0' || s[0] > '9') return -1;
	for (; s[i] >= '0' && s[i] <= '9'; i++) {
		uint64_t nv = v * 10u + (uint64_t)(s[i] - '0');
		if (nv < v) return -1;
		v = nv;
	}
	*out = v;
	return 0;
}

static inline int http_parse_u64_hex(const char *s, uint64_t *out)
{
	if (!s || !out) return -1;
	uint64_t v = 0;
	int have = 0;
	for (size_t i = 0; s[i] != 0; i++) {
		char c = s[i];
		uint8_t d;
		if (c >= '0' && c <= '9') d = (uint8_t)(c - '0');
		else if (c >= 'a' && c <= 'f') d = (uint8_t)(10 + (c - 'a'));
		else if (c >= 'A' && c <= 'F') d = (uint8_t)(10 + (c - 'A'));
		else break;
		have = 1;
		uint64_t nv = (v << 4) | (uint64_t)d;
		if ((nv >> 4) != v) return -1;
		v = nv;
	}
	if (!have) return -1;
	*out = v;
	return 0;
}

/* Parses a comma-separated token list, case-insensitive.
 * Example: "gzip, chunked" contains token "chunked".
 */
static inline int http_value_has_token_ci(const char *value, const char *token)
{
	if (!value || !token) return 0;
	size_t tok_len = c_strlen(token);
	if (tok_len == 0) return 0;

	const char *p = value;
	for (;;) {
		while (*p == ' ' || *p == '\t' || *p == ',') p++;
		if (*p == 0) break;
		const char *t0 = p;
		while (*p && *p != ',' && *p != ' ' && *p != '\t') p++;
		size_t tlen = (size_t)(p - t0);
		if (tlen == tok_len) {
			int eq = 1;
			for (size_t i = 0; i < tok_len; i++) {
				char a = c_tolower_ascii(t0[i]);
				char b = c_tolower_ascii(token[i]);
				if (a != b) {
					eq = 0;
					break;
				}
			}
			if (eq) return 1;
		}
		while (*p && *p != ',') p++;
		if (*p == ',') p++;
	}
	return 0;
}

/* Parses a single header line for Content-Length.
 * line should include the full line up to \n (CRLF is fine).
 * Returns 0 if parsed, -1 if not a Content-Length line or invalid.
 */
static inline int http_parse_content_length_line(const char *line, uint64_t *out_len)
{
	if (!line || !out_len) return -1;
	*out_len = 0;
	const char *name = "Content-Length";
	size_t name_len = c_strlen(name);
	if (!c_ieq_n(line, name, name_len)) return -1;
	if (line[name_len] != ':') return -1;
	const char *p = line + name_len + 1;
	while (*p == ' ' || *p == '\t') p++;
	char tmp[32];
	size_t n = 0;
	while (p[n] && p[n] != '\r' && p[n] != '\n' && n + 1 < sizeof(tmp)) {
		tmp[n] = p[n];
		n++;
	}
	tmp[n] = 0;
	return http_parse_u64_dec(tmp, out_len);
}

/* Minimal streaming decoder for HTTP/1.1 Transfer-Encoding: chunked.
 * - Supports chunk extensions (ignored).
 * - Skips trailers (ignored).
 * - Returns decoded bytes into caller buffer.
 */
struct http_chunked_dec {
	uint64_t chunk_rem;
	char size_line[64];
	size_t size_line_len;
	uint8_t state;
	uint8_t done;
	uint8_t trailer_at_line_start;
	uint8_t trailer_saw_cr;
};

static inline void http_chunked_init(struct http_chunked_dec *d)
{
	if (!d) return;
	d->chunk_rem = 0;
	d->size_line_len = 0;
	d->state = 0;
	d->done = 0;
	d->trailer_at_line_start = 1;
	d->trailer_saw_cr = 0;
}

static inline int http_chunked_feed(struct http_chunked_dec *d,
				    const uint8_t *in, size_t in_len, size_t *in_used,
				    uint8_t *out, size_t out_cap, size_t *out_written)
{
	/* states */
	/* 0: SIZE_LINE, 1: DATA, 2: DATA_CR, 3: DATA_LF, 4: TRAILERS */
	if (!d || !in || !in_used || !out_written) return -1;
	*in_used = 0;
	*out_written = 0;
	if (d->done) return 1;

	while (*in_used < in_len) {
		uint8_t b = in[*in_used];

		if (d->state == 0) {
			/* chunk size line */
			if (b == '\n') {
				d->size_line[d->size_line_len] = 0;
				/* trim CR */
				while (d->size_line_len > 0 && d->size_line[d->size_line_len - 1] == '\r') {
					d->size_line[--d->size_line_len] = 0;
				}
				/* cut at ';' (extensions) */
				for (size_t i = 0; d->size_line[i] != 0; i++) {
					if (d->size_line[i] == ';' || d->size_line[i] == ' ' || d->size_line[i] == '\t') {
						d->size_line[i] = 0;
						break;
					}
				}
				uint64_t sz = 0;
				if (http_parse_u64_hex(d->size_line, &sz) != 0) return -1;
				d->size_line_len = 0;
				if (sz == 0) {
					d->state = 4;
					d->trailer_at_line_start = 1;
					d->trailer_saw_cr = 0;
				} else {
					d->chunk_rem = sz;
					d->state = 1;
				}
				(*in_used)++;
				continue;
			}
			if (d->size_line_len + 1 >= sizeof(d->size_line)) return -1;
			d->size_line[d->size_line_len++] = (char)b;
			(*in_used)++;
			continue;
		}

		if (d->state == 1) {
			/* chunk data */
			if (d->chunk_rem == 0) {
				d->state = 2;
				continue;
			}
			if (out && *out_written < out_cap) {
				out[(*out_written)++] = b;
			}
			d->chunk_rem--;
			(*in_used)++;
			continue;
		}

		if (d->state == 2) {
			/* expect CR after data */
			if (b != '\r' && b != '\n') return -1;
			d->state = (b == '\r') ? 3 : 0;
			(*in_used)++;
			continue;
		}

		if (d->state == 3) {
			/* expect LF after CR */
			if (b != '\n') return -1;
			d->state = 0;
			(*in_used)++;
			continue;
		}

		/* TRAILERS */
		if (d->state == 4) {
			if (d->trailer_at_line_start) {
				if (b == '\n') {
					d->done = 1;
					(*in_used)++;
					return 1;
				}
				if (b == '\r') {
					d->trailer_saw_cr = 1;
					(*in_used)++;
					continue;
				}
				d->trailer_at_line_start = 0;
			}
			if (d->trailer_saw_cr) {
				if (b != '\n') return -1;
				d->done = 1;
				(*in_used)++;
				return 1;
			}
			if (b == '\n') {
				d->trailer_at_line_start = 1;
			}
			(*in_used)++;
			continue;
		}
	}

	return d->done ? 1 : 0;
}
