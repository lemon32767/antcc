#include "ir.h"
#include "obj.h"
#include "u_endian.h"

static int nextdat;

static WriteBuf *out = &bstdout;

static bool
prilitdat(const IRDat *dat, const char *prefix)
{
   uchar *p;
   switch (dat->section) {
      default: assert(0);
      case Sdata: p = objout.data.p + dat->off; break;
      case Srodata: p = objout.rodata.p + dat->off; break;
      case Stext: p = objout.textbegin + dat->off; break;
   }
   if (dat->ctype.t == TYARRAY && typechild(dat->ctype).t == TYCHAR && dat->siz-1 < 60 && p[dat->siz-1] == 0) {
      bfmt(out, "%s%'S", prefix, p, dat->siz-1);
   } else if (dat->ctype.t == TYFLOAT) {
      bfmt(out, "%s%f", prefix, rdf32targ(p));
   } else if (dat->ctype.t == TYDOUBLE) {
      bfmt(out, "%s%f", prefix, rdf64targ(p));
   } else if (dat->ctype.t == TYVLONG) {
      bfmt(out, "%s0x%lx", prefix, rd64targ(p));
   } else return 0;
   return 1;
}

static void
pridat(const IRDat *dat)
{
   static const char *snames[] = { [Sdata] = ".data", [Srodata] = ".rodata", [Stext] = ".text" };
   uchar *p;
   switch (dat->section) {
      default: assert(0);
      case Sdata: p = objout.data.p + dat->off; break;
      case Srodata: p = objout.rodata.p + dat->off; break;
      case Stext: p = objout.textbegin + dat->off; break;
   }
   enum {
      MINZERO = 4,
      MAXLINE = 60,
   };
   int npri = 0;
   int strbegin = 0, nstr = 0;
   bfmt(out, "%s %ty %y(align %d, size %d):\n\t", snames[dat->section], dat->ctype, dat->name, dat->align, dat->siz);
   if (!prilitdat(dat, "lit: ")) {
      for (int i = 0; i < dat->siz; ++i) {
         int c = p[i];
         if (npri > MAXLINE) {
            npri = 0;
            bfmt(out, "\n\t");
         }
         if (aisprint(c)) {
            if (!nstr++) strbegin = i;
         } else {
            if (nstr) {
               npri += bfmt(out, "asc %'S,", p+strbegin, nstr);
               nstr = 0;
               bfmt(out, "b ");
            }
            npri += bfmt(out, "%d,", c);
         }
      }
      if (nstr) npri += bfmt(out, "asc %'S,", p+strbegin, nstr);
   }
   bfmt(out, "\n");
}

static const char *clsname[] = {
   "?", "i32", "i64", "ptr", "f32", "f64"
};

static void
prityp(IRType typ)
{
   if (!typ.isagg)
      bfmt(out, clsname[typ.cls]);
   else {
      const TypeData *td = &typedata[typ.dat];
      const char *tag = td->t == TYSTRUCT ? "struct" : "union";
      if (tagtypetags[td->id])
         bfmt(out, "%s.%s.%d", tag, tagtypetags[td->id], td->id);
      else
         bfmt(out, "%s.%d", tag, td->id);
   }
}

static const char *intrinname[] = {
   "?\??",
#define _(b,...) #b,
#include "ir_intrin.def"
#undef _
};

