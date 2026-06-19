/* EXPECT:
sizeof default=24
sizeof pack1=11
sizeof pack2=12
sizeof pack4=16
sizeof pack8=24
sizeof p_push=11
sizeof p_pop=16
sizeof p_pop2=16
sizeof pushval=12
sizeof pushpop=16
sizeof reset=24
*/

#include <stdio.h>

struct S_default {
   char a;
   long long b;
   short c;
};

#pragma pack(1)
struct S_pack1 {
   char a;
   long long b;
   short c;
};
#pragma pack()

#pragma pack(2)
struct S_pack2 {
   char a;
   long long b;
   short c;
};
#pragma pack()

#pragma pack(4)
struct S_pack4 {
   char a;
   long long b;
   short c;
};
#pragma pack()

#pragma pack(8)
struct S_pack8 {
   char a;
   long long b;
   short c;
};
#pragma pack()

#pragma pack(4)
#pragma pack(push)
#pragma pack(push,1)
struct S_p_push {
   char a;
   long long b;
   short c;
};
#pragma pack(pop)
struct S_p_pop {
   char a;
   long long b;
   short c;
};
#pragma pack(pop)
struct S_p_pop2 {
   char a;
   long long b;
   short c;
};

#pragma pack(push, 2)
#pragma pack(push)
struct S_pushval {
   char a;
   long long b;
   short c;
};
#pragma pack(pop)
#pragma pack(push, 4)
struct S_pushpop {
   char a;
   long long b;
   short c;
};
#pragma pack(pop)
#pragma pack(pop)

#pragma pack()
struct S_reset {
   char a;
   long long b;
   short c;
};


int main(void) {
   printf("sizeof default=%d\n", (int)sizeof(struct S_default));
   printf("sizeof pack1=%d\n", (int)sizeof(struct S_pack1));
   printf("sizeof pack2=%d\n", (int)sizeof(struct S_pack2));
   printf("sizeof pack4=%d\n", (int)sizeof(struct S_pack4));
   printf("sizeof pack8=%d\n", (int)sizeof(struct S_pack8));
   printf("sizeof p_push=%d\n", (int)sizeof(struct S_p_push));
   printf("sizeof p_pop=%d\n", (int)sizeof(struct S_p_pop));
   printf("sizeof p_pop2=%d\n", (int)sizeof(struct S_p_pop2));
   printf("sizeof pushval=%d\n", (int)sizeof(struct S_pushval));
   printf("sizeof pushpop=%d\n", (int)sizeof(struct S_pushpop));
   printf("sizeof reset=%d\n", (int)sizeof(struct S_reset));
}
/* vim:set ts=3 sw=3 expandtab: */
