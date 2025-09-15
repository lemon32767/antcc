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
