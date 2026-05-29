#include "t_aarch64.h"
#include "obj.h"
#include "u_endian.h"

/* References:
 *    ARM ARM https://developer.arm.com/documentation/ddi0628/aa/?lang=en
 *    AAELF ABI https://github.com/ARM-software/abi-aa/blob/main/aaelf64/aaelf64.rst
 */

enum operkind { ONONE, OREGZR, OREG, OIMM, OMEM, OMEMGOT, OSYM, OSYMGOT };
enum shiftkind { SLSL, SLSR, SASR, SROR };
enum addrmode { AIMMIDX, AREGIDX, APREIDX, APOSTIDX };
enum addrregext { XUXTW = 2, XLSL = 3, XSXTW = 6, XSXTX = 7 };
typedef struct Oper {
   uchar t;
   union {
      struct { /* OREG (opt. shifted) */
         uchar reg;
         uchar shft : 2, /* enum shiftkind */
               shamt : 6;
         /*NYI: ext reg mode*/
      };
      struct { /* OMEM */
         uchar mode : 3; /* enum addrmode */
         uchar base : 5; /* reg */
         union {
            struct { /* AREGIDX */
               uchar index : 5; /* reg */
               uchar ext : 3; /* enum addrregext */
               uchar shamt;
            };
            ushort con; /* OMEMGOT */
            short disp; /* AIMM/PRE/POSTIDX */
         };
      } m;
      s64int imm; u64int uimm; /* OIMM */
      struct { /* OSYM */
         ushort con;
         int cdisp;
      };
   };
} Oper;

#define REGZR ((Oper){OREGZR, .reg=31})
#define mkoper(t, ...) ((Oper){(t), __VA_ARGS__})
#define reg2oper(r) (assert((uint)(r) <= V(31)), mkoper(OREG, .reg = (r)))

static inline bool usegot(int c)
{
   const IRCon *con = &contab.p[c];
   return ((ccopt.pic | ccopt.pie) || (con->flag & SFUNC))
       && !con->deref && !con->isdat
       && !(con->flag & SLOCAL);
}

typedef struct Frame {
   regset save;
   struct RPair { uchar a,b; } pairs[10];
   uchar single[2];
   int nfpairs, ngpairs, nsingle;
   bool usefp;
   int size, stksiz;
} Frame;

static Frame frame;

static int
stackdisp(int i)
{
   if (frame.usefp) {
      return i < 0 ? frame.size - i - 8 /* arg */
                   : 16 + i;
   } else {
      return i < 0 ? frame.size - i - 8 /* arg */
                   : i;
   }
}

static Oper
mkmemoper(uint msiz, Ref r)
{
   if (r.t == RTMP) {
      assert(in_range(instrtab[r.i].reg-1, R0, SP));
      return mkoper(OMEM, .m = {AIMMIDX, .base = instrtab[r.i].reg-1});
   } else if (r.t == RREG) {
      return mkoper(OMEM, .m = {AIMMIDX, .base = r.i});
   } else if (r.t == RSTACK) {
      int disp = stackdisp(r.i);
      return mkoper(OMEM, .m = {AIMMIDX, .base = frame.usefp ? FP : SP, .disp = disp});
   } else if (isaddrcon(r,1)) {
      return mkoper(OSYM + usegot(r.i), .con = r.i,);
   } else if (r.t == RADDR) {
      const IRAddr *addr = &addrtab.p[r.i];
      assert(addr->shift <= 3 && (!addr->disp || !addr->index.bits));
      if (isaddrcon(addr->base,0)) {
         assert(!addr->index.bits);
         return mkoper(OSYM + usegot(addr->base.i), .con = addr->base.i, .cdisp = addr->disp);
      }
      if (!addr->index.bits) {
         int base, disp;
         if (addr->base.t == RREG) {
            base = addr->base.i;
            disp = 0;
         } else if (addr->base.t == RSTACK) {
            base = frame.usefp ? FP : SP;
            disp = stackdisp(addr->base.i);
         } else assert(0);
         disp += addr->disp;
         return mkoper(OMEM, .m = {.mode = AIMMIDX, .base = base, .disp = disp});
      } else {
         assert(addr->base.t == RREG);
         assert(addr->index.t == RREG);
         assert(addr->shift == 0 || 1<<addr->shift == msiz);
         return mkoper(OMEM, .m = {
               .mode = AREGIDX,
               .base = addr->base.i,
               .index = addr->index.i,
               .ext = XLSL,
               .shamt = !!addr->shift,
         });
      }
   }
   assert(!"nyi");
}

static Oper
ref2oper(Ref r)
{
   switch (r.t) {
   case RTMP: return instrtab[r.i].reg ? mkoper(ONONE,) : reg2oper(instrtab[r.i].reg-1);
   case RREG: return reg2oper(r.i);
   case RICON: return mkoper(OIMM, .imm = r.i);
   case RXCON:
      if (kisint(contab.p[r.i].cls))
         return mkoper(OIMM, .imm = contab.p[r.i].i);
      else if (kisflt(contab.p[r.i].cls)) {
         assert(contab.p[r.i].f == 0.0);
         return mkoper(OIMM, .imm = 0);
      } else if (!contab.p[r.i].cls) {
   case RSTACK: case RADDR:
         return mkmemoper(0, r);
      }
      assert(0);
   default: assert(0);
   }
}

enum operpat {
   PNONE,
   PGPRZ, /* R0-R30,ZR */
   PGPRSP, /* R0-R30,SP */
   PSP, /* SP */
   PGPRZSHFT, /* R0-30,ZR SFHT #n */
   PFPR, /* V0 - V31 */
   PZERO, /* zero immediate */
   PU6, /* 6-bit uimm */
   PU12SL12, /* 12 bit uimm, optionally left shifted by 12 */
   PU16SL16, /* 16 bit uimm, left shift by 0/16/32/48 */
   PLOGIMM, /* immediate for logical instrs */
   PMEMAIMM,  /* addr 12bit immediate byte offset */
   PMEMAIMMH, /* addr 12bit immediate halfword offset (multiple of 2) */
   PMEMAIMMW, /* addr 12bit immediate word offset (multiple of 4) */
   PMEMAIMMX, /* addr 12bit immediate doubleword offset (multiple of 8) */
   PMEMPREPOST, /* addr signed 9bit immediate byte offset */
   PMEMPREPOSTXP, /* addr signed 7bit immediate doubleword offset */
   PMEMAREG, /* addr reg offset, optionally left shifted */
   PSYM, /* symbol */
};
enum operenc {
   EN_ADDSUBSHFT3R, /* add/sub-shift-reg */
   EN_ADDSUBEXT3R, /* add/sub-ext-reg */
   EN_LOGSHFT3R, /* logical/shifted-reg */
   EN_ARITH2R, /* data-processing/1src */
   EN_ARITH3R, /* data-processing/2src */
   EN_ADDSUBIMM, /* add/subtract-imm */
   EN_LOGIMM, /* logical-imm */
   EN_MOVEIMM, /* move/wide-imm */
   EN_MEMAIMM, /* load/store/unsigned-imm */
   EN_MEMAIMMH, /* load/store/unsigned-imm (halfword) */
   EN_MEMAIMMW, /* load/store/unsigned-imm (word) */
   EN_MEMAIMMX, /* load/store/unsigned-imm (doubleword) */
   EN_MEMAPREPOST, /* load/store/pre/postidx-imm, single */
   EN_MEMAREG, /* load/store/reg-offset */
   EN_MEMPREPOSTXP, /* load/store-xreg pair/pre/postidx-imm */
   EN_ADRSYMLO21, /* for ADR <sym> */
   EN_ADRSYMPGHI21, /* for ADRP <sym:pghi21> */
   EN_ADDSYMLO12, /* for ADD x,x, <sym:lo12> */
   EN_LDSYMLO19, /* for LDR (literal) */
   EN_FP2R, /* float 1src */
   EN_FP1GPR1, /* fpr + gpr */
   EN_FP3R, /* float 2src */
   EN_FPIMM, /* float-imm */
   EN_FPCMPZ, /* float cmp with zero */
   EN_FPCMP, /* float cmp-imm */
};
typedef struct EncDesc {
   uchar psiz; /* subset of {4,8} */
   uchar pt[3]; /* bitsets of enum operpat, up to 3 operands */
   uint opc;
   uchar operenc; /* enum operenc */
} EncDesc;

