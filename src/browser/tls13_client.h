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

/* Performs a TLS 1.3 HTTPS GET and returns response status line, Location (if any),
 * and an initial slice of the response body.
 *
 * If Content-Length is present, reads up to min(Content-Length, body_cap) bytes.
 * If Content-Length is absent, reads until close (best-effort) or until body_cap.
 */
int tls13_https_get_status_location_and_body(int sock,
					const char *host,
					const char *path,
					char *status_line,
					size_t status_line_len,
					int *status_code_out,
					char *location_out,
					size_t location_out_len,
					char *content_type_out,
					size_t content_type_out_len,
					char *content_encoding_out,
					size_t content_encoding_out_len,
					uint8_t *body,
					size_t body_cap,
					size_t *body_len_out,
					uint64_t *content_length_out);

/* TLS 1.3 application traffic keys for TLS_AES_128_GCM_SHA256.
 * Exposed so the keep-alive connection can reuse the existing record helpers.
 */
struct tls13_aead {
	uint8_t key[16];
	uint8_t iv[12];
	uint64_t seq;
	int valid;
};

/* Reusable keep-alive connection (single host per connection).
 *
 * Notes:
 * - This is intentionally tiny: no certificate validation yet.
 * - Responses are always drained fully so subsequent requests stay in sync,
 *   even if the caller only requested a prefix.
 */
struct tls13_https_conn {
	int sock;
	char host[128];
	uint8_t alive;
	/* Application traffic keys (TLS 1.3). */
	struct tls13_aead tx_app, rx_app;
	/* Plaintext bytes that were read but belong to the next response. */
	uint8_t stash[8192];
	size_t stash_len;
};

int tls13_https_conn_open(struct tls13_https_conn *c, int sock, const char *host);
void tls13_https_conn_close(struct tls13_https_conn *c);

/* Performs an HTTP/1.1 GET over an established TLS13 connection.
 *
 * - keep_alive_request: if nonzero, emits Connection: keep-alive.
 * - out_peer_wants_close: set to 1 if the peer requests close or if the
 *   response framing is not compatible with keep-alive.
 * - body receives up to body_cap bytes (may be a prefix); the full response
 *   body is still drained.
 */
int tls13_https_conn_get_status_location_and_body(struct tls13_https_conn *c,
						const char *path,
						char *status_line,
						size_t status_line_len,
						int *status_code_out,
						char *location_out,
						size_t location_out_len,
						char *content_type_out,
						size_t content_type_out_len,
						char *content_encoding_out,
						size_t content_encoding_out_len,
						uint8_t *body,
						size_t body_cap,
						size_t *body_len_out,
						uint64_t *content_length_out,
						int keep_alive_request,
						int *out_peer_wants_close);
