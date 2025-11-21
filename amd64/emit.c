#include "all.h"
#include "../obj/obj.h"
#include "../endian.h"

/** Instruction operands **
 *
 * Can be a register, a 32-bit immediate,
 * a memory reference [base + index * scale + disp],
 * or a relocatable reference to some symbol plus a displacement and maybe index*scale
 */
enum operkind { ONONE, OREG, OIMM, OMEM, OSYM };
enum { NOBASE = 63, NOINDEX = 63 };
struct oper {
   uchar t;
   union {
      struct { uchar base; }; /* OMEM */
      struct { uchar cindex : 6, cshift : 2; }; /* OSYM */
   };
   union {
      struct { uchar index, shift; }; /* OMEM */
      ushort con; /* OSYM */
   };
   union {
      uchar reg; /* OREG */
      int disp; /* OMEM, OSYM */
      int imm; /* OIMM */
   };
};
#define mkoper(t, ...) ((struct oper){(t), __VA_ARGS__})
#define reg2oper(R) (assert((uint)(R) <= XMM15), mkoper(OREG, .reg = (R)))

static struct oper mkmemoper(union ref);

static struct oper
ioper(int i)
{
   int reg = instrtab[i].reg - 1;
   return reg < 0 ? mkoper(ONONE,) : reg2oper(reg);
}

static struct oper
ref2oper(union ref r)
{
   switch (r.t) {
   case RTMP: return ioper(r.i);
   case RREG: return reg2oper(r.i);
   case RICON: return mkoper(OIMM, .imm = r.i);
   case RXCON:
      if (conht[r.i].cls == KI32)
         return mkoper(OIMM, .imm = conht[r.i].i);
      else if (!conht[r.i].cls)
         return mkoper(OSYM, .con = r.i, .cindex = NOINDEX);
      assert(0);
   case RADDR: return mkmemoper(r);
   default: assert(0);
   }
}

static void
addmemoper(struct oper *mem, struct oper add)
{
   assert(mem->t == OMEM);
   if (add.t == OIMM) {
      mem->disp += add.imm;
   } else if (add.t == OREG) {
      if (mem->base == NOBASE)
         mem->base = add.reg;
      else if (mem->index == NOINDEX)
         mem->index = add.reg;
      else
         assert(0);
   }
}

/* helpers to convert a reference to an operand of a specific kind,
 * with assertions to make sure nothing went wrong */

static inline struct oper
mkregoper(union ref r)
{
   assert(r.t == RREG || (r.t == RTMP && ioper(r.i).t == OREG));
   return r.t == RREG ? reg2oper(r.i) : ioper(r.i);
}

static inline struct oper
mkimmoper(union ref r)
{
   assert(iscon(r) && concls(r) == KI32);
   return mkoper(OIMM, .imm = intconval(r));
}

#define ismemref(ref) ((ref).t == RTMP && ioper((ref).i).t == OMEM)
#define isregref(ref) ((ref).t == RREG || ((ref).t == RTMP && ioper((ref).i).t == OREG))

static inline struct oper
mkimmregoper(union ref r)
{
   assert(isregref(r) || (iscon(r) && concls(r) == KI32));
   return ref2oper(r);
}

static inline struct oper
mkdatregoper(union ref r)
{
   assert(isregref(r) || (r.t == RXCON && conht[r.i].deref));
   return ref2oper(r);
}

static inline struct oper
mkimmdatregoper(union ref r)
{
   assert(isregref(r) || r.t == RICON || (r.t == RXCON && (conht[r.i].cls == KI32 || conht[r.i].deref)));
   return ref2oper(r);
}

static int rbpoff;

static struct oper
mkmemoper(union ref r)
{
   if (r.t == RTMP) {
      struct oper wop = ioper(r.i);
      if (wop.t == OMEM) return wop;
      assert(wop.t == OREG);
      return mkoper(OMEM, .base = wop.reg, .index = NOINDEX);
   } else if (r.t == RADDR) {
      const struct addr *addr = &addrht[r.i];
      struct oper mem;

      if (addr->base.t == RTMP && ioper(addr->base.i).t == OMEM) {
         mem = ioper(addr->base.i);
         if (addr->index.bits) addmemoper(&mem, mkregoper(addr->index));
         assert(!mem.shift);
         mem.shift = addr->shift;
         addmemoper(&mem, mkoper(OIMM, .imm = addr->disp));
         return mem;
      }
      if (isaddrcon(addr->base,0)) {
         return mkoper(OSYM, .con = addr->base.i,
                             .cindex = addr->index.bits ? mkregoper(addr->index).reg : NOINDEX,
                             .cshift = addr->shift,
                             .disp = addr->disp);
      } else if (isintcon(addr->base)) {
         assert(!addr->disp);
         return mkoper(OMEM, .base = NOBASE,
                          .index = addr->index.bits ? mkregoper(addr->index).reg : NOINDEX,
                          .disp = intconval(addr->base),
                          .shift = addr->shift);
      } else if (isaddrcon(addr->index,0)) {
         assert(!addr->shift);
         return mkoper(OSYM, .con = addr->index.i,
                             .cindex = addr->base.bits ? mkregoper(addr->base).reg : NOINDEX,
                             .disp = addr->disp);
      }
      return mkoper(OMEM, .base = addr->base.bits ? mkregoper(addr->base).reg : NOBASE,
                          .index = addr->index.bits ? mkregoper(addr->index).reg : NOINDEX,
                          .disp = addr->disp,
                          .shift = addr->shift);
   } else if (r.t == RXCON) {
      assert(!conht[r.i].cls);
      return mkoper(OSYM, .con = r.i, .cindex = NOINDEX);
   } else {
      return mkoper(OMEM, .base = isregref(r) ? ref2oper(r).reg : NOBASE,
                          .index = NOINDEX,
                          .disp = isregref(r) ? 0 : mkimmoper(r).imm);
   }
}

/** Instruction description tables **
 *
 * Each instruction is a list of descs, and the first one that matches
 * is emitted. Each entry has a size pattern field, which is a bitset
 * of the sizes (in bytes) that the entry matches, and 2 operand patterns,
 * which describe the operands that can match (for example, PRAX matches
 * a RAX register operand, PGPR matches any integer register, I8 matches
 * an immediate operand between [-128,127]) The rest of the fields describe
 * the instruction's encoding.
 * (reference:  https://www.felixcloutier.com/x86/  &  https://wiki.osdev.org/X86-64_Instruction_Encoding )
 */

enum operpat {
   PNONE,
   PRAX,
   PRCX,
   PGPR,
   PFPR,
   P1, /* imm = 1 */
   PN1, /* imm = -1 */
   PI8,
   PU8,
   PI16,
   PU16,
   PI32,
   PU32,
   PMEM,
   PSYM,
};
enum operenc {
   EN_R = 1,  /* reg with /r */
   EN_RR,     /* reg, reg with /r */
   EN_RRX,    /* reg, reg with /r (inverted) */
   EN_MR,     /* mem, reg with /r */
   EN_RM,     /* reg, mem with /r */
   EN_M,      /* mem */
   EN_RI8,    /* reg, imm8 with /0 */
   EN_RI32,   /* reg, imm32 with /0 */
   EN_MI8,    /* mem, imm8 with /x */
   EN_MI16,   /* mem, imm16 with /x */
   EN_MI32,   /* mem, imm32 with /x */
   EN_OI,     /* reg, imm32 with op + reg */
   EN_I8,     /* imm8 */
   EN_I32,    /* imm32 */
   EN_R32,    /* rel32 */
   NOPERENC,
};
struct desc {
   uchar psiz; /* subset of {1,2,4,8} */
   uchar ptd, pts; /* bitsets of enum operpat */
   const char *opc; /* opcode bytes */
   uchar operenc; /* enum operenc */
   uchar ext; /* ModR/M.reg opc extension */
   bool r8 : 1; /* uses 8bit register */
   bool norexw : 1; /* do not use REX.W even if size is 64 bits */
};

