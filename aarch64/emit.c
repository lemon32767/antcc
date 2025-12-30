#include "all.h"
#include "../obj/obj.h"
#include "../endian.h"

/* References:
 *    ARM ARM https://developer.arm.com/documentation/ddi0628/aa/?lang=en
 *    AAELF ABI https://github.com/ARM-software/abi-aa/blob/main/aaelf64/aaelf64.rst
 */

enum operkind { ONONE, OREGZR, OREG, OIMM, OMEM, OSYM };
enum shiftkind { SLSL, SLSR, SASR, SROR };
enum addrmode { AIMMIDX, AREGIDX, APREIDX, APOSTIDX };
enum addrregext { XUXTW = 2, XLSL = 3, XSXTW = 6, XSXTX = 7 };
struct oper {
   uchar t;
   union {
      struct { /* OREG (opt. shifted) */
         uchar reg;
         uchar shft : 2, /* enum shiftkind */
               shamt : 6;
      };
      struct { /* OMEM */
         uchar mode : 3; /* enum addrmode */
         uchar base : 5; /* reg */
         union {
            struct {
               uchar index : 5; /* reg */
               uchar ext : 3; /* enum addrregext */
               uchar shamt;
            };
            short disp;
         };
      } m;
      vlong imm; uvlong uimm; /* OIMM */
      struct { /* OSYM */
         ushort con;
         int cdisp;
      };
   };
};

#define REGZR ((struct oper){OREGZR, .reg=31})
#define mkoper(t, ...) ((struct oper){(t), __VA_ARGS__})
#define reg2oper(r) (assert((uint)(r) <= V(31)), mkoper(OREG, .reg = (r)))

