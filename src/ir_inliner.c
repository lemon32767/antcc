#include "ir.h"

typedef struct SavedFunc {
   uint ninstr;
   Instr *instrtab;
   IRCon *contab;
   IRCall *calltab;
   Ref **phitab;
   Block *entry;
   Type fnty, retty;
   ABIArg *abiarg, abiret[2];
   ushort nabiarg, nabiret;
   ushort nretpoints;
} SavedFunc;

enum { MAX_INLINED_FN_NINS = 50,
       MAX_INLINED_FN_NBLK = 16, };

static pmap_of(SavedFunc *) savedfns;

bool
maybeinlinee(Function *fn)
{
   static Arena *savearena;
   extern int ninstr;

   // TODO better heuristics
   if (ccopt.o < OPT1) return 0;
   if (!fn->inlin && ccopt.o < OPT2) return 0;
   if (ninstr > MAX_INLINED_FN_NINS) return 0;
   if (fn->nblk > MAX_INLINED_FN_NBLK) return 0;
   for (int i = 0; i < fn->nabiarg; ++i) {
      /* TODO inlining functions with stack args */
      if (fn->abiarg[i].isstk) return 0;
   }
   if (fn->nabiret > 1) return 0; /* TODO 2reg scalar return */

   if (!savearena) {
      enum { N = 1<<12 };
      static union { char m[sizeof(Arena) + N]; Arena *_align; } amem;
      savearena = (void *)amem.m;
      savearena->cap = N;
   }

   if (ccopt.dbg.y) {
      bfmt(ccopt.dbgout, "> stashing '%s' for inlining\n", fn->name);
   }
   SavedFunc *sv = allocz(&savearena, sizeof *sv, 0);
   sv->fnty = fn->fnty, sv->retty = fn->retty;
   if (fn->abiarg)
      sv->abiarg = alloccopy(&savearena, fn->abiarg, sizeof *sv->abiarg * fn->nabiarg, 0);
   sv->nabiarg = fn->nabiarg;
   if ((sv->nabiret = fn->nabiret) > 0)
      memcpy(sv->abiret, fn->abiret, sizeof sv->abiret);
   Block *bmap[MAX_INLINED_FN_NBLK];
   Block *b = fn->entry;
   int id = 0;
   do {
      b->id = id++;
      Block *q = alloccopy(&savearena, b, sizeof *b, 0);
      if (q->phi.n)
         q->phi.p = alloccopy(&savearena, q->phi.p, sizeof *q->phi.p * q->phi.n, 0);
      if (q->ins.n)
         q->ins.p = alloccopy(&savearena, q->ins.p, sizeof *q->ins.p * q->ins.n, 0);
      if (q->npred > 1)
         q->_pred = alloccopy(&savearena, q->_pred, sizeof *q->_pred * q->npred, 0);
      q->lprev = NULL;
      q->idom = NULL;
      bmap[b->id] = q;
      sv->nretpoints += b->jmp.t == Jret;
   } while ((b = b->lnext) != fn->entry);
   b = sv->entry = bmap[0];
   do {
      if (b->s1) b->s1 = bmap[b->s1->id];
      if (b->s2) b->s2 = bmap[b->s2->id];
      if (b->npred == 1)
         b->_pred0 = bmap[b->_pred0->id];
      else for (int i = 0; i < b->npred; ++i)
         b->_pred[i] = bmap[b->_pred[i]->id];
      b->lnext = b->lnext == fn->entry ? NULL : bmap[b->lnext->id];
   } while ((b = b->lnext));
   
   sv->instrtab = alloccopy(&savearena, instrtab, sizeof *instrtab * (sv->ninstr = ninstr), 0);
   sv->contab = alloccopy(&savearena, contab.p, sizeof *contab.p * contab.n, 0);
   if (calltab.n) {
      sv->calltab = alloccopy(&savearena, calltab.p, sizeof *calltab.p * calltab.n, 0);
      for (int i = 0; i < calltab.n; ++i) {
         if (sv->calltab[i].abiarg)
            sv->calltab[i].abiarg = alloccopy(&savearena, sv->calltab[i].abiarg,
                  sv->calltab[i].narg * sizeof *sv->calltab[i].abiarg, 0);
      }
   }
   if (phitab.n) {
      sv->phitab = alloc(&savearena, sizeof *phitab.p * phitab.n, 0);
      for (int i = 0; i < phitab.n; ++i) {
         sv->phitab[i] = alloccopy(&savearena, phitab.p[i], sizeof *phitab.p[i] * xbcap(phitab.p[i]), 0);
      }
   }
   pmap_set(&savedfns, fn->name, sv);
   return 1;
}

