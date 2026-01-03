#include "x25519.h"

/* Correctness-first X25519 implementation.
 * Field: p = 2^255 - 19.
 * Representation: radix R = 2^15, 17 limbs => exactly 255 bits.
 * Reduction is simple: for limbs >=17, fold back with *19.
 */

enum { FE_LIMBS = 17 };
enum { FE_BITS = 15 };
enum { FE_R = 1 << FE_BITS };
enum { FE_MASK = FE_R - 1 };

typedef struct {
	int64_t v[FE_LIMBS];
} fe;

static const fe FE_P = { .v = {
	FE_R - 19,
	FE_R - 1, FE_R - 1, FE_R - 1, FE_R - 1, FE_R - 1, FE_R - 1, FE_R - 1, FE_R - 1,
	FE_R - 1, FE_R - 1, FE_R - 1, FE_R - 1, FE_R - 1, FE_R - 1, FE_R - 1, FE_R - 1,
}};

static inline void fe_zero(fe *a)
{
	for (int i = 0; i < FE_LIMBS; i++) a->v[i] = 0;
}

static inline void fe_one(fe *a)
{
	fe_zero(a);
	a->v[0] = 1;
}

static inline void fe_copy(fe *dst, const fe *src)
{
	for (int i = 0; i < FE_LIMBS; i++) dst->v[i] = src->v[i];
}

static inline int fe_ge_p(const fe *a)
{
	for (int i = FE_LIMBS - 1; i >= 0; i--) {
		int64_t av = a->v[i];
		int64_t pv = FE_P.v[i];
		if (av > pv) return 1;
		if (av < pv) return 0;
	}
	return 1;
}

static inline void fe_sub_p(fe *a)
{
	int64_t borrow = 0;
	for (int i = 0; i < FE_LIMBS; i++) {
		int64_t r = a->v[i] - FE_P.v[i] - borrow;
		borrow = (r < 0);
		if (borrow) r += FE_R;
		a->v[i] = r;
	}
}

static inline void fe_carry(fe *a)
{
	/* Normalize limbs to [0, R) with p-fold at the top limb. */
	for (int i = 0; i < FE_LIMBS - 1; i++) {
		int64_t carry = a->v[i] >> FE_BITS;
		a->v[i] -= carry * FE_R;
		a->v[i + 1] += carry;
	}
	{
		int64_t carry = a->v[FE_LIMBS - 1] >> FE_BITS;
		a->v[FE_LIMBS - 1] -= carry * FE_R;
		a->v[0] += carry * 19;
	}
	for (int i = 0; i < FE_LIMBS - 1; i++) {
		int64_t carry = a->v[i] >> FE_BITS;
		a->v[i] -= carry * FE_R;
		a->v[i + 1] += carry;
	}
	{
		int64_t carry = a->v[FE_LIMBS - 1] >> FE_BITS;
		a->v[FE_LIMBS - 1] -= carry * FE_R;
		a->v[0] += carry * 19;
	}
	for (int i = 0; i < FE_LIMBS - 1; i++) {
		int64_t carry = a->v[i] >> FE_BITS;
		a->v[i] -= carry * FE_R;
		a->v[i + 1] += carry;
	}
	/* Final fold: the last propagation can re-overflow the top limb. */
	{
		int64_t carry = a->v[FE_LIMBS - 1] >> FE_BITS;
		a->v[FE_LIMBS - 1] -= carry * FE_R;
		a->v[0] += carry * 19;
	}
	for (int i = 0; i < FE_LIMBS - 1; i++) {
		int64_t carry = a->v[i] >> FE_BITS;
		a->v[i] -= carry * FE_R;
		a->v[i + 1] += carry;
	}
}

static inline void fe_reduce(fe *a)
{
	fe_carry(a);
	fe_carry(a);
	if (fe_ge_p(a)) fe_sub_p(a);
}

static inline void fe_add(fe *out, const fe *a, const fe *b)
{
	for (int i = 0; i < FE_LIMBS; i++) out->v[i] = a->v[i] + b->v[i];
}

static inline void fe_sub(fe *out, const fe *a, const fe *b)
{
	for (int i = 0; i < FE_LIMBS; i++) out->v[i] = a->v[i] - b->v[i];
}

static inline void fe_mul(fe *out, const fe *a, const fe *b)
{
	int64_t t[FE_LIMBS * 2] = {0};
	for (int i = 0; i < FE_LIMBS; i++) {
		for (int j = 0; j < FE_LIMBS; j++) {
			t[i + j] += a->v[i] * b->v[j];
		}
	}
	for (int k = (FE_LIMBS * 2) - 1; k >= FE_LIMBS; k--) {
		t[k - FE_LIMBS] += t[k] * 19;
		t[k] = 0;
	}
	for (int i = 0; i < FE_LIMBS; i++) out->v[i] = t[i];
	fe_reduce(out);
	crypto_memset(t, 0, sizeof(t));
}

static inline void fe_sq(fe *out, const fe *a)
{
	fe_mul(out, a, a);
}

static inline void fe_mul_small(fe *out, const fe *a, int64_t k)
{
	for (int i = 0; i < FE_LIMBS; i++) out->v[i] = a->v[i] * k;
	fe_reduce(out);
}

static inline void fe_cswap(fe *a, fe *b, uint64_t swap)
{
	int64_t mask = -(int64_t)(swap & 1u);
	for (int i = 0; i < FE_LIMBS; i++) {
		int64_t t = mask & (a->v[i] ^ b->v[i]);
		a->v[i] ^= t;
		b->v[i] ^= t;
	}
}

static inline uint8_t getbit(const uint8_t s[32], int bit)
{
	return (uint8_t)((s[bit >> 3] >> (bit & 7)) & 1u);
}

