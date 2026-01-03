#pragma once

#include "hkdf_sha256.h"

/* Returns 1 if all tests pass, 0 otherwise. */
int tls_crypto_selftest(void);

/* Returns 1 if all tests pass, 0 otherwise. If it fails, sets *failed_step to
 * a 1-based step index identifying the failing subtest.
 */
int tls_crypto_selftest_detail(int *failed_step);
