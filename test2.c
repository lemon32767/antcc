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
