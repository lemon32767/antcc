/* CFLAGS: -D CMD_WORKING -DV=void */
/* EXPECT:
ok /1 "\n"n ;.& 05.5 ADD(1,2)
hi from header ;73
Foo,5 9 1506
join: "x ## y"
wide   L"abc123 猫,€á💫",  U+1f98b
Output ends here
ok ...
11
11
token spacing:
++ -- ++ === (0)~> ++ 3>0$
sum = 1 + 2 +3;$
[ baz] ;$
+. *$
x=7 7
*/

#if !__has_include("07-pp.h")
#error "hasinclude?"
#endif
#include "07-pp.h"
#include "07-pp.h"
#if __has_include(<stddef.h>)
#include <stddef.h>
#endif
#include <stdio.h>
#include <wchar.h>
#include <locale.h>
//
#define CATl(a) a##bar
#define CATr(a) foo##a
#define CAT(a,b) a##b
#define foobar() foo##bar

#define hash_hash # ## #
#define mkstr(a) # a
#define in_between(a) mkstr(a)
#define join(c, d) in_between(c hash_hash d)
char p[] = join(x, y); // equivalent to   char p[] = "x ## y";

#define PUTS p\
u\
t\
s

#define gnu_ext(a, w, c...) a(w, ## c) __VA_OPT__(;)

# define STACK_OF(type) struct stack_st_##type

/* Helper macro for internal use */
# define SKM_DEFINE_STACK_OF_INTERNAL(t1, t2, t3) \
    STACK_OF(t1); \
    typedef int (*sk_##t1##_compfunc)(const t3 * const *a, const t3 *const *b); \
    typedef void (*sk_##t1##_freefunc)(t3 *a); \
    typedef t3 * (*sk_##t1##_copyfunc)(const t3 *a); \

struct crypto_ex_data_st {
    void *ctx;
    STACK_OF(void) *sk;
};

SKM_DEFINE_STACK_OF_INTERNAL(void, void, void)

char *strchr(const char *, int);
# define __glibc_const_generic(PTR, CTYPE, CALL)	\
  _Generic (0 ? (PTR) : (void *) 1,			\
	    const void *: (CTYPE) (CALL),		\
	    default: CALL)
#define indir(s,c) strchr(s,c)
#define strchr(S, C)	__glibc_const_generic (S, const char *, indir(S, C))

static void f(void) {
   (void)strchr("",0);
}

#define xcat(a,b) CAT(a,b)
#define uniq(p) xcat(p,__COUNTER__)
#define xcountertest(f) int f; f = 11; printf("%d\n", f);
#define countertest(v) xcountertest(uniq(v))

#ifdef CMD_WORKING
int
main(V)
#endif // comment
{
   int CATl(foo);
   ++foobar;
   --CATr(bar);
   CAT(foo,bar) += 3;
   foobar() /=2;
   printf("%s %s\n",STR ( ok /1   "\n"n ;.& 
            05.5), STR(ADD(1,2)));
   hi(ADD(Foo, SQR(Bar+1)));
   int foo123 = 77;
   printf("%s "
         "%s %g\n", str(Foo,5), xstr(Foo), CAT(1.5,e3f) + CAT(7,)-CAT(,1));
   printf("join: \"%s\"\n", p);

   setlocale(LC_ALL, "C.UTF-8");
   printf("wide   L\"%ls\",  U+%x\n", L"abc123 猫,€á💫", L'🦋');

   PUT\
S\
("Output ends here\\
0Not printed" /* After line splicing, the remaining backslash
               * escapes the 0, ending the string early.
               */
);
   gnu_ext(printf, "ok %s", "...") gnu_ext(printf, "\n");

   countertest(ww);
   countertest/* comment
                 */(ww);

#define PLUS +
#define EMPTY
      (void)+PLUS(0); /* (void) + + 0 */
   printf("token spacing:\n"
#define f(x) =x=
#define g(t,k) t##k>
         xstr(+PLUS -EMPTY- PLUS+ f(=) (0)g(,~) g(+,+ 3)0) "$\n"
#define add(x, y, z) x + y +z;
         xstr(sum = add (1,2, 3)) "$\n"
#define foo bar
#define bar EMPTY baz
#define EMPTY
#define EMPTY2 EMPTY
        xstr([foo] EMPTY2;) "$\n"
#define qx1(...)/**/__VA_ARGS__
#define qx2(a) qx1 ( a )
        xstr(+qx2 ( .)//
qx2(*) ) "$\n"
        );

#define A(x,y) x=y
   A(int x,
#if 1
         7
#else 
         3
#endif
      );
   printf("x=%d %g\n",x, CAT(7,.));

   CAT(ret,urn) 0;
}
