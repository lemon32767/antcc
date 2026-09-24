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

const char *clsname[] = {
   "?", "i32", "i64", "ptr", "f32", "f64"
};

static int
prityp(WriteBuf *buf, IRType typ)
{
   if (!typ.isagg)
      return bfmt(buf, clsname[typ.cls]);
   else {
      const TypeData *td = &typedata[typ.dat];
      const char *tag = td->t == TYSTRUCT ? "struct" : "union";
      if (tagtypetags[td->id])
         return bfmt(buf, "%s.%s.%d", tag, tagtypetags[td->id], td->id);
      else
         return bfmt(buf, "%s.%d", tag, td->id);
   }
}

static const char *intrinname[] = {
   "?\??",
#define _(b,...) #b,
#include "ir_intrin.def"
#undef _
};

int
dumpref(WriteBuf *buf, enum op o, Ref ref)
{
   IRCon *con;
   int n = 0;
   switch (ref.t) {
   case RXXX:
      if (ref.bits == UNDREF.bits)
         return bfmt(buf, "undef");
      else
         return bfmt(buf, "??");
   case RTMP:
      n = bfmt(buf, "%%%d", ref.i);
      if (instrtab[ref.i].reg)
         n += bfmt(buf, "(%s)", mctarg->rnames[instrtab[ref.i].reg-1]);
      break;
   case RREG:
      n = bfmt(buf, "%s", mctarg->rnames[ref.i]);
      break;
   case RICON:
      if (o == Ointrin)
         n = bfmt(buf, "\"%s\"", intrinname[ref.i]);
      else
         n = bfmt(buf, "%d", ref.i);
      break;
   case RXCON:
      con = &contab.p[ref.i];
      if (con->deref) n += bfmt(buf, "*[");
      if (con->issym || con->isdat) {
         n += bfmt(buf, "$%y", xcon2sym(ref.i));
         if (con->isdat) {
            IRDat *dat = &dattab.p[con->dat];
            if (prilitdat(dat, " (= ")) {
               if (isscalar(dat->ctype)) {
                  WriteBuf tmp = MEMBUF((char [1]){0}, 1);
                  bfmt(&tmp, "%ty", dat->ctype);
                  ioputc(buf, *tmp.buf), ++n;
               }
               ioputc(buf, ')'), ++n;
            }
         }
      } else switch (con->cls) {
      case KI32: n += bfmt(buf, "%d", (int)con->i); break;
      case KI64: n += bfmt(buf, "%ld", con->i); break;
      case KPTR: n += bfmt(buf, "%'lx", con->i); break;
      case KF32: n += bfmt(buf, "%fs", con->f); break;
      case KF64: n += bfmt(buf, "%fd", con->f); break;
      default: assert(0);
      }
      if (con->deref) n += bfmt(buf, "]");
      break;
   case RTYPE:
      return prityp(buf, ref2type(ref));
   case RADDR:
      {
         const IRAddr *addr = &addrtab.p[ref.i];
         bool k = 0;
         n += bfmt(buf, "addr [");
         if ((k = addr->base.bits)) n += dumpref(buf, 0, addr->base);
         if (addr->index.bits) {
            if (k) n += bfmt(buf, " + %r", addr->index);
            if (addr->shift)
               n += bfmt(buf, " * %d", 1<<addr->shift);
            k = 1;
         }
         if (k && addr->disp) {
            n += bfmt(buf, " %c %d", "-+"[addr->disp > 0], addr->disp < 0 ? -addr->disp : addr->disp);
         }
         assert(k);
         n += bfmt(buf, "]");
      }
      break;
   case RSTACK:
      return bfmt(buf, "stack(%d)", ref.i);
   default: assert(!"ref");
   }
   return n;
}

static void
dumpref1(enum op o, Ref ref)
{
   dumpref(out, o, ref);
}

static void
dumpcall(WriteBuf *buf, IRCall *call)
{
   if (call->ret.isagg) {
      bfmt(buf, "sret ");
      prityp(buf, call->ret);
      bfmt(buf, ", ");
   }
   if (call->vararg < 0) {
      bfmt(buf, "#%d", call->narg);
   } else {
      assert(call->vararg <= call->narg);
      bfmt(buf, "#%d, ... #%d", call->vararg, call->narg - call->vararg);
   }
}

void
dumpinstr(WriteBuf *buf, const Instr *ins)
{
   int i;
   if (ins->op == Omove) {
      bfmt(buf, "move %s ", clsname[ins->cls]);
   } else {
      enum irclass cls = insrescls(*ins);
      if (ins->reg) {
         if (cls)
            bfmt(buf, "%s ", clsname[cls]);
         bfmt(buf, "(%%%d)%s = ", ins - instrtab, mctarg->rnames[ins->reg - 1]);
      } else if (cls) {
         bfmt(buf, "%s %%%d", clsname[cls], ins - instrtab);
         bfmt(buf, " = ");
      }
      bfmt(buf, "%s ", opnames[ins->op]);
      if (oiscmp(ins->op))
         bfmt(buf, "%s ", clsname[ins->cls]);
   }
   for (i = 0; i < opnoper[ins->op]; ++i) {
      if (i) bfmt(buf, ", ");
      if (i == 1 && (ins->op == Ocall || ins->op == Ointrin)) {
         dumpcall(buf, &calltab.p[ins->r.i]);
      } else {
         dumpref(buf, ins->op, ins->oper[i]);
      }
   }
   if (oisalloca(ins->op) && ins->l.t == RICON) {
      bfmt(buf, " \t; %d bytes", ins->l.i << (ins->op - Oalloca1));
   }
   if (ins->keep)
      bfmt(buf, " !keep");
}