/* match operand against pattern */
static inline bool
opermatch(enum operpat pat, enum irclass k, Oper o)
{
   switch (pat) {
   case PNONE: return !o.t;
   case PGPRZ:
      return o.t == OREGZR || (o.t == OREG && in_range(o.reg, R0, R(30)) && !o.shamt);
   case PGPRSP:
      return o.t == OREG && in_range(o.reg, R0, R(31)) && !o.shamt;
   case PGPRZSHFT:
      return o.t == OREGZR || (o.t == OREG && in_range(o.reg, R0, R(30)));
   case PSP:   return o.t == OREG && o.reg == SP;
   case PFPR:  return o.t == OREG && in_range(o.reg, V0, V(31));
   case PZERO: return o.t == OIMM && o.imm == 0;
   case PU6:   return o.t == OIMM && (uint)o.imm < 63;
   case PSYM:  return in_range(o.t, OSYM, OSYMGOT);
   case PU12SL12:
      return o.t == OIMM && ((o.imm &~ 0xFFF) == 0 || (o.imm &~ 0xFFF000) == 0);
   case PU16SL16:
      return o.t == OIMM
         && ((o.imm &~ 0xFFFF) == 0 || (o.imm &~ 0xFFFF0000) == 0
          || (o.imm &~ (0xFFFFull<<32)) == 0 || (o.imm &~ (0xFFFFull<<48)) == 0);
   case PLOGIMM: return o.t == OIMM && aarch64_logimm(NULL, k, o.imm);
   case PMEMAIMM:
      return o.t == OMEM && o.m.mode == AIMMIDX && (uint)o.m.disp < (1<<12);
   case PMEMAIMMH:
      return o.t == OMEM && o.m.mode == AIMMIDX && (uint)o.m.disp < (1<<13) && !(o.m.disp % 2);
   case PMEMAIMMW:
      return o.t == OMEM && o.m.mode == AIMMIDX && (uint)o.m.disp < (1<<14) && !(o.m.disp % 4);
   case PMEMAIMMX:
      return (o.t == OMEM && o.m.mode == AIMMIDX && (uint)o.m.disp < (1<<15) && !(o.m.disp % 8))
          || (o.t == OMEMGOT);
   case PMEMAREG:
      return o.t == OMEM && o.m.mode == AREGIDX;
   case PMEMPREPOST:
      return o.t == OMEM
         && (o.m.mode == APREIDX || o.m.mode == APOSTIDX || o.m.mode == AIMMIDX)
         && o.m.disp >= -256 && o.m.disp < 256;
   case PMEMPREPOSTXP:
      return o.t == OMEM
         && (o.m.mode == APREIDX || o.m.mode == APOSTIDX || o.m.mode == AIMMIDX)
         && o.m.disp >= -512 && o.m.disp <= 504
         && !(o.m.disp % 8);
   }
   assert(0);
}

/* code output helpers */
#define W32(w) (wr32targ(*pcode, (w)), *pcode += 4)

static uchar *fnstart;
static internstr curfnsym;

/* Given an instruction description table, find the first entry that matches
 * the operands and encode it. */
static void
encode(uchar **pcode, const EncDesc *tab, int ntab, enum irclass k, Oper o[3])
{
   const EncDesc *en = NULL;
   for (int i = 0; i < ntab; ++i) {
      if (!(tab[i].psiz & cls2siz[k])) continue;
      for (int j = 0; j < 3; ++j)
         if (!opermatch(tab[i].pt[j], k, o[j]))
            goto Skip;
      en = &tab[i];
      break;
      Skip:;
   }
   assert(en && "no match for instr");

   uint sf = cls2siz[k] >> 3;
   uint ins = en->opc, sh, nimmrs;
   switch (en->operenc) {
   default: assert(!"nyi enc");
   case EN_ADDSUBSHFT3R: case EN_LOGSHFT3R:
      ins |= sf<<31 | o[2].shft<<22 | o[2].reg<<16 | o[2].shamt<<10 | o[1].reg<<5 | o[0].reg;
      break;
   case EN_ADDSUBEXT3R: /*NYI ext*/
      ins |= sf<<31 | o[2].reg<<16 | (/*LSL*/2+sf)<<13 | o[1].reg<<5 | o[0].reg;
      break;
   case EN_ARITH2R:
      ins |= sf<<31 | o[1].reg<<5 | o[0].reg;
      break;
   case EN_ARITH3R:
      ins |= sf<<31 | o[2].reg<<16 | o[1].reg<<5 | o[0].reg;
      break;
   case EN_ADDSUBIMM:
      sh = o[2].imm > 0xFFF;
      ins |= sf<<31 | sh<<22 | (o[2].uimm >> 12*sh)<<10 | o[1].reg<<5 | o[0].reg;
      break;
   case EN_LOGIMM:
      assert(aarch64_logimm(&nimmrs, k, o[2].uimm));
      ins |= sf<<31 | nimmrs<<10 | o[1].reg<<5 | o[0].reg;
      break;
   case EN_MOVEIMM:
      sh = o[1].imm ? lowestsetbit(o[1].imm) / 16 : 0;
      ins |= sf<<31 | sh<<21 | (o[1].uimm >> 16*sh)<<5 | o[0].reg;
      break;
   case EN_MEMAIMM: AImm:
      ins |= o[1].m.disp<<10 | o[1].m.base<<5 | (o[0].reg&31);
      break;
   case EN_MEMAIMMH: o[1].m.disp >>= 1; goto AImm;
   case EN_MEMAIMMW: o[1].m.disp >>= 2; goto AImm;
   case EN_MEMAIMMX:
      if (o[1].t != OMEMGOT) {
         o[1].m.disp >>= 3;
         goto AImm;
      }
      ins |= o[1].m.base<<5 | (o[0].reg&31);
      objrelocxcon(o[1].m.con, REL_LD64_GOT_LO12_NC, Stext, *pcode - objout.textbegin, 0);
      break;
   case EN_MEMAPREPOST:
      ins |= (o[1].m.disp&0x1FF)<<12 | o[1].m.base<<5 | (o[0].reg&31);
      if (o[1].m.mode == APREIDX) ins |= 3<<10;
      else if (o[1].m.mode == APOSTIDX) ins |= 1<<10;
      break;
   case EN_MEMAREG:
      assert(o[1].m.shamt <= 1);
      assert(o[1].m.ext >= 2);
      ins |= o[1].m.index<<16 | o[1].m.ext<<13 | o[1].m.shamt<<12 | o[1].m.base<<5 | (o[0].reg&31);
      break;
   case EN_MEMPREPOSTXP:
      assert(o[2].m.disp % 8 == 0);
      ins |= (o[2].m.disp/8&0x7F)<<15 | (o[1].reg&31)<<10 | o[2].m.base<<5 | (o[0].reg&31);
      if (o[2].m.mode == APREIDX) ins |= 3<<23;
      else if (o[2].m.mode == APOSTIDX) ins |= 1<<23;
      else ins |= 2<<23;
      break;
   case EN_ADRSYMLO21:
      ins |= o[0].reg;
      objrelocxcon(o[1].con, REL_ADR_PREL_LO21, Stext, *pcode - objout.textbegin, o[1].cdisp);
      break;
   case EN_ADRSYMPGHI21:
      ins |= o[0].reg;
      objrelocxcon(o[1].con, o[1].t == OSYMGOT ? REL_ADR_GOT_PAGE
                                               : REL_ADR_PREL_PG_HI21,
                   Stext, *pcode - objout.textbegin, o[1].cdisp);
      break;
   case EN_ADDSYMLO12:
      ins |= sf<<31 | o[1].reg<<5 | o[0].reg;
      objrelocxcon(o[2].con, REL_ADD_ABS_LO12_NC, Stext, *pcode - objout.textbegin, o[2].cdisp);
      break;
   case EN_LDSYMLO19:
      ins |= o[0].reg;
      objrelocxcon(o[1].con, REL_LD_PREL_LO19, Stext, *pcode - objout.textbegin, o[1].cdisp);
      break;
   case EN_FP2R:
      ins |= sf<<22 | (o[1].reg&31)<<5 | (o[0].reg&31);
      break;
   case EN_FP1GPR1:
      ins |= (o[1].reg&31)<<5 | (o[0].reg&31);
      break;
   case EN_FP3R:
      ins |= sf<<22 | (o[2].reg&31)<<16 | (o[1].reg&31)<<5 | (o[0].reg&31);
      break;
   case EN_FPCMPZ:
      ins |= sf<<22 | (o[0].reg&31)<<5;
      break;
   case EN_FPCMP:
      ins |= sf<<22 | (o[1].reg&31)<<16 | (o[0].reg&31)<<5;
      break;
   }
   W32(ins);
}
#define DEFINSTR1(X, ...)                                   \
   static void                                              \
   X(uchar **pcode, enum irclass k, Oper a)                 \
   {                                                        \
      static const EncDesc tab[] = { __VA_ARGS__ };         \
      encode(pcode, tab, countof(tab), k, ((Oper [3]){a})); \
   }

