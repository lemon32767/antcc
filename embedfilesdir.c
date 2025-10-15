#include <stddef.h>

struct embedfile {
   const char *name;
   const char *s;
   size_t len;
};

#define S(s) (char [4096]){s}, sizeof #s - 1

struct embedfile embedfilesdir[] = {
{"stddef.h", S("\
typedef __typeof__((char*)0 - (char*)0) ptrdiff_t;\n\
typedef __typeof__(sizeof 0) size_t;\n\
/*typedef __typeof__(L'a') wchar_t;*/\n\
typedef int wchar_t;\n\
#define NULL ((void *)0)\n\
#define offsetof(_Type, _Memb) ((size_t)(&((_Type *)0)->_Memb - (_Type *)0))\n\
")},

{"stdarg.h", S("\
typedef __builtin_va_list va_list;\n\
#ifndef __GNUC_VA_LIST\n\
#define __GNUC_VA_LIST\n\
typedef __builtin_va_list __gnuc_va_list;\n\
#endif\n\
#define va_start(ap,n)   __builtin_va_start(ap)\n\
#define va_arg(ap,type)  __builtin_va_arg(ap, type)\n\
#define va_copy(dst,src) (void)((dst)=(src))\n\
#define va_end(ap)       __builtin_va_end(ap)\n\
")},

{"stdbool.h", S("\
#define bool _Bool \n\
#define true 1\n\
#define false 0\n\
#define __bool_true_false_are_defined 1\n\
")},

   {NULL}
};

/* vim:set ts=3 sw=3 expandtab: */