void
dumpinstr1(const Instr *ins)
{
   dumpinstr(out, ins);
   ioputc(out, '\n');
}

static bool prinums;

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
   if (blk->loopdepth)
      bfmt(out, "\t; loop depth: %d", blk->loopdepth);
   ioputc(out, '\n');
   for (i = 0; i < blk->phi.n; ++i) {
      Instr *phi = &instrtab[blk->phi.p[i]];
      Ref *refs = phitab.p[phi->l.i];
      if (prinums) {
         if (i == 0)
            bfmt(out, "%-4d", blk->inumstart);
         else
            bfmt(out, " |> ");
      }
      bfmt(out, "    %s ", clsname[phi->cls]);
      if (!phi->reg) bfmt(out, "%%%d = %s ", blk->phi.p[i], opnames[phi->op]);
      else bfmt(out, "(%%%d)%s = %s ", phi - instrtab, mctarg->rnames[phi->reg-1], opnames[phi->op]);
      for (int i = 0; i < blk->npred; ++i) {
         if (i) bfmt(out, ", ");
         bfmt(out, "@%d ", blkpred(blk, i)->id);
         dumpref1(0, refs[i]);
      }
      ioputc(out, '\n');
   }
   for (i = 0; i < blk->ins.n; ++i) {
      if (prinums)
         bfmt(out, "%-4d", blk->inumstart + 1 + i);
      iowrite(out, "    ", 4);
      dumpinstr1(&instrtab[blk->ins.p[i]]);
   }
   if (prinums)
      bfmt(out, "%-4d", blk->inumstart + 1 + i);
   bfmt(out, "    %s ", jnames[blk->jmp.t]);
   if (blk->jmp.t == Jret && blk->jmp.arg[0].bits && !fn->nabiret && (isagg(fn->retty) || iscomplex(fn->retty))) {
      /* un-lowered struct return */
      dumpref1(0, mktyperef(mkirtype(fn->retty)));
      bfmt(out, " ");
   }
   for (i = 0; i < 2; ++i) {
      if (!blk->jmp.arg[i].bits) break;
      if (i > 0) bfmt(out, ", ");
      dumpref1(0, blk->jmp.arg[i]);
   }
   if (i && blk->s1) bfmt(out, ", ");
   if (blk->s1 && blk->s2) bfmt(out, "@%d, @%d", blk->s1->id, blk->s2->id);
   else if (blk->s1) bfmt(out, "@%d", blk->s1->id);
   bfmt(out, "\n");
}

void
irdump(Function *fn)
{
   out = ccopt.dbg.out;

   if (ccopt.dbg.dumpparsed) {
      /* print datas that haven't been printed before */
      while (nextdat < dattab.n) pridat(&dattab.p[nextdat++]);
   }

   bfmt(out, "function %s : %ty\n", fn->name, fn->fnty);
   if (fn->abiarg || fn->nabiret) {
      bfmt(out, "abi: (");
      for (int i = 0; i < fn->nabiarg; ++i) {
         if (i > 0) bfmt(out, ", ");
         if (!fn->abiarg[i].isstk) {
            bfmt(out, "%s", mctarg->rnames[fn->abiarg[i].reg]);
         } else {
            prityp(out, fn->abiarg[i].ty);
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
   char *getenv(char *), *s;
   prinums = 0;
   if ((s = getenv("DUMP_INSTRNUMS")) && *s > '0') {
      prinums = 1;
      numberinstrs(fn);
   }
   Block *blk = fn->entry;
   do {
      assert(blk->lprev->lnext == blk);
      dumpblk(fn, blk);
      assert(blk->lnext != NULL);
   } while ((blk = blk->lnext) != fn->entry);
   bfmt(out, "\n");
}

static bool
inlist(const char *list, const char *x)
{
   assert(list && x && *x);
   if (!strcmp(list, "*")) return 1;
   const char *y = strstr(list, x);
   if (!y || (y != list && y[-1] != ',')) return 0;
   char e = y[strlen(x)];
   return e == ',' || !e;
}

bool
dumpfilt(const char *fn)
{
   return !ccopt.dbg.dumpfilt || inlist(ccopt.dbg.dumpfilt, fn);
}

bool
dumpbefore(const char *fn, const char *pass)
{
   if (!ccopt.dbg.any) return 0;
   if (!ccopt.dbg.dumpbefore) return 0;
   return inlist(ccopt.dbg.dumpbefore, pass) && dumpfilt(fn);
}

bool
dumpafter(const char *fn, const char *pass)
{
   if (!ccopt.dbg.any) return 0;
   if (!ccopt.dbg.dumpafter) return 0;
   return inlist(ccopt.dbg.dumpafter, pass) && dumpfilt(fn);
}

/* vim:set ts=3 sw=3 expandtab: */
