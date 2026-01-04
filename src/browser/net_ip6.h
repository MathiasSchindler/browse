#pragma once

#include "util.h"

/* Shared IPv6 socket definitions (no libc headers). */

enum {
	AF_INET6 = 10,
};

#ifndef NET_SOCK_COMMON_CONSTANTS
#define NET_SOCK_COMMON_CONSTANTS
enum {
	SOCK_STREAM = 1,
	SOCK_DGRAM = 2,
};

enum {
	IPPROTO_TCP = 6,
	IPPROTO_UDP = 17,
};
#endif

typedef uint32_t socklen_t;

enum {
	SOL_SOCKET = 1,
	SO_ERROR = 4,
	SO_RCVTIMEO = 20,
	SO_SNDTIMEO = 21,
};

struct timeval {
	int64_t tv_sec;
	int64_t tv_usec;
};

struct in6_addr {
	uint8_t s6_addr[16];
};

struct sockaddr_in6 {
	uint16_t sin6_family;
	uint16_t sin6_port;
	uint32_t sin6_flowinfo;
	struct in6_addr sin6_addr;
	uint32_t sin6_scope_id;
};

static inline void ip6_to_str(char out[48], const uint8_t ip[16])
{
	/* Very small formatter (no :: compression). */
	static const char *hex = "0123456789abcdef";
	char *p = out;
	for (int i = 0; i < 8; i++) {
		uint16_t w = (uint16_t)((ip[i * 2 + 0] << 8) | ip[i * 2 + 1]);
		int started = 0;
		for (int n = 12; n >= 0; n -= 4) {
			uint8_t v = (uint8_t)((w >> n) & 0xF);
			if (!started) {
				if (v == 0 && n != 0) continue;
				started = 1;
			}
			*p++ = hex[v];
		}
		if (i != 7) *p++ = ':';
	}
	*p = 0;
}

static inline int ip6_eq(const uint8_t a[16], const uint8_t b[16])
{
	uint8_t acc = 0;
	for (int i = 0; i < 16; i++) acc |= (uint8_t)(a[i] ^ b[i]);
	return acc == 0;
}
