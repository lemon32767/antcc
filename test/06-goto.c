/* EXPECT:
7 -> 14
-3 -> 0
0 -> 0
2 -> 4
*/
int crz(int x) {
/* x <<= 1; while (x < 0) ++x; return x; */
  goto e;
a:
  return x;
j:
  ++x;
  goto q;
b:
  if (x < 0)
    goto j;
  goto a;
q:
  goto b;
e:
  x <<= 1;
  goto b;
}
void dummy() {
   for (;;) {
      ;
   }
}

int printf(const char *, ...);
int main(int n) {
   n = 7,  printf("%d -> %d\n", n, crz(n));
   n = -3, printf("%d -> %d\n", n, crz(n));
   n = 0,  printf("%d -> %d\n", n, crz(n));
   n = 2,  printf("%d -> %d\n", n, crz(n));
}