#define DEFINSTR2(X, ...)                                         \
   static void                                                    \
   X(uchar **pcode, enum irclass k, Oper op1, Oper op2)           \
   {                                                              \
      static const EncDesc tab[] = { __VA_ARGS__ };               \
      encode(pcode, tab, countof(tab), k, ((Oper [3]){op1,op2})); \
   }

#define DEFINSTR3(X, ...)                                             \
   static void                                                        \
   X(uchar **pcode, enum irclass k, Oper op1, Oper op2, Oper op3)     \
   {                                                                  \
      static const EncDesc tab[] = { __VA_ARGS__ };                   \
      encode(pcode, tab, countof(tab), k, ((Oper [3]){op1,op2,op3})); \
   }

DEFINSTR2(Xadrp,
   {8, {PGPRZ, PSYM}, 0x90000000, EN_ADRSYMPGHI21} /* ADR (sym pg hi21) */
)
DEFINSTR2(Xadr,
   {8, {PGPRZ, PSYM}, 0x10000000, EN_ADRSYMLO21} /* ADR (sym lo21) */
)

DEFINSTR3(Xadd,
   {4|8, {PGPRSP, PGPRSP, PU12SL12}, 0x11000000, EN_ADDSUBIMM}, /* ADD (immediate) */
   {4|8, {PGPRZ,  PGPRZ, PGPRZSHFT}, 0x0B000000, EN_ADDSUBSHFT3R}, /* ADD (shifted register) */
   {4|8, {PGPRSP, PGPRSP, PGPRZ},    0x0B200000, EN_ADDSUBEXT3R}, /* ADD ((extended) register) */
   {  8, {PGPRZ,  PGPRZ, PSYM},      0x11000000, EN_ADDSYMLO12}, /* ADD (sym lo12) */
)
DEFINSTR3(Xsub,
   {4|8, {PGPRSP, PGPRSP, PU12SL12}, 0x51000000, EN_ADDSUBIMM}, /* SUB (immediate) */
   {4|8, {PGPRZ,  PGPRZ, PGPRZSHFT}, 0x4B000000, EN_ADDSUBSHFT3R}, /* SUB (shifted register) */
   {4|8, {PGPRSP, PGPRSP, PGPRZ},    0x4B200000, EN_ADDSUBEXT3R}, /* SUB ((extended) register) */
)
DEFINSTR3(Xsubs,
   {4|8, {PGPRZ, PGPRSP, PU12SL12}, 0x71000000, EN_ADDSUBIMM}, /* SUBS (immediate) */
   {4|8, {PGPRZ, PGPRZ, PGPRZSHFT},  0x6B000000, EN_ADDSUBSHFT3R}, /* SUBS (shifted register) */
)

static void
Xmadd(uchar **pcode, enum irclass k, Oper d, Oper n, Oper m, Oper a)
{
   assert(opermatch(PGPRZ, k, d) && opermatch(PGPRZ, k, n)
       && opermatch(PGPRZ, k, a) && opermatch(PGPRZ, k, m));
   uint sf = k > KI32;
   W32(0x1B000000 | sf<<31 | m.reg<<16 | a.reg<<10 | n.reg<<5 | d.reg);
}

static void
Xmsub(uchar **pcode, enum irclass k, Oper d, Oper n, Oper m, Oper a)
{
   assert(opermatch(PGPRZ, k, d) && opermatch(PGPRZ, k, n)
       && opermatch(PGPRZ, k, a) && opermatch(PGPRZ, k, m));
   uint sf = k > KI32;
   W32(0x1B008000 | sf<<31 | m.reg<<16 | a.reg<<10 | n.reg<<5 | d.reg);
}

DEFINSTR3(Xsdiv, {4|8, {PGPRZ, PGPRZ, PGPRZ}, 0x1AC00C00, EN_ARITH3R})
DEFINSTR3(Xudiv, {4|8, {PGPRZ, PGPRZ, PGPRZ}, 0x1AC00800, EN_ARITH3R})

DEFINSTR3(Xand,
   {4|8, {PGPRSP, PGPRZ, PLOGIMM},   0x12000000, EN_LOGIMM}, /* AND (immediate) */
   {4|8, {PGPRZ,  PGPRZ, PGPRZSHFT}, 0x0A000000, EN_LOGSHFT3R}, /* AND (shifted register) */
)
DEFINSTR3(Xorr,
   {4|8, {PGPRSP, PGPRZ, PLOGIMM},   0x32000000, EN_LOGIMM}, /* ORR (immediate) */
   {4|8, {PGPRZ,  PGPRZ, PGPRZSHFT}, 0x2A000000, EN_LOGSHFT3R}, /* ORR (shifted register) */
)
DEFINSTR3(Xorn, {4|8, {PGPRZ,  PGPRZ, PGPRZSHFT}, 0x2A200000, EN_LOGSHFT3R})
DEFINSTR3(Xeor,
   {4|8, {PGPRSP, PGPRZ, PLOGIMM},   0x52000000, EN_LOGIMM}, /* EOR (immediate) */
   {4|8, {PGPRZ,  PGPRZ, PGPRZSHFT}, 0x4A000000, EN_LOGSHFT3R}, /* EOR (shifted register) */
)
DEFINSTR3(Xlslv, {4|8, {PGPRZ, PGPRZ, PGPRZ}, 0x1AC02000, EN_ARITH3R})
DEFINSTR3(Xlsrv, {4|8, {PGPRZ, PGPRZ, PGPRZ}, 0x1AC02400, EN_ARITH3R})
DEFINSTR3(Xasrv, {4|8, {PGPRZ, PGPRZ, PGPRZ}, 0x1AC02800, EN_ARITH3R})
static void
Xubfm(uchar **pcode, enum irclass k, Oper rd, Oper rn, uint immr, uint imms)
{
   uint x = k != KI32;
   uint nbit = x ? 64 : 32;
   assert(opermatch(PGPRZ, k, rd) && opermatch(PGPRZ, k, rn) && immr < nbit && imms < nbit);
   W32(x<<31 | 0x53000000 | x<<22 | immr<<16 | imms<<10 | rn.reg<<5 | rd.reg);
}
static void
Xsbfm(uchar **pcode, enum irclass k, Oper rd, Oper rn, uint immr, uint imms)
{
   uint x = k != KI32;
   uint nbit = x ? 64 : 32;
   assert(opermatch(PGPRZ, k, rd) && opermatch(PGPRZ, k, rn) && immr < nbit && imms < nbit);
   W32(x<<31 | 0x13000000 | x<<22 | immr<<16 | imms<<10 | rn.reg<<5 | rd.reg);
}

