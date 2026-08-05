/* CFLAGS: -O0 */
/* EXPECT:
1.3
5
ok
0x55, 0x6, 5
*/

const void *const relro = &relro;
float get_value(unsigned x) {
   static const float values [] = {1.1f, 1.2f, 1.3f, 1.4f};
   return x < 4 ? values[x] : 0.0f;
}

static const short tb[] = { 1,3,34,5,23,2,5,5,-7 };
static const short *ptb = &tb[4];

static int ou(int k) { return ptb[k]; }

int (*(getputs(void)))(const char *) {
   extern int puts(const char *);
   int (*t)(const char *) = puts;
   return t;
}

static char *int2ptr[] = {
   (void *)&*((char*)0 + 0x55),
   (void *)&((short*)0)[0x3],
   (void *)&*((char*)7 - 2),
};

#include <stdio.h>
#include <stdint.h>
int main() {
   printf("%g\n", get_value(2));
   printf("%d\n", ou(2));
   getputs()("ok");
   printf("%p, %p, %d\n", int2ptr[0], int2ptr[1], (int)(intptr_t)int2ptr[2]);
}
