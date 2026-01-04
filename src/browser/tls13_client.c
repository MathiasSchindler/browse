#include "tls13_client.h"

#include "http.h"
#include "http_parse.h"
#include "net_ip6.h"
#include "url.h"
#include "util.h"

#include "../core/log.h"

#include "../tls/gcm.h"
#include "../tls/hkdf_sha256.h"
#include "../tls/hmac_sha256.h"
#include "../tls/sha256.h"
#include "../tls/tls13_kdf.h"
#include "../tls/x25519.h"

#define TLS13_CIPHER_TLS_AES_128_GCM_SHA256 0x1301u

#define TLS13_MAX_RECORD (18432u)

static int read_full(int fd, uint8_t *buf, size_t n)
{
	size_t off = 0;
	while (off < n) {
		long r = sys_read(fd, buf + off, n - off);
		if (r <= 0) return -1;
		off += (size_t)r;
	}
	return 0;
}

static int write_full(int fd, const uint8_t *buf, size_t n)
{
	size_t off = 0;
	while (off < n) {
		long r = sys_write(fd, buf + off, n - off);
		if (r <= 0) return -1;
		off += (size_t)r;
	}
	return 0;
}

struct http_resp_feed_ctx {
	char *status_line;
	size_t status_line_len;
	int *status_code_out;

	char *location_out;
	size_t location_out_len;

	char *content_type_out;
	size_t content_type_out_len;

	char *content_encoding_out;
	size_t content_encoding_out_len;

	uint8_t *body;
	size_t body_cap;

	char *line;
	size_t line_cap;
	size_t line_len;

	uint8_t *header_buf;
	size_t header_cap;
	size_t header_len;

	int got_status;
	int got_headers_end;

	uint64_t content_len;
	int have_content_len;
	int is_chunked;
	int peer_close;

	struct http_chunked_dec *chunked;
	int chunked_done;

	uint64_t body_total_read;
	size_t body_stored;
};

static int http_resp_feed_body(struct http_resp_feed_ctx *ctx, const uint8_t *in, size_t in_len)
{
	if (!ctx || !in) return -1;

	if (ctx->is_chunked) {
		size_t off = 0;
		while (off < in_len) {
			size_t in_used = 0;
			size_t wrote = 0;
			uint8_t *outp = (ctx->body && ctx->body_stored < ctx->body_cap) ? (ctx->body + ctx->body_stored) : 0;
			size_t out_cap2 = (ctx->body && ctx->body_stored < ctx->body_cap) ? (ctx->body_cap - ctx->body_stored) : 0;
			int r = http_chunked_feed(ctx->chunked,
						in + off, in_len - off, &in_used,
						outp, out_cap2, &wrote);
			ctx->body_stored += wrote;
			ctx->body_total_read += wrote;
			if (r < 0) return -1;
			if (r == 1) ctx->chunked_done = 1;
			if (in_used == 0) return -1;
			off += in_used;
			if (ctx->chunked_done) break;
		}
		return 0;
	}

	size_t can_read = in_len;
	if (ctx->have_content_len) {
		if (ctx->body_total_read >= ctx->content_len) return 0;
		uint64_t remain = ctx->content_len - ctx->body_total_read;
		if ((uint64_t)can_read > remain) can_read = (size_t)remain;
	}

	if (ctx->body && ctx->body_stored < ctx->body_cap) {
		size_t cap = ctx->body_cap - ctx->body_stored;
		size_t to_store = (can_read < cap) ? can_read : cap;
		if (to_store) {
			crypto_memcpy(ctx->body + ctx->body_stored, in, to_store);
			ctx->body_stored += to_store;
		}
	}
	ctx->body_total_read += (uint64_t)can_read;
	return 0;
}

static int http_resp_feed(struct http_resp_feed_ctx *ctx, const uint8_t *in, size_t in_len, size_t *out_used)
{
	if (!ctx || !in) return -1;

	size_t used = 0;
	while (used < in_len) {
		uint8_t b = in[used];

		if (!ctx->got_headers_end) {
			if (ctx->header_len >= ctx->header_cap) return -1;
			ctx->header_buf[ctx->header_len++] = b;

			if (ctx->line_len + 1 < ctx->line_cap) {
				ctx->line[ctx->line_len++] = (char)b;
				ctx->line[ctx->line_len] = 0;
			}

			if (b == '\n') {
				if (!ctx->got_status) {
					size_t n = ctx->line_len;
					while (n > 0 && (ctx->line[n - 1] == '\n' || ctx->line[n - 1] == '\r')) n--;
					size_t w = 0;
					for (; w < n && w + 1 < ctx->status_line_len; w++) ctx->status_line[w] = ctx->line[w];
					ctx->status_line[w] = 0;
					if (ctx->status_code_out) *ctx->status_code_out = http_parse_status_code(ctx->status_line);
					ctx->got_status = 1;
				} else {
					if (!ctx->have_content_len) {
						uint64_t v = 0;
						if (http_parse_content_length_line(ctx->line, &v) == 0) {
							ctx->have_content_len = 1;
							ctx->content_len = v;
						}
					}
					if (!ctx->is_chunked) {
						char tmp[256];
						if (http_header_extract_value(ctx->line, "Transfer-Encoding", tmp, sizeof(tmp)) == 0) {
							if (http_value_has_token_ci(tmp, "chunked")) ctx->is_chunked = 1;
						}
					}
					if (!ctx->peer_close) {
						char tmp[256];
						if (http_header_extract_value(ctx->line, "Connection", tmp, sizeof(tmp)) == 0) {
							if (http_value_has_token_ci(tmp, "close")) ctx->peer_close = 1;
						}
					}
					if (ctx->location_out && ctx->location_out_len && ctx->location_out[0] == 0) {
						char tmp[512];
						if (http_header_extract_value(ctx->line, "Location", tmp, sizeof(tmp)) == 0) {
							(void)c_strlcpy_s(ctx->location_out, ctx->location_out_len, tmp);
						}
					}
					if (ctx->content_type_out && ctx->content_type_out_len && ctx->content_type_out[0] == 0) {
						char tmp[256];
						if (http_header_extract_value(ctx->line, "Content-Type", tmp, sizeof(tmp)) == 0) {
							(void)c_strlcpy_s(ctx->content_type_out, ctx->content_type_out_len, tmp);
						}
					}
					if (ctx->content_encoding_out && ctx->content_encoding_out_len && ctx->content_encoding_out[0] == 0) {
						char tmp[256];
						if (http_header_extract_value(ctx->line, "Content-Encoding", tmp, sizeof(tmp)) == 0) {
							(void)c_strlcpy_s(ctx->content_encoding_out, ctx->content_encoding_out_len, tmp);
						}
					}
				}
				ctx->line_len = 0;
				ctx->line[0] = 0;
			}

			size_t hdr_end = 0;
			if (http_find_header_end(ctx->header_buf, ctx->header_len, &hdr_end) == 0) {
				ctx->got_headers_end = 1;
				/* Flush any bytes already beyond header end as body. */
				size_t pre_body = ctx->header_len - hdr_end;
				if (pre_body) {
					const uint8_t *pb = ctx->header_buf + hdr_end;
					if (http_resp_feed_body(ctx, pb, pre_body) != 0) return -1;
				}
			}
		} else {
			/* Body */
			if (http_resp_feed_body(ctx, &b, 1) != 0) return -1;
		}

		used++;

		if (ctx->got_headers_end) {
			if (ctx->is_chunked) {
				if (ctx->chunked_done) break;
			} else if (ctx->have_content_len) {
				if (ctx->body_total_read >= ctx->content_len) break;
			} else {
				/* No framing known; only safe to keep-alive if peer closes. */
				break;
			}
		}
	}

	if (out_used) *out_used = used;
	if (!ctx->got_headers_end) return 0;
	if (ctx->is_chunked) return ctx->chunked_done ? 1 : 0;
	if (ctx->have_content_len) return (ctx->body_total_read >= ctx->content_len) ? 1 : 0;
	return 1;
}