DEFINSTR2(Xmovz, {4|8, {PGPRZ, PU16SL16}, 0x52800000, EN_MOVEIMM}, /* MOVZ */)
DEFINSTR2(Xmovn, {4|8, {PGPRZ, PU16SL16}, 0x12800000, EN_MOVEIMM}, /* MOVN */)
DEFINSTR2(Xmovk, {4|8, {PGPRZ, PU16SL16}, 0x72800000, EN_MOVEIMM}, /* MOVK */)
DEFINSTR2(Xldr,
   {4, {PGPRZ, PMEMAIMMW}, 0xB9400000, EN_MEMAIMMW}, /* LDR (immediate) */
   {8, {PGPRZ, PMEMAIMMX}, 0xF9400000, EN_MEMAIMMX},
   {4, {PGPRZ, PMEMAREG}, 0xB8600800, EN_MEMAREG}, /* LDR (register) */
   {8, {PGPRZ, PMEMAREG}, 0xF8600800, EN_MEMAREG},
   {4, {PGPRZ, PSYM}, 0x18000000, EN_LDSYMLO19}, /* LDR (literal) */
   {8, {PGPRZ, PSYM}, 0x58000000, EN_LDSYMLO19},
   {4, {PGPRZ, PMEMPREPOST}, 0xB8400000, EN_MEMAPREPOST}, /* LDR (immediate, (pre/postinc)) */
   {8, {PGPRZ, PMEMPREPOST}, 0xF8400000, EN_MEMAPREPOST},
)
DEFINSTR2(Xfldr,
   {4, {PFPR, PMEMAIMMW}, 0xBD400000, EN_MEMAIMMW}, /* LDR (immediate) */
   {8, {PFPR, PMEMAIMMX}, 0xFD400000, EN_MEMAIMMX},
   {4, {PFPR, PMEMAREG},  0xBC600800, EN_MEMAREG}, /* LDR (register) */
   {8, {PFPR, PMEMAREG},  0xFC600800, EN_MEMAREG},
   {4, {PFPR, PSYM}, 0x1C000000, EN_LDSYMLO19}, /* LDR (literal) */
   {8, {PFPR, PSYM}, 0x5C000000, EN_LDSYMLO19},
   {4, {PFPR, PMEMPREPOST}, 0xBC400000, EN_MEMAPREPOST}, /* LDR (immediate, (pre/postinc)) */
   {8, {PFPR, PMEMPREPOST}, 0xFC400000, EN_MEMAPREPOST},
)
DEFINSTR2(Xldrsw,
   {8, {PGPRZ, PMEMAIMMW},   0xB9800000, EN_MEMAIMMW}, /* LDRSW (immediate) */
//   {8, {PGPRZ, PMEMAREG},    0xB8A00800, EN_MEMAREG}, /* LDRSW (register) */
   {8, {PGPRZ, PMEMPREPOST}, 0xB8800000, EN_MEMAPREPOST}, /* LDRSW (immediate, (pre/postinc)) */
)
DEFINSTR2(Xldrh,
   {4|8, {PGPRZ, PMEMAIMMH},   0x79400000, EN_MEMAIMMH}, /* LDRH (immediate) */
   {4|8, {PGPRZ, PMEMAREG},    0x78600800, EN_MEMAREG}, /* LDRH (register) */
   {4|8, {PGPRZ, PMEMPREPOST}, 0x78400000, EN_MEMAPREPOST}, /* LDRH (immediate, (pre/postinc)) */
)
DEFINSTR2(Xldrsh,
   {4, {PGPRZ, PMEMAIMMH}, 0x79C00000, EN_MEMAIMMH}, /* LDRSH (immediate) */
   {8, {PGPRZ, PMEMAIMMH}, 0x79800000, EN_MEMAIMMH},
   {4, {PGPRZ, PMEMAREG},  0x78E00800, EN_MEMAREG}, /* LDRSH (register) */
   {8, {PGPRZ, PMEMAREG},  0x78A00800, EN_MEMAREG},
   {4, {PGPRZ, PMEMPREPOST}, 0x78C00000, EN_MEMAPREPOST}, /* LDRSH (immediate, (pre/postinc)) */
   {8, {PGPRZ, PMEMPREPOST}, 0x78800000, EN_MEMAPREPOST},
)
DEFINSTR2(Xldrb,
   {4|8, {PGPRZ, PMEMAIMM}, 0x39400000, EN_MEMAIMM}, /* LDRB (immediate) */
   {4|8, {PGPRZ, PMEMAREG}, 0x38600800, EN_MEMAREG}, /* LDRB (register) */
   {4|8, {PGPRZ, PMEMPREPOST}, 0x38400000, EN_MEMAPREPOST}, /* LDRB (immediate, (pre/postinc)) */
)
DEFINSTR2(Xldrsb,
   {4, {PGPRZ, PMEMAIMM}, 0x39C00000, EN_MEMAIMM}, /* LDRSB (immediate) */
   {8, {PGPRZ, PMEMAIMM}, 0x39800000, EN_MEMAIMM},
   {4, {PGPRZ, PMEMAREG}, 0x38E00800, EN_MEMAREG}, /* LDRSB (register) */
   {8, {PGPRZ, PMEMAREG}, 0x38A00800, EN_MEMAREG},
   {4, {PGPRZ, PMEMPREPOST}, 0x38C00000, EN_MEMAPREPOST}, /* LDRSB (immediate, (pre/postinc)) */
   {8, {PGPRZ, PMEMPREPOST}, 0x38800000, EN_MEMAPREPOST},
)
DEFINSTR2(Xstr,
   {4, {PGPRZ, PMEMAIMMW}, 0xB9000000, EN_MEMAIMMW}, /* STR (immediate) */
   {8, {PGPRZ, PMEMAIMMX}, 0xF9000000, EN_MEMAIMMX},
   {4, {PGPRZ, PMEMAREG}, 0xB8200800, EN_MEMAREG}, /* STR (register) */
   {8, {PGPRZ, PMEMAREG}, 0xF8200800, EN_MEMAREG},
   {4, {PGPRZ, PMEMPREPOST}, 0xB8000000, EN_MEMAPREPOST}, /* STR (immediate, (pre/postinc)) */
   {8, {PGPRZ, PMEMPREPOST}, 0xF8000000, EN_MEMAPREPOST},
)
DEFINSTR2(Xfstr,
   {4, {PFPR, PMEMAIMMW}, 0xBD000000, EN_MEMAIMMW}, /* STR (immediate) */
   {8, {PFPR, PMEMAIMMX}, 0xFD000000, EN_MEMAIMMX},
   {4, {PFPR, PMEMAREG},  0xBC200800, EN_MEMAREG}, /* STR (register) */
   {8, {PFPR, PMEMAREG},  0xFC200800, EN_MEMAREG},
   {4, {PFPR, PMEMPREPOST}, 0xBC000000, EN_MEMAPREPOST}, /* STR (immediate, (pre/postinc)) */
   {8, {PFPR, PMEMPREPOST}, 0xFC000000, EN_MEMAPREPOST},
)
DEFINSTR2(Xstrh,
   {4|8, {PGPRZ, PMEMAIMMH},   0x79000000, EN_MEMAIMMH}, /* STRH (immediate) */
   {4|8, {PGPRZ, PMEMAREG},    0x78200800, EN_MEMAREG}, /* STRH (register) */
   {4|8, {PGPRZ, PMEMPREPOST}, 0x78000000, EN_MEMAPREPOST}, /* STRH (immediate, (pre/postinc)) */
)
DEFINSTR2(Xstrb,
   {4|8, {PGPRZ, PMEMAIMM},    0x39000000, EN_MEMAIMM}, /* STRB (immediate) */
   {4|8, {PGPRZ, PMEMAREG},    0x38200800, EN_MEMAREG}, /* STRB (register) */
   {4|8, {PGPRZ, PMEMPREPOST}, 0x38000000, EN_MEMAPREPOST}, /* STRB (immediate, (pre/postinc)) */
)
DEFINSTR3(Xldp,
   {8, {PGPRZ, PGPRZ, PMEMPREPOSTXP}, 0xA8400000, EN_MEMPREPOSTXP} /* LDP (immediate, (pre/postinc)) */
)
DEFINSTR3(Xstp,
   {8, {PGPRZ, PGPRZ, PMEMPREPOSTXP}, 0xA8000000, EN_MEMPREPOSTXP} /* STP (immediate, (pre/postinc)) */
)
DEFINSTR3(Xfldp,
   {8, {PFPR, PFPR, PMEMPREPOSTXP}, 0x6CC00000, EN_MEMPREPOSTXP} /* LDP (immediate, (pre/postinc)) */
)
DEFINSTR3(Xfstp,
   {8, {PFPR, PFPR, PMEMPREPOSTXP}, 0x6C800000, EN_MEMPREPOSTXP} /* STP (immediate, (pre/postinc)) */
)
static void
Xcall(uchar **pcode, Oper dst)
{
   if (in_range(dst.t, OSYM, OSYMGOT)) {
      objrelocxcon(dst.con, REL_CALL26, Stext, *pcode - objout.textbegin, 0);
      W32(0x94000000); /* BL <rel26> */
   } else {
      assert(opermatch(PGPRZ, KPTR, dst));
      W32(0xD63F0000 | dst.reg<<5); /* BLR Xn */
   }
}
DEFINSTR2(Xfmov,
   {4|8, {PFPR, PFPR},  0x1E204000, EN_FP2R},
   {4,   {PFPR, PGPRZ}, 0x1E270000, EN_FP1GPR1},
   {  8, {PFPR, PGPRZ}, 0x9E670000, EN_FP1GPR1},
   {4,   {PGPRZ, PFPR}, 0x1E260000, EN_FP1GPR1},
   {  8, {PGPRZ, PFPR}, 0x9E660000, EN_FP1GPR1},
)
DEFINSTR2(Xfneg, {4|8, {PFPR, PFPR}, 0x1E214000, EN_FP2R})
DEFINSTR2(Xscvtfw,  {4|8, {PFPR, PGPRZ}, 0x1E220000, EN_FP2R})
DEFINSTR2(Xscvtfx,  {4|8, {PFPR, PGPRZ}, 0x9E220000, EN_FP2R})
DEFINSTR2(Xfcvtzsw, {4|8, {PGPRZ, PFPR}, 0x1E380000, EN_FP2R})
DEFINSTR2(Xfcvtzsx, {4|8, {PGPRZ, PFPR}, 0x9E380000, EN_FP2R})
DEFINSTR2(Xucvtfw,  {4|8, {PFPR, PGPRZ}, 0x1E230000, EN_FP2R})
DEFINSTR2(Xucvtfx,  {4|8, {PFPR, PGPRZ}, 0x9E230000, EN_FP2R})
DEFINSTR2(Xfcvtzuw, {4|8, {PGPRZ, PFPR}, 0x1E390000, EN_FP2R})
DEFINSTR2(Xfcvtzux, {4|8, {PGPRZ, PFPR}, 0x9E390000, EN_FP2R})
DEFINSTR2(Xfcvtds,  {4,   {PFPR,  PFPR}, 0x1E624000, EN_FP2R})
DEFINSTR2(Xfcvtsd,  {4,   {PFPR,  PFPR}, 0x1E22C000, EN_FP2R})
DEFINSTR3(Xfadd, {4|8, {PFPR, PFPR, PFPR}, 0x1E202800, EN_FP3R})
DEFINSTR3(Xfsub, {4|8, {PFPR, PFPR, PFPR}, 0x1E203800, EN_FP3R})
DEFINSTR3(Xfmul, {4|8, {PFPR, PFPR, PFPR}, 0x1E200800, EN_FP3R})
DEFINSTR3(Xfdiv, {4|8, {PFPR, PFPR, PFPR}, 0x1E201800, EN_FP3R})
DEFINSTR2(Xfcmp,
   {4|8, {PFPR, PZERO}, 0x1E602008, EN_FPCMPZ},
   {4|8, {PFPR,  PFPR}, 0x1E602000, EN_FPCMP},
)
DEFINSTR2(Xrev,
   {4  , {PGPRZ, PGPRZ}, 0x5AC00800, EN_ARITH2R},
   {  8, {PGPRZ, PGPRZ}, 0xDAC00C00, EN_ARITH2R},
)

