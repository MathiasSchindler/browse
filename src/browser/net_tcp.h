#pragma once

#include "net_ip4.h"
#include "net_ip6.h"

static inline int tcp__set_blocking(int fd, int is_blocking)
{
	int nb = is_blocking ? 0 : 1;
	return sys_ioctl(fd, FIONBIO, &nb);
}

static inline void tcp__set_timeouts(int fd, int sec)
{
	struct timeval tv;
	tv.tv_sec = (int64_t)sec;
	tv.tv_usec = 0;
	(void)sys_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, (uint32_t)sizeof(tv));
	(void)sys_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, (uint32_t)sizeof(tv));
}

static inline int tcp__connect_with_timeout(int fd, const void *sa, uint32_t sa_len, int timeout_ms)
{
	if (tcp__set_blocking(fd, 0) < 0) return -1;
	int rc = sys_connect(fd, sa, sa_len);
	if (rc == 0) {
		(void)tcp__set_blocking(fd, 1);
		return 0;
	}

	/* syscalls return negative errno. */
	if (rc != -EINPROGRESS) {
		(void)tcp__set_blocking(fd, 1);
		/* rc is negative errno */
		return rc;
	}

	struct pollfd pfd;
	pfd.fd = fd;
	pfd.events = (short)(POLLOUT | POLLERR | POLLHUP);
	pfd.revents = 0;
	int prc = sys_poll(&pfd, 1, timeout_ms);
	if (prc < 0) {
		(void)tcp__set_blocking(fd, 1);
		return prc;
	}
	if (prc == 0) {
		(void)tcp__set_blocking(fd, 1);
		/* Timed out waiting for connect completion. */
		return -110; /* ETIMEDOUT */
	}

	int soerr = 0;
	uint32_t optlen = (uint32_t)sizeof(soerr);
	int grc = sys_getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &optlen);
	if (grc < 0) {
		(void)tcp__set_blocking(fd, 1);
		return grc;
	}
	(void)tcp__set_blocking(fd, 1);
	return (soerr == 0) ? 0 : -(int)soerr;
}

static inline int tcp6_connect(const uint8_t ip[16], uint16_t port)
{
	int fd = sys_socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) return -1;

	struct sockaddr_in6 sa;
	c_memset(&sa, 0, sizeof(sa));
	sa.sin6_family = (uint16_t)AF_INET6;
	sa.sin6_port = htons(port);
	c_memcpy(sa.sin6_addr.s6_addr, ip, 16);

	int crc = tcp__connect_with_timeout(fd, &sa, (uint32_t)sizeof(sa), 3000);
	if (crc < 0) {
		sys_close(fd);
		return crc;
	}

	/* Prevent TLS/HTTP from blocking forever on reads/writes. */
	tcp__set_timeouts(fd, 5);

	return fd;
}

static inline int tcp4_connect(const uint8_t ip[4], uint16_t port)
{
	int fd = sys_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) return -1;

	struct sockaddr_in sa;
	c_memset(&sa, 0, sizeof(sa));
	sa.sin_family = (uint16_t)AF_INET;
	sa.sin_port = htons(port);
	/* ip is a 4-byte sequence in dotted-decimal order (a.b.c.d). */
	uint32_t host = ((uint32_t)ip[0] << 24u) |
			 ((uint32_t)ip[1] << 16u) |
			 ((uint32_t)ip[2] << 8u) |
			 ((uint32_t)ip[3] << 0u);
	sa.sin_addr.s_addr = htonl(host);

	int crc = tcp__connect_with_timeout(fd, &sa, (uint32_t)sizeof(sa), 3000);
	if (crc < 0) {
		sys_close(fd);
		return crc;
	}

	/* Prevent TLS/HTTP from blocking forever on reads/writes. */
	tcp__set_timeouts(fd, 5);

	return fd;
}
