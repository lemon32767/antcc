int xor(int a) {
   return a ^ 3 | 233333;
}

int cmp(float x, float y) {
   return x < y && x > 0.f;
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

