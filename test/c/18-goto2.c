/* EXPECT:
ok
*/

void h(int x){}

/* isolated mem2reg bug from something in regalloc: the goto into the loop past
 * the `int nqueue` declaration caused a miscompilation */
void test(void) {
   int curi = 5;
   goto start2;
   for (curi; curi >= 0; curi--) {
      int nqueue;
   start2:
      for (nqueue = 2; nqueue > 0; ) {
         h(nqueue--);
      }
   }
}
int puts(const char *);
int main(void) {
   test();
   puts("ok");
}
