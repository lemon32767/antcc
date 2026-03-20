#include <stddef.h>

#define S(s) s"\0\0\0\0\0", (sizeof s) - 1

struct EmbedFile {
   const char *name;
   const char *s;
   size_t len;
} embedfilesdir[] = {
{"stddef.h", S("\
#pragma once\n\
typedef __typeof__((char*)0 - (char*)0) ptrdiff_t;\n\
typedef __typeof__(sizeof 0) size_t;\n\
typedef __typeof__(L'a') wchar_t;\n\
#undef NULL\n\
#define NULL ((void*)0)\n\
#undef offsetof\n\
#define offsetof(type, memb) ((size_t)((char *)&((type *)0)->memb - (char *)0))\n\
")},

{"stdarg.h", S("\
#pragma once\n\
typedef __builtin_va_list va_list;\n\
#ifndef __GNUC_VA_LIST\n\
#define __GNUC_VA_LIST\n\
typedef __builtin_va_list __gnuc_va_list;\n\
#endif\n\
#define va_start(ap,n)   __builtin_va_start(ap)\n\
#define va_arg(ap,type)  __builtin_va_arg(ap, type)\n\
#define va_copy(dst,src) __builtin_va_copy(dst, src)\n\
#define va_end(ap)       __builtin_va_end(ap)\n\
")},

{"stdbool.h", S("\
#pragma once\n\
#if __STDC_VERSION__ < 202311L /* in C23 they are keywords */\n\
#define bool _Bool \n\
#define true 1\n\
#define false 0\n\
#endif\n\
#define __bool_true_false_are_defined 1\n\
")},

{"float.h", S("\
#pragma once\n\
#define FLT_ROUNDS       (-1)\n\
#define FLT_EVAL_METHOD  (-1)\n\
#define FLT_HAS_SUBNORM  (-1)\n\
#define DBL_HAS_SUBNORM  (-1)\n\
#define LDBL_HAS_SUBNORM (-1)\n\
#define FLT_RADIX        2\n\
#define FLT_MANT_DIG     24\n\
#define DBL_MANT_DIG     53\n\
#define LDBL_MANT_DIG    53\n\
#define FLT_DECIMAL_DIG  9\n\
#define DBL_DECIMAL_DIG  17\n\
#define LDBL_DECIMAL_DIG 17\n\
#define DECIMAL_DIG      17\n\
#define FLT_DIG          6\n\
#define DBL_DIG          15\n\
#define LDBL_DIG         15\n\
#define FLT_MIN_EXP      (-125)\n\
#define DBL_MIN_EXP      (-1021)\n\
#define LDBL_MIN_EXP     (-1021)\n\
#define FLT_MIN_10_EXP   (-37)\n\
#define DBL_MIN_10_EXP   (-307)\n\
#define LDBL_MIN_10_EXP  (-307)\n\
#define FLT_MAX_EXP      128\n\
#define DBL_MAX_EXP      1024\n\
#define LDBL_MAX_EXP     1024\n\
#define FLT_MAX_10_EXP   38\n\
#define DBL_MAX_10_EXP   308\n\
#define LDBL_MAX_10_EXP  308\n\
#define FLT_MAX          3.40282e+38\n\
#define DBL_MAX          1.79769e+308\n\
#define LDBL_MAX         1.79769e+308\n\
#define FLT_EPSILON      1.19209e-07\n\
#define DBL_EPSILON      2.22045e-16\n\
#define LDBL_EPSILON     2.22045e-16\n\
#define FLT_MIN          1.17549e-38\n\
#define DBL_MIN          2.22507e-308\n\
#define LDBL_MIN         2.22507e-308\n\
#define FLT_TRUE_MIN     1.4013e-45\n\
#define DBL_TRUE_MIN     4.94066e-324\n\
#define LDBL_TRUE_MIN    4.94066e-324\n\
")},

{"stdnoreturn.h", S("\
#define noreturn _Noreturn\n\
")},

{"stdalign.h", S("\
#if __STDC_VERSION__ < 202311L\n\
#define alignas _Alignas\n\
#define alignof _Alignof\n\
#define __alignas_is_defined 1\n\
#define __alignof_is_defined 1\n\
#endif\n\
")},

   {NULL}
};

/* vim:set ts=3 sw=3 expandtab: */