static void
genmovimm(uchar **pcode, enum irclass cls, Oper dst, u64int u)
{
   assert(dst.t == OREG && dst.reg <= R(31));
   /* MOV r, #imm */
   if (~u <= 0xFFFF) {
      /* immediate can be encoded with 1 MOVN instruction */
      Xmovn(pcode, cls, dst, mkoper(OIMM, .imm = ~u));
   } else {
      /* generate MOV (+ MOVKs) */
      if (cls == KI32) u = (uint)u;
      int s = 0;
      while (s < 48 && (u >> s & 0xFFFF) == 0) s += 16;
      if ((u &~ (0xFFFFull << s)) != 0 && aarch64_logimm(NULL, cls, u)) {
         /* can be encoded as a logical immediate in 1 instr */
         Xorr(pcode, cls, dst, REGZR, mkoper(OIMM, .uimm = u));
      } else {
         Xmovz(pcode, cls, dst, mkoper(OIMM, .imm = u & (0xFFFFull << s)));
         for (s += 16; s <= 48; s += 16) {
            if ((u >> s) & 0xFFFF)
               Xmovk(pcode, cls, dst, mkoper(OIMM, .imm = u & (0xFFFFull << s)));
         }
      }
   }
}

#define RMEMSCRATCH R(17)

/* if can encode given immediate with pattern, return it
 * otherwise, load imm to scratch reg #2 (IP1/r17) and return reg */
static Oper
imm_or_reg(uchar **pcode, enum irclass cls, enum operpat pat, int imm)
{
   assert(cls == KI32);
   Oper oper = mkoper(OIMM, .imm = imm);
   if (!opermatch(pat, cls, oper)) {
      oper = mkoper(OREG, .reg = RMEMSCRATCH);
      genmovimm(pcode, cls, oper, imm);
   }
   return oper;
}

static void
gencopy(uchar **pcode, enum irclass cls, Oper dst, Ref val)
{
   assert(dst.t == OREG);
   assert(cls);
   Oper src;
   if (val.bits == UNDREF.bits) return;
   if (isintcon(val)) {
      genmovimm(pcode, cls, dst, intconval(val));
      return;
   } else if (val.t == RSTACK) {
      Xadd(pcode, cls, dst,
            reg2oper(frame.usefp ? FP : SP),
            imm_or_reg(pcode, KI32, PU12SL12, stackdisp(val.i)));
      return;
   }
   src = ref2oper(val);
   if (opermatch(PGPRZ, cls, src) && kisint(cls)) {
      Xorr(pcode, cls, dst, REGZR, src); /* MOV Rd, Rn ==> ORR Rd, zr, Rn */
   } else if (kisflt(cls) || opermatch(PFPR, 0, src)) {
      if (src.t == OREG)
         Xfmov(pcode, cls, dst, src);
      else if (src.t == OIMM && src.imm == 0)
         Xfmov(pcode, cls, dst, REGZR);
      else assert(0);
   } else if (in_range(src.t, OSYM, OSYMGOT)) {
      if ((ccopt.pic|ccopt.pie) || src.t == OSYMGOT || !(contab.p[src.con].flag & SLOCAL)) {
         Xadrp(pcode, KPTR, dst, src);
         if (src.t == OSYM) {
            Xadd(pcode, KPTR, dst, dst, src);
         } else { /* load from GOT (reg + got_lo12) */
            assert(dst.t == OREG);
            src = mkoper(OMEMGOT, .m = {AIMMIDX, .base = dst.reg, .con = src.con});
            Xldr(pcode, KPTR, dst, src);
         }
      } else {
         Xadr(pcode, KPTR, dst, src);
      }
   } else if (src.t == OMEM) {
      assert(dst.t == OREG);
      assert(src.m.mode == AIMMIDX);
      Xadd(pcode, cls, dst, reg2oper(src.m.base),
            imm_or_reg(pcode, KI32, PU12SL12, src.m.disp));
   } else assert(0);
}

