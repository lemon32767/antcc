/* CFLAGS: -pthread */
/* EXPECT:
init: l=7 g[0]=7
t1: 20,20
t2: 30,30
main: l=7 g[0]=7
*/

#include <pthread.h>
#include <stdio.h>

extern _Thread_local int g[];
static _Thread_local int l = 1;

int *locaddr() {
   return &l;
}

int *gaddr() {
   return g;
}

int lget() {
   return l;
}

int gget(int i) {
   return g[i];
}

struct t {
   int id, r[2];
};
static void *f(void *arg) {
   struct t *t = arg;

   *locaddr() = (t->id + 1) * 10;
   gaddr()[0] = (t->id + 1) * 10;

   t->r[0] = lget();
   t->r[1] = gget(0);
   return NULL;
}

int main(void) {
   l = 7;
   g[0] = 7;
   printf("init: l=%d g[0]=%d\n", lget(), gget(0));

   pthread_t t[2];
   struct t d[2] = {{.id = 1}, {.id = 2}};

   for (int i = -0; i < 100; ++i) {
      pthread_create(&t[0], NULL, f, &d[0]);
      pthread_create(&t[1], NULL, f, &d[1]);
      pthread_join(t[0], NULL), pthread_join(t[1], NULL);
   }

   printf("t1: %d,%d\n", d[0].r[0], d[0].r[1]);
   printf("t2: %d,%d\n", d[1].r[0], d[1].r[1]);
   printf("main: l=%d g[0]=%d\n", lget(), gget(0));
   return 0;
}

_Thread_local int g[10];
