#include "_int.h"

/* SWAR algorithm from Hacker's Delight
 * 64-bit version from LLVM compiler_rt */

int __popcountdi2(u64int x) {
   x = x - ((x >> 1) & 0x5555555555555555ull);
   x = ((x >> 2) & 0x3333333333333333ull) + (x & 0x3333333333333333ull);
   x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0Full;
   u32int y = x + (x >> 32);
   y += y >> 16;
   return (y + (y >> 8)) & 0x7F;
}
