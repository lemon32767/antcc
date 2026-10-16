/* EXPECT:
sizeof packed4i1i4f=9
sizeof packfield=7
sizeof packenum=1
sizeof packenum2=2
sizeof attrafter=6
alignof algn1=8
alignof algn2=16
sizeof packedwaligned=16
foo[:lo4] 0
bar[:lo4] 0
pos 4
AA=7
mode word=8
mode TI u=16
mode TI i=16
mode HI=2
mode QI=1
mode SF=4
mode pointer=8
mode alias=8
mode signed word=0
mode unsigned HI=1
mode local=1
mode word gvar=8
mode HI gvar=2
mode QI gvar=1
mode QI direct gvar=1
*/

#if !__has_attribute(noreturn)
#error "no noreturn"
#elif __has_attribute(foobar)
#error "attribute foobar !?"
#endif

struct __attribute__((packed)) packed4i1i4f {
   int x;
   char y;
   float z;
};

struct packfield {
   char a;
   short y[3] __attribute__((packed));
};

enum __attribute__((packed)) packenum {
   E123,
};

enum packenum2 {
   E456 = -256,
} __attribute__((packed));

struct algn1 {
   int q __attribute((aligned(8)));
};
union __attribute__((aligned(16))) algn2 {
   short t;
};

struct attrafter { int x; short y; } __attribute__((packed));

struct __attribute__((packed)) {
   int y,
    __attribute__((aligned(8))) x;/* takes precedence over packed */
} packedwaligned;


char __attribute((aligned(16))) sfoo[] = "aaa";
char __attribute((aligned(16))) sbar[] = "bbb";

enum at { AA __attribute__((deprecated)) = 7, AB };

int __attribute__((noinline)) add1(int x) { return x + 1; }

int nested(int x) {
   int (*f)(int) __attribute__((unused)) = add1;
   int (__attribute__((unused)) *g)(int) = f;
   return g(x);
}

typedef int mode_word_t __attribute__((__mode__(__word__)));
typedef unsigned int mode_ti_u __attribute__((__mode__(TI)));
typedef int mode_ti_i __attribute__((__mode__(TI)));
typedef unsigned int mode_hi_u __attribute__((__mode__(__HI__)));
typedef int mode_qi_i __attribute__((mode(QI)));
typedef float mode_sf __attribute__((__mode__(SF)));
typedef double mode_df __attribute__((__mode__(__DF__)));
typedef unsigned int mode_ptr_u __attribute__((__mode__(pointer)));

typedef mode_word_t mode_word_alias_t;
mode_word_t mode_glob;
mode_hi_u mode_g16;
mode_qi_i mode_g8;
int mode_gvar __attribute__((mode(QI)));

#include <stdio.h>
#include <stdint.h>
#undef __attribute__

int main(){
   int r = 0;
   int t = AA;
   printf("sizeof packed4i1i4f=%d\n", (int)sizeof(struct packed4i1i4f));
   printf("sizeof packfield=%d\n", (int)sizeof(struct packfield));
   printf("sizeof packenum=%d\n", (int)sizeof(enum packenum));
   printf("sizeof packenum2=%d\n", (int)sizeof(enum packenum2));
   printf("sizeof attrafter=%d\n", (int)sizeof(struct attrafter));
   printf("alignof algn1=%d\n", (int)_Alignof(struct algn1));
   printf("alignof algn2=%d\n", (int)_Alignof(union algn2));
   printf("sizeof packedwaligned=%d\n", (int)sizeof(packedwaligned));
   printf("foo[:lo4] %d\n", (int)(intptr_t)sfoo &0xF);
   printf("bar[:lo4] %d\n", (int)(intptr_t)sbar &0xF);
lab: __attribute__((unused));
   switch (t) {
   case AA:
      r = nested(1);
      __attribute__((fallthrough));
   case AB:
      r = r + (AB - 6);
   }
   printf("pos %d\n", r);
   printf("AA=%d\n", AA);
   printf("mode word=%d\n", (int)sizeof(mode_word_t));
   printf("mode TI u=%d\n", (int)sizeof(mode_ti_u));
   printf("mode TI i=%d\n", (int)sizeof(mode_ti_i));
   printf("mode HI=%d\n", (int)sizeof(mode_hi_u));
   printf("mode QI=%d\n", (int)sizeof(mode_qi_i));
   printf("mode SF=%d\n", (int)sizeof(mode_sf));
   printf("mode pointer=%d\n", (int)sizeof(mode_ptr_u));
   printf("mode alias=%d\n", (int)sizeof(mode_word_alias_t));
   printf("mode signed word=%d\n", (mode_word_t)-1 > 0);
   printf("mode unsigned HI=%d\n", (mode_hi_u)-1 > 0);
   {
      __attribute__((mode(QI))) int q;
      q = 0;
      printf("mode local=%d\n", (int)sizeof q);
   }
   printf("mode word gvar=%d\n", (int)sizeof mode_glob);
   printf("mode HI gvar=%d\n", (int)sizeof mode_g16);
   printf("mode QI gvar=%d\n", (int)sizeof mode_g8);
   printf("mode QI direct gvar=%d\n", (int)sizeof mode_gvar);
}
