/* EXPECT:
sizeof packed4i1i4f=9
sizeof packfield=7
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

#include <stdio.h>
int main(){
   printf("sizeof packed4i1i4f=%d\n", (int)sizeof(struct packed4i1i4f));
   printf("sizeof packfield=%d\n", (int)sizeof(struct packfield));
}
