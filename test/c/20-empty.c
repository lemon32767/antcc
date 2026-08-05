/* EXPECT:
sizeof (struct empty) === 0
sizeof bleh[1] === 0
sizeof (struct flx0) == sizeof (int) === 1
sizeof (a) === 0
sizeof (aa) === 0
*/

void f(){}

struct empty {};
extern struct empty bleh[10];
struct flx0 { int t; int m[0]; };

int main() {
   int a[] = {};
   int aa[][0] = {};
   struct empty empty = {};

   extern int printf(const char *, ...);
#define P(x) printf(#x" === %d\n", (int)x)
   P(sizeof (struct empty));
   P(sizeof bleh[1]);
   P(sizeof (struct flx0) == sizeof (int));
   P(sizeof (a));
   P(sizeof (aa));
}

