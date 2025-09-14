int xor(int a) {
   return a ^ 3 | 233333;
}

int cmp(float x, float y) {
   return x < y && x > 0.f;
}

struct foo {
   int x : 10;
   unsigned y : 7;
   short k:3;
int : 0;
   short a:15;
};

int bitf(struct foo q) {
   return q.x + q.y - q.k + q.a;
}

int main() {
   int x = 42,
       *a = &x,
       **b = &a,
       ***c = &b,
       ****d = &c,
       *****e = &d,
       ******f = &e;
   return ******f;
}

