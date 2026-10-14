/* EXPECT:
ok
*/

/* hang minimized from a curl test harness*/

int puts(const char *);

static int f(int x, int y) {
   if (x)
      goto L;
   if (y)
      goto L;
   return 0;
   goto L; /* dead goto */
L:
   return 42;
}

int main(void) {
   if (f(1, 0) != 42 || f(0, 1) != 42 || f(0, 0) != 0)
      return 1;
   puts("ok");
   return 0;
}
