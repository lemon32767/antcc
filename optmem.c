#include "ir.h"

static const uchar loadszcls[] = {
   [Oloads1 - Oloads1] = 1|KI4<<4, [Oloadu1 - Oloads1] = 1|KI4<<4,
   [Oloads2 - Oloads1] = 2|KI4<<4, [Oloadu2 - Oloads1] = 2|KI4<<4,
   [Oloads4 - Oloads1] = 4|KI4<<4, [Oloadu4 - Oloads1] = 4|KI4<<4,
   [Oloadi8 - Oloads1] = 8|KI8<<4,
   [Oloadf4 - Oloads1] = 4|KF4<<4,
   [Oloadf8 - Oloads1] = 8|KF8<<4,
};
static const uchar load2ext[] = {
   [Oloads1 - Oloads1] = Oexts1, [Oloadu1 - Oloads1] = Oextu1,
   [Oloads2 - Oloads1] = Oexts2, [Oloadu2 - Oloads1] = Oextu2,
   [Oloads4 - Oloads1] = Oexts4, [Oloadu4 - Oloads1] = Oextu4,
   [Oloadi8 - Oloads1] = Ocopy,
};
#define loadsz(o) (loadszcls[(o) - Oloads1] & 0xF)
#define loadcls(o) (loadszcls[(o) - Oloads1] >> 4)
#define load2ext(o) (load2ext[(o) - Oloads1])
#define storesz(o) (1 << ((o) - Ostore1))

void
mem2reg(struct function *fn)
{
   extern int ninstr;
   struct uses *uses = ssauses(fn);
   struct block *blk = fn->entry;

   do {
      for (int i = 0; i < blk->ins.n; ++i) {
         struct use *use, *uend;
         union ref var;
         enum irclass k = 0;
         int sz = 0, ndef = 0;
         enum op ext;
         int t = blk->ins.p[i];
         struct instr *ins = &instrtab[t];
         
         if (!oisalloca(ins->op)) continue;
         for (use = uses[t].use, uend = &uses[t].use[uses[t].nuse]; use < uend; ++use) {
            struct instr *m;
            
            if (use->isjmp) goto Next;
            m = &instrtab[use->ins];
            if (oisload(m->op) && (!sz || sz == loadsz(m->op))) {
               sz = loadsz(m->op);
               k = loadcls(m->op);
               if (sz < 4) ext = load2ext(m->op);
               continue;
            }
            if (oisstore(m->op) && m->l.bits == mkref(RTMP, t).bits
            && (!sz || sz == storesz(m->op))) {
               sz = storesz(m->op);
               ++ndef;
               continue;
            }
            goto Next;
         }
         if (!ndef) /* slot is read from but never written to */
            goto Next;

         if (ndef>1) /* TODO phi construction */
            goto Next;

         /* remove alloca */
         *ins = mkinstr(Onop, 0,);
         
         var.t = 0;
         for (use = uses[t].use; use < uend; ++use) {
            struct instr *m = &instrtab[use->ins];

            if (oisstore(m->op)) {
               if (sz < 4) {
                  *m = mkinstr(ext, KI4, m->r);
                  var = mkref(RTMP, use->ins);
               } else {
                  var = m->r;
                  *m = mkinstr(Onop,0,);
               }
            } else if (oisload(m->op)) {
               if (!var.t) /* use before def */
                  goto Next;
               *m = mkinstr(Ocopy, k, var);
            }
         }
         
      Next:;
      }
   } while ((blk = blk->lnext) != fn->entry);

   freeuses(uses, ninstr);
   if (ccopt.dbg.m) {
      efmt("<< After mem2reg >>\n");
      irdump(fn);
   }
}


/* vim:set ts=3 sw=3 expandtab: */
