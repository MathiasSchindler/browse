#include <stdio.h>

#include "../src/tls/selftest.h"

int main(void)
{
	int failed_step = 0;
	int ok = tls_crypto_selftest_detail(&failed_step);
	if (ok) {
		puts("crypto selftest: OK");
		return 0;
	}
	printf("crypto selftest: FAIL (step %d)\n", failed_step);
	return 1;
}