static void sha256_ctx_digest(const struct sha256_ctx *ctx, uint8_t out[32])
{
	struct sha256_ctx tmp = *ctx;
	sha256_final(&tmp, out);
}

static void nonce_from_iv_seq(uint8_t nonce[12], const uint8_t iv[12], uint64_t seq)
{
	for (size_t i = 0; i < 12; i++) nonce[i] = iv[i];
	/* XOR seq (big-endian) into last 8 bytes */
	for (size_t i = 0; i < 8; i++) {
		nonce[12 - 1 - i] ^= (uint8_t)((seq >> (8u * i)) & 0xffu);
	}
}

static void put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)((v >> 8) & 0xffu);
	p[1] = (uint8_t)(v & 0xffu);
}

static void put_u24(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)((v >> 16) & 0xffu);
	p[1] = (uint8_t)((v >> 8) & 0xffu);
	p[2] = (uint8_t)(v & 0xffu);
}

static uint16_t get_u16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t get_u24(const uint8_t *p)
{
	return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static void tls13_aead_reset(struct tls13_aead *a)
{
	a->seq = 0;
}

static void tls13_aead_invalidate(struct tls13_aead *a)
{
	if (!a) return;
	a->seq = 0;
	a->valid = 0;
	crypto_memset(a->key, 0, sizeof(a->key));
	crypto_memset(a->iv, 0, sizeof(a->iv));
}

static int build_client_hello(const char *host,
			    uint8_t out_hs[1024], size_t *out_hs_len,
			    uint8_t priv[X25519_KEY_SIZE], uint8_t pub[X25519_KEY_SIZE]);
static int send_plain_handshake_record(int fd, const uint8_t *hs, size_t hs_len);
static int tls_read_record(int fd, uint8_t hdr[5], uint8_t *payload, size_t payload_cap, size_t *payload_len);
static int parse_server_hello(const uint8_t *hs, size_t hs_len, uint8_t server_pub[X25519_KEY_SIZE]);
static int derive_hs_traffic(const struct sha256_ctx *transcript,
			     const uint8_t shared[X25519_KEY_SIZE],
			     uint8_t c_hs_traffic[32], uint8_t s_hs_traffic[32],
			     struct tls13_aead *out_tx_hs, struct tls13_aead *out_rx_hs);
static int tls13_open_record(struct tls13_aead *rx,
			    const uint8_t hdr[5],
			    const uint8_t *payload, size_t payload_len,
			    uint8_t *out, size_t out_cap,
			    uint8_t *out_type, size_t *out_len);
static int derive_app_traffic(const struct sha256_ctx *transcript,
			      const uint8_t shared[X25519_KEY_SIZE],
			      uint8_t c_ap_traffic[32], uint8_t s_ap_traffic[32],
			      struct tls13_aead *out_tx_app, struct tls13_aead *out_rx_app);
static int tls13_seal_record(int fd, struct tls13_aead *tx,
			    uint8_t inner_type,
			    const uint8_t *in, size_t in_len);
static int tls_read_record_stream(int fd,
				 uint8_t hdr[5],
				 uint8_t *payload, size_t payload_cap,
				 size_t *payload_len);

static int tls13_handshake_to_app(int sock, const char *host, struct tls13_aead *out_tx_app, struct tls13_aead *out_rx_app)
{
	if (!host || !out_tx_app || !out_rx_app) return -1;
	*out_tx_app = (struct tls13_aead){0};
	*out_rx_app = (struct tls13_aead){0};

	/* Avoid hanging forever during bring-up. */
	{
		struct timeval tv;
		tv.tv_sec = 3;
		tv.tv_usec = 0;
		(void)sys_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, (uint32_t)sizeof(tv));
		(void)sys_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, (uint32_t)sizeof(tv));
	}

	struct sha256_ctx transcript;
	sha256_init(&transcript);

	uint8_t priv[X25519_KEY_SIZE];
	uint8_t pub[X25519_KEY_SIZE];
	uint8_t ch_hs[1024];
	size_t ch_hs_len = 0;
	if (build_client_hello(host, ch_hs, &ch_hs_len, priv, pub) != 0) return -1;

	/* Send ClientHello */
	if (send_plain_handshake_record(sock, ch_hs, ch_hs_len) != 0) return -1;
	sha256_update(&transcript, ch_hs, ch_hs_len);

	/* Read until ServerHello */
	uint8_t hdr[5];
	uint8_t payload[TLS13_MAX_RECORD];
	size_t payload_len = 0;
	uint8_t server_pub[X25519_KEY_SIZE];
	uint8_t sh_hs[1024];
	size_t sh_hs_len = 0;
	for (;;) {
		if (tls_read_record(sock, hdr, payload, sizeof(payload), &payload_len) != 0) return -1;
		uint8_t typ = hdr[0];
		if (typ == 0x14) {
			/* ChangeCipherSpec: ignore. */
			continue;
		}
		if (typ != 0x16) return -1;
		if (payload_len < 4) return -1;
		uint8_t hs_type = payload[0];
		uint32_t body_len = get_u24(&payload[1]);
		if (hs_type != 0x02) return -1;
		if ((size_t)body_len + 4u > payload_len) return -1;
		sh_hs_len = (size_t)body_len + 4u;
		crypto_memcpy(sh_hs, payload, sh_hs_len);
		break;
	}

	if (parse_server_hello(sh_hs, sh_hs_len, server_pub) != 0) return -1;
	sha256_update(&transcript, sh_hs, sh_hs_len);

	uint8_t shared[32];
	x25519(shared, priv, server_pub);
	crypto_memset(priv, 0, sizeof(priv));
	crypto_memset(pub, 0, sizeof(pub));
	crypto_memset(server_pub, 0, sizeof(server_pub));

	struct tls13_aead tx_hs = (struct tls13_aead){0};
	struct tls13_aead rx_hs = (struct tls13_aead){0};
	struct tls13_aead tx_app = (struct tls13_aead){0};
	struct tls13_aead rx_app = (struct tls13_aead){0};
	uint8_t c_hs_traffic[32];
	uint8_t s_hs_traffic[32];
	if (derive_hs_traffic(&transcript, shared, c_hs_traffic, s_hs_traffic, &tx_hs, &rx_hs) != 0) return -1;

	/* Process encrypted handshake messages until server Finished. */
	uint8_t got_server_finished = 0;
	uint8_t server_finished_verify[32];
	crypto_memset(server_finished_verify, 0, sizeof(server_finished_verify));

	uint8_t dec[TLS13_MAX_RECORD];
	uint8_t dec_type = 0;
	size_t dec_len = 0;

	for (;;) {
		if (tls_read_record(sock, hdr, payload, sizeof(payload), &payload_len) != 0) return -1;
		uint8_t typ = hdr[0];
		if (typ == 0x14) continue;
		if (typ != 0x17) return -1;

		if (tls13_open_record(&rx_hs, hdr, payload, payload_len, dec, sizeof(dec), &dec_type, &dec_len) != 0) return -1;
		if (dec_type != 0x16) {
			continue;
		}

		size_t off = 0;
		while (off + 4u <= dec_len) {
			uint8_t hs_type = dec[off];
			uint32_t bl = get_u24(&dec[off + 1]);
			if (off + 4u + (size_t)bl > dec_len) return -1;
			const uint8_t *hs_msg = &dec[off];
			size_t hs_msg_len = 4u + (size_t)bl;

			if (hs_type == 0x14) {
				uint8_t finished_key[32];
				if (tls13_hkdf_expand_label_sha256(s_hs_traffic, "finished", NULL, 0, finished_key, 32) != 0) return -1;
				uint8_t th[32];
				sha256_ctx_digest(&transcript, th);
				hmac_sha256(finished_key, 32, th, 32, server_finished_verify);
				crypto_memset(finished_key, 0, sizeof(finished_key));
				crypto_memset(th, 0, sizeof(th));
				if (bl != 32u) return -1;
				if (!crypto_memeq(server_finished_verify, hs_msg + 4, 32)) return -1;
				sha256_update(&transcript, hs_msg, hs_msg_len);
				got_server_finished = 1;
				break;
			}

			sha256_update(&transcript, hs_msg, hs_msg_len);
			off += hs_msg_len;
		}

		if (got_server_finished) break;
	}

	/* Derive application traffic secrets using transcript up to server Finished. */
	uint8_t c_ap_traffic[32];
	uint8_t s_ap_traffic[32];
	if (derive_app_traffic(&transcript, shared, c_ap_traffic, s_ap_traffic, &tx_app, &rx_app) != 0) return -1;

	/* Server may start using application keys after Finished. */
	tls13_aead_reset(&rx_app);

	/* Send client Finished under handshake keys. */
	uint8_t client_finished_key[32];
	if (tls13_hkdf_expand_label_sha256(c_hs_traffic, "finished", NULL, 0, client_finished_key, 32) != 0) return -1;
	uint8_t th[32];
	sha256_ctx_digest(&transcript, th);
	uint8_t verify[32];
	hmac_sha256(client_finished_key, 32, th, 32, verify);
	crypto_memset(client_finished_key, 0, sizeof(client_finished_key));
	crypto_memset(th, 0, sizeof(th));

	uint8_t fin_hs[4 + 32];
	fin_hs[0] = 0x14;
	put_u24(&fin_hs[1], 32);
	crypto_memcpy(&fin_hs[4], verify, 32);
	crypto_memset(verify, 0, sizeof(verify));

	if (tls13_seal_record(sock, &tx_hs, 0x16, fin_hs, sizeof(fin_hs)) != 0) return -1;
	sha256_update(&transcript, fin_hs, sizeof(fin_hs));
	crypto_memset(fin_hs, 0, sizeof(fin_hs));

	/* Now use application write keys. */
	tls13_aead_reset(&tx_app);

	/* Export app keys and clear sensitive temporaries. */
	*out_tx_app = tx_app;
	*out_rx_app = rx_app;

	crypto_memset(shared, 0, sizeof(shared));
	crypto_memset(c_hs_traffic, 0, sizeof(c_hs_traffic));
	crypto_memset(s_hs_traffic, 0, sizeof(s_hs_traffic));
	crypto_memset(c_ap_traffic, 0, sizeof(c_ap_traffic));
	crypto_memset(s_ap_traffic, 0, sizeof(s_ap_traffic));
	crypto_memset(server_finished_verify, 0, sizeof(server_finished_verify));
	crypto_memset(dec, 0, sizeof(dec));
	crypto_memset(payload, 0, sizeof(payload));
	return 0;
}