static Oper
genmemoper(uchar **pcode,  enum irclass k, uint msiz, Ref r)
{
   Oper oper = mkmemoper(msiz, r);
   assert(oper.t == OMEM || in_range(oper.t, OSYM, OSYMGOT));
   if (oper.t == OMEM && oper.m.base == FP && oper.m.mode == AIMMIDX) {
      enum operpat xpat = PMEMAIMM + ilog2(msiz);
      assert(in_range(xpat, PMEMAIMM, PMEMAIMMX));
      if (!opermatch(PMEMPREPOST, k, oper) && !opermatch(xpat, k, oper)) {
         genmovimm(pcode, KI32, mkoper(OREG, .reg = RMEMSCRATCH), (s64int)oper.m.disp);
         oper = mkoper(OMEM, .m.mode = AREGIDX, .m.base = FP,
                             .m.index = RMEMSCRATCH, .m.ext = XLSL);
      }
   }
   return oper;
}


/* maps blk -> address when resolved; or to linked list of jump displacement
 * relocations */
static struct BlkAddr {
   bool resolved;
   union {
      uint addr;
      uint relreloc;
   };
} *blkaddr;

enum cc {
   CCEQ, CCNE, CCCS, CCCC, CCMI, CCPL, CCVS, CCVC,
   CCHI, CCLS, CCGE, CCLT, CCGT, CCLE, CCAL, CCNV,
   CCHS = CCCS, CCLO = CCCC,
};

static void
Xbcc(uchar **pcode, enum cc cc, Block *dst)
{
   int disp, insaddr = *pcode - objout.textbegin;

   if (blkaddr[dst->id].resolved) {
      disp = (int)(blkaddr[dst->id].addr - insaddr)/4;
      assert(disp >= -(1<<18) && disp < (1<<18));
   } else {
      disp = blkaddr[dst->id].relreloc;
      blkaddr[dst->id].relreloc = insaddr;
   }
   assert(in_range(cc, 0, 0xF));
   W32(0x54000000 | (disp & 0x7FFFF)<<5 | cc);
}

static void
Xcbcc(uchar **pcode, enum irclass k, uint rt, enum cc cc, Block *dst)
{
   int disp, insaddr = *pcode - objout.textbegin;
   if (blkaddr[dst->id].resolved) {
      disp = (int)(blkaddr[dst->id].addr - insaddr)/4;
      assert(disp >= -(1<<18) && disp < (1<<18));
   } else {
      disp = blkaddr[dst->id].relreloc;
      blkaddr[dst->id].relreloc = insaddr;
   }
   assert(in_range(cc, CCEQ, CCNE));
   assert(in_range(rt, 0, 31));
   W32(0x34000000 | (uint)(k > KI32)<<31 | cc<<24 | (disp & 0x7FFFF)<<5 | rt);
}

static void
Xvaprologue(uchar **pcode, Function *fn, Oper sav)
{
   int named_gr = 0, named_vr = 0;
   for (int i = 0; i < fn->nabiarg; ++i) {
      ABIArg abi = fn->abiarg[i];
      if (!abi.isstk) {
         if (abi.reg >= V0) ++named_vr;
         else ++named_gr;
      }
   }
   assert(sav.t == OMEM);
   /* save GPRS */
   for (int r = R(0)+named_gr; r <= R(7); ++r) {
      Xstr(pcode, KPTR, reg2oper(r), sav);
      sav.m.disp += 8;
   }

   sav.m.disp = alignup(sav.m.disp, 16);
   for (int r = V(0)+named_vr; r <= V(7); ++r) {
      Xfstr(pcode, KF64, reg2oper(r), sav);
      sav.m.disp += 16;
   }
}

/* condition code for CMP */
static const schar icmpop2cc[] = {
   [Oequ]  = CCEQ, [Oneq]  = CCNE,
   [Olth]  = CCLT, [Ogth]  = CCGT, [Olte]  = CCLE, [Ogte]  = CCGE,
   [Oulth] = CCLO, [Ougth] = CCHI, [Oulte] = CCLS, [Ougte] = CCHS,
}, fcmpop2cc[] = {
   [Oequ] = CCEQ, [Oneq] = CCNE,
   [Olth] = CCLO, [Ogth] = CCGT, [Olte] = CCLS, [Ogte] = CCGE,
};

static bool
cmpzero2branchok(Block *blk, Instr *ins)
{
   /* can transform compare-with-zero + branch sequence into CBZ/CBNZ (compare-zero-and-branch)
    * only if cmp instr is last in block; regalloc could have clobbered reg otherwise
    * when inserting moves for phis. overly conservative but it's ok */
   return in_range(ins->op, Oequ, Oneq) && kisint(ins->cls) && ins->r.bits == ZEROREF.bits
      && (ins - instrtab) == blk->ins.p[blk->ins.n-1];
}

static void
emitbranch(uchar **pcode, Block *blk)
{
   enum irclass cbk = 0;
   Oper cbopr;
   enum cc cc = CCAL;
   assert(blk->s1);
   if (blk->s2) {
      /* conditional branch.. */
      Ref arg = blk->jmp.arg[0];
      assert(arg.t == RTMP);
      Instr *ins = &instrtab[arg.i];

      if (cmpzero2branchok(blk, ins)) {
         cc = ins->op == Oequ ? CCEQ : CCNE;
         cbk = ins->cls;
         cbopr = ref2oper(ins->l);
         assert(opermatch(PGPRZ, ins->cls, cbopr));
      } else if (oiscmp(ins->op)) {
         /* for CMP instr */
         cc = (kisint(ins->cls) ? icmpop2cc : fcmpop2cc)[ins->op];
      } else {
         /* implicit by ZF */
         cc = CCNE;
      }
      if (blk->s1 == blk->lnext) {
         /* if s1 is next adjacent block, swap s1,s2 and flip condition to emit a
          * single jump */
         Block *tmp = blk->s1;
         blk->s1 = blk->s2;
         blk->s2 = tmp;
         cc ^= 1;
      }
   }
   /* make sure to fallthru if jumping to next adjacent block */
   if (blk->s2 || blk->s1 != blk->lnext) {
      if (cbk) Xcbcc(pcode, cbk, cbopr.reg, cc, blk->s1);
      else Xbcc(pcode, cc, blk->s1);
   }
   if (blk->s2 && blk->s2 != blk->lnext)
      Xbcc(pcode, CCAL, blk->s2);
}

