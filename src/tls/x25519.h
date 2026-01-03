#pragma once

#include "types.h"

/* X25519 key exchange (RFC 7748). */

#define X25519_KEY_SIZE 32u

/* Computes out = X25519(scalar, u).
 * - scalar: 32-byte little-endian, clamped internally.
 * - u: 32-byte little-endian u-coordinate.
 */
void x25519(uint8_t out[X25519_KEY_SIZE], const uint8_t scalar[X25519_KEY_SIZE], const uint8_t u[X25519_KEY_SIZE]);

/* Convenience: out = X25519(scalar, basepoint=9). */
void x25519_base(uint8_t out[X25519_KEY_SIZE], const uint8_t scalar[X25519_KEY_SIZE]);