static Ref
mapref(short *instrmap, SavedFunc *sv, Ref r)
{
   assert(r.bits);
   if (r.t == RTMP) return r.i = instrmap[r.i], r;
   if (r.t == RXCON) return newxcon(&sv->contab[r.i]);
   assert(r.t != RADDR);
   assert(r.t != RSTACK);
   return r;
}

static Block *
inlcall(Function *fn, Block *blk, int curi, SavedFunc *sv)
{
   int res = blk->ins.p[curi], res2;
   Instr *ins = &instrtab[res];
   IRCall *call = &calltab.p[ins->r.i];
   Ref retvals[64];
   Ref args[64];
   assert(sv->nabiret < 2 && sv->nretpoints < countof(retvals));
   for (int n = call->narg, i = curi-1; n > 0; --i) {
      assert(i >= 0);
      Instr *ins = &instrtab[blk->ins.p[i]];
      if (ins->op == Oarg) {
         args[--n] = ins->r;
         *ins = mkinstr(Onop,0,);
      }
   }
   if (call->abiret[1].ty.bits) {
      assert(curi+1 < blk->ins.n);
      res2 = blk->ins.p[curi+1];
      assert(instrtab[res2].op == Ocall2r);
   }
   Block *exit = blksplitafter(fn, blk, curi-1);
   if (!ins->cls) {
      *ins = mkinstr(Onop,0,);
   } else {
      if (sv->nretpoints == 1) {
         *ins = mkinstr(Ocopy, ins->cls, );
      } else {
         /* turn into a phi */
         *ins = mkinstr(Ophi, ins->cls, );
         exit->ins.p[0] = newinstr(blk, mkinstr(Onop,0,));
         vpush(&exit->phi, res);
      }
   }

   Block *bmap[MAX_INLINED_FN_NBLK];
   short instrmap[MAX_INLINED_FN_NINS];
   for (Block *b = sv->entry; b; b = b->lnext) {
      bmap[b->id] = newblk(fn);
   }
   for (int i = 0; i < sv->ninstr; ++i) {
      /* TODO don't wastefully allocate for tombstone instructions
       * - mark instructions some way when they are freed (put in instrfreelist)
       * */
      int allocinstr(void);
      instrmap[i] = allocinstr();
   }

   exit->npred = 0;
   exit->_pred0 = NULL;
   blk->s1 = bmap[0];
   int iret = 0;
   for (Block *b = sv->entry, *prev = blk, *new; b; prev = new, b = b->lnext) {
      new = bmap[b->id];
      new->id = fn->nblk++;
      new->lprev = prev;
      prev->lnext = new;
      new->lnext = exit;
      exit->lprev = new;
      if (b->npred == 1) new->_pred0 = bmap[b->_pred0->id];
      else if (b->npred > 0) {
         xbgrow(&new->_pred, b->npred);
         for (int i = 0; i < b->npred; ++i)
            new->_pred[i] = bmap[b->_pred[i]->id];
      }
      new->npred = b->npred;
      vresize(&new->phi, b->phi.n);
      for (int i = 0; i < b->phi.n; ++i) {
         int t = b->phi.p[i];
         Ref *refs = NULL,
                   *src = sv->phitab[sv->instrtab[t].l.i];
         xbgrow(&refs, b->npred);
         for (int i = 0; i < b->npred; ++i)
            refs[i] = mapref(instrmap, sv, src[i]);
         vpush(&phitab, refs);
         instrtab[instrmap[t]] = mkinstr(Ophi, sv->instrtab[t].cls, .l.i = phitab.n-1);
         new->phi.p[i] = instrmap[t];
      }
      vresize(&new->ins, b->ins.n);
      for (int i = 0; i < b->ins.n; ++i) {
         int t = b->ins.p[i];
         Instr *ins = &instrtab[instrmap[t]];
         *ins = sv->instrtab[t];
         if (ins->op == Oparam) {
            ins->op = Ocopy;
            assert(ins->l.t == RICON && (uint) ins->l.i < call->narg);
            ins->l = args[ins->l.i];
         } else if (ins->op == Ocall || ins->op == Ointrin) {
            ins->l = mapref(instrmap, sv, ins->l);
            for (Instr *ins2;
                 ins->l.t == RTMP && (ins2 = &instrtab[ins->l.i])->op == Ocopy
                  && (isaddrcon(ins2->l,0) || (ins2->l.t == RTMP && instrtab[ins->l.i].cls == KPTR));) {
               /* for an indirect function call, eagerly copy-propagate the callee. this allows
                * subsequent inlining of function pointers statically known after the inlining */
               ins->l = ins2->l;
            }
            vpush(&calltab, sv->calltab[ins->r.i]);
            ins->r.i = calltab.n-1;
         } else {
            if (ins->l.t) ins->l = mapref(instrmap, sv, ins->l);
            if (ins->r.t) ins->r = mapref(instrmap, sv, ins->r);
         }
         new->ins.p[i] = instrmap[t];
      }
      if (b->jmp.t == Jret) {
         new->jmp.t = Jb;
         new->s1 = exit;
         retvals[iret++] = b->jmp.arg[0].bits ? mapref(instrmap, sv, b->jmp.arg[0]) : UNDREF;
         addpred(exit, new);
      } else {
         new->jmp.t = b->jmp.t;
         for (int i = 0; i < 2; ++i) {
            if (!b->jmp.arg[i].bits) break;
            new->jmp.arg[i] = mapref(instrmap, sv, b->jmp.arg[i]);
         }
      }
      if (b->s1) {
         new->s1 = bmap[b->s1->id];
         if (b->s2) new->s2 = bmap[b->s2->id];
      }
   }
   exit->id = fn->nblk;
   fn->prop &= ~FNUSE;
   if (ins->cls && sv->nretpoints > 0) {
      assert(sv->nretpoints == exit->npred);
      if (sv->nretpoints == 1) {
         /* fill copy */
         ins->l = retvals[0];
      } else {
         /* fill phi */
         Ref *refs = NULL;
         xbgrow(&refs, sv->nretpoints);
         memcpy(refs, retvals, sizeof *refs * sv->nretpoints);
         vpush(&phitab, refs);
         ins->l = mkref(RXXX, phitab.n-1);
      }
   }
   bmap[0]->_pred0 = blk;
   bmap[0]->npred = 1;
   return exit;
}