static void
emitinstr(uchar **pcode, Function *fn, Block *blk, int curi, Instr *ins)
{
   Oper dst, o1, o2;
   enum irclass cls = ins->cls;
   void (*X3)(uchar **, enum irclass, Oper, Oper, Oper) = NULL;
   void (*X2)(uchar **, enum irclass, Oper, Oper) = NULL;

   switch (ins->op) {
   default: fatal(NULL, "aarch64 unimplemented instr: %s", opnames[ins->op]);
   case Onop: break;
   case Omove:
      dst = ref2oper(ins->l);
      gencopy(pcode, cls, dst, ins->r);
      break;
   case Oextu32: cls = KI32;
      /* fallthru */
   case Ocopy:
      dst = reg2oper(ins->reg-1);
      gencopy(pcode, cls, dst, ins->l);
      break;
   case Oswap:
      o1 = ref2oper(ins->l), o2 = ref2oper(ins->r);
      if (kisflt(ins->cls) && ins->l.i != mctarg->fprscratch && ins->r.i != mctarg->fprscratch) {
         dst = reg2oper(mctarg->fprscratch);
         Xfmov(pcode, cls, dst, o1);
         Xfmov(pcode, cls, o1, o2);
         Xfmov(pcode, cls, o2, dst);
      } else if (ins->l.i != mctarg->gprscratch && ins->r.i != mctarg->gprscratch) {
         dst = reg2oper(mctarg->gprscratch);
         Xorr(pcode, cls, dst, REGZR, o1);
         Xorr(pcode, cls, o1, REGZR, o2);
         Xorr(pcode, cls, o2, REGZR, dst);
      } else {
         Xeor(pcode, cls, o1, o1, o2);
         Xeor(pcode, cls, o2, o1, o2);
         Xeor(pcode, cls, o1, o1, o2);
      }
      break;
   case Onot: /* MVN Rd, Rn ==> ORN Rd, zr, Rn */
      Xorn(pcode, cls, reg2oper(ins->reg-1), REGZR, ref2oper(ins->l));
      break;
   case Oneg:
      if (kisint(ins->cls)) /* NEG Rd, Rn ==> SUB Rd, zr, Rn */
         Xsub(pcode, cls, reg2oper(ins->reg-1), REGZR, ref2oper(ins->l));
      else
         Xfneg(pcode, cls, reg2oper(ins->reg-1), ref2oper(ins->l));
      break;
   case Oexts8: case Oexts16: case Oexts32: /* SXTB/H/W Rd, Rn ==> SBFM Rd, Rn, #0, #7/15/31 */
      Xsbfm(pcode, cls, reg2oper(ins->reg-1), ref2oper(ins->l), 0, (8<<(ins->op-Oexts8)/2)-1);
      break;
   case Oextu8: case Oextu16: /* UXTB/H Rd, Rn ==> UBFM Rd, Rn, #0, #7/15 */
      Xubfm(pcode, cls, reg2oper(ins->reg-1), ref2oper(ins->l), 0, (8<<(ins->op-Oexts8)/2)-1);
      break;
   case Ocvts32f: X2 = Xscvtfw; goto Cvt;
   case Ocvts64f: X2 = Xscvtfx; goto Cvt;
   case Ocvtf32s:
      X2 = cls == KI32 ? Xfcvtzsw : Xfcvtzsx;
      cls = KF32;
      goto Cvt;
   case Ocvtf64s:
      X2 = cls == KI32 ? Xfcvtzsw : Xfcvtzsx;
      cls = KF64;
      goto Cvt;
   case Ocvtu32f: X2 = Xucvtfw; goto Cvt;
   case Ocvtu64f: X2 = Xucvtfx; goto Cvt;
   case Ocvtf32u:
      X2 = cls == KI32 ? Xfcvtzuw : Xfcvtzux;
      cls = KF32;
      goto Cvt;
   case Ocvtf64u:
      X2 = cls == KI32 ? Xfcvtzuw : Xfcvtzux;
      cls = KF64;
      goto Cvt;
   case Ocvtf32f64: cls = KF32; X2 = Xfcvtsd; goto Cvt;
   case Ocvtf64f32: cls = KF32; X2 = Xfcvtds; goto Cvt;
   Cvt:
      X2(pcode, cls, reg2oper(ins->reg-1), ref2oper(ins->l));
      break;
   case Obswap32: case Obswap64:
      Xrev(pcode, cls, reg2oper(ins->reg-1), ref2oper(ins->l));
      break;
   case Oadd: X3 = kisint(cls) ? Xadd : Xfadd; goto ALU3;
   case Osub: X3 = kisint(cls) ? Xsub : Xfsub; goto ALU3;
   case Omul: if (kisflt(cls)) { X3 = Xfmul; goto ALU3; }
      /* MUL Rd,Rn,Rm ==> MADD Rd,Rn,Rm,zr */
      Xmadd(pcode, cls, reg2oper(ins->reg-1), ref2oper(ins->l), ref2oper(ins->r), REGZR);
      break;
   case Omsub:
      Xmsub(pcode, cls, reg2oper(ins->reg-1), ref2oper(ins->l), ref2oper(ins->r), ref2oper(ins->oper[2]));
      break;
   case Odiv: X3 = kisint(cls) ? Xsdiv : Xfdiv; goto ALU3;
   case Oudiv: X3 = Xudiv; goto ALU3;
   case Oand: X3 = Xand; goto ALU3;
   case Oior: X3 = Xorr; goto ALU3;
   case Oxor: X3 = Xeor; goto ALU3;
   ALU3:
      X3(pcode, cls, reg2oper(ins->reg-1), ref2oper(ins->l), ref2oper(ins->r));
      break;
   case Oshl:
      if (ins->r.t == RICON) {
         uint nbit = cls == KI32 ? 32 : 64, s = ins->r.i & (nbit-1);
         assert(s > 0);
         Xubfm(pcode, cls, reg2oper(ins->reg-1), ref2oper(ins->l), nbit-s, nbit-s-1);
      } else {
         X3 = Xlslv;
         goto ALU3;
      }
      break;
   case Oslr:
      if (ins->r.t == RICON) {
         uint nbit = cls == KI32 ? 32 : 64, s = ins->r.i & (nbit-1);
         assert(s > 0);
         Xubfm(pcode, cls, reg2oper(ins->reg-1), ref2oper(ins->l), s, nbit-1);
      } else {
         X3 = Xlsrv;
         goto ALU3;
      }
      break;
   case Osar:
      if (ins->r.t == RICON) {
         uint nbit = cls == KI32 ? 32 : 64, s = ins->r.i & (nbit-1);
         assert(s > 0);
         Xsbfm(pcode, cls, reg2oper(ins->reg-1), ref2oper(ins->l), s, nbit-1);
      } else {
         X3 = Xasrv;
         goto ALU3;
      }
      break;
   case Oequ: case Oneq:
      if (!ins->reg && cmpzero2branchok(blk, ins)) /* handled by emitbranch for CBZ/CBNZ */
         break;
   case Olth: case Ogth: case Olte: case Ogte:
   case Oulth: case Ougth: case Oulte: case Ougte:
      if (kisflt(cls))
         Xfcmp(pcode, cls, ref2oper(ins->l), ref2oper(ins->r));
      else /* CMP ... ==> SUBS zr, ... */
         Xsubs(pcode, cls, REGZR, ref2oper(ins->l), ref2oper(ins->r));
      if (ins->reg) {
         enum cc cc = (kisflt(cls) ? fcmpop2cc : icmpop2cc)[ins->op];
         dst = reg2oper(ins->reg-1);
         assert(dst.reg < R(31));
         W32(0x1A9F07E0 | (cc^1)<<12 | dst.reg); /* CSET Wd, <invcond> */
      }
      break;
   case Oloadu8: X2 = Xldrb; goto Load;
   case Oloads8: X2 = Xldrsb; goto Load;
   case Oloadu16: X2 = Xldrh; goto Load;
   case Oloads16: X2 = Xldrsh; goto Load;
   case Oloads32:
      if (cls != KI32) {
         X2 = Xldrsw;
         goto Load;
      }
   case Oloadu32:
      cls = KI32;
      /* fallthru */
   case Oloadi64: X2 = Xldr;
   Load:
      X2(pcode, cls, reg2oper(ins->reg-1),
            genmemoper(pcode, ins->cls, 1<<(ins->op - Oloads8)/2, ins->l));
      break;
   case Oloadf32: case Oloadf64:
      Xfldr(pcode, cls, reg2oper(ins->reg-1),
            genmemoper(pcode, ins->cls, ins->op == Oloadf32 ? 4 : 8, ins->l));
      break;
   case Ostorei8:  cls = KI32; X2 = Xstrb; goto Store;
   case Ostorei16: cls = KI32; X2 = Xstrh; goto Store;
   case Ostorei32: cls = KI32; X2 = Xstr; goto Store;
   case Ostorei64: cls = KI64; X2 = Xstr;
   Store:
      X2(pcode, cls, ins->r.bits == ZEROREF.bits ? REGZR : ref2oper(ins->r),
            genmemoper(pcode, ins->cls, 1<<(ins->op-Ostorei8), ins->l));
      break;
   case Ostoref32: case Ostoref64:
      Xfstr(pcode, KF32 + ins->op-Ostoref32, ref2oper(ins->r),
            genmemoper(pcode, ins->cls, ins->op == Oloadf32 ? 4 : 8, ins->l));
      break;
   case Ocall:
      Xcall(pcode, ref2oper(ins->l));
      break;
   case Oxvaprologue:
      Xvaprologue(pcode, fn, genmemoper(pcode, KPTR, 8, ins->l));
      break;
   }
}