static void
dumpref(enum op o, Ref ref)
{
   IRCon *con;
   switch (ref.t) {
   case RXXX:
      if (ref.bits == UNDREF.bits)
         bfmt(out, "undef");
      else
         bfmt(out, "??");
      break;
   case RTMP:
      bfmt(out, "%%%d", ref.i);
      if (instrtab[ref.i].reg)
         bfmt(out, "(%s)", mctarg->rnames[instrtab[ref.i].reg-1]);
      break;
   case RREG:
      bfmt(out, "%s", mctarg->rnames[ref.i]);
      break;
   case RICON:
      if (o == Ointrin) bfmt(out, "\"%s\"", intrinname[ref.i]);
      else bfmt(out, "%d", ref.i);
      break;
   case RXCON:
      con = &contab.p[ref.i];
      if (con->deref) bfmt(out, "*[");
      if (con->issym || con->isdat) {
         bfmt(out, "$%y", xcon2sym(ref.i));
         if (con->isdat) {
            IRDat *dat = &dattab.p[con->dat];
            if (prilitdat(dat, " (= ")) {
               if (isscalar(dat->ctype)) {
                  WriteBuf tmp = MEMBUF((char [1]){0}, 1);
                  bfmt(&tmp, "%ty", dat->ctype);
                  ioputc(out, *tmp.buf);
               }
               ioputc(out, ')');
            }
         }
      } else switch (con->cls) {
      case KI32: bfmt(out, "%d", (int)con->i); break;
      case KI64: bfmt(out, "%ld", con->i); break;
      case KPTR: bfmt(out, "%'lx", con->i); break;
      case KF32: bfmt(out, "%fs", con->f); break;
      case KF64: bfmt(out, "%fd", con->f); break;
      default: assert(0);
      }
      if (con->deref) bfmt(out, "]");
      break;
   case RTYPE:
      prityp(ref2type(ref));
      break;
   case RADDR:
      {
         const IRAddr *addr = &addrtab.p[ref.i];
         bool k = 0;
         bfmt(out, "addr [");
         if ((k = addr->base.bits)) dumpref(0, addr->base);
         if (addr->index.bits) {
            if (k) bfmt(out, " + ");
            dumpref(0, addr->index);
            if (addr->shift)
               bfmt(out, " * %d", 1<<addr->shift);
            k = 1;
         }
         if (k && addr->disp) {
            bfmt(out, " %c %d", "-+"[addr->disp > 0], addr->disp < 0 ? -addr->disp : addr->disp);
         }
         assert(k);
         bfmt(out, "]");
      }
      break;
   case RSTACK:
      bfmt(out, "[stack %d]", ref.i);
      break;
   default: assert(!"ref");
   }
}

static void
dumpcall(IRCall *call)
{
   if (call->ret.isagg) {
      bfmt(out, "sret ");
      prityp(call->ret);
      bfmt(out, ", ");
   }
   if (call->vararg < 0) {
      bfmt(out, "#%d", call->narg);
   } else {
      assert(call->vararg <= call->narg);
      bfmt(out, "#%d, ... #%d", call->vararg, call->narg - call->vararg);
   }
}

static void
dumpinst(const Instr *ins)
{
   int i;
   if (ins->op == Omove) {
      bfmt(out, "move %s ", clsname[ins->cls]);
   } else {
      enum irclass cls = insrescls(*ins);
      if (ins->reg) {
         if (cls)
            bfmt(out, "%s ", clsname[cls]);
         bfmt(out, "(%%%d)%s = ", ins - instrtab, mctarg->rnames[ins->reg - 1]);
      } else if (cls) {
         bfmt(out, "%s %%%d", clsname[cls], ins - instrtab);
         bfmt(out, " = ");
      }
      bfmt(out, "%s ", opnames[ins->op]);
      if (oiscmp(ins->op))
         bfmt(out, "%s ", clsname[ins->cls]);
   }
   for (i = 0; i < opnarg[ins->op]; ++i) {
      if (i) bfmt(out, ", ");
      if (i == 1 && (ins->op == Ocall || ins->op == Ointrin)) {
         dumpcall(&calltab.p[ins->r.i]);
      } else {
         dumpref(ins->op, (&ins->l)[i]);
      }
   }
   if (oisalloca(ins->op) && ins->l.t == RICON) {
      bfmt(out, " \t; %d bytes", ins->l.i << (ins->op - Oalloca1));
   }
   bfmt(out, "\n");
}

