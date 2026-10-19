#include "_int.h"

/* SWAR algorithm from Hacker's Delight */

int __popcountsi2(u32int x) {
   x = x - ((x >> 1) & 0x55555555);
   x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
   x = (x + (x >> 4)) & 0x0F0F0F0Full;
   x += x >> 16;
   return (x + (x >> 8)) & 0x3F;
}