/* match operand against pattern */
static inline bool
opermatch(enum operpat pat, struct oper oper)
{
   switch (pat) {
   case PNONE: return !oper.t;
   case PRAX: return oper.t == OREG && oper.reg == RAX;
   case PRCX: return oper.t == OREG && oper.reg == RCX;
   case PGPR: return oper.t == OREG && oper.reg <= R15;
   case PFPR: return oper.t == OREG && oper.reg >= XMM0;
   case P1:   return oper.t == OIMM && oper.imm == 1;
   case PN1:  return oper.t == OIMM && oper.imm == -1;
   case PI8:  return oper.t == OIMM && (schar)oper.imm == oper.imm;
   case PU8:  return oper.t == OIMM && (uchar)oper.imm == oper.imm;
   case PI16: return oper.t == OIMM && (short)oper.imm == oper.imm;
   case PU16: return oper.t == OIMM && (ushort)oper.imm == oper.imm;
   case PI32: return oper.t == OIMM;
   case PU32: return oper.t == OIMM && oper.imm >= 0;
   case PMEM: return in_range(oper.t, OMEM, OSYM);
   case PSYM: return oper.t == OSYM;
   }
   assert(0);
}

/* code output helpers */
#define B(b) (*(*pcode)++ = (b))
#define D(xs, N) (memcpy(*pcode, (xs), (N)), (*pcode) += (N))
#define I16(w) (wr16le(*pcode, (w)), *pcode += 2)
#define I32(w) (wr32le(*pcode, (w)), *pcode += 4)
#define DS(S) D(S, sizeof S - 1)

static bool usebp; /* use RBP? */
static const char *curfnsym;
static uchar *fnstart;

/* Given an instruction description table, find the first entry that matches
 * the operands (where dst, src are the operands in intel syntax order) and encode it */
static void
encode(uchar **pcode, const struct desc *tab, int ntab, enum irclass k, struct oper dst, struct oper src)
{
   const uchar *opc;
   int nopc, mod, rex;
   const char *sym;
   uchar reg;
   struct oper mem;
   bool sib = 0;
   const struct desc *en = NULL;

   for (int i = 0; i < ntab; ++i) {
      if ((tab[i].psiz & cls2siz[k]) && opermatch(tab[i].ptd, dst) && opermatch(tab[i].pts, src)) {
         en = &tab[i];
         break;
      }
   }
   assert(en && "no match for instr");

   if (en->ptd == PFPR) dst.reg &= 15;
   if (en->pts == PFPR) src.reg &= 15;
   opc = (uchar *)en->opc;
   nopc = strlen(en->opc);
   /* mandatory prefixes go before REX */
   if (*opc == 0x66 || *opc == 0xF2 || *opc == 0xF3)
      B(*opc++), --nopc;
   rex = in_range(k, KI64, KPTR) << 3; /* REX.W */
   if (en->norexw) rex = 0;
   switch (en->operenc) {
   case EN_RR: /* mod = 11; reg = dst; rm = src */
      rex |= (dst.reg >> 3) << 2; /* REX.R */
      rex |= (src.reg >> 3) << 0; /* REX.B */
      if (rex) B(0x40 | rex);
      else if (en->r8 && in_range(dst.reg, RSP, RDI)) {
         /* /r8 needs REX to encode SP,BP,SI,DI (otherwise -> AH..BH) */
         B(0x40);
      }
      D(opc, nopc);
      B(0300 | (dst.reg & 7) << 3 | (src.reg & 7));
      break;
   case EN_RRX: /* mod = 11; reg = src; rm = dst */
      rex |= (src.reg >> 3) << 2; /* REX.R */
      rex |= (dst.reg >> 3) << 0; /* REX.B */
      if (rex) B(0x40 | rex);
      else if (en->r8 && in_range(src.reg, RSP, RDI)) {
         /* /r8 needs REX to encode SP,BP,SI,DI (otherwise -> AH..BH) */
         B(0x40);
      }
      D(opc, nopc);
      B(0300 | (src.reg & 7) << 3 | (dst.reg & 7));
      break;
   case EN_MR:
      mem = dst;
      reg = src.reg;
      goto Mem;
   case EN_RM:
      mem = src;
      reg = dst.reg;
      goto Mem;
   case EN_M: case EN_MI8: case EN_MI16: case EN_MI32:
      mem = dst;
      reg = en->ext;
   Mem:
      if (mem.t == OMEM) {
         if (mem.base != NOBASE) rex |= mem.base >> 3; /* REX.B */
         if (mem.index != NOINDEX) rex |= mem.index >> 3 << 1; /* REX.X */
      } else {
         if (mem.cindex != NOINDEX) rex |= mem.cindex >> 3 << 1; /* REX.X */
      }
      if (en->operenc != EN_M)
         rex |= (reg >> 3) << 2; /* REX.R */
      if (rex) B(0x40 | rex);
      else if (en->r8 && in_range(reg, RSP, RDI)) B(0x40);

      if (mem.t == OSYM) {
         D(opc, nopc);
         if (mem.cindex == NOINDEX) {
            /* %rip(var) */
            static uchar offs[NOPERENC] = { [EN_MI8] = 1, [EN_MI16] = 2, [EN_MI32] = 4 };
            enum relockind r =
               (!conht[mem.con].deref && ccopt.pic) ? (rex ? REL_GOTPCRELX : REL_GOTPCRELX_REX)
                                                    : REL_PCREL32;
            int off = -4 - offs[en->operenc];
            B(/*mod 0*/ (reg & 7) << 3 | RBP);
            objreloc(xcon2sym(mem.con), r, Stext, *pcode - objout.textbegin, mem.disp + off);
         } else {
            /* var(,%reg,shift) */
            assert(!ccopt.pic && !ccopt.pie && "cannot encode [RIP-rel + REG] for position independent");
            B(/*mod 0*/ (reg & 7) << 3 | RSP);
            B(mem.cshift << 6 | mem.cindex << 3 | RBP); /* SIB [index*s + disp32] */
            objreloc(xcon2sym(mem.con), REL_ABS32S, Stext, *pcode - objout.textbegin, mem.disp);
         }
         I32(0);
      } else {
         if (mem.base == RBP) {
            if (!usebp) {
               /* if RBP isn't being set up (leaf functions with no stack allocations),
                * access thru RSP (function arguments in the stack) */
               mem.base = RSP;
               mem.disp -= 8;
            } else if (mem.disp < 0) {
               mem.disp += rbpoff;
            }
         }

         if (mem.base != NOBASE) {
            if (mem.index == NOINDEX && mem.shift == 0) sib = 0;
            else sib = 1;
            mod = !mem.disp ? 0                    /* disp = 0 -> mod = 00 */
                : (uint)(mem.disp + 128) < 256 ? 1 /* disp8 -> mod = 01 */
                : 2;                               /* disp32 -> mod = 10 */
            if (mod == 0 && (mem.base == RBP || mem.base == R13)) mod = 1;
            if (mem.base == RSP || mem.base == R12) sib = 1;
         } else {
            /* [disp + (index*s)] */
            sib = 1;
            mem.base = RBP;
            mod = 0;
            assert(mem.index != RSP);
         }
         D(opc, nopc);
         B(mod << 6 | (reg & 7) << 3 | (sib ? 4 : (mem.base & 7)));
         if (sib) {
            if (mem.index == NOINDEX) mem.index = RSP;
            B(mem.shift << 6 | (mem.index & 7) << 3 | (mem.base & 7));
         }
         if (mod == 1) B(mem.disp);
         else if (mod == 2 || (mod == 0 && mem.base == RBP/*RIP-rel*/) || (mod == 0 && sib && mem.base == RBP/*absolute*/)) {
            I32(mem.disp);
         }
      }
      if (en->operenc == EN_MI8) B(src.imm);
      if (en->operenc == EN_MI16) I16(src.imm);
      if (en->operenc == EN_MI32) I32(src.imm);
      break;
   case EN_R: case EN_RI32: case EN_RI8:
      rex |= (dst.reg >> 3) << 0; /* REX.B */
      if (rex) B(0x40 | rex);
      else if (en->r8 && in_range(dst.reg, RSP, RDI)) {
         /* /r8 needs REX to encode SP,BP,SI,DI (otherwise -> AH..BH) */
         B(0x40);
      }
      D(opc, nopc);
      B(0300 | en->ext << 3 | (dst.reg & 7));
      if (en->operenc == EN_RI32)
         I32(src.imm);
      else if (en->operenc == EN_RI8)
         B(src.imm);
      break;
   case EN_OI:
      rex |= (dst.reg >> 3) << 0; /* REX.B */
      if (rex) B(0x40 | rex);
      B(*opc++ + (dst.reg & 7));
      D(opc, nopc - 1);
      I32(src.imm);
      break;
   case EN_I8:
      if (rex) B(0x40 | rex);
      D(opc, nopc);
      B(src.imm);
      break;
   case EN_I32:
      if (rex) B(0x40 | rex);
      D(opc, nopc);
      I32(src.imm);
      break;
   case EN_R32:
      if (rex) B(0x40 | rex);
      D(opc, nopc);
      assert(dst.t == OSYM);
      sym = xcon2sym(dst.con);
      if (sym != curfnsym) {
         enum relockind r = (ccopt.pie|ccopt.pic) ? REL_PLT32 : REL_PCREL32;
         objreloc(sym, r, Stext, *pcode - objout.textbegin, -4);
         I32(0);
      } else {
         /* self-recursive call */
         I32(fnstart - *pcode - 4);
      }
      break;
   }
}

