/* EXPECT:
expect 6, -4, 7, -1
       6, -4, 7, -1
CEFA CDAB3412 AA000000000000FF
*/

int xor(int a) {
   return a ^ 3 | 555;
}

int cmp(float x, float y) {
   return x < y && x > 0.f;
}

struct foo {
   unsigned int x : 10;
   unsigned y : 7;
   short k:3;
int : 0;
   short a:15;
   long long h:60;
};

int bitf(struct foo *q) {
   extern void aeiou(int);
   aeiou(q->h);
   return q->x + q->y - q->k + q->a;
}

struct s1 {
   short x : 3, y : 12;
};
struct s2 {
   struct s1 a;
};

struct s2 bitfcopy2(struct s2 x) {
   return (struct s2){x.a};
}

int bswap16(int x) { return __builtin_bswap16(x); }
int bswap32(int x) { return __builtin_bswap32(x); }
long long bswap64(long long x) { return __builtin_bswap64(x); }

int main(int p) {
   extern int printf(const char *, ...);

   static struct foo foo;

   foo.x = 5+p;
   foo.k = 3;
   foo.h += 7;
   bitf(&foo);
   foo.a = -1;
   foo.a = xor(foo.a |= 3);
   printf("expect %d, -4, 7, %d\n", 5+p, (short)((-1|3)^3|555));
   printf("       %d, %d, %lld, %d\n", foo.x, foo.k+=1, foo.h, foo.a);
   printf("%X %X %llX\n", bswap16(0xFACE), bswap32(0x1234ABCD), bswap64(0xFF000000000000AAll));

   int x = 42,
       *a = &x,
       **b = &a,
       ***c = &b,
       ****d = &c,
       *****e = &d,
       ******f = &e;
   return ******f;
}

void aeiou(int _){}