static void
prologue(uchar **pcode, Frame *frame, Function *fn)
{
   *frame = (Frame){0};
   regset save = frame->save = fn->regusage & mctarg->rcallee;
   Oper adr = mkoper(OMEM, .m = {.mode = APREIDX, .base = SP, .disp = -16});
   if (save) {
      int prev = 0;
      struct RPair *p = frame->pairs;
      for (uint reg = V(8); reg <= V(15); ++reg) {
         if (!rstest(save, reg)) continue;
         if (prev) {
            *p++ = (struct RPair) {prev, reg};
            ++frame->nfpairs;
            prev = 0;
         } else prev = reg;
      }
      if (prev) {
         frame->single[frame->nsingle++] = prev;
         prev = 0;
      }
      for (uint reg = R(19); reg < FP; ++reg) {
         if (!rstest(save, reg)) continue;
         if (prev) {
            *p++ = (struct RPair) {prev, reg};
            ++frame->ngpairs;
            prev = 0;
         } else prev = reg;
      }
      if (prev) frame->single[frame->nsingle++] = prev;

      p = frame->pairs;
      for (int i = 0; i < frame->nfpairs; ++i, ++p) {
         Xfstp(pcode, KF64, reg2oper(p->a), reg2oper(p->b), adr);
         frame->size += 16;
      }
      for (int i = 0; i < frame->ngpairs; ++i, ++p) {
         Xstp(pcode, KPTR, reg2oper(p->a), reg2oper(p->b), adr);
         frame->size += 16;
      }
      for (int i = 0; i < frame->nsingle; ++i) {
         int r = frame->single[i];
         int off = -8 - i * 8;
         (r < 32 ? Xstr : Xfstr)(pcode, r < 32 ? KI64 : KF64, reg2oper(r),
               mkoper(OMEM, .m = {.mode = AIMMIDX, .base = SP, .disp = off}));
         adr.m.disp -= 8;
         frame->size += 8;
      }
   }
   /* ensure stack is 16-byte aligned  */
   if (((fn->stksiz + frame->size) & 0xF) != 0) {
      fn->stksiz += 8;
   }
   frame->size += fn->stksiz;
   if ((frame->usefp = !fn->isleaf)) {
      frame->size += 16;
      adr.m.disp -= fn->stksiz;
      if (opermatch(PMEMPREPOSTXP, KPTR, adr)) {
         Xstp(pcode, KPTR, reg2oper(FP), reg2oper(LR), adr);
      } else {
         /* disp too large for stp immediate, do subtract + stp */
         Oper disp = imm_or_reg(pcode, KI32, PU12SL12, -adr.m.disp);
         Xsub(pcode, KPTR, reg2oper(SP), reg2oper(SP), disp);
         adr.m.mode = AIMMIDX, adr.m.disp = 0;
         Xstp(pcode, KPTR, reg2oper(FP), reg2oper(LR), adr);
      }
      Xadd(pcode, KPTR, reg2oper(R(29)), reg2oper(SP), mkoper(OIMM, .imm=0)); /* MOV x29,sp */
   } else if (fn->stksiz) {
      Xsub(pcode, KPTR, reg2oper(SP), reg2oper(SP),
         imm_or_reg(pcode, KI32, PU12SL12, fn->stksiz + 8*frame->nsingle));
   }
   frame->stksiz = fn->stksiz;
}

static void
epilogue(uchar **pcode, Function *fn, Frame *frame)
{
   Oper adr = mkoper(OMEM, .m = {.mode = APOSTIDX, .base = SP, .disp = 16+fn->stksiz+8*frame->nsingle});
   if (frame->usefp) {
      if (opermatch(PMEMPREPOSTXP, KPTR, adr)) {
         Xldp(pcode, KPTR, reg2oper(FP), reg2oper(LR), adr);
      } else {
         /* disp too large for stp immediate, do add + stp */
          Oper disp = imm_or_reg(pcode, KI32, PU12SL12, adr.m.disp);
          adr.m.mode = AIMMIDX, adr.m.disp = 0;
          Xldp(pcode, KPTR, reg2oper(FP), reg2oper(LR), adr);
          Xadd(pcode, KPTR, reg2oper(SP), reg2oper(SP), disp);
          adr.m.mode = APOSTIDX, adr.m.disp = 16;
       }
   } else if (fn->stksiz) {
      Xadd(pcode, KPTR, reg2oper(SP), reg2oper(SP),
         imm_or_reg(pcode, KI32, PU12SL12, fn->stksiz + 8*frame->nsingle));
   }
   if (frame->save) {
      struct RPair *p = frame->pairs + frame->nfpairs + frame->ngpairs - 1;
      for (int i = 0; i < frame->nsingle; ++i) {
         int r = frame->single[i];
         int off = -8-8*i;
         (r < 32 ? Xldr : Xfldr)(pcode, r < 32 ? KI64 : KF64, reg2oper(r),
               mkoper(OMEM, .m = {.mode = AIMMIDX, .base = SP, .disp = off}));
         adr.m.disp -= 8;
         frame->size += 8;
      }
      adr.m.disp = 16;
      for (int i = 0; i < frame->ngpairs; ++i, --p)
         Xldp(pcode, KPTR, reg2oper(p->a), reg2oper(p->b), adr);
      adr.m.disp = 16;
      for (int i = 0; i < frame->nfpairs; ++i, --p)
         Xfldp(pcode, KF64, reg2oper(p->a), reg2oper(p->b), adr);
   }
}

static void
emitbin(Function *fn)
{
   Block *blk;
   uchar **pcode = &objout.code;

   while ((*pcode - objout.textbegin) % 4) ++*pcode;
   fnstart = *pcode;
   curfnsym = fn->name;

   /** prologue **/

   prologue(pcode, &frame, fn);

   if (*pcode - fnstart > 8) {
      /* largue prologue -> largue epilogue -> transform to use single exit point */
      Block *exit = NULL;
      blk = fn->entry->lprev;
      do {
         if (blk->jmp.t == Jret) {
            if (!exit) {
               if (blk->ins.n == 0) {
                  exit = blk;
                  continue;
               } else {
                  exit = newblk(fn);
                  exit->lnext = blk->lnext;
                  exit->lprev = blk;
                  blk->lnext = exit;
                  exit->lnext->lprev = exit;
                  exit->id = fn->nblk++;
                  exit->jmp.t = Jret;
               }
            }
            blk->jmp.t = Jb;
            memset(blk->jmp.arg, 0, sizeof blk->jmp.arg);
            blk->s1 = exit;
         } else if (exit) {
            /* thread jumps to the exit block */
            if (blk->s1 && !blk->s1->ins.n && blk->s1->s1 == exit && !blk->s1->s2) blk->s1 = exit;
            if (blk->s2 && !blk->s2->ins.n && blk->s2->s1 == exit && !blk->s2->s2) blk->s2 = exit;
         }
      } while ((blk = blk->lprev) != fn->entry);
   }

   blkaddr = allocz(fn->passarena, fn->nblk * sizeof *blkaddr, 0);

   blk = fn->entry;
   do {
      struct BlkAddr *bb = &blkaddr[blk->id];
      uint bbaddr = *pcode - objout.textbegin;
      assert(!bb->resolved);
      while (bb->relreloc) {
         int disp = (bbaddr - bb->relreloc)/4;
         assert(disp >= -(1<<18) && disp < (1<<18));
         uint tmp = rd32targ(objout.textbegin + bb->relreloc);
         wr32le(objout.textbegin + bb->relreloc, (tmp &~ (0x7FFFFu<<5)) | (disp & 0x7FFFF)<<5);
         bb->relreloc = tmp>>5 & 0x7FFFF;
      }
      bb->resolved = 1;
      bb->addr = bbaddr;

      for (int i = 0; i < blk->ins.n; ++i)
         emitinstr(pcode, fn, blk, i, &instrtab[blk->ins.p[i]]);
      if (blk->jmp.t == Jret) {
         if (blk->lnext != fn->entry && blk->lnext->jmp.t == Jret && blk->lnext->ins.n == 0)
            continue; /* fallthru to next blk's RET */
         epilogue(pcode, fn, &frame);
         W32(0xD65F03C0); /* RET */
      } else if (blk->jmp.t == Jtrap) {
         W32(0xD4200020); /* BRK #0x1 */
      } else emitbranch(pcode, blk);
   } while ((blk = blk->lnext) != fn->entry);
   objdeffunc(fn->name, fn->globl, fnstart - objout.textbegin, *pcode - fnstart);
}

void
aarch64_emit(Function *fn)
{
   fn->stksiz = alignup(fn->stksiz, 8);
   if (fn->stksiz > 1<<24) error(NULL, "'%s' stack frame too big", fn->name);
   emitbin(fn);
}

/* vim:set ts=3 sw=3 expandtab: */
