/* EXPECT:
gexplicit[4] -> 7,3,4,5
gimplicit[] -> 3,5
dim2[2][2] -> {1,2}, {4,3}
S.x = 1, S.a[0][1] = 3, S.a[1][0] = 2
U.x = 1
str[] -> "abcdef"
strs -> "axcxdxbx"
desgn1[] -> {0.000000,0.000000}, {1.000000,3.000000}, {-0.500000,0.000000}
desgn2 -> {(nil),{0,1.000000},(null)}
desgn3 -> {(nil),{{{-1},0.000000},{{0},0.000000},{{6},1.000000}},"k"}
fi -> {1.500000 | 1069547520}
arrdsgn[] -> {0,5,0,0,1}
s -> "abc"
ss[] -> {"red","blue","green"}
*/
int gexplicit[4] = { 7, 3, 4, 5, };
_Static_assert(sizeof gexplicit/sizeof *gexplicit ==4, "");
int gimplicit[] = { 3, 5, };
_Static_assert(sizeof gimplicit/sizeof *gimplicit ==2, "");
char dim2[][2] = { {1,2},4,3 };
_Static_assert(sizeof dim2 == 4, "dim2");
struct S{int x; int a[2][12];} S = {1,{{0,3},2}};

const union U {int x; int y[2];} U = {1,};

char str[] = "abcdef";
_Static_assert(sizeof str == 7, "str[7]");

const char strs[][2] = {"ax", {'c','x'}, 'd',"?x"[1], "bx"};

/*
struct { int y; signed char x[]; } flex1  = {0, {0}};
struct { int y; signed char x[]; } flex2  = {0, 1,4,5};
struct { int y; signed char x[]; } flex3  = {0, "/"};
*/

struct  { float x, y; } desgn1[] = {[1]=1.f,3, -0.5};
_Static_assert(sizeof desgn1/sizeof *desgn1 == 3,"");

struct n { void * j; struct { int y; float z; }; void *g;} desgn2 = { .y = 0, 1.0f, 0, 8 };
struct n2 { void * j; struct as { struct { int y; }; float z; } a[3]; char *g;} desgn3 = { .a={-1}, .a[2].y = 6, 1.0f, "k" };

char *s = "abc";
const char *ss[] = {"red","blue","green"};

union fi {float f; int i;} fi = {.i = 0xFF<<22,};

char arrdsgn[] = { [4] = 1, [1] = 5 };
_Static_assert(sizeof arrdsgn == 5, "");

// int q[1] = {{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{1}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}};

/*
void f() {
   struct {int x,y;} a = {1,2}, b = {3,4}, c[] = {a,b};
}
*/

void *rec[1] = {rec+1};

int printf(char *, ...);
int main() {
   printf("gexplicit[4] -> %d,%d,%d,%d\n", gexplicit[0], gexplicit[1], gexplicit[2], gexplicit[3]);
   printf("gimplicit[] -> %d,%d\n", gimplicit[0], gimplicit[1]);
   printf("dim2[2][2] -> {%d,%d}, {%d,%d}\n", dim2[0][0],dim2[0][1],dim2[1][0],dim2[1][1]);
   printf("S.x = %d, S.a[0][1] = %d, S.a[1][0] = %d\n", S.x, S.a[0][1], S.a[1][0]);
   printf("U.x = %d\n", U.x);
   printf("str[] -> \"%s\"\n", str);
   printf("strs -> \"%.*s\"\n", (int)sizeof strs, strs);
   printf("desgn1[] -> {%f,%f}, {%f,%f}, {%f,%f}\n", desgn1[0].x, desgn1[0].y, desgn1[1].x, desgn1[1].y, desgn1[2].x, desgn1[2].y);
   printf("desgn2 -> {%p,{%d,%f},%s}\n", desgn2.j, desgn2.y, desgn2.z, desgn2.g);
   printf("desgn3 -> {%p,{{{%d},%f},{{%d},%f},{{%d},%f}},\"%s\"}\n", desgn3.j,
         desgn3.a[0].y, desgn3.a[0].z, desgn3.a[1].y, desgn3.a[1].z,
         desgn3.a[2].y, desgn3.a[2].z, desgn3.g);
   printf("fi -> {%f | %d}\n", fi.f, fi.i);
   printf("arrdsgn[] -> {%d,%d,%d,%d,%d}\n", arrdsgn[0], arrdsgn[1], arrdsgn[2], arrdsgn[3], arrdsgn[4]);
   printf("s -> \"%s\"\n", s);
   printf("ss[] -> {\"%s\",\"%s\",\"%s\"}\n", ss[0],ss[1],ss[2]);
   //printf("rec -> %p{%p}\n",rec,rec[0]);
}
