/* CFLAGS: -Werror */
/* EXPECT:
f(3) = 2
X*10.f==2 = 1
likely(t > 1) = 0
unlikely(t == 1) = 1
__builtin_expect(7, atoi("7!")) = 7
*/

#include <stdio.h>
#include <assert.h>

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
}
