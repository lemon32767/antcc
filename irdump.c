#include "ir.h"
#include "obj.h"

static int nextdat;

static void
pridat(const struct irdat *dat)
{
   uchar *p = (dat->section == Sdata ? objout.data.p : objout.rodata.p) + dat->off; 
   enum {
      MINZERO = 4,
      MAXLINE = 60,
   };
   int npri = 0;
   int strbegin = 0, nstr = 0;
   assert(dat->section == Sdata || dat->section == Srodata);
   efmt("%s %y(align %d, size %d):\n\t", dat->section == Sdata ? "data" : "rodata", dat->name, dat->align, dat->siz);
   for (int i = 0; i < dat->siz; ++i) {
      int c = p[i];
      if (npri > MAXLINE) {
         npri = 0;
         efmt("\n\t");
      }
      if (aisprint(c)) {
         if (!nstr++) strbegin = i;
      } else {
         if (nstr) {
            npri += efmt("asc %'S,", p+strbegin, nstr);
            nstr = 0;
            efmt("b ");
         }
         npri += efmt("%d,", c);
      }
   }
   if (nstr) npri += efmt("asc %'S,", p+strbegin, nstr);
   efmt("\n");
}

static const char *clsname[] = {
   "?", "i4", "i8", "ptr", "f4", "f8"
};

static void
prityp(union irtype typ)
{
   if (!typ.isagg)
      efmt(clsname[typ.cls]);
   else {
      const struct typedata *td = &typedata[typ.dat];
      const char *tag = td->t == TYSTRUCT ? "struct" : "union";
      if (ttypenames[td->id])
         efmt("%s.%s.%d", tag, ttypenames[td->id], td->id);
      else
         efmt("%s.%d", tag, td->id);
   }
}

static const char *intrinname[] = {
   "?\??",
#define _(b,...) #b,
#include "intrin.def"
#undef _
};

static void
dumpref(enum op o, union ref ref)
{
   struct xcon *con;
   switch (ref.t) {
   case RXXX:
      if (ref.bits == UNDREF.bits)
         efmt("undef");
      else
         efmt("??");
      break;
   case RTMP:
      efmt("%%%d", ref.i);
      if (instrtab[ref.i].reg)
         efmt("(%s)", mctarg->rnames[instrtab[ref.i].reg-1]);
      break;
   case RREG:
      efmt("%s", mctarg->rnames[ref.i]);
      break;
   case RICON:
      if (o == Ointrin) efmt("\"%s\"", intrinname[ref.i]);
      else efmt("%d", ref.i);
      break;
   case RXCON:
      con = &conht[ref.i];
      if (con->deref) efmt("*[");
      if (con->issym || con->isdat) efmt("$%y", xcon2sym(ref.i));
      else switch (con->cls) {
      case KI4: efmt("%d", (int)con->i); break;
      case KI8: efmt("%ld", con->i); break;
      case KPTR: efmt("%'lx", con->i); break;
      case KF4: efmt("%fs", con->f); break;
      case KF8: efmt("%fd", con->f); break;
      default: assert(0);
      }
      if (con->deref) efmt("]");
      break;
   case RTYPE:
      prityp(ref2type(ref));
      break;
   case RADDR:
      {
         const struct addr *addr = &addrht[ref.i];
         bool k = 0;
         efmt("addr [");
         if ((k = addr->base.bits)) dumpref(0, addr->base);
         if (addr->index.bits) {
            if (k) efmt(" + ");
            dumpref(0, addr->index);
            if (addr->shift)
               efmt(" * %d", 1<<addr->shift);
            k = 1;
         }
         if (k && addr->disp) {
            efmt(" %c %d", "-+"[addr->disp > 0], addr->disp < 0 ? -addr->disp : addr->disp);
         }
         assert(k);
         efmt("]");
      }
      break;
   default: assert(!"ref");
   }
}

static void
dumpcall(struct call *call)
{
   if (call->ret.isagg) {
      efmt("sret ");
      prityp(call->ret);
      efmt(", ");
   }
   if (call->vararg < 0) {
      efmt("#%d", call->narg);
   } else {
      assert(call->vararg <= call->narg);
      efmt("#%d, ... #%d", call->vararg, call->narg - call->vararg);
   }
}