#define DEFINSTR1(X, ...)                                          \
   static void                                                     \
   X(uchar **pcode, enum irclass k, struct oper oper)              \
   {                                                               \
      static const struct desc tab[] = { __VA_ARGS__ };            \
      encode(pcode, tab, arraylength(tab), k, oper, mkoper(0,));   \
   }

#define DEFINSTR2(X, ...)                                             \
   static void                                                        \
   X(uchar **pcode, enum irclass k, struct oper dst, struct oper src) \
   {                                                                  \
      static const struct desc tab[] = { __VA_ARGS__ };               \
      encode(pcode, tab, arraylength(tab), k, dst, src);              \
   }

DEFINSTR2(Xmovb,
   {-1, PMEM, PGPR, "\x88", EN_MR,  .r8=1}, /* MOV m8, r8 */
   {-1, PMEM, PI8,  "\xC6", EN_MI8, .r8=1}, /* MOV m8, imm8 */
   {-1, PMEM, PU8,  "\xC6", EN_MI8, .r8=1}, /* MOV m8, imm8 */
)
DEFINSTR2(Xmovw,
   {-1, PMEM, PGPR, "\x66\x89", EN_MR},   /* MOV m16, r16 */
   {-1, PMEM, PI16, "\x66\xC7", EN_MI16}, /* MOV m16, imm16 */
   {-1, PMEM, PU16, "\x66\xC7", EN_MI16}, /* MOV m16, imm16 */
)
static void Xmov(uchar **pcode, enum irclass k, struct oper dst, struct oper src)
{
   static const struct desc all[] = {
      {4  , PGPR, PI32, "\xB8",     EN_OI}, /* MOV r32, imm */
      {4|8, PGPR, PGPR, "\x8B",     EN_RR}, /* MOV r32/64, r32/64 */
      {4|8, PMEM, PGPR, "\x89",     EN_MR}, /* MOV m32/64, r32/64 */
      {4|8, PGPR, PMEM, "\x8B",     EN_RM}, /* MOV r32/64, m32/64 */
      {4|8, PMEM, PI32, "\xC7",     EN_MI32}, /* MOV m32/64, imm */
      {  8, PGPR, PU32, "\xB8",     EN_OI, .norexw=1}, /* MOV r64, uimm */
      {  8, PGPR, PI32, "\xC7",     EN_RI32}, /* MOV r64, imm */
      {4, PFPR, PFPR, "\x0F\x28", EN_RR}, /* MOVPS xmm, xmm */
      {4, PFPR, PMEM, "\xF3\x0F\x10", EN_RM}, /* MOVSS xmm, m32 */
      {4, PMEM, PFPR, "\xF3\x0F\x11", EN_MR}, /* MOVSS m32, xmm */
      {8, PFPR, PFPR, "\x0F\x28", EN_RR}, /* MOVPS xmm, xmm */
      {8, PFPR, PMEM, "\xF2\x0F\x10", EN_RM}, /* MOVSD xmm, m64 */
      {8, PMEM, PFPR, "\xF2\x0F\x11", EN_MR}, /* MOVSS m64, xmm */
      {4|8, PFPR, PGPR, "\x66\x0F\x6E", EN_RR},  /* MOVD/Q xmm, r64/32 */
      {4|8, PGPR, PFPR, "\x66\x0F\x7E", EN_RRX}, /* MOVD/Q r64/32, xmm */
   };
   static const uchar k2off[] = {
      [KI32] = 0,
      [KI64] = 1, [KPTR] = 1,
      [KF32] = 7,
      [KF64] = 10,
   };
   encode(pcode, all + k2off[k], arraylength(all) - k2off[k], k, dst, src);
}
DEFINSTR2(Xmovsxl,
   {8, PGPR, PMEM, "\x63", EN_RM}, /* MOVSXD r64, m32 */
   {8, PGPR, PGPR, "\x63", EN_RR}, /* MOVSXD r64, r32 */
   {4, PGPR, PMEM, "\x8B", EN_RM}, /* MOV r32, m32 */
   {4, PGPR, PGPR, "\x8B", EN_RR}, /* MOV r32, r32 */
)
DEFINSTR2(Xmovsxw,
   {4|8, PGPR, PMEM, "\x0F\xBF", EN_RM}, /* MOVSX r64, m16 */
   {4|8, PGPR, PGPR, "\x0F\xBF", EN_RR}, /* MOVSX r64, r16 */
)
DEFINSTR2(Xmovsxb,
   {4|8, PGPR, PMEM, "\x0F\xBE", EN_RM}, /* MOVSX r64, m8 */
   {4|8, PGPR, PGPR, "\x0F\xBE", EN_RR, .r8=1}, /* MOVSX r64, r8 */
)
DEFINSTR2(Xmovzxw,
   {4|8, PGPR, PMEM, "\x0F\xB7", EN_RM}, /* MOVZX r64, m16 */
   {4|8, PGPR, PGPR, "\x0F\xB7", EN_RR}, /* MOVZX r64, r16 */
)
DEFINSTR2(Xmovzxb,
   {4|8, PGPR, PMEM, "\x0F\xB6", EN_RM}, /* MOVZX r64, m8 */
   {4|8, PGPR, PGPR, "\x0F\xB6", EN_RR, .r8=1}, /* MOVZX r64, r8 */
)
DEFINSTR2(Xmovaps,
   {-1, PMEM, PFPR, "\x0F\x29", EN_MR}, /* MOVAPS mem, xmm */
)
DEFINSTR2(Xxchg,
   {4|8, PGPR, PGPR, "\x87", EN_RR}, /* XCHG r32/64, r32/64 */
   //{4|8, PGPR, PMEM, "\x87", EN_RM}, /* XCHG r32/64, m32/64 */
)
DEFINSTR2(Xlea,
   {4|8, PGPR, PMEM, "\x8D", EN_RM}, /* LEA r32/64,m32/64 */
   {  8, PGPR, PSYM, "\x8D", EN_RM}, /* LEA rel32 */
)
DEFINSTR2(Xadd,
   {4|8, PGPR, PGPR, "\x03", EN_RR},   /* ADD r32/64, r32/64 */
   {4|8, PGPR, P1,   "\xFF", EN_R, .ext=0}, /* INC r32/64 */
   {4|8, PGPR, PN1,  "\xFF", EN_R, .ext=1}, /* DEC r32/64 */
   {4|8, PGPR, PI8,  "\x83", EN_RI8},  /* ADD r32/64, imm8 */
   {4|8, PRAX, PI32, "\x05", EN_I32},  /* ADD eax/rax, imm */
   {4|8, PGPR, PI32, "\x81", EN_RI32}, /* ADD r32/64, imm */
   {  8, PGPR, PMEM, "\x03", EN_RM},   /* ADD r64, m64 */
)
DEFINSTR2(Xaddf,
   {4, PFPR, PFPR, "\xF3\x0F\x58", EN_RR}, /* ADDSS xmm, xmm */
   {8, PFPR, PFPR, "\xF2\x0F\x58", EN_RR}, /* ADDSD xmm, xmm */
   {4, PFPR, PMEM, "\xF3\x0F\x58", EN_RM}, /* ADDSS xmm, m32 */
   {8, PFPR, PMEM, "\xF2\x0F\x58", EN_RM}, /* ADDSD xmm, m64 */
)
DEFINSTR2(Xsub,
   {4|8, PGPR, PGPR, "\x2B", EN_RR},           /* SUB r32/64, r32/64 */
   {4|8, PGPR, P1,   "\xFF", EN_R, .ext=1},    /* DEC r32/64 */
   {4|8, PGPR, PN1,  "\xFF", EN_R, .ext=0},    /* INC r32/64 */
   {4|8, PGPR, PI8,  "\x83", EN_RI8, .ext=5},  /* SUB r32/64, imm8 */
   {4|8, PRAX, PI32, "\x2D", EN_I32},          /* SUB eax/rax, imm */
   {4|8, PGPR, PI32, "\x81", EN_RI32, .ext=5}, /* SUB r32/64, imm */
   {  8, PGPR, PMEM, "\x2B", EN_RM},           /* SUB r64, m64 */
)
DEFINSTR2(Xsubf,
   {4, PFPR, PFPR, "\xF3\x0F\x5C", EN_RR}, /* SUBSS xmm, xmm */
   {8, PFPR, PFPR, "\xF2\x0F\x5C", EN_RR}, /* SUBSD xmm, xmm */
   {4, PFPR, PMEM, "\xF3\x0F\x5C", EN_RM}, /* SUBSS xmm, m32 */
   {8, PFPR, PMEM, "\xF2\x0F\x5C", EN_RM}, /* SUBSD xmm, m64 */
)
DEFINSTR2(Xmulf,
   {4, PFPR, PFPR, "\xF3\x0F\x59", EN_RR}, /* MULSS xmm, xmm */
   {8, PFPR, PFPR, "\xF2\x0F\x59", EN_RR}, /* MULSD xmm, xmm */
   {4, PFPR, PMEM, "\xF3\x0F\x59", EN_RM}, /* MULSS xmm, m32 */
   {8, PFPR, PMEM, "\xF2\x0F\x59", EN_RM}, /* MULSD xmm, m64 */
)
DEFINSTR2(Xdivf,
   {4, PFPR, PFPR, "\xF3\x0F\x5E", EN_RR}, /* DIVSS xmm, xmm */
   {8, PFPR, PFPR, "\xF2\x0F\x5E", EN_RR}, /* DIVSD xmm, xmm */
   {4, PFPR, PMEM, "\xF3\x0F\x5E", EN_RM}, /* DIVSS xmm, m32 */
   {8, PFPR, PMEM, "\xF2\x0F\x5E", EN_RM}, /* DIVSD xmm, m64 */
)
DEFINSTR2(Xand,
   {4|8, PGPR, PGPR, "\x23", EN_RR},           /* AND r32/64, r32/64 */
   {4|8, PGPR, PI8,  "\x83", EN_RI8, .ext=4},  /* AND r32/64, imm8 */
   {4|8, PRAX, PI32, "\x25", EN_I32},          /* AND eax/rax, imm */
   {4|8, PGPR, PI32, "\x81", EN_RI32, .ext=4}, /* AND r32/64, imm */
   {  8, PGPR, PMEM, "\x23", EN_RM},           /* AND r64, m64 */
)
DEFINSTR2(Xior,
   {4|8, PGPR, PGPR, "\x0B", EN_RR},           /* OR r32/64, r32/64 */
   {4|8, PGPR, PI8,  "\x83", EN_RI8, .ext=1},  /* OR r32/64, imm8 */
   {4|8, PRAX, PI32, "\x0D", EN_I32},          /* OR eax/rax, imm */
   {4|8, PGPR, PI32, "\x81", EN_RI32, .ext=1}, /* OR r32/64, imm */
   {  8, PGPR, PMEM, "\x0B", EN_RM},           /* OR r64, m64 */
   {4|8, PFPR, PFPR, "\x0F\x57", EN_RR},       /* ORPS xmm, xmm */
)
DEFINSTR2(Xxor,
   {4|8, PGPR, PGPR, "\x33", EN_RR},           /* XOR r32/64, r32/64 */
   {4|8, PGPR, PI8,  "\x83", EN_RI8, .ext=6},  /* XOR r32/64, imm8 */
   {4|8, PRAX, PI32, "\x35", EN_I32},          /* XOR eax/rax, imm */
   {4|8, PGPR, PI32, "\x81", EN_RI32, .ext=6}, /* XOR r32/64, imm */
   {  8, PGPR, PMEM, "\x33", EN_RM},           /* XOR r64, m64 */
   {4|8, PFPR, PFPR, "\x0F\x57", EN_RR},       /* XORPS xmm, xmm */
)
DEFINSTR2(Xshl,
   {4|8, PGPR, P1,   "\xD1", EN_R, .ext=4},   /* SHL r32/64, 1 */
   {4|8, PGPR, PI32, "\xC1", EN_RI8, .ext=4}, /* SHL r32/64, imm */
   {4|8, PGPR, PRCX, "\xD3", EN_R, .ext=4},   /* SHL r32/64, CL */
)
DEFINSTR2(Xsar,
   {4|8, PGPR, P1,   "\xD1", EN_R, .ext=7},   /* SAR r32/64, 1 */
   {4|8, PGPR, PI32, "\xC1", EN_RI8, .ext=7}, /* SAR r32/64, imm */
   {4|8, PGPR, PRCX, "\xD3", EN_R, .ext=7},   /* SAR r32/64, CL */
)
DEFINSTR2(Xshr,
   {4|8, PGPR, P1,   "\xD1", EN_R, .ext=5},   /* SHR r32/64, 1 */
   {4|8, PGPR, PI32, "\xC1", EN_RI8, .ext=5}, /* SHR r32/64, imm */
   {4|8, PGPR, PRCX, "\xD3", EN_R, .ext=5},   /* SHR r32/64, CL */
)
DEFINSTR2(Xcvtss2sd,
   {-1,  PFPR, PFPR, "\xF3\x0F\x5A", EN_RR}, /* CVTSS2SD xmm, xmm */
   {-1,  PFPR, PMEM, "\xF3\x0F\x5A", EN_RM}, /* CVTSS2SD xmm, m32/64 */
)
DEFINSTR2(Xcvtsd2ss,
   {-1,  PFPR, PFPR, "\xF2\x0F\x5A", EN_RR}, /* CVTSD2SS xmm, xmm */
   {-1,  PFPR, PMEM, "\xF2\x0F\x5A", EN_RM}, /* CVTSD2SS xmm, m32/64 */
)
DEFINSTR2(Xcvtsi2ss,
   {-1,  PFPR, PGPR, "\xF3\x0F\x2A", EN_RR}, /* CVTSI2SS xmm, r32/64 */
   {-1,  PFPR, PMEM, "\xF3\x0F\x2A", EN_RM}, /* CVTSI2SS xmm, m32/64 */
)
DEFINSTR2(Xcvtsi2sd,
   {-1,  PFPR, PGPR, "\xF2\x0F\x2A", EN_RR}, /* CVTSI2SD xmm, r32/64 */
   {-1,  PFPR, PMEM, "\xF2\x0F\x2A", EN_RM}, /* CVTSI2SD xmm, m32/64 */
)
DEFINSTR2(Xcvttss2si,
   {-1,  PGPR, PFPR, "\xF3\x0F\x2C", EN_RR}, /* CVTTSS2SI r32/64, xmm */
   {-1,  PGPR, PMEM, "\xF3\x0F\x2C", EN_RM}, /* CVTTSS2SI r32/64, m32 */
)
DEFINSTR2(Xcvttsd2si,
   {-1,  PGPR, PFPR, "\xF2\x0F\x2C", EN_RR}, /* CVTTSD2SI r32/64, xmm */
   {-1,  PGPR, PMEM, "\xF2\x0F\x2C", EN_RM}, /* CVTTSD2SI r32/64, m32 */
)
DEFINSTR1(Xneg,
   {4|8, PGPR, 0, "\xF7", EN_R, .ext=3} /* NEG r32/64 */
)
DEFINSTR1(Xnot,
   {4|8, PGPR, 0, "\xF7", EN_R, .ext=2} /* NOT r32/64 */
)
DEFINSTR1(Xidiv,
   {4|8, PGPR, 0, "\xF7", EN_R, .ext=7}, /* IDIV r32/64 */
   {4|8, PMEM, 0, "\xF7", EN_M, .ext=7}, /* IDIV m32/64 */
)
DEFINSTR1(Xdiv,
   {4|8, PGPR, 0, "\xF7", EN_R, .ext=6}, /* DIV r32/64 */
   {4|8, PMEM, 0, "\xF7", EN_M, .ext=6}, /* DIV m32/64 */
)
DEFINSTR1(Xcall,
   {-1, PSYM, 0, "\xE8", EN_R32, .norexw=1}, /* CALL rel32 */
   {-1, PGPR, 0, "\xFF", EN_R, .ext=2, .norexw=1}, /* CALL r64 */
   {-1, PMEM, 0, "\xFF", EN_M, .ext=2, .norexw=1}, /* CALL m64 */
)
DEFINSTR2(Xcmp,
   {4|8, PGPR, PGPR, "\x3B", EN_RR},           /* CMP r32/64, r32/64 */
   {4|8, PGPR, PI8,  "\x83", EN_RI8, .ext=7},  /* CMP r32/64, imm8 */
   {4|8, PRAX, PI32, "\x3D", EN_I32},          /* CMP eax/rax, imm */
   {4|8, PGPR, PI32, "\x81", EN_RI32, .ext=7}, /* CMP r32/64, imm */
   {  8, PGPR, PMEM, "\x3B", EN_RM},           /* CMP r64, m64 */
   {4  , PFPR, PFPR, "\x0F\x2F", EN_RR},       /* COMISS xmm, xmm */
   {4  , PFPR, PMEM, "\x0F\x2F", EN_RM},       /* COMISS xmm, m32 */
   {  8, PFPR, PFPR, "\x66\x0F\x2F", EN_RR},   /* COMISD xmm, xmm */
   {  8, PFPR, PMEM, "\x66\x0F\x2F", EN_RM},   /* COMISD xmm, m64 */
)
DEFINSTR2(Xtest,
   {4|8, PRAX, PI8,  "\xA8", EN_I8},  /* TEST AL, imm8 */
   {4,   PRAX, PI32, "\xA9", EN_I32}, /* TEST EAX, imm32 */
   {  8, PRAX, PU32, "\xA9", EN_I32}, /* TEST EAX, imm32 */
   {  8, PRAX, PI32, "\xA9", EN_I32}, /* TEST RAX, imm32 */
   {4|8, PGPR, PI8,  "\xF6", EN_RI8, .r8=1,.norexw=1},  /* TEST r8, imm8 */
   {4|8, PGPR, PI32, "\xF7", EN_RI32, .ext=0}, /* TEST r32/64, imm32 */
   {4|8, PGPR, PGPR, "\x85", EN_RR},   /* TEST r32/64, r32/64 */
   {4|8, PGPR, PMEM, "\x85", EN_RM},   /* TEST r32/64, m32/64 */
)

