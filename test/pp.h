#ifndef GUARD
#define GUARD

extern warnhere();
#define Foo 9
void hi() {
   extern int printf();
   printf("hi from header\n");
}

#elifndef Ww
#define Bar 7

#endif