static const uchar opnarg[] = {
   0,
#define _(o,n) n,
#include "op.def"
#undef _
};

static void
dumpinst(const struct instr *ins)
{
   int i;
   efmt("    ");
   if (ins->op == Omove) {
      efmt("move %s ", clsname[ins->cls]);
   } else {
      enum irclass cls = insrescls(*ins);
      if (ins->reg) {
         if (cls)
            efmt("%s ", clsname[cls]);
         efmt("(%%%d)%s = ", ins - instrtab, mctarg->rnames[ins->reg - 1]);
      } else if (cls) {
         efmt("%s %%%d", clsname[cls], ins - instrtab);
         efmt(" = ");
      }
      efmt("%s ", opnames[ins->op]);
      if (oiscmp(ins->op))
         efmt("%s ", clsname[ins->cls]);
   }
   for (i = 0; i < opnarg[ins->op]; ++i) {
      if (i) efmt(", ");
      if (i == 1 && (ins->op == Ocall || ins->op == Ointrin)) {
         dumpcall(&calltab.p[ins->r.i]);
      } else {
         dumpref(ins->op, (&ins->l)[i]);
      }
   }
   efmt("\n");
}

static void
dumpblk(struct function *fn, struct block *blk)
{
   static const char *jnames[] = { 0, "b", "ret" };
   int i;
   efmt("  @%d:\n", blk->id);
   for (i = 0; i < blk->phi.n; ++i) {
      struct instr *phi = &instrtab[blk->phi.p[i]];
      union ref *refs = phitab.p[phi->l.i];
      efmt("    %s ", clsname[phi->cls]);
      if (!phi->reg) efmt("%%%d = %s ", blk->phi.p[i], opnames[phi->op]);
      else efmt("(%%%d)%s = %s ", phi - instrtab, mctarg->rnames[phi->reg-1], opnames[phi->op]);
      for (int i = 0; i < blk->npred; ++i) {
         if (i) efmt(", ");
         efmt("@%d ", blkpred(blk, i)->id);
         dumpref(0, refs[i]);
      }
      efmt("\n");
   }
   for (i = 0; i < blk->ins.n; ++i) {
      dumpinst(&instrtab[blk->ins.p[i]]);
   }
   efmt("    %s ", jnames[blk->jmp.t]);
   if (blk->jmp.arg[0].bits && !fn->nabiret && isagg(fn->retty)) {
      /* un-lowered struct return */
      dumpref(0, mktyperef(mkirtype(fn->retty)));
      efmt(" ");
   }
   for (i = 0; i < 2; ++i) {
      if (!blk->jmp.arg[i].bits) break;
      if (i > 0) efmt(", ");
      dumpref(0, blk->jmp.arg[i]);
   }
   if (i && blk->s1) efmt(", ");
   if (blk->s1 && blk->s2) efmt("@%d, @%d", blk->s1->id, blk->s2->id);
   else if (blk->s1) efmt("@%d", blk->s1->id);
   efmt("\n");
}

void
irdump(struct function *fn)
{
   struct block *blk;

   /* print datas that have never been printed before */
   while (nextdat < dattab.n) pridat(&dattab.p[nextdat++]);

   efmt("function %s : %ty\n", fn->name, fn->fnty);
   if (fn->abiarg || fn->nabiret) {
      efmt("abi: (");
      for (int i = 0; i < fn->nabiarg; ++i) {
         if (i > 0) efmt(", ");
         if (fn->abiarg[i].reg >= 0) {
            efmt("%s", mctarg->rnames[fn->abiarg[i].reg]);
         } else {
            prityp(fn->abiarg[i].ty);
            efmt(" <stk>");
         }
      }
      efmt(")");
      if (fn->retty.t != TYVOID) {
         efmt(" -> %s", mctarg->rnames[fn->abiret[0].reg]);
         if (fn->nabiret > 1)
            efmt(", %s", mctarg->rnames[fn->abiret[1].reg]);
      }
      efmt("\n");
   }
   blk = fn->entry;
   do {
      dumpblk(fn, blk);
   } while ((blk = blk->lnext) != fn->entry);
   efmt("\n");
}

/* vim:set ts=3 sw=3 expandtab: */