DEFINSTR2(Ximul2,
   {4|8, PGPR, PGPR, "\x0F\xAF", EN_RR}, /* IMUL r32/64, r32/64 */
   {4|8, PGPR, PMEM, "\x0F\xAF", EN_RM}, /* IMUL r32/64, m32/64 */
)
static void
Ximul(uchar **pcode, enum irclass k, struct oper dst, struct oper s1, struct oper s2)
{
   static const struct desc imm8tab[] = {
      {4|8, PGPR, PGPR, "\x6B", EN_RR}, /* IMUL r32/64, r32/64, (imm8) */
      {4|8, PGPR, PMEM, "\x6B", EN_RM}, /* IMUL r32/64, m32/64, (imm8) */
   }, imm32tab[] = {
      {4|8, PGPR, PGPR, "\x69", EN_RR}, /* IMUL r32/64, r32/64, (imm32) */
      {4|8, PGPR, PMEM, "\x69", EN_RM}, /* IMUL r32/64, m32/64, (imm32) */
   };
   if (!memcmp(&dst, &s1, sizeof dst) && s2.t != OIMM) {
      Ximul2(pcode, k, dst, s2);
      return;
   }
   assert(s2.t == OIMM);
   if ((uint)(s2.imm + 128) < 256) {
      encode(pcode, imm8tab, arraylength(imm8tab), k, dst, s1);
      B(s2.imm);
   } else {
      encode(pcode, imm32tab, arraylength(imm32tab), k, dst, s1);
      I32(s2.imm);
   }
}