int tls13_https_conn_open(struct tls13_https_conn *c, int sock, const char *host)
{
	if (!c || sock < 0 || !host || !host[0]) return -1;
	c->sock = sock;
	c->alive = 0;
	c->stash_len = 0;
	(void)c_strlcpy_s(c->host, sizeof(c->host), host);
	tls13_aead_invalidate(&c->tx_app);
	tls13_aead_invalidate(&c->rx_app);
	if (tls13_handshake_to_app(sock, host, &c->tx_app, &c->rx_app) != 0) {
		return -1;
	}
	c->alive = 1;
	return 0;
}

void tls13_https_conn_close(struct tls13_https_conn *c)
{
	if (!c) return;
	if (c->alive && c->sock >= 0) {
		sys_close(c->sock);
	}
	c->sock = -1;
	c->alive = 0;
	c->stash_len = 0;
	tls13_aead_invalidate(&c->tx_app);
	tls13_aead_invalidate(&c->rx_app);
}

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
						int *out_peer_wants_close)
{
	if (!c || !c->alive || c->sock < 0 || !path || !status_line || status_line_len == 0) return -1;
	status_line[0] = 0;
	if (status_code_out) *status_code_out = -1;
	if (location_out && location_out_len) location_out[0] = 0;
	if (content_type_out && content_type_out_len) content_type_out[0] = 0;
	if (content_encoding_out && content_encoding_out_len) content_encoding_out[0] = 0;
	if (body_len_out) *body_len_out = 0;
	if (content_length_out) *content_length_out = 0;
	if (out_peer_wants_close) *out_peer_wants_close = 0;

	char req[768];
	int req_len = http_format_get_ex(req, sizeof(req), c->host, path, keep_alive_request ? 1 : 0);
	if (req_len < 0) return -1;
	if (tls13_seal_record(c->sock, &c->tx_app, 0x17, (const uint8_t *)req, (size_t)req_len) != 0) return -1;
	crypto_memset(req, 0, sizeof(req));

	char line[512];
	uint8_t header_buf[8192];
	struct http_chunked_dec chunked;
	http_chunked_init(&chunked);

	struct http_resp_feed_ctx feed;
	feed.status_line = status_line;
	feed.status_line_len = status_line_len;
	feed.status_code_out = status_code_out;
	feed.location_out = location_out;
	feed.location_out_len = location_out_len;
	feed.content_type_out = content_type_out;
	feed.content_type_out_len = content_type_out_len;
	feed.content_encoding_out = content_encoding_out;
	feed.content_encoding_out_len = content_encoding_out_len;
	feed.body = body;
	feed.body_cap = body_cap;
	feed.line = line;
	feed.line_cap = sizeof(line);
	feed.line_len = 0;
	feed.header_buf = header_buf;
	feed.header_cap = sizeof(header_buf);
	feed.header_len = 0;
	feed.got_status = 0;
	feed.got_headers_end = 0;
	feed.content_len = 0;
	feed.have_content_len = 0;
	feed.is_chunked = 0;
	feed.peer_close = 0;
	feed.chunked = &chunked;
	feed.chunked_done = 0;
	feed.body_total_read = 0;
	feed.body_stored = 0;

	uint8_t hdr[5];
	uint8_t payload[TLS13_MAX_RECORD];
	size_t payload_len = 0;
	uint8_t dec[TLS13_MAX_RECORD];
	uint8_t dec_type = 0;
	size_t dec_len = 0;

	/* Consume any stashed plaintext bytes first. */
	if (c->stash_len) {
		size_t used = 0;
		int done = http_resp_feed(&feed, c->stash, c->stash_len, &used);
		if (done < 0) return -1;
		if (used < c->stash_len) {
			/* shift remaining */
			size_t rem = c->stash_len - used;
			for (size_t i = 0; i < rem; i++) c->stash[i] = c->stash[used + i];
			c->stash_len = rem;
		} else {
			c->stash_len = 0;
		}
		if (done == 1) {
			goto out_done;
		}
	}

	for (;;) {
		int rr = tls_read_record_stream(c->sock, hdr, payload, sizeof(payload), &payload_len);
		if (rr == 1) {
			/* EOF */
			feed.peer_close = 1;
			break;
		}
		if (rr != 0) return -1;
		uint8_t typ = hdr[0];
		if (typ == 0x14) continue;
		if (typ != 0x17) continue;
		if (tls13_open_record(&c->rx_app, hdr, payload, payload_len, dec, sizeof(dec), &dec_type, &dec_len) != 0) return -1;
		if (dec_type == 0x15) {
			feed.peer_close = 1;
			break;
		}
		if (dec_type != 0x17) continue;

		size_t used = 0;
		int done = http_resp_feed(&feed, dec, dec_len, &used);
		if (done < 0) return -1;
		if (done == 1) {
			/* Stash any remaining plaintext for the next response. */
			if (used < dec_len) {
				size_t rem = dec_len - used;
				if (rem <= sizeof(c->stash)) {
					/* Replace stash (no pipelining expected). */
					for (size_t i = 0; i < rem; i++) c->stash[i] = dec[used + i];
					c->stash_len = rem;
				} else {
					c->stash_len = 0;
				}
			}
			break;
		}
	}

out_done:
	if (body_len_out) *body_len_out = feed.body_stored;
	if (content_length_out) *content_length_out = (!feed.is_chunked && feed.have_content_len) ? feed.content_len : 0;

	/* Decide whether the connection can be safely reused. */
	if (!keep_alive_request) {
		/* Legacy callers can close; no requirement to be reusable. */
		if (out_peer_wants_close) *out_peer_wants_close = 1;
		return 0;
	}

	if (!feed.got_headers_end) feed.peer_close = 1;
	if (!feed.is_chunked && !feed.have_content_len) feed.peer_close = 1;
	if (feed.peer_close && out_peer_wants_close) *out_peer_wants_close = 1;
	return 0;
}

