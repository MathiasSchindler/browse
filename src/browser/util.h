#pragma once

#include "../core/syscall.h"

static inline void *c_memset(void *dst, int v, size_t n)
{
	uint8_t *p = (uint8_t *)dst;
	for (size_t i = 0; i < n; i++) {
		p[i] = (uint8_t)v;
	}
	return dst;
}

static inline void *c_memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;
	for (size_t i = 0; i < n; i++) {
		d[i] = s[i];
	}
	return dst;
}

static inline int c_starts_with(const char *s, const char *prefix)
{
	for (size_t i = 0; prefix[i] != 0; i++) {
		if (s[i] != prefix[i]) {
			return 0;
		}
	}
	return 1;
}

static inline int c_is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static inline uint16_t bswap16(uint16_t v)
{
	return (uint16_t)((v << 8) | (v >> 8));
}

static inline uint32_t bswap32(uint32_t v)
{
	return ((v & 0x000000ffu) << 24) |
	       ((v & 0x0000ff00u) << 8) |
	       ((v & 0x00ff0000u) >> 8) |
	       ((v & 0xff000000u) >> 24);
}

static inline uint16_t htons(uint16_t v)
{
	return bswap16(v);
}

static inline uint16_t ntohs(uint16_t v)
{
	return bswap16(v);
}

static inline uint32_t htonl(uint32_t v)
{
	return bswap32(v);
}

static inline uint32_t ntohl(uint32_t v)
{
	return bswap32(v);
}

static inline void u32_to_hex8(char out[9], uint32_t v)
{
	static const char *hex = "0123456789abcdef";
	for (int i = 0; i < 8; i++) {
		out[7 - i] = hex[v & 0xFu];
		v >>= 4;
	}
	out[8] = 0;
}
