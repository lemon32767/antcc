/* EXPECT:
si(-1) = 18
si(0) = 10
si(1) = 11
si(2) = 12
si(3) = 13
si(4) = 14
si(5) = 15
si(6) = 16
si(7) = 17
si(8) = -1
ui(0) = 100
ui(1) = 101
ui(2) = 102
ui(3) = 103
ui(0x7fffffff) = 104
ui(0x80000000) = 105
ui(0x80000001) = 106
ui(0xffffffff) = 107
ui(0x40000000) = -1
ull(0) = 200
ull(5) = 205
ull(0x7fffffffffffffffll) = 201
ull(0x8000000000000000ll) = 202
ull(0x8000000000000001ll) = 203
ull(0xffffffffffffffffll) = 204
ull(0x123456789abcdef0ll) = -1
*/

#include <stdio.h>

static int
si(int x) {
   switch (x) {
   case -1: return 18;
   case 0: return 10;
   case 1: return 11;
   case 2: return 12;
   case 3: return 13;
   case 4: return 14;
   case 5: return 15;
   case 6: return 16;
   case 7: return 17;
   default: return -1;
   }
}

static int
ui(unsigned x) {
   switch (x) {
   case 0u: return 100;
   case 1u: return 101;
   case 2u: return 102;
   case 3u: return 103;
   case 0x7fffffffu: return 104;
   case 0x80000000u: return 105;
   case 0x80000001u: return 106;
   case 0xffffffffu: return 107;
   default: return -1;
   }
}

static int
ull(unsigned long long x) {
   switch (x) {
   case 0: return 200;
   case 5: return 205;
   case 0x7fffffffffffffffULL: return 201;
   case 0x8000000000000000ULL: return 202;
   case 0x8000000000000001ULL: return 203;
   case 0xffffffffffffffffULL: return 204;
   default: return -1;
   }
}

int
main(void) {
#define pri(x) printf(#x" = %d\n", (int)(x))
   pri(si(-1)), pri(si(0)), pri(si(1)), pri(si(2)), pri(si(3)), pri(si(4)), pri(si(5));
   pri(si(6)), pri(si(7)), pri(si(8)), pri(ui(0)), pri(ui(1)), pri(ui(2)), pri(ui(3));
   pri(ui(0x7fffffff)), pri(ui(0x80000000)), pri(ui(0x80000001));
   pri(ui(0xffffffff)), pri(ui(0x40000000));
   pri(ull(0)), pri(ull(5));
   pri(ull(0x7fffffffffffffffll)), pri(ull(0x8000000000000000ll)), pri(ull(0x8000000000000001ll));
   pri(ull(0xffffffffffffffffll)), pri(ull(0x123456789abcdef0ll));
   return 0;
}