enum cc {
   CCO   = 0x0, /* OF = 1*/
   CCNO  = 0x1, /* OF = 0*/
   CCB   = 0x2, CCC   = 0x2, CCNAE = 0x2, /* below; CF = 1; not above or equal */
   CCAE  = 0x3, CCNB  = 0x3, CCNC  = 0x3, /* above or equal; not below; CF = 0 */
   CCE   = 0x4, CCZ   = 0x4, /* equal; ZF = 1 */
   CCNE  = 0x5, CCNZ  = 0x5, /* not equal; ZF = 0 */
   CCBE  = 0x6, CCNA  = 0x6, /* below or equal; not above; CF=1 or  ZF=1 */
   CCA   = 0x7, CCNBE = 0x7, /* above; not below or equal; CF=0 and ZF=0 */
   CCS   = 0x8, /* ZS = 1; negative */
   CCNS  = 0x9, /* ZS = 0; non-negative */
   CCP   = 0xA, CCPE  = 0xA, /* PF = 1; parity even */
   CCNP  = 0xB, CCPO  = 0xB, /* PF = 0; parity odd */
   CCL   = 0xC, CCNGE = 0xC, /* lower; not greater or equal; SF != OF */
   CCGE  = 0xD, CCNL  = 0xD, /* greater or equal; not lower; SF == OF */
   CCLE  = 0xE, CCNG  = 0xE, /* less or equal; not greater;  ZF=1 or SF != OF */
   CCG   = 0xF, CCNLE = 0xF, /* greater; not less or equal;  ZF=0 and SF = OF*/
   ALWAYS,
};

/* maps blk -> address when resolved; or to linked list of jump displacement
 * relocations */
static struct blkaddr {
   bool resolved;
   union {
      uint addr;
      uint relreloc;
   };
} *blkaddr;
static uint nblkaddr;

static void
Xjcc(uchar **pcode, enum cc cc, struct block *dst)
{
   int disp, insaddr = *pcode - objout.textbegin;
   bool rel8 = 0;

   if (blkaddr[dst->id].resolved) {
      disp = blkaddr[dst->id].addr - (insaddr + 2);
      if ((uint)(disp + 128) < 256) /* can use 1-byte displacement? */
         rel8 = 1;
      else { /* otherwise 4-byte displacement */
         disp -= 3;
         disp -= cc != ALWAYS; /* 'Jcc rel32' has 2 opcode bytes */
      }
   } else {
      disp = blkaddr[dst->id].relreloc;
      blkaddr[dst->id].relreloc = insaddr + 1 + (cc != ALWAYS);
   }
   if (cc == ALWAYS) {
      B(rel8 ? 0xEB : 0xE9); /* JMP rel8/rel32 */
   } else {
      assert(in_range(cc, 0, 0xF));
      if (rel8) B(0x70 + cc);     /* Jcc rel8 */
      else B(0x0F), B(0x80 + cc); /* Jcc rel32 */
   }
   if (rel8) B(disp); else I32(disp);
}