enum { MAX_REC_INLINE = 16 };
void
doinline(Function *fn)
{
   if (calltab.n == 0) return;
   Block *b = fn->entry;
   struct Stack { /* stack of callees being inline expanded */
      Block *b; /* block after the end of expansion */
      SavedFunc *sv;
   } stkbuf[MAX_REC_INLINE], *stk = stkbuf + MAX_REC_INLINE, *stkend = stk;
   bool dumpbefore = 0;
   do {
      if (stk != stkend && b == stk->b)
         ++stk; /* pop */
      else if (stk == stkbuf) /* stack full? */
         continue;
      for (int i = 0; i < b->ins.n; ++i) {
         Instr *ins = &instrtab[b->ins.p[i]];
         if (ins->op != Ocall) continue;
         if (!isaddrcon(ins->l,0)) continue;
         IRCall *call = &calltab.p[ins->r.i];
         internstr fname = xcon2sym(ins->l.i);
         SavedFunc **pcallee, *sv;
         if ((pcallee = pmap_get(&savedfns, fname))
           && (sv = *pcallee)->nabiarg == call->narg && call->vararg == -1
           && call->narg == sv->nabiarg
           && (!call->narg || !memcmp(sv->abiarg, call->abiarg, sizeof *sv->abiarg * sv->nabiarg))
           && !memcmp(sv->abiret, call->abiret, sizeof sv->abiret)) {
            for (struct Stack *s = stk; s != stkend; ++s) {
               if (s->sv == sv) goto Skip; /* recursion encountered */
            }
            if (ccopt.dbg.y) {
               if (!dumpbefore) {
                  bfmt(ccopt.dbgout, "<< Before inlining >>\n");
                  irdump(fn);
                  dumpbefore = 1;
               }
            }

            (--stk)->b = inlcall(fn, b, i, *pcallee);
            stk->sv = sv;
            if (ccopt.dbg.y) {
               bfmt(ccopt.dbgout, "<< After inlining '%s' (@%d-@%d) >>\n", fname, b->lnext->id, stk->b->id);
               irdump(fn);
            }
            break;
         }
      Skip:;
      }
   } while ((b = b->lnext) != fn->entry);
}

/* vim:set ts=3 sw=3 expandtab: */
