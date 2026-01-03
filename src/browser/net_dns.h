#pragma once

#include "net_ip6.h"

/* IPv6-only DNS AAAA resolver over UDP.
 * - Uses Google DNS via IPv6:
 *   - 2001:4860:4860::8888
 *   - 2001:4860:4860::8844
 * - No IPv4 fallback, no A queries.
 * Returns 0 on success and writes IPv6 address to out_ip (16 bytes).
 */

static inline void google_dns_primary(struct in6_addr *out)
{
	/* 2001:4860:4860::8888 */
	static const uint8_t ip[16] = {0x20,0x01,0x48,0x60,0x48,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x88,0x88};
	c_memcpy(out->s6_addr, ip, 16);
}

static inline void google_dns_secondary(struct in6_addr *out)
{
	/* 2001:4860:4860::8844 */
	static const uint8_t ip[16] = {0x20,0x01,0x48,0x60,0x48,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x88,0x44};
	c_memcpy(out->s6_addr, ip, 16);
}

struct dns_header {
	uint16_t id;
	uint16_t flags;
	uint16_t qdcount;
	uint16_t ancount;
	uint16_t nscount;
	uint16_t arcount;
};

static inline size_t dns_write_qname(uint8_t *out, size_t cap, const char *name)
{
	/* Writes labels; returns bytes written or 0 on error. */
	size_t o = 0;
	size_t start = 0;
	for (size_t i = 0;; i++) {
		char ch = name[i];
		if (ch == '.' || ch == 0) {
			size_t len = i - start;
			if (len == 0 || len > 63 || o + 1 + len >= cap) {
				return 0;
			}
			out[o++] = (uint8_t)len;
			for (size_t k = 0; k < len; k++) {
				out[o++] = (uint8_t)name[start + k];
			}
			start = i + 1;
			if (ch == 0) break;
		}
	}
	if (o + 1 > cap) return 0;
	out[o++] = 0;
	return o;
}

static inline size_t dns_skip_name(const uint8_t *msg, size_t len, size_t off)
{
	/* Skips a possibly-compressed name; returns new offset or 0 on error. */
	for (;;) {
		if (off >= len) return 0;
		uint8_t b = msg[off];
		if (b == 0) return off + 1;
		if ((b & 0xC0u) == 0xC0u) {
			/* compression pointer */
			if (off + 1 >= len) return 0;
			return off + 2;
		}
		uint8_t labellen = b;
		off++;
		if (off + labellen > len) return 0;
		off += labellen;
	}
}

static inline int dns_try_server_aaaa(const struct in6_addr *ns_ip, const char *host, uint8_t out_ip[16])
{
	int fd = sys_socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if (fd < 0) return -1;

	/* Set a small recv timeout so we can try secondary server. */
	struct timeval tv;
	tv.tv_sec = 1;
	tv.tv_usec = 0;
	(void)sys_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, (uint32_t)sizeof(tv));

	struct sockaddr_in6 ns;
	c_memset(&ns, 0, sizeof(ns));
	ns.sin6_family = (uint16_t)AF_INET6;
	ns.sin6_port = htons(53);
	ns.sin6_addr = *ns_ip;

	uint8_t pkt[512];
	c_memset(pkt, 0, sizeof(pkt));
	struct dns_header *h = (struct dns_header *)pkt;

	uint16_t id = 0x1234;
	/* Best effort randomize id if available */
	uint16_t rid = 0;
	if (sys_getrandom(&rid, sizeof(rid), 0) == (long)sizeof(rid)) {
		id = rid;
	}

	h->id = htons(id);
	h->flags = htons(0x0100); /* RD */
	h->qdcount = htons(1);

	size_t off = sizeof(struct dns_header);
	size_t qn = dns_write_qname(&pkt[off], sizeof(pkt) - off, host);
	if (qn == 0) {
		sys_close(fd);
		return -1;
	}
	off += qn;
	if (off + 4 > sizeof(pkt)) {
		sys_close(fd);
		return -1;
	}
	/* QTYPE AAAA (28), QCLASS IN (1) */
	pkt[off + 0] = 0;
	pkt[off + 1] = 28;
	pkt[off + 2] = 0;
	pkt[off + 3] = 1;
	off += 4;

	ssize_t sent = sys_sendto(fd, pkt, off, 0, &ns, (uint32_t)sizeof(ns));
	if (sent < 0) {
		sys_close(fd);
		return -1;
	}

	uint8_t resp[512];
	struct sockaddr_in6 from;
	socklen_t fromlen = (socklen_t)sizeof(from);
	ssize_t rcv = sys_recvfrom(fd, resp, sizeof(resp), 0, &from, &fromlen);
	sys_close(fd);
	if (rcv < (ssize_t)sizeof(struct dns_header)) {
		return -1;
	}

	size_t rlen = (size_t)rcv;
	const struct dns_header *rh = (const struct dns_header *)resp;
	if (ntohs(rh->id) != id) {
		return -1;
	}
	uint16_t flags = ntohs(rh->flags);
	if ((flags & 0x8000u) == 0) {
		return -1; /* not a response */
	}
	if ((flags & 0x000Fu) != 0) {
		return -1; /* rcode != 0 */
	}

	uint16_t qd = ntohs(rh->qdcount);
	uint16_t an = ntohs(rh->ancount);
	if (qd < 1 || an < 1) {
		return -1;
	}

	size_t roff = sizeof(struct dns_header);
	/* Skip questions */
	for (uint16_t qi = 0; qi < qd; qi++) {
		size_t noff = dns_skip_name(resp, rlen, roff);
		if (noff == 0 || noff + 4 > rlen) return -1;
		off = noff + 4;
		roff = off;
	}

	/* Parse answers; take first AAAA record */
	for (uint16_t ai = 0; ai < an; ai++) {
		size_t noff = dns_skip_name(resp, rlen, roff);
		if (noff == 0 || noff + 10 > rlen) return -1;
		uint16_t type = (uint16_t)((resp[noff + 0] << 8) | resp[noff + 1]);
		uint16_t cls = (uint16_t)((resp[noff + 2] << 8) | resp[noff + 3]);
		uint16_t rdlen = (uint16_t)((resp[noff + 8] << 8) | resp[noff + 9]);
		size_t rdata = noff + 10;
		if (rdata + rdlen > rlen) return -1;

		if (type == 28 && cls == 1 && rdlen == 16) {
			c_memcpy(out_ip, &resp[rdata], 16);
			return 0;
		}
		roff = rdata + rdlen;
	}

	return -1;
}

static inline int dns_resolve_aaaa_google(const char *host, uint8_t out_ip[16])
{
	struct in6_addr ns;
	google_dns_primary(&ns);
	if (dns_try_server_aaaa(&ns, host, out_ip) == 0) return 0;
	google_dns_secondary(&ns);
	return dns_try_server_aaaa(&ns, host, out_ip);
}
