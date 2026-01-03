#pragma once

#include "sha256.h"

#define HMAC_SHA256_SIZE 32u

void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t out[HMAC_SHA256_SIZE]);
