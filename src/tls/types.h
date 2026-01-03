#pragma once

/* Minimal types for freestanding crypto code (no libc). */

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;

typedef signed char int8_t;
typedef signed short int16_t;
typedef signed int int32_t;
typedef signed long long int64_t;

typedef unsigned long size_t;

typedef int bool;

#ifndef NULL
#define NULL ((void *)0)
#endif

static inline void *crypto_memset(void *dst, int v, size_t n)
{
	uint8_t *p = (uint8_t *)dst;
	for (size_t i = 0; i < n; i++) p[i] = (uint8_t)v;
	return dst;
}

static inline void *crypto_memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;
	for (size_t i = 0; i < n; i++) d[i] = s[i];
	return dst;
}

static inline int crypto_memeq(const void *a, const void *b, size_t n)
{
	const uint8_t *pa = (const uint8_t *)a;
	const uint8_t *pb = (const uint8_t *)b;
	uint8_t acc = 0;
	for (size_t i = 0; i < n; i++) acc |= (uint8_t)(pa[i] ^ pb[i]);
	return acc == 0;
}