static inline void setbit(uint8_t s[32], int bit, uint8_t v)
{
	uint8_t m = (uint8_t)(1u << (bit & 7));
	if (v) s[bit >> 3] |= m;
	else s[bit >> 3] &= (uint8_t)~m;
}

static void fe_frombytes(fe *out, const uint8_t s[32])
{
	uint8_t in[32];
	crypto_memcpy(in, s, 32);
	in[31] &= 0x7f;
	for (int i = 0; i < FE_LIMBS; i++) {
		int64_t limb = 0;
		for (int b = 0; b < FE_BITS; b++) {
			int bit = i * FE_BITS + b;
			limb |= (int64_t)getbit(in, bit) << b;
		}
		out->v[i] = limb;
	}
	fe_reduce(out);
	crypto_memset(in, 0, sizeof(in));
}

static void fe_tobytes(uint8_t s[32], fe *a)
{
	fe_reduce(a);
	crypto_memset(s, 0, 32);
	for (int i = 0; i < FE_LIMBS; i++) {
		int64_t limb = a->v[i];
		for (int b = 0; b < FE_BITS; b++) {
			int bit = i * FE_BITS + b;
			setbit(s, bit, (uint8_t)((limb >> b) & 1u));
		}
	}
	/* bit255 must be zero */
	s[31] &= 0x7f;
}

static void fe_inv(fe *out, const fe *z)
{
	/* z^(p-2) with square-and-multiply. */
	static const uint8_t EXP[32] = {
		0xeb,
		0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
		0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
		0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
		0xff,0xff,0xff,0xff,0xff,0xff,
		0x7f,
	};
	fe result;
	fe base;
	fe_one(&result);
	fe_copy(&base, z);
	for (int i = 254; i >= 0; i--) {
		fe_sq(&result, &result);
		uint8_t bit = (uint8_t)((EXP[i / 8] >> (i & 7)) & 1u);
		if (bit) fe_mul(&result, &result, &base);
	}
	fe_copy(out, &result);
	crypto_memset(&result, 0, sizeof(result));
	crypto_memset(&base, 0, sizeof(base));
}

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t u[32])
{
	uint8_t e[32];
	crypto_memcpy(e, scalar, 32);
	e[0] &= 248u;
	e[31] &= 127u;
	e[31] |= 64u;

	fe x1;
	fe_frombytes(&x1, u);

	fe x2, z2, x3, z3;
	fe_one(&x2);
	fe_zero(&z2);
	fe_copy(&x3, &x1);
	fe_one(&z3);

	uint64_t swap = 0;
	for (int t = 254; t >= 0; t--) {
		uint64_t k_t = (uint64_t)((e[t / 8] >> (t & 7)) & 1u);
		swap ^= k_t;
		fe_cswap(&x2, &x3, swap);
		fe_cswap(&z2, &z3, swap);
		swap = k_t;

		fe A, AA, B, BB, E;
		fe C, D, DA, CB;
		fe_add(&A, &x2, &z2); fe_reduce(&A);
		fe_sq(&AA, &A);
		fe_sub(&B, &x2, &z2); fe_reduce(&B);
		fe_sq(&BB, &B);
		fe_sub(&E, &AA, &BB); fe_reduce(&E);
		fe_add(&C, &x3, &z3); fe_reduce(&C);
		fe_sub(&D, &x3, &z3); fe_reduce(&D);
		fe_mul(&DA, &D, &A);
		fe_mul(&CB, &C, &B);

		fe tmp1, tmp2;
		fe_add(&tmp1, &DA, &CB); fe_reduce(&tmp1);
		fe_sq(&x3, &tmp1);
		fe_sub(&tmp2, &DA, &CB); fe_reduce(&tmp2);
		fe_sq(&tmp2, &tmp2);
		fe_mul(&z3, &tmp2, &x1);

		fe_mul(&x2, &AA, &BB);
		fe_mul_small(&tmp1, &E, 121665u);
		fe_add(&tmp1, &AA, &tmp1); fe_reduce(&tmp1);
		fe_mul(&z2, &E, &tmp1);

		crypto_memset(&A, 0, sizeof(A));
		crypto_memset(&AA, 0, sizeof(AA));
		crypto_memset(&B, 0, sizeof(B));
		crypto_memset(&BB, 0, sizeof(BB));
		crypto_memset(&E, 0, sizeof(E));
		crypto_memset(&C, 0, sizeof(C));
		crypto_memset(&D, 0, sizeof(D));
		crypto_memset(&DA, 0, sizeof(DA));
		crypto_memset(&CB, 0, sizeof(CB));
		crypto_memset(&tmp1, 0, sizeof(tmp1));
		crypto_memset(&tmp2, 0, sizeof(tmp2));
	}

	fe_cswap(&x2, &x3, swap);
	fe_cswap(&z2, &z3, swap);

	fe zinv;
	fe_inv(&zinv, &z2);
	fe_mul(&x2, &x2, &zinv);
	fe_tobytes(out, &x2);

	crypto_memset(e, 0, sizeof(e));
	crypto_memset(&x1, 0, sizeof(x1));
	crypto_memset(&x2, 0, sizeof(x2));
	crypto_memset(&z2, 0, sizeof(z2));
	crypto_memset(&x3, 0, sizeof(x3));
	crypto_memset(&z3, 0, sizeof(z3));
	crypto_memset(&zinv, 0, sizeof(zinv));
}

void x25519_base(uint8_t out[32], const uint8_t scalar[32])
{
	uint8_t base[32];
	crypto_memset(base, 0, 32);
	base[0] = 9;
	x25519(out, scalar, base);
	crypto_memset(base, 0, 32);
}
