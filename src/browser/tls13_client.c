#include "tls13_client.h"

#include "http.h"
#include "net_ip6.h"
#include "util.h"

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

struct tls13_aead {
	uint8_t key[16];
	uint8_t iv[12];
	uint64_t seq;
	int valid;
};

static void tls13_aead_reset(struct tls13_aead *a)
{
	a->seq = 0;
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
	put_u16(p, 4);
	p += 2;
	put_u16(p, (uint16_t)TLS13_CIPHER_TLS_AES_128_GCM_SHA256);
	p += 2;
	put_u16(p, 0x1302); /* TLS_AES_256_GCM_SHA384 (advertise only) */
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
	if (!host || !path || !status_line || status_line_len == 0) return -1;
	status_line[0] = 0;

	/* Avoid hanging forever during bring-up. */
	{
		struct timeval tv;
		tv.tv_sec = 3;
		tv.tv_usec = 0;
		(void)sys_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, (uint32_t)sizeof(tv));
		(void)sys_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, (uint32_t)sizeof(tv));
	}

	dbg_write("tls: building ClientHello\n");

	struct sha256_ctx transcript;
	sha256_init(&transcript);

	uint8_t priv[X25519_KEY_SIZE];
	uint8_t pub[X25519_KEY_SIZE];
	uint8_t ch_hs[1024];
	size_t ch_hs_len = 0;
	if (build_client_hello(host, ch_hs, &ch_hs_len, priv, pub) != 0) return -1;

	/* Send ClientHello */
	dbg_write("tls: sending ClientHello\n");
	if (send_plain_handshake_record(sock, ch_hs, ch_hs_len) != 0) return -1;
	sha256_update(&transcript, ch_hs, ch_hs_len);
	dbg_write("tls: ClientHello sent\n");

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
	dbg_write("tls: got ServerHello\n");

	if (parse_server_hello(sh_hs, sh_hs_len, server_pub) != 0) return -1;
	sha256_update(&transcript, sh_hs, sh_hs_len);

	uint8_t shared[32];
	x25519(shared, priv, server_pub);
	crypto_memset(priv, 0, sizeof(priv));
	crypto_memset(pub, 0, sizeof(pub));
	crypto_memset(server_pub, 0, sizeof(server_pub));

	struct tls13_aead tx_hs = {0};
	struct tls13_aead rx_hs = {0};
	struct tls13_aead tx_app = {0};
	struct tls13_aead rx_app = {0};
	uint8_t c_hs_traffic[32];
	uint8_t s_hs_traffic[32];
	if (derive_hs_traffic(&transcript, shared, c_hs_traffic, s_hs_traffic, &tx_hs, &rx_hs) != 0) return -1;
	dbg_write("tls: derived handshake keys\n");

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
				dbg_write("tls: server Finished verified\n");
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
	dbg_write("tls: client Finished sent\n");

	/* Now use application write keys. */
	tls13_aead_reset(&tx_app);

	/* Send HTTP request as application data. */
	char req[768];
	int req_len = http_format_get(req, sizeof(req), host, path);
	if (req_len < 0) return -1;
	if (tls13_seal_record(sock, &tx_app, 0x17, (const uint8_t *)req, (size_t)req_len) != 0) return -1;
	crypto_memset(req, 0, sizeof(req));
	dbg_write("tls: HTTP request sent\n");

	/* Read application data until we have the HTTP status line. */
	size_t so = 0;
	int got_line = 0;
	dbg_write("tls: waiting for HTTP response...\n");
	for (;;) {
		if (tls_read_record(sock, hdr, payload, sizeof(payload), &payload_len) != 0) {
			dbg_write("tls: read app record failed\n");
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
				if (t_type == 0x15) dbg_write("tls: got alert under handshake keys\n");
				else if (t_type == 0x16) dbg_write("tls: got handshake under handshake keys\n");
				else if (t_type == 0x17) dbg_write("tls: got appdata under handshake keys\n");
				else dbg_write("tls: got unknown inner type under handshake keys\n");
				return -1;
			}
			dbg_write("tls: decrypt app record failed\n");
			return -1;
		}
		if (dec_type == 0x15) return -1;
		if (dec_type != 0x17) continue;
		for (size_t i = 0; i < dec_len; i++) {
			uint8_t b = dec[i];
			if (b == '\n') {
				/* Strip optional preceding '\r'. */
				if (so > 0 && status_line[so - 1] == '\r') so--;
				status_line[so] = 0;
				got_line = 1;
				break;
			}
			if (so + 1 < status_line_len) {
				status_line[so++] = (char)b;
			}
		}
		if (got_line) break;
	}
	dbg_write("tls: got HTTP status line\n");

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