static void
Xsetcc(uchar **pcode, enum cc cc, enum reg reg)
{
   int rex = 0;
   assert(in_range(cc, 0x0, 0xF));
   assert(in_range(reg, RAX, R15));

   if (in_range(reg, RSP, RDI)) rex = 0x40;
   rex |= (reg >> 3); /* REX.B */
   if (rex) B(rex | 0x40);
   B(0x0F), B(0x90+cc); /* SETcc */
   B(0xC0 + (reg & 7)); /* ModR/M with mod=11, rm=reg */

}

static void
Xpush(uchar **pcode, enum reg reg)
{
   assert(in_range(reg, RAX, R15));
   if (reg >> 3) B(0x41); /* REX.B */
   B(0x50 + (reg & 7));
}

static void
Xpop(uchar **pcode, enum reg reg)
{
   assert(in_range(reg, RAX, R15));
   if (reg >> 3) B(0x41); /* REX.B */
   B(0x58 + (reg & 7));
}

/* are flags live at given instruction? */
static bool
flagslivep(struct block *blk, int curi)
{
   int cmpi;
   /* conditional branch that references a previous comparison instruction? */
   if (blk->jmp.t != Jb || !blk->jmp.arg[0].bits)
      return 0;
   assert(blk->jmp.arg[0].t == RTMP);
   cmpi = blk->jmp.arg[1].i;
   for (int i = blk->ins.n - 1; i > curi; --i) {
      if (blk->ins.p[i] == cmpi)
         /* flags defined after given instruction, dead here */
         return 0;
   }
   /* flags defined before given instruction, live here */
   return 1;
}

/* Copy dst = val, with some peephole optimizations */
static void
gencopy(uchar **pcode, enum irclass cls, struct block *blk, int curi, struct oper dst, union ref val)
{
   assert(dst.t == OREG);
   if (val.bits == UNDREF.bits) {
      /* can be generated by ssa construction, since value is undefined no move is needed */
      return;
   }
   if (val.t == RADDR) {
      /* this is a LEA, but maybe it can be lowered to a 2-address instruction,
       * which may clobber flags */
      const struct addr *addr = &addrht[val.i];
      if (flagslivep(blk, curi)) goto Lea;
      if (addr->base.t != RREG) goto Lea;
      if (addr->base.bits && dst.reg == mkregoper(addr->base).reg) { /* base = dst */
         if (addr->index.bits && !addr->disp && !addr->shift){
            /* lea Rx, [Rx + Ry] -> add Rx, Ry */
            Xadd(pcode, cls, dst, mkregoper(addr->index));
            return;
         } else if (!addr->index.bits) {
            if (!addr->disp) /* lea Rx, [Rx] -> mov Rx, Rx */
               Xmov(pcode, cls, dst, dst);
            else /* lea Rx, [Rx + Imm] -> add Rx, Imm */
               Xadd(pcode, cls, dst, mkoper(OIMM, .imm = addr->disp));
            return;
         }
      } else if (addr->index.bits && dst.reg == mkregoper(addr->index).reg) { /* index = dst */
         if (addr->base.bits && !addr->disp && !addr->shift) {
            /* lea Rx, [Ry + Rx] -> add Rx, Ry */
            Xadd(pcode, cls, dst, mkregoper(addr->base));
            return;
         } else if (!addr->base.bits) {
            if (!addr->disp && !addr->shift) /* lea Rx, [Rx] -> mov Rx, Rx */
               Xmov(pcode, cls, dst, dst);
            else if (!addr->shift) /* lea Rx, [Rx + Imm] -> add Rx, Imm */
               Xadd(pcode, cls, dst, mkoper(OIMM, .imm = addr->disp));
            else if (!addr->disp) /* lea Rx, [Rx LSL s] -> shl Rx, s */
               Xshl(pcode, cls, dst, mkoper(OIMM, .imm = addr->shift));
            else
               goto Lea;
            return;
         }
      }
      /* normal (not 2-address) case */
   Lea:
      if (isaddrcon(addr->base,0) && ccopt.pic) {
         assert(!addr->disp && !addr->index.bits);
         val = addr->base;
         goto GOTLoad;
      }
      Xlea(pcode, cls, dst, ref2oper(val));
   } else if (val.bits == ZEROREF.bits && dst.t == OREG && (kisflt(cls) || !flagslivep(blk, curi))) {
      /* dst = 0 -> xor dst, dst; but only if it is ok to clobber flags */
      Xxor(pcode, kisint(cls) ? KI32 : cls, dst, dst);
   } else if (isaddrcon(val,0)) {
      if (ccopt.pic) GOTLoad:
         /* for  mov reg, [rip(sym@GOTPCREL)] */
         Xmov(pcode, cls, dst, mkoper(OSYM, .con = val.i, .cindex = NOINDEX));
      else
         /* for  lea reg, [rip(sym)] */
         Xlea(pcode, cls, dst, mkoper(OSYM, .con = val.i, .cindex = NOINDEX));
   } else if (val.t == RXCON && in_range(concls(val), KI64, KPTR)) {
      /* movabs */
      assert(dst.t == OREG && in_range(dst.reg, RAX, R15));
      B(0x48 | (dst.reg >> 3));       /* REX.W (+ REX.B)   */
      B(0xB8 + (dst.reg & 0x7));      /* MOVABS r64,       */
      wr64le(*pcode, intconval(val)); /*             imm64 */
      *pcode += 8;
   } else {
      struct oper src = mkimmdatregoper(val);
      if (memcmp(&dst, &src, sizeof dst) != 0)
         Xmov(pcode, cls == KF64 && src.t == OREG && src.reg < XMM0 ? KI64 : cls, dst, src);
   }
}

static void
Xvaprologue(uchar **pcode, struct function *fn, struct oper sav)
{
   uint gpr0 = 0, fpr0 = 0, jmpaddr;
   for (int i = 0; i < fn->nabiarg; ++i) {
      struct abiarg abi = fn->abiarg[i];
      if (!abi.isstk) {
         if (abi.reg < XMM0) ++gpr0;
         else ++fpr0;
      }
   }
   assert(sav.t == OMEM && sav.base == RBP);
   /* save GPRS */
   for (int r = 0; r < 6; ++r) {
      static const char reg[] = {RDI,RSI,RDX,RCX,R8,R9};
      if (r >= gpr0)
         Xmov(pcode, KI64, sav, reg2oper(reg[r]));
      sav.disp += 8;
   }

   /* save FPRs, but only if al is non zero */
   if (fpr0 < 8) {
      DS("\x84\xC0"); /* TEST al,al */
      jmpaddr = *pcode - objout.textbegin;
      DS("\x74\xFE"); /* JE rel8 */
   }
   for (int r = 0; r < 8; ++r) {
      if (r >= fpr0)
         Xmovaps(pcode, KF64, sav, reg2oper(XMM0 + r));     
      sav.disp += 16;
   }
   if (fpr0 < 8) {/* patch relative jump */
      int off = (*pcode - objout.textbegin) - jmpaddr - 2;
      objout.textbegin[jmpaddr+1] = off;
   }
}

/* condition code for CMP */
static const uchar icmpop2cc[] = {
   [Oequ]  = CCE, [Oneq]  = CCNE,
   [Olth]  = CCL, [Ogth]  = CCG, [Olte]  = CCLE, [Ogte]  = CCGE,
   [Oulth] = CCB, [Ougth] = CCA, [Oulte] = CCBE, [Ougte] = CCAE,
   [Oand] = CCNE, [Osub] = CCNE,
};
/* condition code for TEST reg,reg (compare with zero) */
static const uchar icmpzero2cc[] = {
   [Oequ] = CCE,  [Oulte] = CCE,
   [Oneq] = CCNE, [Ougth] = CCNE,
   [Olth] = CCS,  [Ogte] = CCNS,
   [Olte] = CCLE, [Ogth] = CCG,
   [Oulth] = CCB, [Ougte] = CCAE, /* actually constants */
};

