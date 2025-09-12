#ifndef GUARD
#define GUARD

extern int printf(const char *, ...);
extern warnhere();
#define Foo 9
void hi(int x) {
   printf("hi from header ;%d\n", x);
}
#if 1
#endif
#elifndef Ww
#define Bar 7
#define SQR_(x) ((x)*(x))
#define SQR(y) SQR_(1+(y)-1)
#define ADD(a,b) (a)+(b)
#define STR(h) #h 

#define xstr(s1) str(s1)
#define str(s) #s

#endif

extern int printf(const char *, ...);
