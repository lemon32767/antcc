int __popcountdi2(unsigned long long x) {
   int n = 0;
   while (x) x &= x-1, ++n;
   return n;
}
