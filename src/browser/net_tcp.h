#pragma once

#include "net_ip6.h"

static inline int tcp6_connect(const uint8_t ip[16], uint16_t port)
{
	int fd = sys_socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) return -1;

	struct sockaddr_in6 sa;
	c_memset(&sa, 0, sizeof(sa));
	sa.sin6_family = (uint16_t)AF_INET6;
	sa.sin6_port = htons(port);
	c_memcpy(sa.sin6_addr.s6_addr, ip, 16);

	if (sys_connect(fd, &sa, (uint32_t)sizeof(sa)) < 0) {
		sys_close(fd);
		return -1;
	}

	return fd;
}