static void
emitinstr(uchar **pcode, struct function *fn, struct block *blk, int curi, struct instr *ins)
{
   struct oper dst, src;
   bool regzeroed;
   enum irclass cls = ins->cls;
   void (*X)(uchar **, enum irclass, struct oper, struct oper) = NULL;
   void (*X1)(uchar **, enum irclass, struct oper) = NULL;

   switch (ins->op) {
   default:
      fatal(NULL, "amd64: in %y; unimplemented instr '%s'", fn->name, opnames[ins->op]);
   case Onop: break;
   case Ostore8:  cls = KI32, X = Xmovb; goto Store;
   case Ostore16: cls = KI32, X = Xmovw; goto Store;
   case Ostore32: cls = KI32, X = Xmov; goto Store;
   case Ostore64: cls = KI64, X = Xmov;
   Store:
      src = mkimmregoper(ins->r);
      if (cls == KI32 && src.t == OREG && src.reg >= XMM0) cls = KF32;
      if (cls == KI64 && src.t == OREG && src.reg >= XMM0) cls = KF64;
      X(pcode, cls, mkmemoper(ins->l), src);
      break;
   case Oexts8:  src = mkregoper(ins->l); goto Movsxb;
   case Oextu8:  src = mkregoper(ins->l); goto Movzxb;
   case Oexts16: src = mkregoper(ins->l); goto Movsxw;
   case Oextu16: src = mkregoper(ins->l); goto Movzxw;
   case Oexts32: src = mkregoper(ins->l); goto Movsxl;
   case Oextu32: src = mkregoper(ins->l); goto Movzxl;
   case Oloads8: src = mkmemoper(ins->l); Movsxb: Xmovsxb(pcode, cls, reg2oper(ins->reg-1), src); break;
   case Oloadu8: src = mkmemoper(ins->l); Movzxb: Xmovzxb(pcode, cls, reg2oper(ins->reg-1), src); break;
   case Oloads16: src = mkmemoper(ins->l); Movsxw: Xmovsxw(pcode, cls, reg2oper(ins->reg-1), src); break;
   case Oloadu16: src = mkmemoper(ins->l); Movzxw: Xmovzxw(pcode, cls, reg2oper(ins->reg-1), src); break;
   case Oloads32: src = mkmemoper(ins->l); Movsxl: Xmovsxl(pcode, cls, reg2oper(ins->reg-1), src); break;
   case Oloadu32: src = mkmemoper(ins->l); Movzxl: Xmov(pcode, KI32, reg2oper(ins->reg-1), src); break;
   case Oloadf32: case Oloadf64: Xmov(pcode, cls, reg2oper(ins->reg-1), mkmemoper(ins->l)); break;
   case Oloadi64: Xmov(pcode, KI64, reg2oper(ins->reg-1), mkmemoper(ins->l)); break;
   case Ocvtf32f64: X = Xcvtss2sd; goto FloatsCvt;
   case Ocvtf64f32: X = Xcvtsd2ss; goto FloatsCvt;
   case Ocvtf32s: X = Xcvttss2si; goto FloatsCvt;
   case Ocvtf64s: X = Xcvttsd2si; goto FloatsCvt;
   case Ocvts32f: X = cls == KF32 ? Xcvtsi2ss : Xcvtsi2sd; cls = KI32; goto FloatsCvt;
   case Ocvts64f: X = cls == KF32 ? Xcvtsi2ss : Xcvtsi2sd; cls = KI64; goto FloatsCvt;
   FloatsCvt:
      X(pcode, cls, reg2oper(ins->reg-1), mkdatregoper(ins->l));
      break;
   case Oadd:
      dst = mkregoper(ins->l);
      if (kisflt(cls)) {
         Xaddf(pcode, cls, dst, mkimmdatregoper(ins->r));
      } else if (ins->reg-1 == dst.reg) { /* two-address add */
         Xadd(pcode, cls, dst, ref2oper(ins->r));
      } else if (isregref(ins->r) && ins->reg-1 == mkregoper(ins->r).reg) {
         /* also two-address after swapping operands */
         Xadd(pcode, cls, reg2oper(ins->reg-1), mkimmdatregoper(ins->l));
      } else { /* three-address add (lea) */
         struct oper mem = { OMEM, .base = NOBASE, .index = NOINDEX };
         dst = reg2oper(ins->reg-1);
         addmemoper(&mem, ref2oper(ins->l));
         addmemoper(&mem, ref2oper(ins->r));
         Xlea(pcode, cls, dst, mem);
      }
      break;
   case Osub:
      dst = mkregoper(ins->l);
      if (kisflt(cls)) {
         Xsubf(pcode, cls, dst, mkimmdatregoper(ins->r));
      } else if (ins->reg-1 == dst.reg) { /* two-address */
         Xsub(pcode, cls, dst, ref2oper(ins->r));
      } else {
         assert(isintcon(ins->r));
         Xlea(pcode, cls, reg2oper(ins->reg-1),
              mkoper(OMEM, .base = mkregoper(ins->l).reg, .index = NOINDEX, .disp = -intconval(ins->r)));
      }
      break;
   case Oshl: X = Xshl; goto ALU2;
   case Osar: X = Xsar; goto ALU2;
   case Oslr: X = Xshr; goto ALU2;
   case Oand:
      if (!ins->reg) {
         Xtest(pcode, cls, mkregoper(ins->l), mkimmdatregoper(ins->r));
         break;
      }
      X = Xand;
      goto ALU2;
   case Oxor: X = Xxor; goto ALU2;
   case Oior: X = Xior; goto ALU2;
   ALU2:
      dst = mkregoper(ins->l);
      assert(ins->reg-1 == dst.reg);
      X(pcode, cls, dst, mkimmdatregoper(ins->r));
      break;
   case Oneg: X1 = Xneg; goto ALU1;
   case Onot: X1 = Xnot; goto ALU1;
   ALU1:
      dst = mkregoper(ins->l);
      assert(ins->reg-1 == dst.reg);
      X1(pcode, cls, dst);
      break;
   case Omul:
      if (kisint(cls))
         Ximul(pcode, cls, reg2oper(ins->reg-1), ref2oper(ins->l), ref2oper(ins->r));
      else
         Xmulf(pcode, cls, reg2oper(ins->reg-1), ref2oper(ins->r));
      break;
   case Odiv:
      switch (cls) {
      default: assert(0);
      case KPTR:
      case KI64: B(0x48); /* REX.W */
      case KI32: B(0x99); /* CDQ/CQO */
                assert(mkregoper(ins->l).reg == RAX);
                Xidiv(pcode, cls, mkdatregoper(ins->r));
                break;
      case KF32: case KF64:
                Xdivf(pcode, cls, reg2oper(ins->reg-1), mkdatregoper(ins->r));
                break;
      }
      break;
   case Oudiv:
      DS("\x31\xD2"); /* XOR EDX,EDX */
      assert(mkregoper(ins->l).reg == RAX);
      Xdiv(pcode, cls, mkdatregoper(ins->r));
      break;
   case Oequ: case Oneq:
   case Olth: case Ogth: case Olte: case Ogte:
   case Oulth: case Ougth: case Oulte: case Ougte:
      dst = mkregoper(ins->l);
      src = ref2oper(ins->r);
      regzeroed = 0;
      if (ins->reg && dst.reg != ins->reg-1 && (src.t != OREG || src.reg != ins->reg-1)) {
         /* can zero output reg before test instruction (differs from both inputs) */
         /* XXX this doesn't check if a source operand is an addr containing the register */
         struct oper dst = reg2oper(ins->reg-1);
         Xxor(pcode, KI32, dst, dst);
         regzeroed = 1;
      }
      if (kisint(ins->cls) && ins->r.bits == ZEROREF.bits)
         Xtest(pcode, cls, dst, dst);
      else
         Xcmp(pcode, cls, dst, src);
      if (ins->reg) {
         enum cc cc;
         dst = reg2oper(ins->reg-1);
         if (ins->r.bits != ZEROREF.bits) { /* CMP */
            cc = icmpop2cc[ins->op];
         } else { /* TEST r,r  (CMP r, 0) */
            cc = icmpzero2cc[ins->op];
         }
         Xsetcc(pcode, cc, dst.reg);
         if (!regzeroed)
            Xmovzxb(pcode, KI32, dst, dst);
      }
      break;
   case Omove:
      dst = ref2oper(ins->l);
      gencopy(pcode, cls, blk, curi, dst, ins->r);
      break;
   case Ocopy:
      dst = reg2oper(ins->reg-1);
      gencopy(pcode, cls, blk, curi, dst, ins->l);
      break;
   case Oswap:
      if (kisint(cls))
         Xxchg(pcode, cls, mkregoper(ins->l), mkregoper(ins->r));
      else {
         struct oper l = mkregoper(ins->l), r = mkregoper(ins->r);
         Xxor(pcode, cls, l, r);
         Xxor(pcode, cls, r, l);
         Xxor(pcode, cls, l, r);
      }
      break;
   case Oxsave:
      Xpush(pcode, mkregoper(ins->l).reg);
      break;
   case Oxrestore:
      Xpop(pcode, mkregoper(ins->l).reg);
      break;
   case Ocall:
      if (calltab.p[ins->r.i].vararg >= 0) {
         struct call *call = &calltab.p[ins->r.i];
         /* variadic functions need the caller to write num of args in sse regs to %al */
         int n = 0;
         for (int i = 0; i < call->narg; ++i)
            if (!call->abiarg[i].isstk && call->abiarg[i].reg >= XMM0)
               ++n;
         if (!n) DS("\x31\xC0"); /* XOR EAX, EAX */
         else B(0xB0), B(n); /* MOV AL, n */
      }
      Xcall(pcode, KPTR, ref2oper(ins->l));
      break;
   case Oxvaprologue:
      Xvaprologue(pcode, fn, mkmemoper(ins->l));
      break;
   }
}