static int tls_read_record(int fd, uint8_t hdr[5], uint8_t *payload, size_t payload_cap, size_t *payload_len)
{
	if (read_full(fd, hdr, 5) != 0) return -1;
	size_t len = ((size_t)hdr[3] << 8) | (size_t)hdr[4];
	if (len > payload_cap) return -1;
	if (read_full(fd, payload, len) != 0) return -1;
	*payload_len = len;
	return 0;
}

static int tls13_open_record(struct tls13_aead *rx,
			    const uint8_t hdr[5],
			    const uint8_t *payload, size_t payload_len,
			    uint8_t *out, size_t out_cap,
			    uint8_t *out_type, size_t *out_len)
{
	if (!rx || !rx->valid) return -1;
	if (payload_len < GCM_TAG_SIZE) return -1;

	size_t ct_len = payload_len - GCM_TAG_SIZE;
	if (ct_len > out_cap) return -1;

	uint8_t nonce[12];
	nonce_from_iv_seq(nonce, rx->iv, rx->seq);

	int ok = aes128_gcm_decrypt(rx->key, nonce, sizeof(nonce), hdr, 5,
				  payload, ct_len, out, payload + ct_len);
	crypto_memset(nonce, 0, sizeof(nonce));
	if (!ok) return -1;
	rx->seq++;

	/* TLSInnerPlaintext: content || type || zeros */
	size_t end = ct_len;
	while (end > 0 && out[end - 1] == 0) end--;
	if (end == 0) return -1;
	*out_type = out[end - 1];
	*out_len = end - 1;
	return 0;
}

static int tls13_seal_record(int fd, struct tls13_aead *tx,
			    uint8_t inner_type,
			    const uint8_t *in, size_t in_len)
{
	if (!tx || !tx->valid) return -1;
	if (in_len + 1u > (TLS13_MAX_RECORD - 5u - GCM_TAG_SIZE)) return -1;

	uint8_t hdr[5];
	uint8_t nonce[12];
	uint8_t tag[GCM_TAG_SIZE];

	uint8_t pt[TLS13_MAX_RECORD];
	size_t pt_len = in_len + 1u;
	crypto_memcpy(pt, in, in_len);
	pt[in_len] = inner_type;

	size_t rec_len = pt_len + GCM_TAG_SIZE;
	hdr[0] = 0x17;
	hdr[1] = 0x03;
	hdr[2] = 0x03;
	hdr[3] = (uint8_t)((rec_len >> 8) & 0xffu);
	hdr[4] = (uint8_t)(rec_len & 0xffu);

	uint8_t ct[TLS13_MAX_RECORD];
	nonce_from_iv_seq(nonce, tx->iv, tx->seq);
	aes128_gcm_encrypt(tx->key, nonce, sizeof(nonce), hdr, 5, pt, pt_len, ct, tag);
	crypto_memset(nonce, 0, sizeof(nonce));
	crypto_memset(pt, 0, pt_len);

	if (write_full(fd, hdr, 5) != 0) return -1;
	if (write_full(fd, ct, pt_len) != 0) return -1;
	if (write_full(fd, tag, sizeof(tag)) != 0) return -1;
	crypto_memset(ct, 0, pt_len);
	crypto_memset(tag, 0, sizeof(tag));

	tx->seq++;
	return 0;
}

static int send_plain_handshake_record(int fd, const uint8_t *hs, size_t hs_len)
{
	uint8_t hdr[5];
	hdr[0] = 0x16; /* handshake */
	hdr[1] = 0x03;
	hdr[2] = 0x01; /* legacy record version */
	hdr[3] = (uint8_t)((hs_len >> 8) & 0xffu);
	hdr[4] = (uint8_t)(hs_len & 0xffu);
	if (write_full(fd, hdr, 5) != 0) return -1;
	if (write_full(fd, hs, hs_len) != 0) return -1;
	return 0;
}

