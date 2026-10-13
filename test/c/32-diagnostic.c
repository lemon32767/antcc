/* EXPECT:
ok 1
ok 2
ok 3
sizeof Q=9
*/

#include <stdio.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored /* c */ "-Wunused-function"
static int unused(void) { return 1; }
#pragma GCC diagnostic warning /* c */ "-Wall"
#pragma GCC diagnostic error "-Werror"
#pragma GCC diagnostic pop

#pragma clang diagnostic push
#pragma clang diagnostic ignored /* c */ "-Wshadow"
_Pragma("GCC diagnostic push")
_Pragma("GCC diagnostic ignored /* c */ \"-Wmissing-braces\" //cm")
static int f(void) { return 2; }
_Pragma("GCC diagnostic pop")
#pragma clang diagnostic pop

#pragma pack(push /* c */, 1)//a
struct Q { char a; long b; };
#pragma pack(pop)

int main(void) {
   puts("ok 1");
   f();
   puts("ok 2");
   puts("ok 3");
   printf("sizeof Q=%zu\n", sizeof(struct Q));
   return 0;
}
