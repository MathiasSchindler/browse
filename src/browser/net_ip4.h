#pragma once

#include "util.h"

/* Shared IPv4 socket definitions (no libc headers). */

enum {
	AF_INET = 2,
};

enum {
	SOCK_STREAM = 1,
	SOCK_DGRAM = 2,
};

enum {
	IPPROTO_TCP = 6,
	IPPROTO_UDP = 17,
};

struct in_addr {
	uint32_t s_addr;
};

struct sockaddr_in {
	uint16_t sin_family;
	uint16_t sin_port;
	struct in_addr sin_addr;
	uint8_t sin_zero[8];
};