static int build_client_hello(const char *host,
			    uint8_t out_hs[1024], size_t *out_hs_len,
			    uint8_t priv[X25519_KEY_SIZE], uint8_t pub[X25519_KEY_SIZE])
{
	uint8_t ch_body[512];
	uint8_t rnd[32];
	if (sys_getrandom(rnd, sizeof(rnd), 0) != (long)sizeof(rnd)) return -1;
	if (sys_getrandom(priv, X25519_KEY_SIZE, 0) != (long)X25519_KEY_SIZE) return -1;
	x25519_base(pub, priv);

	uint8_t *p = ch_body;
	put_u16(p, 0x0303);
	p += 2;
	crypto_memcpy(p, rnd, sizeof(rnd));
	p += sizeof(rnd);
	*p++ = 0; /* legacy_session_id_len */

	/* cipher_suites */
	/*
	 * Only advertise ciphers we actually implement.
	 * Some servers will pick the strongest common suite.
	 */
	put_u16(p, 2);
	p += 2;
	put_u16(p, (uint16_t)TLS13_CIPHER_TLS_AES_128_GCM_SHA256);
	p += 2;

	/* legacy_compression_methods */
	*p++ = 1;
	*p++ = 0;

	uint8_t *ext_len_p = p;
	p += 2;
	uint8_t *ext_start = p;

	/* server_name (SNI) */
	{
		size_t host_len = 0;
		while (host && host[host_len]) host_len++;
		uint16_t ext_type = 0x0000;
		uint16_t name_list_len = (uint16_t)(1u + 2u + host_len);
		uint16_t ext_len = (uint16_t)(2u + name_list_len);
		put_u16(p, ext_type);
		p += 2;
		put_u16(p, ext_len);
		p += 2;
		put_u16(p, name_list_len);
		p += 2;
		*p++ = 0; /* host_name */
		put_u16(p, (uint16_t)host_len);
		p += 2;
		for (size_t i = 0; i < host_len; i++) *p++ = (uint8_t)host[i];
	}

	/* supported_groups */
	{
		put_u16(p, 0x000a);
		p += 2;
		put_u16(p, 4);
		p += 2;
		put_u16(p, 2);
		p += 2;
		put_u16(p, 0x001d); /* x25519 */
		p += 2;
	}

	/* signature_algorithms */
	{
		static const uint16_t sigalgs[] = {
			0x0804, /* rsa_pss_rsae_sha256 */
			0x0809, /* rsa_pss_pss_sha256 */
			0x0403, /* ecdsa_secp256r1_sha256 */
			0x0401, /* rsa_pkcs1_sha256 */
		};
		const uint16_t list_len = (uint16_t)(sizeof(sigalgs));
		put_u16(p, 0x000d);
		p += 2;
		put_u16(p, (uint16_t)(2u + list_len));
		p += 2;
		put_u16(p, list_len);
		p += 2;
		for (size_t i = 0; i < sizeof(sigalgs) / sizeof(sigalgs[0]); i++) {
			put_u16(p, sigalgs[i]);
			p += 2;
		}
	}

	/* ALPN: offer http/1.1 */
	{
		static const char proto[] = "http/1.1";
		put_u16(p, 0x0010);
		p += 2;
		/* ext_len = 2 + (1 + proto_len) */
		put_u16(p, (uint16_t)(2u + 1u + (uint16_t)(sizeof(proto) - 1u)));
		p += 2;
		put_u16(p, (uint16_t)(1u + (uint16_t)(sizeof(proto) - 1u)));
		p += 2;
		*p++ = (uint8_t)(sizeof(proto) - 1u);
		for (size_t i = 0; i < sizeof(proto) - 1u; i++) {
			*p++ = (uint8_t)proto[i];
		}
	}

	/* supported_versions */
	{
		put_u16(p, 0x002b);
		p += 2;
		put_u16(p, 3);
		p += 2;
		*p++ = 2;
		put_u16(p, 0x0304);
		p += 2;
	}

	/* key_share */
	{
		put_u16(p, 0x0033);
		p += 2;
		put_u16(p, (uint16_t)(2u + 2u + 2u + X25519_KEY_SIZE));
		p += 2;
		put_u16(p, (uint16_t)(2u + 2u + X25519_KEY_SIZE));
		p += 2;
		put_u16(p, 0x001d);
		p += 2;
		put_u16(p, X25519_KEY_SIZE);
		p += 2;
		crypto_memcpy(p, pub, X25519_KEY_SIZE);
		p += X25519_KEY_SIZE;
	}

	uint16_t ext_len = (uint16_t)(p - ext_start);
	put_u16(ext_len_p, ext_len);

	size_t ch_len = (size_t)(p - ch_body);
	if (ch_len + 4u > 1024u) return -1;
	out_hs[0] = 0x01; /* ClientHello */
	put_u24(&out_hs[1], (uint32_t)ch_len);
	crypto_memcpy(&out_hs[4], ch_body, ch_len);
	*out_hs_len = ch_len + 4u;
	return 0;
}

static int parse_server_hello(const uint8_t *hs, size_t hs_len,
			     uint8_t server_pub[X25519_KEY_SIZE])
{
	if (hs_len < 4u) return -1;
	if (hs[0] != 0x02) return -1;
	uint32_t body_len = get_u24(&hs[1]);
	if ((size_t)body_len + 4u != hs_len) return -1;
	const uint8_t *p = &hs[4];
	const uint8_t *end = p + body_len;
	if (end - p < 2 + 32 + 1) return -1;
	/* legacy_version */
	(void)get_u16(p);
	p += 2;
	/* random */
	const uint8_t hrr_random[32] = {
		0xcf,0x21,0xad,0x74,0xe5,0x9a,0x61,0x11,0xbe,0x1d,0x8c,0x02,0x1e,0x65,0xb8,0x91,
		0xc2,0xa2,0x11,0x16,0x7a,0xbb,0x8c,0x5e,0x07,0x9e,0x09,0xe2,0xc8,0xa8,0x33,0x9c,
	};
	if (crypto_memeq(p, hrr_random, 32)) {
		/* HelloRetryRequest not supported yet. */
		return -1;
	}
	p += 32;
	uint8_t sid_len = *p++;
	if (end - p < sid_len + 2 + 1 + 2) return -1;
	p += sid_len;
	uint16_t cs = get_u16(p);
	p += 2;
	if (cs != TLS13_CIPHER_TLS_AES_128_GCM_SHA256) return -1;
	/* legacy_compression_method */
	(void)*p++;
	uint16_t exts_len = get_u16(p);
	p += 2;
	if ((size_t)(end - p) != (size_t)exts_len) return -1;

	int got_vers = 0;
	int got_ks = 0;
	while (p + 4 <= end) {
		uint16_t et = get_u16(p);
		uint16_t el = get_u16(p + 2);
		p += 4;
		if (p + el > end) return -1;
		if (et == 0x002b) {
			if (el != 2) return -1;
			uint16_t v = get_u16(p);
			if (v != 0x0304) return -1;
			got_vers = 1;
		} else if (et == 0x0033) {
			if (el < 4) return -1;
			uint16_t group = get_u16(p);
			uint16_t klen = get_u16(p + 2);
			if (group != 0x001d || klen != X25519_KEY_SIZE) return -1;
			if (4u + (size_t)klen != (size_t)el) return -1;
			crypto_memcpy(server_pub, p + 4, X25519_KEY_SIZE);
			got_ks = 1;
		}
		p += el;
	}
	return (got_vers && got_ks) ? 0 : -1;
}