static struct oper
mkmemoper(uint msiz, union ref r)
{
   if (r.t == RTMP) {
      assert(in_range(instrtab[r.i].reg-1, R0, SP));
      return mkoper(OMEM, .m = {AIMMIDX, .base = instrtab[r.i].reg-1});
   } else if (r.t == RREG) {
      return mkoper(OMEM, .m = {AIMMIDX, .base = r.i});
   } else if (r.t == RADDR) {
      const struct addr *addr = &addrht[r.i];
      assert(addr->shift <= 3 && (!addr->disp || !addr->index.bits));
      if (isaddrcon(addr->base,0)) {
         assert(!addr->index.bits);
         return mkoper(OSYM, .con = addr->base.i, .cdisp = addr->disp);
      }
      assert(addr->base.t == RREG);
      if (!addr->index.bits) {
         return mkoper(OMEM, .m = {.mode = AIMMIDX, .base = addr->base.i, .disp = addr->disp});
      } else {
         assert(addr->index.t == RREG);
         assert(addr->shift == 0 || 8<<addr->shift == msiz);
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

static struct oper
ref2oper(union ref r)
{
   switch (r.t) {
   case RTMP: return instrtab[r.i].reg ? mkoper(ONONE,) : reg2oper(instrtab[r.i].reg-1);
   case RREG: return reg2oper(r.i);
   case RICON: return mkoper(OIMM, .imm = r.i);
   case RXCON:
      if (conht[r.i].cls == KI32)
         return mkoper(OIMM, .imm = conht[r.i].i);
      else if (conht[r.i].cls == KI64) {
         vlong i = conht[r.i].i;
         return mkoper(OIMM, .imm = i);
      } else if (!conht[r.i].cls) {
         return mkoper(OSYM, .con = r.i);
      }
      assert(0);
   //case RADDR: return mkmemoper(r);
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
   PMEMAREG, /* addr reg offset, optionally left shifted */
   PSYM, /* symbol */
};
enum operenc {
   EN_ADDSUBEXT3R, /* add/sub-ext-reg */
   EN_ADDSUBSHFT3R, /* add/sub-shift-reg */
   EN_LOGSHFT3R, /* logical/shifted-reg */
   EN_ARITH2R, /* data-processing/1src */
   EN_ARITH3R, /* data-processing/2src */
   EN_ARITH4R, /* data-processing/3src */
   EN_ADDSUBIMM, /* add/subtract-imm */
   EN_LOGIMM, /* logical-imm */
   EN_MOVEIMM, /* move/wide-imm */
   EN_MEMAIMM, /* load/store/unsigned-imm */
   EN_MEMAIMMH, /* load/store/unsigned-imm (halfword) */
   EN_MEMAIMMW, /* load/store/unsigned-imm (word) */
   EN_MEMAIMMX, /* load/store/unsigned-imm (doubleword) */
   EN_MEMAPREPOST, /* load/store/pre/postidx-imm */
   EN_MEMAREG, /* load/store/reg-offset */
   EN_MEMPPREPOST, /* load/store-pair/pre/postidx-imm */
   EN_ADRSYMLO21, /* for ADR <sym> */
   EN_ADRSYMPGHI21, /* for ADRP <sym:pghi21> */
   EN_ADDSYMLO12, /* for ADD x,x, <sym:lo12> */
};
struct desc {
   uchar psiz; /* subset of {4,8} */
   uchar pt[3]; /* bitsets of enum operpat, up to 3 operands */
   uint opc;
   uchar operenc; /* enum operenc */
};

/* match operand against pattern */
static inline bool
opermatch(enum operpat pat, enum irclass k, struct oper o)
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
   case PSYM:  return o.t == OSYM;
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
      return o.t == OMEM && o.m.mode == AIMMIDX && (uint)o.m.disp < (1<<15) && !(o.m.disp % 8);
   case PMEMAREG:
      return o.t == OMEM && o.m.mode == AREGIDX;
   case PMEMPREPOST:
      return o.t == OMEM && (o.m.mode == APREIDX || o.m.mode == APOSTIDX
                         || (o.m.mode == AIMMIDX && o.m.disp >= -256 && o.m.disp < 256));
   }
   assert(0);
}

/* code output helpers */
#define W32(w) (wr32targ(*pcode, (w)), *pcode += 4)

static uchar *fnstart;
static internstr curfnsym;
static bool usebp;
static int rbpoff;

/* Given an instruction description table, find the first entry that matches
 * the operands and encode it. */
static void
encode(uchar **pcode, const struct desc *tab, int ntab, enum irclass k, struct oper o[3])
{
   const struct desc *en = NULL;
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
   case EN_ADDSUBSHFT3R:
   case EN_LOGSHFT3R:
      ins |= sf<<31 | o[2].shft<<22 | o[2].reg<<16 | o[2].shamt<<10 | o[1].reg<<5 | o[0].reg;
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
      ins |= o[1].m.disp<<10 | o[1].m.base<<5 | o[0].reg;
      break;
   case EN_MEMAIMMH: o[1].m.disp >>= 1; goto AImm;
   case EN_MEMAIMMW: o[1].m.disp >>= 2; goto AImm;
   case EN_MEMAIMMX: o[1].m.disp >>= 3; goto AImm;
   case EN_MEMAPREPOST:
      ins |= (o[1].m.disp&0x1FF)<<12 | o[1].m.base<<5 | o[0].reg;
      if (o[1].m.mode == APREIDX) ins |= 3<<10;
      else if (o[1].m.mode == APOSTIDX) ins |= 1<<10;
      break;
   case EN_MEMAREG:
      assert(o[1].m.shamt <= 1);
      ins |= o[1].m.index<<16 | o[1].m.ext<<13 | o[1].m.shamt<<12 | o[1].m.base<<5 | o[0].reg;
      break;
   case EN_MEMPPREPOST:
      assert(o[2].m.disp % 8 == 0);
      ins |= (o[2].m.disp/8&0x7F)<<15 | o[1].reg<<10 | o[2].m.base<<5 | o[0].reg;
      if (o[2].m.mode == APREIDX) ins |= 3<<23;
      else if (o[2].m.mode == APOSTIDX) ins |= 1<<23;
      else ins |= 2<<23;
      break;
   case EN_ADRSYMLO21:
      ins |= o[0].reg;
      objreloc(xcon2sym(o[1].con), REL_ADR_PREL_LO21, Stext, *pcode - objout.textbegin, 0);
      break;
   case EN_ADRSYMPGHI21:
      ins |= o[0].reg;
      objreloc(xcon2sym(o[1].con), REL_ADR_PREL_PG_HI21, Stext, *pcode - objout.textbegin, 0);
      break;
   case EN_ADDSYMLO12:
      ins |= sf<<31 | o[1].reg<<5 | o[0].reg;
      objreloc(xcon2sym(o[2].con), REL_ADD_ABS_LO12_NC, Stext, *pcode - objout.textbegin, 0);
      break;
   }
   W32(ins);
}
#define DEFINSTR1(X, ...)                                          \
   static void                                                     \
   X(uchar **pcode, enum irclass k, struct oper a)                 \
   {                                                               \
      static const struct desc tab[] = { __VA_ARGS__ };            \
      encode(pcode, tab, countof(tab), k, ((struct oper [3]){a})); \
   }

#define DEFINSTR2(X, ...)                                                \
   static void                                                           \
   X(uchar **pcode, enum irclass k, struct oper op1, struct oper op2)    \
   {                                                                     \
      static const struct desc tab[] = { __VA_ARGS__ };                  \
      encode(pcode, tab, countof(tab), k, ((struct oper [3]){op1,op2})); \
   }
#define DEFINSTR3(X, ...)                                                              \
   static void                                                                         \
   X(uchar **pcode, enum irclass k, struct oper op1, struct oper op2, struct oper op3) \
   {                                                                                   \
      static const struct desc tab[] = { __VA_ARGS__ };                                \
      encode(pcode, tab, countof(tab), k, ((struct oper [3]){op1,op2,op3}));           \
   }

DEFINSTR2(Xadrp,
   {8, {PGPRZ, PSYM}, 0x90000000, EN_ADRSYMPGHI21} /* ADR (sym pg hi21) */
)
DEFINSTR2(Xadr,
   {8, {PGPRZ, PSYM}, 0x10000000, EN_ADRSYMLO21} /* ADR (sym pg hi21) */
)

DEFINSTR3(Xadd,
   {4|8, {PGPRSP, PGPRSP, PU12SL12}, 0x11000000, EN_ADDSUBIMM}, /* ADD (immediate) */
   {4|8, {PGPRZ,  PGPRZ, PGPRZSHFT}, 0x0B000000, EN_ADDSUBSHFT3R}, /* ADD (shifted register) */
   {  8, {PGPRZ,  PGPRZ, PSYM},      0x11000000, EN_ADDSYMLO12}, /* ADD (sym lo12) */
)
DEFINSTR3(Xsub,
   {4|8, {PGPRSP, PGPRSP, PU12SL12}, 0x51000000, EN_ADDSUBIMM}, /* SUB (immediate) */
   {4|8, {PGPRZ,  PGPRZ, PGPRZSHFT}, 0x4B000000, EN_ADDSUBSHFT3R}, /* SUB (shifted register) */
)
DEFINSTR3(Xsubs,
   {4|8, {PGPRZ, PGPRSP, PU12SL12}, 0x71000000, EN_ADDSUBIMM}, /* SUBS (immediate) */
   {4|8, {PGPRZ, PGPRZ, PGPRZSHFT},  0x6B000000, EN_ADDSUBSHFT3R}, /* SUBS (shifted register) */
)

static void
Xmadd(uchar **pcode, enum irclass k, struct oper d, struct oper n, struct oper m, struct oper a)
{
   assert(opermatch(PGPRZ, k, d) && opermatch(PGPRZ, k, n)
       && opermatch(PGPRZ, k, a) && opermatch(PGPRZ, k, m));
   uint sf = k > KI32;
   W32(0x1B000000 | sf<<31 | m.reg<<16 | a.reg<<10 | n.reg<<5 | d.reg);
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
DEFINSTR3(Xeor,
   {4|8, {PGPRSP, PGPRZ, PLOGIMM},   0x52000000, EN_LOGIMM}, /* EOR (immediate) */
   {4|8, {PGPRZ,  PGPRZ, PGPRZSHFT}, 0x4A000000, EN_LOGSHFT3R}, /* EOR (shifted register) */
)

static void
Xubfm(uchar **pcode, enum irclass k, struct oper rd, struct oper rn, uint immr, uint imms)
{
   uint x = k != KI32;
   uint nbit = x ? 64 : 32;
   assert(opermatch(PGPRZ, k, rd) && opermatch(PGPRZ, k, rn) && immr < nbit && imms < nbit);
   W32(x<<31 | 0x53000000 | x<<22 | immr<<16 | imms<<10 | rn.reg<<5 | rd.reg);
}
static void
Xsbfm(uchar **pcode, enum irclass k, struct oper rd, struct oper rn, uint immr, uint imms)
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
   {4, {PGPRZ, PMEMPREPOST}, 0xB8400000, EN_MEMAPREPOST}, /* LDR (immediate, (pre/postinc)) */
   {8, {PGPRZ, PMEMPREPOST}, 0xF8400000, EN_MEMAPREPOST},
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
   {8, {PGPRZ, PGPRZ, PMEMPREPOST}, 0xA8400000, EN_MEMPPREPOST} /* LDP (immediate, (pre/postinc)) */
)
DEFINSTR3(Xstp,
   {8, {PGPRZ, PGPRZ, PMEMPREPOST}, 0xA8000000, EN_MEMPPREPOST} /* STP (immediate, (pre/postinc)) */
)
static void
Xcall(uchar **pcode, struct oper dst)
{
   if (dst.t == OSYM) {
      objreloc(xcon2sym(dst.con), REL_CALL26, Stext, *pcode - objout.textbegin, 0);
      W32(0x94000000); /* BL <rel26> */
   } else {
      assert(opermatch(PGPRZ, KPTR, dst));
      W32(0xD63F0000 | dst.reg<<5); /* BLR Xn */
   }
}

static void
gencopy(uchar **pcode, enum irclass cls, struct block *blk, int curi, struct oper dst, union ref val)
{
   assert(dst.t == OREG);
   if (val.bits == UNDREF.bits) return;
   if (kisint(cls) && isintcon(val)) {
      /* MOV r, #imm */
      uvlong u = intconval(val);
      if (~u <= 0xFFFF) {
         /* immediate can be encoded with 1 MOVN instruction */
         Xmovn(pcode, cls, dst, mkoper(OIMM, .imm = ~u));
      } else if (u > 0xFFFF && aarch64_logimm(NULL, cls, u)) {
         /* can be encoded as a logical immediate */
         Xorr(pcode, cls, dst, REGZR, mkoper(OIMM, .uimm = u));
      } else {
         /* generate MOV (+ MOVKs) */
         if (cls == KI32) u = (uint)u;
         int s = 0;
         while (s < 48 && (u >> s & 0xFFFF) == 0) s += 16;
         Xmovz(pcode, cls, dst, mkoper(OIMM, .imm = u & (0xFFFFull << s)));
         for (s += 16; s <= 48; s += 16) {
            if ((u >> s) & 0xFFFF)
               Xmovk(pcode, cls, dst, mkoper(OIMM, .imm = u & (0xFFFFull << s)));
         }
      }
   } else if (val.t == RREG || val.t == RTMP) {
      Xorr(pcode, cls, dst, REGZR, ref2oper(val)); /* MOV Rd, Rn ==> ORR Rd, zr, Rn */
   } else if (isaddrcon(val,0)) {
      struct oper sym = mkoper(OSYM, .con = val.i);
      if ((ccopt.pic || (conht[val.i].flag & SFUNC)) && !(conht[val.i].flag & SLOCAL)) {
         Xadrp(pcode, KPTR, dst, sym);
         Xadd(pcode, KPTR, dst, dst, sym);
      } else {
         Xadr(pcode, KPTR, dst, sym);
      }
   } else assert(0);
}

/* maps blk -> address when resolved; or to linked list of jump displacement
 * relocations */
static struct blkaddr {
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
Xbcc(uchar **pcode, enum cc cc, struct block *dst)
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
Xcbcc(uchar **pcode, enum irclass k, uint rt, enum cc cc, struct block *dst)
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

/* condition code for CMP */
static const schar icmpop2cc[] = {
   [Oequ]  = CCEQ, [Oneq]  = CCNE,
   [Olth]  = CCLT, [Ogth]  = CCGT, [Olte]  = CCLE, [Ogte]  = CCGE,
   [Oulth] = CCLO, [Ougth] = CCHI, [Oulte] = CCLS, [Ougte] = CCHS,
}, fcmpop2cc[] = {
   [Oequ] = CCEQ, [Oneq] = CCNE,
   [Olth] = CCLO, [Ogth] = CCGT, [Olte] = CCLS, [Ogte] = CCGE,
};

static void
emitbranch(uchar **pcode, struct block *blk)
{
   enum irclass cbk = 0;
   struct oper cbopr;
   enum cc cc = CCAL;
   assert(blk->s1);
   if (blk->s2) {
      /* conditional branch.. */
      union ref arg = blk->jmp.arg[0];
      assert(arg.t == RTMP);
      struct instr *ins = &instrtab[arg.i];
      if (in_range(ins->op, Oequ, Oneq) && ins->r.bits == ZEROREF.bits) {
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
         struct block *tmp = blk->s1;
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
emitinstr(uchar **pcode, struct function *fn, struct block *blk, int curi, struct instr *ins)
{
   struct oper dst, o1, o2;
   enum irclass cls = ins->cls;
   void (*X3)(uchar **, enum irclass, struct oper, struct oper, struct oper) = NULL;
   void (*X2)(uchar **, enum irclass, struct oper, struct oper) = NULL;

   switch (ins->op) {
   default: fatal(NULL, "aarch64 unimplemented instr: %s", opnames[ins->op]);
   case Onop: break;
   case Omove:
      dst = ref2oper(ins->l);
      gencopy(pcode, cls, blk, curi, dst, ins->r);
      break;
   case Oextu32: cls = KI32;
      /* fallthru */
   case Ocopy:
      dst = reg2oper(ins->reg-1);
      gencopy(pcode, cls, blk, curi, dst, ins->l);
      break;
   case Oswap:
      o1 = ref2oper(ins->l), o2 = ref2oper(ins->r);
      if (ins->l.i != mctarg->gprscratch && ins->r.i != mctarg->gprscratch) {
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
   case Oneg: /* NEG Rd, Rn ==> SUB Rd, zr, Rn */
      Xsub(pcode, cls, reg2oper(ins->reg-1), REGZR, ref2oper(ins->l));
      break;
   case Oexts8: case Oexts16: case Oexts32: /* SXTB/H/W Rd, Rn ==> SBFM Rd, Rn, #0, #7/15/31 */
      Xsbfm(pcode, cls, reg2oper(ins->reg-1), ref2oper(ins->l), 0, (8<<(ins->op-Oexts8)/2)-1);
      break;
   case Oextu8: case Oextu16: /* UXTB/H Rd, Rn ==> UBFM Rd, Rn, #0, #7/15 */
      Xubfm(pcode, cls, reg2oper(ins->reg-1), ref2oper(ins->l), 0, (8<<(ins->op-Oexts8)/2)-1);
      break;
   case Oadd: X3 = Xadd; goto ALU3;
   case Osub: X3 = Xsub; goto ALU3;
   case Oand: X3 = Xand; goto ALU3;
   case Oior: X3 = Xorr; goto ALU3;
   case Oxor: X3 = Xeor; goto ALU3;
   ALU3:
      X3(pcode, cls, reg2oper(ins->reg-1), ref2oper(ins->l), ref2oper(ins->r));
      break;
   case Omul:
      /* MUL Rd,Rn,Rm ==> MADD Rd,Rn,Rm,zr */
      Xmadd(pcode, cls, reg2oper(ins->reg-1), ref2oper(ins->l), ref2oper(ins->r), REGZR);
      break;
   case Odiv:
      Xsdiv(pcode, cls, reg2oper(ins->reg-1), ref2oper(ins->l), ref2oper(ins->r));
      break;
   case Oudiv:
      Xudiv(pcode, cls, reg2oper(ins->reg-1), ref2oper(ins->l), ref2oper(ins->r));
      break;
   case Oshl:
      if (ins->r.t == RICON) {
         uint nbit = cls == KI32 ? 32 : 64, s = ins->r.i & nbit-1;
         assert(s > 0);
         Xubfm(pcode, cls, reg2oper(ins->reg-1), ref2oper(ins->l), nbit-s, nbit-s-1);
      } else assert(!"nyi lslv");
      break;
   case Oslr:
      if (ins->r.t == RICON) {
         uint nbit = cls == KI32 ? 32 : 64, s = ins->r.i & nbit-1;
         assert(s > 0);
         Xubfm(pcode, cls, reg2oper(ins->reg-1), ref2oper(ins->l), s, nbit-1);
      } else assert(!"nyi lsrv");
      break;
   case Osar:
      if (ins->r.t == RICON) {
         uint nbit = cls == KI32 ? 32 : 64, s = ins->r.i & nbit-1;
         assert(s > 0);
         Xsbfm(pcode, cls, reg2oper(ins->reg-1), ref2oper(ins->l), s, nbit-1);
      } else assert(!"nyi lsrv");
      break;
   case Oequ: case Oneq:
      if (!ins->reg && ins->r.bits == ZEROREF.bits) break; /* handled by emitbranch for CBZ/CBNZ */
   case Olth: case Ogth: case Olte: case Ogte:
   case Oulth: case Ougth: case Oulte: case Ougte:
      /* CMP ... ==> SUBS zr, ... */
      Xsubs(pcode, ins->cls, REGZR, ref2oper(ins->l), ref2oper(ins->r));
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
      /* fallthru */
   case Oloadu32:
      cls = KI32;
      /* fallthru */
   case Oloadi64: X2 = Xldr;
   Load:
      X2(pcode, cls, reg2oper(ins->reg-1), mkmemoper(8<<(ins->op - Oloads8)/2, ins->l));
      break;
   case Ostore8:  cls = KI32; X2 = Xstrb; goto Store;
   case Ostore16: cls = KI32; X2 = Xstrh; goto Store;
   case Ostore32: cls = KI32; X2 = Xstr; goto Store;
   case Ostore64: cls = KI64; X2 = Xstr;
   Store:
      X2(pcode, cls, ref2oper(ins->r), mkmemoper(8<<(ins->op-Ostore8), ins->l));
      break;
   case Ocall:
      Xcall(pcode, ref2oper(ins->l));
      break;
   }
}

static bool
calleesave(int *npush, uchar **pcode, struct function *fn)
{
   regset save = (fn->regusage & mctarg->rcallee) | (usebp * BIT(FP)) | (!fn->isleaf * BIT(LR));
   if (!save) return 0;
   int prev = 0;
   bool zr = popcnt(save) & 1;
   for (uint reg = R(19); reg <= LR; ++reg) {
      if (!rstest(save, reg)) continue;
      if (zr) {
         zr = 0;
         Xstp(pcode, KPTR, reg2oper(reg), REGZR,
               mkoper(OMEM, .m = {.mode = APREIDX, .base = SP, .disp = -16}));
      } else if (prev) {
         *npush += 2;
         Xstp(pcode, KPTR, reg2oper(prev), reg2oper(reg),
               mkoper(OMEM, .m = {.mode = APREIDX, .base = SP, .disp = -16}));
         prev = 0;
      } else prev = reg;
   }
   return 1;
}

static void
calleerestore(uchar **pcode, struct function *fn)
{
   regset save = (fn->regusage & mctarg->rcallee) | (usebp * BIT(FP)) | (!fn->isleaf * BIT(LR));
   if (!save) return;
   int prev = 0;
   for (uint reg = LR; reg >= R(19); --reg) {
      if (!rstest(save, reg)) continue;
      if (prev) {
         Xldp(pcode, KPTR, reg2oper(reg), reg2oper(prev),
               mkoper(OMEM, .m = {.mode = APOSTIDX, .base = SP, .disp = 16}));
         prev = 0;
      } else prev = reg;
   }
   if (prev) {
      Xldp(pcode, KPTR, reg2oper(prev), REGZR,
            mkoper(OMEM, .m = {.mode = APOSTIDX, .base = SP, .disp = 16}));
      prev = 0;
   }
}

static void
emitbin(struct function *fn)
{
   struct block *blk;
   uchar **pcode = &objout.code;
   int npush = 0;

   fnstart = *pcode;
   curfnsym = fn->name;

   /** prologue **/

   /* only use frame pointer in non-leaf functions and functions that use the stack */
   usebp = !fn->isleaf || fn->stksiz;
   calleesave(&npush, pcode, fn);
   if (usebp) {
      /* MOV x29, sp */
      Xadd(pcode, KPTR, reg2oper(FP), reg2oper(SP), mkoper(OIMM,));
   }

   /* ensure stack is 16-byte aligned for function calls */
   if (!fn->isleaf && ((fn->stksiz + npush*8) & 0xF) != 0) {
      assert(usebp);
      if ((rbpoff & 0xF) == 0) {
         rbpoff -= 16;
         fn->stksiz += 24;
      } else {
         rbpoff -= 8;
         fn->stksiz += 8;
      }
   }

   if (*pcode - fnstart > 8) {
      /* largue prologue -> largue epilogue -> transform to use single exit point */
      struct block *exit = NULL;
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
      struct blkaddr *bb = &blkaddr[blk->id];
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
         /* epilogue */
         calleerestore(pcode, fn);
         W32(0xD65F03C0); /* RET */
      } else if (blk->jmp.t == Jtrap) {
         W32(0xD4200020); /* BRK #0x1 */
      } else emitbranch(pcode, blk);
   } while ((blk = blk->lnext) != fn->entry);
   objdeffunc(fn->name, fn->globl, fnstart - objout.textbegin, *pcode - fnstart);
}

void
aarch64_emit(struct function *fn)
{
   fn->stksiz = alignup(fn->stksiz, 8);
   if (fn->stksiz > 1<<24) error(NULL, "'%s' stack frame too big", fn->name);
   emitbin(fn);
}

/* vim:set ts=3 sw=3 expandtab: */
