#pragma once

#include "util.h"

/* Shared IPv4 socket definitions (no libc headers). */

enum {
	AF_INET = 2,
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

struct in_addr {
	uint32_t s_addr;
};

struct sockaddr_in {
	uint16_t sin_family;
	uint16_t sin_port;
	struct in_addr sin_addr;
	uint8_t sin_zero[8];
};

static inline void ip4_to_str(char out[16], const uint8_t ip[4])
{
	/* Minimal dotted decimal formatter. */
	char *p = out;
	for (int i = 0; i < 4; i++) {
		uint32_t v = ip[i];
		if (v >= 100) {
			*p++ = (char)('0' + (v / 100u));
			v %= 100u;
			*p++ = (char)('0' + (v / 10u));
			v %= 10u;
			*p++ = (char)('0' + v);
		} else if (v >= 10) {
			*p++ = (char)('0' + (v / 10u));
			v %= 10u;
			*p++ = (char)('0' + v);
		} else {
			*p++ = (char)('0' + v);
		}
		if (i != 3) *p++ = '.';
	}
	*p = 0;
}