static int derive_hs_traffic(const struct sha256_ctx *transcript,
			     const uint8_t shared_secret[32],
			     uint8_t c_hs_traffic[32],
			     uint8_t s_hs_traffic[32],
			     struct tls13_aead *tx_hs,
			     struct tls13_aead *rx_hs)
{
	static const uint8_t sha256_empty[32] = {
		0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
		0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55,
	};
	static const uint8_t psk_zeros[32] = {0};
	uint8_t early_secret[32];
	hkdf_extract_sha256(NULL, 0, psk_zeros, sizeof(psk_zeros), early_secret);
	uint8_t derived[32];
	if (tls13_derive_secret_sha256(early_secret, "derived", sha256_empty, derived) != 0) return -1;

	uint8_t handshake_secret[32];
	hkdf_extract_sha256(derived, 32, shared_secret, 32, handshake_secret);

	uint8_t thash[32];
	sha256_ctx_digest(transcript, thash);
	if (tls13_derive_secret_sha256(handshake_secret, "c hs traffic", thash, c_hs_traffic) != 0) return -1;
	if (tls13_derive_secret_sha256(handshake_secret, "s hs traffic", thash, s_hs_traffic) != 0) return -1;

	/* traffic -> key/iv */
	if (tls13_hkdf_expand_label_sha256(c_hs_traffic, "key", NULL, 0, tx_hs->key, 16) != 0) return -1;
	if (tls13_hkdf_expand_label_sha256(c_hs_traffic, "iv", NULL, 0, tx_hs->iv, 12) != 0) return -1;
	if (tls13_hkdf_expand_label_sha256(s_hs_traffic, "key", NULL, 0, rx_hs->key, 16) != 0) return -1;
	if (tls13_hkdf_expand_label_sha256(s_hs_traffic, "iv", NULL, 0, rx_hs->iv, 12) != 0) return -1;
	tx_hs->seq = 0;
	rx_hs->seq = 0;
	tx_hs->valid = 1;
	rx_hs->valid = 1;
	crypto_memset(early_secret, 0, sizeof(early_secret));
	crypto_memset(derived, 0, sizeof(derived));
	crypto_memset(handshake_secret, 0, sizeof(handshake_secret));
	crypto_memset(thash, 0, sizeof(thash));
	return 0;
}

static int derive_app_traffic(const struct sha256_ctx *transcript,
			      const uint8_t shared_secret[32],
			      uint8_t c_ap_traffic[32],
			      uint8_t s_ap_traffic[32],
			      struct tls13_aead *tx_app,
			      struct tls13_aead *rx_app)
{
	static const uint8_t sha256_empty[32] = {
		0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
		0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55,
	};
	static const uint8_t psk_zeros[32] = {0};
	uint8_t early_secret[32];
	hkdf_extract_sha256(NULL, 0, psk_zeros, sizeof(psk_zeros), early_secret);
	uint8_t derived1[32];
	if (tls13_derive_secret_sha256(early_secret, "derived", sha256_empty, derived1) != 0) return -1;
	uint8_t handshake_secret[32];
	hkdf_extract_sha256(derived1, 32, shared_secret, 32, handshake_secret);
	uint8_t derived2[32];
	if (tls13_derive_secret_sha256(handshake_secret, "derived", sha256_empty, derived2) != 0) return -1;
	uint8_t master_secret[32];
	/* TLS 1.3 uses a string of Hash.length zeros here (not an empty string). */
	hkdf_extract_sha256(derived2, 32, psk_zeros, sizeof(psk_zeros), master_secret);

	uint8_t thash[32];
	sha256_ctx_digest(transcript, thash);
	if (tls13_derive_secret_sha256(master_secret, "c ap traffic", thash, c_ap_traffic) != 0) return -1;
	if (tls13_derive_secret_sha256(master_secret, "s ap traffic", thash, s_ap_traffic) != 0) return -1;

	if (tls13_hkdf_expand_label_sha256(c_ap_traffic, "key", NULL, 0, tx_app->key, 16) != 0) return -1;
	if (tls13_hkdf_expand_label_sha256(c_ap_traffic, "iv", NULL, 0, tx_app->iv, 12) != 0) return -1;
	if (tls13_hkdf_expand_label_sha256(s_ap_traffic, "key", NULL, 0, rx_app->key, 16) != 0) return -1;
	if (tls13_hkdf_expand_label_sha256(s_ap_traffic, "iv", NULL, 0, rx_app->iv, 12) != 0) return -1;
	tx_app->seq = 0;
	rx_app->seq = 0;
	tx_app->valid = 1;
	rx_app->valid = 1;
	crypto_memset(early_secret, 0, sizeof(early_secret));
	crypto_memset(derived1, 0, sizeof(derived1));
	crypto_memset(handshake_secret, 0, sizeof(handshake_secret));
	crypto_memset(derived2, 0, sizeof(derived2));
	crypto_memset(master_secret, 0, sizeof(master_secret));
	crypto_memset(thash, 0, sizeof(thash));
	return 0;
}

int tls13_https_get_status_line(int sock,
				const char *host,
				const char *path,
				char *status_line,
				size_t status_line_len)
{
	return tls13_https_get_status_and_location(sock,
					 host,
					 path,
					 status_line,
					 status_line_len,
					 0,
					 0,
					 0);
}

int tls13_https_get_status_and_location(int sock,
					const char *host,
					const char *path,
					char *status_line,
					size_t status_line_len,
					int *status_code_out,
					char *location_out,
					size_t location_out_len)
{
	return tls13_https_get_status_location_and_body(sock,
					 host,
					 path,
					 status_line,
					 status_line_len,
					 status_code_out,
					 location_out,
					 location_out_len,
				 0,
				 0,
				 0,
				 0,
					 0,
					 0,
					 0,
					 0);
}

