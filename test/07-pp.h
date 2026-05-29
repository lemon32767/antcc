#ifndef GUARD
#define GUARD

#define THIS "07-pp.h"
#if !( __has_include(THIS) & !__has_include_next(THIS))
#error "has include next"
#endif


extern int printf(const char *, ...);
extern warnhere();
#define Foo 9
void hi(int x) {
   printf("hi from header ;%d\n", x);
}
#if 1
#endif
#elifndef Ww /*
             t'e   */
#define Bar 7
#define SQR_(x) ((x)*(x))
#define SQR(y) SQR_(1+(y)-1)
#define ADD(a,b) (a)+(b)
#define STR(h) #h 

#define xstr(s1) str(s1)
#define str(...) #__VA_ARGS__

#endif

extern int printf(const char *, ...);
