/* EXPECT:
sizeof packed4i1i4f=9
sizeof packfield=7
alignof algn1=8
alignof algn2=16
sizeof packedwaligned=16
foo[:lo4] 0
bar[:lo4] 0
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

struct algn1 {
   int q __attribute((aligned(8)));
};
union __attribute__((aligned(16))) algn2 {
   short t;
};

/* NYI after definition:
   struct foo { .. } __attribute__((packed)); 
this is annoying because we currently build the struct type and its layout on the fly
as we parse it. Either refactor that to do it on two passes or 'rebuild' aggregate
when we encounter attributes after the '}'
*/

struct __attribute__((packed)) {
   int y,
    __attribute__((aligned(8))) x;/* takes precedence over packed */
} packedwaligned;


char __attribute((aligned(16))) sfoo[] = "aaa";
char __attribute((aligned(16))) sbar[] = "bbb";

#include <stdio.h>
#include <stdint.h>

int main(){
   printf("sizeof packed4i1i4f=%d\n", (int)sizeof(struct packed4i1i4f));
   printf("sizeof packfield=%d\n", (int)sizeof(struct packfield));
   printf("alignof algn1=%d\n", (int)_Alignof(struct algn1));
   printf("alignof algn2=%d\n", (int)_Alignof(union algn2));
   printf("sizeof packedwaligned=%d\n", (int)sizeof(packedwaligned));
   printf("foo[:lo4] %d\n", (int)(intptr_t)sfoo &0xF);
   printf("bar[:lo4] %d\n", (int)(intptr_t)sbar &0xF);
}
