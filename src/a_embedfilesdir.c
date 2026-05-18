#include <stddef.h>

#define S(s) s"\0\0\0\0\0", (sizeof s) - 1

struct EmbedFile {
   const char *name;
   const char *s;
   size_t len;
} embedfilesdir[] = {
{"stddef.h", S("\
#pragma once\n\
typedef __PTRDIFF_TYPE__ ptrdiff_t;\n\
typedef __SIZE_TYPE__ size_t;\n\
typedef __WCHAR_TYPE__ wchar_t;\n\
#undef NULL\n\
#define NULL ((void*)0)\n\
#undef offsetof\n\
#define offsetof(type, memb) ((size_t)((char *)&((type *)0)->memb - (char *)0))\n\
#if __STDC_VERSION__ >= 201112L\n\
typedef struct {long long __ll; long double __ld;} max_align_t;\n\
#endif\n\
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

{"complex.h", S("\
#pragma once\n\
#include_next <complex.h>\n\
#undef _Complex_I\n\
#define _Complex_I 1.0iF\n\
")},

{"iso646.h", S("\
#define and     &&\n\
#define and_eq  &=\n\
#define bitand  &\n\
#define bitor   |\n\
#define compl   ~\n\
#define not     !\n\
#define not_eq  !=\n\
#define or      ||\n\
#define or_eq   |=\n\
#define xor     ^\n\
#define xor_eq  ^=\n\
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

{"limits.h", S("\
#pragma once\n\
/* Tell glibc not to try to recursively #include_next gcc's <limits.h> */\n\
#if defined __GNUC__ && !defined _GCC_LIMITS_H\n\
#define _GCC_LIMITS_H_\n\
#endif\n\
/* We want the system libc header for POSIX constants */\n\
#ifdef __STDC_HOSTED__\n\
#include_next <limits.h>\n\
#endif\n\
\n\
#undef CHAR_BIT\n\
#undef CHAR_MAX\n\
#undef CHAR_MIN\n\
#undef UCHAR_MAX\n\
#undef SCHAR_MAX\n\
#undef SCHAR_MIN\n\
#undef USHRT_MAX\n\
#undef SHRT_MAX\n\
#undef SHRT_MIN\n\
#undef MB_LEN_MAX\n\
#undef UINT_MAX\n\
#undef INT_MAX\n\
#undef INT_MIN\n\
#undef ULONG_MAX\n\
#undef LONG_MAX\n\
#undef LONG_MIN\n\
#undef ULLONG_MAX\n\
#undef LLONG_MAX\n\
#undef LLONG_MIN\n\
\n\
#define CHAR_BIT    8\n\
#ifdef __CHAR_UNSIGNED__\n\
#define CHAR_MAX    UCHAR_MAX\n\
#define CHAR_MIN    0\n\
#else\n\
#define CHAR_MAX    SCHAR_MAX\n\
#define CHAR_MIN    SCHAR_MIN\n\
#endif\n\
#define UCHAR_MAX   255\n\
#define SCHAR_MAX   127\n\
#define SCHAR_MIN   (-128)\n\
#define USHRT_MAX   65535\n\
#define SHRT_MAX    32767\n\
#define SHRT_MIN    (-32768)\n\
#define MB_LEN_MAX  16\n\
#define UINT_MAX    4294967295U\n\
#define INT_MAX     2147483647\n\
#define INT_MIN     (-INT_MAX - 1)\n\
#if __SIZEOF_LONG__ == __SIZEOF__INT__\n\
#define ULONG_MAX   4294967295UL\n\
#define LONG_MAX    2147483647L\n\
#else\n\
#define ULONG_MAX   18446744073709551615UL\n\
#define LONG_MAX    9223372036854775807L\n\
#endif\n\
#define LONG_MIN    (-LONG_MAX - 1L)\n\
#define ULLONG_MAX  18446744073709551615ULL\n\
#define LLONG_MAX   9223372036854775807LL\n\
#define LLONG_MIN   (-LLONG_MAX-1)\n\
")},

   {NULL}
};

/* vim:set ts=3 sw=3 expandtab: */
