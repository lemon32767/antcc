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

int printf(const char *, ...);
int main() {
   printf("should print 14: %d\n", crz(7));
   printf("should print 0: %d\n", crz(-3));
   printf("should print 0: %d\n", crz(0));
}
