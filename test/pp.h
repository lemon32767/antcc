#ifndef GUARD
#define GUARD

extern warnhere();
#define Foo 9
void hi(int x) {
   extern int printf(const char *, ...);
   printf("hi from header ;%d\n", x);
}
#if 1
#endif
#elifndef Ww
#define Bar 7
#define SQR_(x) (x)*(x)
#define SQR(y) SQR_(y)
#define ADD(a,b) (a)+(b)

#endif