static void
emitbranch(uchar **pcode, struct block *blk)
{
   enum cc cc = ALWAYS;
   assert(blk->s1);
   if (blk->s2) {
      /* conditional branch.. */
      union ref arg = blk->jmp.arg[0];
      struct instr *ins;
      assert(arg.t == RTMP);
      ins = &instrtab[arg.i];
      if ((oiscmp(ins->op) || ins->op == Oand || ins->op == Osub)) {
         if (ins->r.bits != ZEROREF.bits) {
            /* for CMP instr */
            cc = icmpop2cc[ins->op];
         } else {
            /* for TEST instr, which modifies ZF and SF and sets CF = OF = 0 */
            cc = icmpzero2cc[ins->op];
         }
      } else {
         /* implicit by ZF */
         cc = CCNZ;
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
   if (blk->s2 || blk->s1 != blk->lnext)
      Xjcc(pcode, cc, blk->s1);
   if (blk->s2 && blk->s2 != blk->lnext)
      Xjcc(pcode, ALWAYS, blk->s2);
}

static bool
calleesave(int *npush, uchar **pcode, struct function *fn)
{
   bool any = 0;
   if (rstest(fn->regusage, RBX)) {
      Xpush(pcode, RBX);
      ++*npush;
      any = 1;
   }
   for (int r = R12; r <= R15; ++r)
      if (rstest(fn->regusage, r)) {
         Xpush(pcode, r);
         ++*npush;
         any = 1;
      }
   return any;
}

static void
calleerestore(uchar **pcode, struct function *fn)
{
   for (int r = R15; r >= R12; --r)
      if (rstest(fn->regusage, r))
            Xpop(pcode, r);
   if (rstest(fn->regusage, RBX)) Xpop(pcode, RBX);
}

/* align code using NOPs */
static void
nops(uchar **pcode, int align)
{
   int rem;
   while ((rem = (*pcode - objout.textbegin) & (align - 1)) != 0) {
      switch (align - rem) {
      case 15: case 14: case 13: case 12: case 11: case 10:
      case 9: B(0x66);
      case 8: DS("\x0f\x1f\x84\x00\x00\x00\x00\x00"); break;
      case 7: DS("\x0f\x1f\x80\x00\x00\x00\x00"); break;
      case 6: B(0x66);
      case 5: DS("\x0f\x1f\x44\x00\x00"); break;
      case 4: DS("\x0f\x1f\x40\x00"); break;
      case 3: DS("\x0f\x1f\00"); break;
      case 2: B(0x66);
      case 1: B(0x90); break;
      }
   }
}

static void
emitbin(struct function *fn)
{
   struct block *blk;
   uchar **pcode = &objout.code;
   int npush = 0;
   uint epilogueaddr = 0;
   bool saverestore;

   if (nblkaddr < fn->nblk) {
      blkaddr = xrealloc(blkaddr, fn->nblk * sizeof *blkaddr);
      nblkaddr = fn->nblk;
   }
   memset(blkaddr, 0, nblkaddr * sizeof *blkaddr);

   nops(pcode, 16);
   fnstart = *pcode;
   curfnsym = fn->name;

   /** prologue **/

   /* only use frame pointer in non-leaf functions and functions that use the stack */
   usebp = 0;
   if (!fn->isleaf || fn->stksiz) {
      usebp = 1;
      /* push rbp; mov rbp, rsp */
      DS("\x55\x48\x89\xE5");
   }
   saverestore = calleesave(&npush, pcode, fn);
   if (usebp) rbpoff = -npush*8;

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

   if (fn->stksiz != 0) {
      /* sub rsp, <stack size> */
      if (fn->stksiz < 128)
         DS("\x48\x83\xEC"), B(fn->stksiz);
      else if (fn->stksiz == 128)
         DS("\x48\x83\xC4\x80"); /* add rsp, -128 */
      else
         DS("\x48\x81\xEC"), I32(fn->stksiz);
   }

   blk = fn->entry;
   do {
      struct blkaddr *bb = &blkaddr[blk->id];
      uint bbaddr = *pcode - objout.textbegin;
      assert(!bb->resolved);
      while (bb->relreloc) {
         uint next;
         int disp = bbaddr - bb->relreloc - 4;

         memcpy(&next, objout.textbegin + bb->relreloc, 4);
         wr32le(objout.textbegin + bb->relreloc, disp);
         bb->relreloc = next;
      }
      bb->resolved = 1;
      bb->addr = bbaddr;

      for (int i = 0; i < blk->ins.n; ++i) {
         emitinstr(pcode, fn, blk, i, &instrtab[blk->ins.p[i]]);
      }
      if (blk->jmp.t == Jret) {
         /* epilogue */
         uint here = *pcode - fnstart;
         if (epilogueaddr) {
            int disp = epilogueaddr - (here + 2);
            if ((uint)(disp + 128) < 256) {/* can use 1-byte displacement? */
               B(0xEB), B(disp); /* JMP rel8 */
            } else {
               B(0xE9), I32(disp - 3); /* JMP rel32 */
            }
         } else {
            if (fn->stksiz && (saverestore || !usebp))
               Xadd(pcode, KPTR, mkoper(OREG, .reg = RSP), mkoper(OIMM, .imm = fn->stksiz));
            if (saverestore) {
               epilogueaddr = here;
               calleerestore(pcode, fn);
            }
            if (usebp) B(0xC9); /* leave */
            B(0xC3); /* ret */
         }
      } else if (blk->jmp.t == Jtrap) {
         DS("\x0F\x0B"); /* UD2 */
      } else emitbranch(pcode, blk);
   } while ((blk = blk->lnext) != fn->entry);
   objdeffunc(fn->name, fn->globl, fnstart - objout.textbegin, *pcode - fnstart);
}

void
amd64_emit(struct function *fn)
{
   fn->stksiz = alignup(fn->stksiz, 8);
   if (fn->stksiz > 1<<24) error(NULL, "'%s' stack frame too big", fn->name);
   emitbin(fn);
}

/* vim:set ts=3 sw=3 expandtab: */
