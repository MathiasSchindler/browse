#pragma once

#include "../core/syscall.h"

/* Minimal TLS 1.3 client for TLS_AES_128_GCM_SHA256 over an already-connected
 * TCP socket.
 *
 * Current scope (intentionally tiny):
 * - IPv6 TCP connect is done by caller.
 * - X25519 key share, no PSK, no 0-RTT.
 * - No certificate validation yet (insecure; for bring-up only).
 */

/* Performs a TLS 1.3 handshake with SNI=host, then sends an HTTP/1.1 GET for
 * (host,path) using the existing http formatter.
 *
 * On success, writes the HTTP status line (without CRLF) into status_line.
 * Returns 0 on success, negative on failure.
 */
int tls13_https_get_status_line(int sock,
				const char *host,
				const char *path,
				char *status_line,
				size_t status_line_len);

/* Like tls13_https_get_status_line, but also parses response headers enough to
 * extract the numeric status code and a Location header (if present).
 *
 * location_out is set to an empty string if not present.
 */
int tls13_https_get_status_and_location(int sock,
					const char *host,
					const char *path,
					char *status_line,
					size_t status_line_len,
					int *status_code_out,
					char *location_out,
					size_t location_out_len);
