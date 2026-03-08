/* EXPECT:
-1155497588
*/

typedef unsigned long long uvlong;
int fn1(uvlong p_9) {
/* extract from ldrgen seed=637312671 */
   uvlong v_73, q;
   if (p_9) {
      v_73 = 909910719;
   }
   q = (uvlong)p_9 / ((v_73 - 572547313ull) + 445ull);
   return q;
}

extern int printf(const char *, ...);
int main() {
   printf("%d\n", fn1(-77ull));
}
