#include "ir.h"
#include <stdint.h>

struct uses *
ssauses(struct function *fn)
{
   extern int ninstr;
   struct uses *uses = xcalloc(ninstr * sizeof *uses);
   vec_of(struct use) *vuse = xcalloc(ninstr * sizeof *vuse), useacc = {0};
   struct block *blk = fn->entry;

   #define USE(r, isjmp, x) do {                       \
      struct use u0, u = {isjmp, {x}};                 \
      if ((r).t == RTMP) {                             \
         if (vuse[(r).i].n == 0){                      \
            memcpy(&vuse[(r).i].p, &u, sizeof u);      \
            ++vuse[(r).i].n;                           \
         } else {                                      \
            if (vuse[(r).i].n == 1) {                  \
               memcpy(&u0, &vuse[(r).i].p, sizeof u0); \
               vuse[(r).i].n = 0;                      \
               vuse[(r).i].p = 0;                      \
               vpush(&vuse[(r).i], u0);                \
            }                                          \
            vpush(&vuse[(r).i], u);                    \
         }                                             \
      }                                                \
   } while (0)

   do {
      for (int i = 0; i < blk->phi.n; ++i) {
         int ins = blk->phi.p[i];
         union ref *phi = phitab.p[instrtab[ins].l.i];
         for (int i = 0; i < blk->npred; ++i) {
            USE(phi[i], 0, ins);
         }
      }
      for (int i = 0; i < blk->ins.n; ++i) {
         int ins = blk->ins.p[i];
         assert(instrtab[ins].l.t != RMORE);
         if (instrtab[ins].op != Ocall) assert(instrtab[ins].l.t != RMORE);
         USE(instrtab[ins].l, 0, ins);
         USE(instrtab[ins].r, 0, ins);
      }
      USE(blk->jmp.arg[0], 1, blk->id);
      USE(blk->jmp.arg[1], 1, blk->id);
   } while ((blk = blk->lnext) != fn->entry);
   for (int i = 0; i < ninstr; ++i) {
      int nuse = uses[i].nuse = vuse[i].n;
      if (!nuse) continue;
      if (nuse == 1) {
         uses[i].use = uses[i]._use0;
         memcpy(uses[i].use, &vuse[i].p, sizeof *uses[i].use);
      } else if (nuse < arraylength(uses[i]._use0)) {
         uses[i].use = uses[i]._use0;
         memcpy(uses[i].use, vuse[i].p, nuse * sizeof(struct use));
         vfree(&vuse[i]);
      } else {
         intptr_t o;
         o = useacc.n;
         memcpy(&uses[i].use, &o, sizeof o);
         vpushn(&useacc, vuse[i].p, nuse);
         vfree(&vuse[i]);
      }
   }
   for (int i = 0; i < ninstr; ++i) {
      if (uses[i].use != uses[i]._use0) {
         intptr_t o;
         memcpy(&o, &uses[i].use, sizeof o);
         uses[i].use = useacc.p + o;
      }
   }
   free(vuse);
   return uses;
}

void
freeuses(struct uses *u, int n)
{
   for (int i = 0; i < n; ++i) {
      if (u[i].use != u[i]._use0) {
         free(u[i].use);
         break;
      }
   }
   free(u);
}

/* vim:set ts=3 sw=3 expandtab: */
