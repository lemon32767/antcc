typedef struct v2d { double x,y; } v2d;



void addp(v2d *a, const v2d *b)
{
    a->x += b->x;
    a->y += b->y;
}

v2d add(v2d a, v2d b)
{
    addp(&a, &b);
    return a;
}

v2d addx(v2d a, v2d b)
{
    return (v2d){a.x+b.x, a.y+b.y};
}

short s(int a, int b) {
    extern h();
    h(a +a * a);
    ++a;
    return a + b;
}

int i() {
    return s(1,2);
}

v2d v2v2() {
   v2d x = add((v2d){1,2}, (v2d){3,4});
   return x;
}