void
dumpblk(Function *fn, Block *blk)
{
   static const char *jnames[] = { 0, "b", "ret", "trap" };
   int i;
   bfmt(out, "  @%d:", blk->id);
   if (blk->npred) {
      bfmt(out, " \t; preds:");
      for (i = 0; i < blk->npred; ++i) {
         if (i) ioputc(out, ',');
         bfmt(out, " @%d", blkpred(blk, i)->id);
      }
   }
   if (fn->prop & FNDOM && blk->idom)
      bfmt(out, "\t; idom: @%d", blk->idom->id);
   if (blk->loop)
      bfmt(out, "\t; loop depth: %d", blk->loop);
   ioputc(out, '\n');
   for (i = 0; i < blk->phi.n; ++i) {
      Instr *phi = &instrtab[blk->phi.p[i]];
      Ref *refs = phitab.p[phi->l.i];
      if (i == 0) bfmt(out, "%-4d", blk->inumstart);
      else bfmt(out, " |> ");
      bfmt(out, "  %s ", clsname[phi->cls]);
      if (!phi->reg) bfmt(out, "%%%d = %s ", blk->phi.p[i], opnames[phi->op]);
      else bfmt(out, "(%%%d)%s = %s ", phi - instrtab, mctarg->rnames[phi->reg-1], opnames[phi->op]);
      for (int i = 0; i < blk->npred; ++i) {
         if (i) bfmt(out, ", ");
         bfmt(out, "@%d ", blkpred(blk, i)->id);
         dumpref(0, refs[i]);
      }
      ioputc(out, '\n');
   }
   for (i = 0; i < blk->ins.n; ++i) {
      bfmt(out, "%-4d  ", blk->inumstart + 1 + i);
      dumpinst(&instrtab[blk->ins.p[i]]);
   }
   bfmt(out, "%-4d  %s ", blk->inumstart + 1 + i, jnames[blk->jmp.t]);
   if (blk->jmp.t == Jret && blk->jmp.arg[0].bits && !fn->nabiret && isagg(fn->retty)) {
      /* un-lowered struct return */
      dumpref(0, mktyperef(mkirtype(fn->retty)));
      bfmt(out, " ");
   }
   for (i = 0; i < 2; ++i) {
      if (!blk->jmp.arg[i].bits) break;
      if (i > 0) bfmt(out, ", ");
      dumpref(0, blk->jmp.arg[i]);
   }
   if (i && blk->s1) bfmt(out, ", ");
   if (blk->s1 && blk->s2) bfmt(out, "@%d, @%d", blk->s1->id, blk->s2->id);
   else if (blk->s1) bfmt(out, "@%d", blk->s1->id);
   bfmt(out, "\n");
}

void
irdump(Function *fn)
{
   Block *blk;

   /* print datas that have never been printed before */
   while (nextdat < dattab.n) pridat(&dattab.p[nextdat++]);

   bfmt(out, "function %s : %ty\n", fn->name, fn->fnty);
   if (fn->abiarg || fn->nabiret) {
      bfmt(out, "abi: (");
      for (int i = 0; i < fn->nabiarg; ++i) {
         if (i > 0) bfmt(out, ", ");
         if (!fn->abiarg[i].isstk) {
            bfmt(out, "%s", mctarg->rnames[fn->abiarg[i].reg]);
         } else {
            prityp(fn->abiarg[i].ty);
            bfmt(out, " <stk>");
         }
      }
      bfmt(out, ")");
      if (fn->retty.t != TYVOID) {
         bfmt(out, " -> %s", mctarg->rnames[fn->abiret[0].reg]);
         if (fn->nabiret > 1)
            bfmt(out, ", %s", mctarg->rnames[fn->abiret[1].reg]);
      }
      bfmt(out, "\n");
   }
   numberinstrs(fn);
   blk = fn->entry;
   do {
      assert(blk->lprev->lnext == blk);
      dumpblk(fn, blk);
      assert(blk->lnext != NULL);
   } while ((blk = blk->lnext) != fn->entry);
   bfmt(out, "\n");
}

/* vim:set ts=3 sw=3 expandtab: */
