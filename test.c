/* coment */

#if 1+1 < (-2*'a')
wawa
#elif 9<<1
#define wow 3
#else
boop
#endif
int glob [ wow+wow];

struct foo {
    int x, y, z;
};

int test0(struct foo *foo) { return foo->x ? foo->y : foo->z; }
int test1(int x, int y, int z) { return x && y || z; }
int test2(int x, int y, int z) { return x || y && z; }

//