static int tls_read_record_stream(int fd,
				 uint8_t hdr[5],
				 uint8_t *payload,
				 size_t payload_cap,
				 size_t *payload_len)
{
	/* Like tls_read_record, but returns 1 on clean EOF before reading a record. */
	size_t off = 0;
	while (off < 5) {
		long r = sys_read(fd, hdr + off, 5 - off);
		if (r == 0) return 1;
		if (r < 0) return -1;
		off += (size_t)r;
	}
	size_t len = ((size_t)hdr[3] << 8) | (size_t)hdr[4];
	if (len > payload_cap) return -1;
	off = 0;
	while (off < len) {
		long r = sys_read(fd, payload + off, len - off);
		if (r <= 0) return -1;
		off += (size_t)r;
	}
	*payload_len = len;
	return 0;
}

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
					uint64_t *content_length_out)
{
	if (!host || !path || !status_line || status_line_len == 0) return -1;
	status_line[0] = 0;
	if (status_code_out) *status_code_out = -1;
	if (location_out && location_out_len) location_out[0] = 0;
	if (content_type_out && content_type_out_len) content_type_out[0] = 0;
	if (content_encoding_out && content_encoding_out_len) content_encoding_out[0] = 0;
	if (body_len_out) *body_len_out = 0;
	if (content_length_out) *content_length_out = 0;

	/* Avoid hanging forever during bring-up. */
	{
		struct timeval tv;
		tv.tv_sec = 3;
		tv.tv_usec = 0;
		(void)sys_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, (uint32_t)sizeof(tv));
		(void)sys_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, (uint32_t)sizeof(tv));
	}

	LOGI("tls", "building ClientHello\n");

	struct sha256_ctx transcript;
	sha256_init(&transcript);

	uint8_t priv[X25519_KEY_SIZE];
	uint8_t pub[X25519_KEY_SIZE];
	uint8_t ch_hs[1024];
	size_t ch_hs_len = 0;
	if (build_client_hello(host, ch_hs, &ch_hs_len, priv, pub) != 0) return -1;

	/* Send ClientHello */
	LOGI("tls", "sending ClientHello\n");
	if (send_plain_handshake_record(sock, ch_hs, ch_hs_len) != 0) return -1;
	sha256_update(&transcript, ch_hs, ch_hs_len);
	LOGI("tls", "ClientHello sent\n");

	/* Read until ServerHello */
	uint8_t hdr[5];
	uint8_t payload[TLS13_MAX_RECORD];
	size_t payload_len = 0;
	uint8_t server_pub[X25519_KEY_SIZE];
	uint8_t sh_hs[1024];
	size_t sh_hs_len = 0;
	for (;;) {
		if (tls_read_record(sock, hdr, payload, sizeof(payload), &payload_len) != 0) return -1;
		uint8_t typ = hdr[0];
		if (typ == 0x14) {
			/* ChangeCipherSpec: ignore. */
			continue;
		}
		if (typ != 0x16) return -1;
		/* Parse handshake messages; expect first is ServerHello. */
		if (payload_len < 4) return -1;
		uint8_t hs_type = payload[0];
		uint32_t body_len = get_u24(&payload[1]);
		if (hs_type != 0x02) return -1;
		if ((size_t)body_len + 4u > payload_len) return -1;
		sh_hs_len = (size_t)body_len + 4u;
		crypto_memcpy(sh_hs, payload, sh_hs_len);
		break;
	}
	LOGI("tls", "got ServerHello\n");

	LOGI("tls", "parsing ServerHello\n");
	if (parse_server_hello(sh_hs, sh_hs_len, server_pub) != 0) {
		LOGE("tls", "parse ServerHello failed (cipher/extension mismatch?)\n");
		return -1;
	}
	LOGI("tls", "ServerHello ok\n");
	sha256_update(&transcript, sh_hs, sh_hs_len);

	LOGI("tls", "computing shared secret (x25519)\n");
	uint8_t shared[32];
	x25519(shared, priv, server_pub);
	LOGI("tls", "shared secret computed\n");
	crypto_memset(priv, 0, sizeof(priv));
	crypto_memset(pub, 0, sizeof(pub));
	crypto_memset(server_pub, 0, sizeof(server_pub));

	struct tls13_aead tx_hs = {0};
	struct tls13_aead rx_hs = {0};
	struct tls13_aead tx_app = {0};
	struct tls13_aead rx_app = {0};
	uint8_t c_hs_traffic[32];
	uint8_t s_hs_traffic[32];
	LOGI("tls", "deriving handshake keys\n");
	if (derive_hs_traffic(&transcript, shared, c_hs_traffic, s_hs_traffic, &tx_hs, &rx_hs) != 0) {
		LOGE("tls", "derive handshake keys failed\n");
		return -1;
	}
	LOGI("tls", "derived handshake keys\n");

	/* Process encrypted handshake messages until server Finished. */
	uint8_t got_server_finished = 0;
	uint8_t server_finished_verify[32];
	crypto_memset(server_finished_verify, 0, sizeof(server_finished_verify));

	uint8_t dec[TLS13_MAX_RECORD];
	uint8_t dec_type = 0;
	size_t dec_len = 0;

	for (;;) {
		if (tls_read_record(sock, hdr, payload, sizeof(payload), &payload_len) != 0) return -1;
		uint8_t typ = hdr[0];
		if (typ == 0x14) continue; /* CCS */
		if (typ != 0x17) return -1;

		if (tls13_open_record(&rx_hs, hdr, payload, payload_len, dec, sizeof(dec), &dec_type, &dec_len) != 0) return -1;
		if (dec_type != 0x16) {
			/* Ignore non-handshake until handshake done. */
			continue;
		}

		/* One or more handshake messages. */
		size_t off = 0;
		while (off + 4u <= dec_len) {
			uint8_t hs_type = dec[off];
			uint32_t bl = get_u24(&dec[off + 1]);
			if (off + 4u + (size_t)bl > dec_len) return -1;
			const uint8_t *hs_msg = &dec[off];
			size_t hs_msg_len = 4u + (size_t)bl;

			if (hs_type == 0x14) {
				/* Verify server Finished before adding it to transcript. */
				uint8_t finished_key[32];
				if (tls13_hkdf_expand_label_sha256(s_hs_traffic, "finished", NULL, 0, finished_key, 32) != 0) return -1;
				uint8_t th[32];
				sha256_ctx_digest(&transcript, th);
				hmac_sha256(finished_key, 32, th, 32, server_finished_verify);
				crypto_memset(finished_key, 0, sizeof(finished_key));
				crypto_memset(th, 0, sizeof(th));
				if (bl != 32u) return -1;
				if (!crypto_memeq(server_finished_verify, hs_msg + 4, 32)) return -1;
				sha256_update(&transcript, hs_msg, hs_msg_len);
				got_server_finished = 1;
				LOGI("tls", "server Finished verified\n");
				break;
			}

			sha256_update(&transcript, hs_msg, hs_msg_len);
			off += hs_msg_len;
		}

		if (got_server_finished) break;
	}

	/* Derive application traffic secrets using transcript up to server Finished. */
	uint8_t c_ap_traffic[32];
	uint8_t s_ap_traffic[32];
	if (derive_app_traffic(&transcript, shared, c_ap_traffic, s_ap_traffic, &tx_app, &rx_app) != 0) return -1;

	/* Server may start using application keys after Finished. */
	tls13_aead_reset(&rx_app);

	/* Send client Finished under handshake keys. */
	uint8_t client_finished_key[32];
	if (tls13_hkdf_expand_label_sha256(c_hs_traffic, "finished", NULL, 0, client_finished_key, 32) != 0) return -1;
	uint8_t th[32];
	sha256_ctx_digest(&transcript, th);
	uint8_t verify[32];
	hmac_sha256(client_finished_key, 32, th, 32, verify);
	crypto_memset(client_finished_key, 0, sizeof(client_finished_key));
	crypto_memset(th, 0, sizeof(th));

	uint8_t fin_hs[4 + 32];
	fin_hs[0] = 0x14;
	put_u24(&fin_hs[1], 32);
	crypto_memcpy(&fin_hs[4], verify, 32);
	crypto_memset(verify, 0, sizeof(verify));

	if (tls13_seal_record(sock, &tx_hs, 0x16, fin_hs, sizeof(fin_hs)) != 0) return -1;
	sha256_update(&transcript, fin_hs, sizeof(fin_hs));
	crypto_memset(fin_hs, 0, sizeof(fin_hs));
	LOGI("tls", "client Finished sent\n");

	/* Now use application write keys. */
	tls13_aead_reset(&tx_app);

	/* Send HTTP request as application data. */
	char req[768];
	int req_len = http_format_get(req, sizeof(req), host, path);
	if (req_len < 0) return -1;
	if (tls13_seal_record(sock, &tx_app, 0x17, (const uint8_t *)req, (size_t)req_len) != 0) return -1;
	crypto_memset(req, 0, sizeof(req));
	LOGI("tls", "HTTP request sent\n");

	/* Read response headers, then (optionally) a bounded amount of body.
	 * Phase 0.1: Content-Length supported; chunked not yet.
	 */
	char line[512];
	size_t line_len = 0;
	int got_status = 0;
	int got_headers_end = 0;
	uint8_t header_buf[8192];
	size_t header_len = 0;
	uint64_t content_len = 0;
	int have_content_len = 0;
	int is_chunked = 0;
	struct http_chunked_dec chunked;
	http_chunked_init(&chunked);
	int chunked_done = 0;
	size_t body_len = 0;

	LOGI("tls", "waiting for HTTP response...\n");
	for (;;) {
		int rr = tls_read_record_stream(sock, hdr, payload, sizeof(payload), &payload_len);
		if (rr == 1) {
			/* EOF: if we had no Content-Length, treat as end-of-body. */
			break;
		}
		if (rr != 0) {
			LOGE("tls", "read app record failed\n");
			return -1;
		}
		uint8_t typ = hdr[0];
		if (typ == 0x14) continue;
		if (typ != 0x17) continue;
		if (tls13_open_record(&rx_app, hdr, payload, payload_len, dec, sizeof(dec), &dec_type, &dec_len) != 0) {
			/* Helpful during bring-up: check if the peer is still using handshake keys. */
			struct tls13_aead tmp = rx_hs;
			uint8_t t_type = 0;
			size_t t_len = 0;
			if (tls13_open_record(&tmp, hdr, payload, payload_len, dec, sizeof(dec), &t_type, &t_len) == 0) {
				rx_hs = tmp;
				if (t_type == 0x15) LOGI("tls", "got alert under handshake keys\n");
				else if (t_type == 0x16) LOGI("tls", "got handshake under handshake keys\n");
				else if (t_type == 0x17) LOGI("tls", "got appdata under handshake keys\n");
				else LOGW("tls", "got unknown inner type under handshake keys\n");
				return -1;
			}
			LOGE("tls", "decrypt app record failed\n");
			return -1;
		}
		if (dec_type == 0x15) return -1;
		if (dec_type != 0x17) continue;

		for (size_t i = 0; i < dec_len; i++) {
			uint8_t b = dec[i];

			if (!got_headers_end) {
				if (header_len >= sizeof(header_buf)) return -1;
				header_buf[header_len++] = b;

				/* Build current line for parsing (headers only). */
				if (line_len + 1 < sizeof(line)) {
					line[line_len++] = (char)b;
					line[line_len] = 0;
				}

				/* Parse line on LF. */
				if (b == '\n') {
					if (!got_status) {
						size_t n = line_len;
						while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
						size_t w = 0;
						for (; w < n && w + 1 < status_line_len; w++) status_line[w] = line[w];
						status_line[w] = 0;
						if (status_code_out) *status_code_out = http_parse_status_code(status_line);
						got_status = 1;
					} else {
						if (!have_content_len) {
							uint64_t v = 0;
							if (http_parse_content_length_line(line, &v) == 0) {
								have_content_len = 1;
								content_len = v;
							}
						}
						if (!is_chunked) {
							char tmp[256];
							if (http_header_extract_value(line, "Transfer-Encoding", tmp, sizeof(tmp)) == 0) {
								if (http_value_has_token_ci(tmp, "chunked")) {
									is_chunked = 1;
								}
							}
						}
						if (location_out && location_out_len && location_out[0] == 0) {
							char tmp[512];
							if (http_header_extract_value(line, "Location", tmp, sizeof(tmp)) == 0) {
								(void)c_strlcpy_s(location_out, location_out_len, tmp);
							}
						}
						if (content_type_out && content_type_out_len && content_type_out[0] == 0) {
							char tmp[256];
							if (http_header_extract_value(line, "Content-Type", tmp, sizeof(tmp)) == 0) {
								(void)c_strlcpy_s(content_type_out, content_type_out_len, tmp);
							}
						}
						if (content_encoding_out && content_encoding_out_len && content_encoding_out[0] == 0) {
							char tmp[256];
							if (http_header_extract_value(line, "Content-Encoding", tmp, sizeof(tmp)) == 0) {
								(void)c_strlcpy_s(content_encoding_out, content_encoding_out_len, tmp);
							}
						}
					}
					line_len = 0;
					line[0] = 0;
				}

				/* Check for end-of-headers and flush any already-buffered body bytes. */
				size_t hdr_end = 0;
				if (http_find_header_end(header_buf, header_len, &hdr_end) == 0) {
					got_headers_end = 1;
					size_t pre_body = header_len - hdr_end;
					if (pre_body && body && body_cap) {
						if (is_chunked) {
							size_t used = 0;
							size_t wrote = 0;
							int r = http_chunked_feed(&chunked,
										 header_buf + hdr_end,
										 pre_body,
										 &used,
										 body + body_len,
										 body_cap - body_len,
										 &wrote);
							body_len += wrote;
							if (r < 0) return -1;
							if (r == 1) chunked_done = 1;
						} else {
							size_t want = body_cap;
							if (have_content_len && content_len < (uint64_t)want) want = (size_t)content_len;
							size_t ncopy = pre_body;
							if (body_len >= want) ncopy = 0;
							else if (ncopy > want - body_len) ncopy = want - body_len;
							if (ncopy) {
								crypto_memcpy(body + body_len, header_buf + hdr_end, ncopy);
								body_len += ncopy;
							}
						}
					}
				}
			} else {
				/* Body */
				if (body && body_cap) {
					if (is_chunked) {
						size_t used = 0;
						size_t wrote = 0;
						int r = http_chunked_feed(&chunked,
									 &b,
									 1,
									 &used,
									 body + body_len,
									 body_cap - body_len,
									 &wrote);
						body_len += wrote;
						if (r < 0) return -1;
						if (r == 1) chunked_done = 1;
					} else {
						size_t want = body_cap;
						if (have_content_len && content_len < (uint64_t)want) want = (size_t)content_len;
						if (body_len < want) {
							body[body_len++] = b;
						}
					}
				}
			}
		}

		if (got_headers_end) {
			if (!body || body_cap == 0) break;
			if (is_chunked) {
				if (chunked_done) break;
				if (body_len >= body_cap) break;
			} else {
				size_t want = body_cap;
				if (have_content_len && content_len < (uint64_t)want) want = (size_t)content_len;
				if (body_len >= want) break;
			}
		}
	}

	if (body_len_out) *body_len_out = body_len;
	if (content_length_out) *content_length_out = (!is_chunked && have_content_len) ? content_len : 0;
	LOGI("tls", "got HTTP response headers/body\n");

	crypto_memset(shared, 0, sizeof(shared));
	crypto_memset(c_hs_traffic, 0, sizeof(c_hs_traffic));
	crypto_memset(s_hs_traffic, 0, sizeof(s_hs_traffic));
	crypto_memset(c_ap_traffic, 0, sizeof(c_ap_traffic));
	crypto_memset(s_ap_traffic, 0, sizeof(s_ap_traffic));
	crypto_memset(server_finished_verify, 0, sizeof(server_finished_verify));
	crypto_memset(dec, 0, sizeof(dec));
	crypto_memset(payload, 0, sizeof(payload));
	return 0;
}
