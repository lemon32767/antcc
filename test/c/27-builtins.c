/* CFLAGS: -Werror -lm */
/* EXPECT:
f(3) = 2
X*10.f==2 = 1
likely(t > 1) = 0
unlikely(t == 1) = 1
__builtin_expect(7, atoi("7!")) = 7
isinf(__builtin_huge_val()) = 1
isinf(__builtin_inf()) = 1
isinf(__builtin_huge_valf()) = 1
isinf(__builtin_inff()) = 1
isinf(HVAL) = 1
isnan(__builtin_nan("")) = 1
isnan(__builtin_nanf("")) = 1
isnan(NNAN) = 1
isnan(__builtin_nan(t ? "" : "x")) = 1
isnan(__builtin_nanf("q"+t)) = 1
__builtin_clz(0) = 32
__builtin_clz(1) = 31
__builtin_clz(-1) = 0
__builtin_ctz(1u << 31) = 31
__builtin_clrsb(0) = 31
__builtin_clrsb(-1) = 31
__builtin_clrsb(1) = 30
__builtin_ctzll(1ull << 40) = 40
__builtin_clrsbll(-1) = 63
__builtin_popcount(0) = 0
__builtin_popcount(-1) = 32
__builtin_popcount(0x80000000u) = 1
__builtin_popcountll(~0ull) = 64
__builtin_parity(7) = 1
__builtin_parityll(~0ull) = 0
__builtin_clz(x) = 8
__builtin_ctz(x) = 20
__builtin_clrsb(n) = 28
__builtin_popcount(x) = 4
__builtin_parity(x) = 0
__builtin_clzll(w) = 23
__builtin_ctzll(w) = 40
__builtin_clrsbll(w) = 22
__builtin_popcountll(w) = 1
__builtin_parityll(w) = 1
*/

#include <stdio.h>
#include <assert.h>
#include <math.h>

int f(int x) {
    if ((x & 1) == 0) return 1;
    if ((x & 1) == 1) return 2;
    __builtin_unreachable(); // should not warn now
}

#define prid(x) printf(#x" = %d\n", (int)(x))
#define min(a,b) ((a)<(b) ? (a) : (b))
#define likely(x) __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

int main(int t) {
    prid(f(3));
    static const float X = .2f;
    float y = t < 0 ? 0 : 1;
    assert(__builtin_constant_p(X?X:X));
    prid(X*10.f==2);
    assert(!__builtin_constant_p(y));
    assert(!__builtin_constant_p(main));
    assert(__builtin_constant_p(min(sizeof(int), sizeof(long))));
    prid(likely(t > 1));
    prid(unlikely(t == 1));
    int atoi(char *);
    prid(__builtin_expect(7, atoi("7!")));

    static const double HVAL = __builtin_huge_val();
    static const float NNAN = __builtin_nanf("");
    prid(isinf(__builtin_huge_val()));
    prid(isinf(__builtin_inf()));
    prid(isinf(__builtin_huge_valf()));
    prid(isinf(__builtin_inff()));
    prid(isinf(HVAL));
    prid(isnan(__builtin_nan("")));
    prid(isnan(__builtin_nanf("")));
    prid(isnan(NNAN));
    prid(isnan(__builtin_nan(t ? "" : "x")));
    prid(isnan(__builtin_nanf("q"+t)));

    prid(__builtin_clz(0)), prid(__builtin_clz(1)), prid(__builtin_clz(-1)),
    prid(__builtin_ctz(1u << 31));
    prid(__builtin_clrsb(0)), prid(__builtin_clrsb(-1)), prid(__builtin_clrsb(1));
    prid(__builtin_ctzll(1ull << 40));
    prid(__builtin_clrsbll(-1));
    prid(__builtin_popcount(0)), prid(__builtin_popcount(-1)), prid(__builtin_popcount(0x80000000u));
    prid(__builtin_popcountll(~0ull));
    prid(__builtin_parity(7)), prid(__builtin_parityll(~0ull));

    int x = t ? 0x00f00000 : 1;
    long long w = t ? (1ll << 40) : 1;
    int n = t ? 5 : 6;
    prid(__builtin_clz(x)), prid(__builtin_ctz(x));
    prid(__builtin_clrsb(n));
    prid(__builtin_popcount(x)), prid(__builtin_parity(x));
    prid(__builtin_clzll(w)), prid(__builtin_ctzll(w)), prid(__builtin_clrsbll(w)),
    prid(__builtin_popcountll(w)), prid(__builtin_parityll(w));
}
