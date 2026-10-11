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
}
