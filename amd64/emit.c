#include "all.h"
#include "../obj.h"
#include "../endian.h"

/** Instruction operands **
 *
 * Can be a register, a 32-bit immediate,
 * a memory reference [base + index * scale + disp],
 * or a RIP-relative reference to some symbol
 */
enum operkind { ONONE, OREG, OIMM, OMEM, OCONR };
enum { NOBASE = 99, NOINDEX = 99 };
static struct oper {
   uchar t;
   struct { uchar shift, index, base; }; /* OMEM */
   union {
      uchar reg; /* OREG */
      int disp; /* OMEM */
      int imm; /* OIMM */
      int con; /* OCONR, conht index*/
   };
} ioper[MAXINSTR];
#define mkoper(t, ...) ((struct oper){(t), __VA_ARGS__})
#define reg2oper(R) (assert((uint)(R) <= XMM15), mkoper(OREG, .reg = (R)))

static struct oper mkmemoper(union ref);

static struct oper
ref2oper(union ref r)
{
   switch (r.t) {
   case RTMP: return ioper[r.i];
   case RREG: return reg2oper(r.i);
   case RICON: return mkoper(OIMM, .imm = r.i);
   case RXCON:
      if (conht[r.i].cls == KI4)
         return mkoper(OIMM, .imm = conht[r.i].i);
      else if (!conht[r.i].cls)
         return mkoper(OCONR, .con = r.i);
      assert(0);
   case RMORE: return mkmemoper(r);
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
   assert(r.t == RREG || (r.t == RTMP && ioper[r.i].t == RREG));
   return r.t == RREG ? reg2oper(r.i) : ioper[r.i];
}

static inline struct oper
mkimmoper(union ref r)
{
   assert(iscon(r) && concls(r) == KI4);
   return mkoper(OIMM, .imm = intconval(r));
}

#define ismemref(ref) ((ref).t == RTMP && ioper[(ref).i].t == OMEM)
#define isregref(ref) ((ref).t == RREG || ((ref).t == RTMP && ioper[(ref).i].t == OREG))

static inline struct oper
mkimmregoper(union ref r)
{
   assert(isregref(r) || (iscon(r) && concls(r) == KI4));
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
   assert(isregref(r) || r.t == RICON || (r.t == RXCON && (conht[r.i].cls == KI4 || conht[r.i].deref)));
   return ref2oper(r);
}

static struct oper
mkmemoper(union ref r)
{
   if (r.t == RTMP) {
      struct oper wop = ioper[r.i];
      if (wop.t == OMEM) return wop;
      assert(wop.t == OREG);
      return mkoper(OMEM, .base = wop.reg, .index = NOINDEX);
   } else if (r.t == RMORE) {
      const struct addr *addr = &addrht[r.i];
      struct oper mem;
      if (addr->base.t == RTMP && ioper[addr->base.i].t == OMEM) {
         mem = ioper[addr->base.i];
         if (addr->index.t) addmemoper(&mem, mkregoper(addr->index));
         assert(!mem.shift);
         mem.shift = addr->shift;
         addmemoper(&mem, mkoper(OIMM, .imm = addr->disp));
         return mem;
      }
      return mkoper(OMEM, .base = addr->base.t ? mkregoper(addr->base).reg : NOBASE,
                          .index = addr->index.t ? mkregoper(addr->index).reg : NOINDEX,
                          .disp = addr->disp,
                          .shift = addr->shift);
   } else if (r.t == RXCON) {
      assert(!conht[r.i].cls);
      return mkoper(OCONR, .con = r.i);
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
   PI8,
   PI16,
   PI32,
   PU32,
   PMEM,
   PSYM,
};
enum operenc {
   EN_R = 1,  /* reg with /r */
   EN_RR,     /* reg, reg with /r */
   EN_MR,     /* mem, reg with /r */
   EN_RM,     /* reg, mem with /r */
   EN_M,      /* mem */
   EN_RI8,    /* reg, imm8 with /0 */
   EN_RI32,   /* reg, imm32 with /0 */
   EN_MI8,    /* mem, imm8 with /x */
   EN_MI16,   /* mem, imm16 with /x */  
   EN_MI32,   /* mem, imm32 with /x */
   EN_OI,     /* reg, imm32 with op + reg */
   EN_I32,    /* imm32 */
   EN_R32,    /* rel32 */
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
   case PI8:  return oper.t == OIMM && (uint)(oper.imm+128) < 256;
   case PI16: return oper.t == OIMM && (short)oper.imm == oper.imm;
   case PI32: return oper.t == OIMM;
   case PU32: return oper.t == OIMM && oper.imm >= 0;
   case PMEM: return in_range(oper.t, OMEM, OCONR);
   case PSYM: return oper.t == OCONR;
   }
   assert(0);
}

/* code output helpers */
#define B(b) (*(*pcode)++ = (b))
#define D(xs, N) (memcpy(*pcode, (xs), (N)), (*pcode) += (N))
#define I16(w) (wr16le(*pcode, (w)), *pcode += 2)
#define I32(w) (wr32le(*pcode, (w)), *pcode += 4)
#define DS(S) D(S, sizeof S - 1)

/* Given an instruction description table, find the first entry that matches
 * the operands (where dst, src are the operands in intel syntax order) and encode it */
static void
encode(uchar **pcode, const struct desc *tab, int ntab, enum irclass k, struct oper dst, struct oper src)
{
   const uchar *opc; int nopc, mod, rex;
   bool sib = 0; uchar reg; struct oper mem; const struct desc *en = NULL;

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
   rex = in_range(k, KI8, KPTR) << 3; /* REX.W */
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
      if (mem.t == OCONR) { /* RIP-relative addressing with relocation */
         mod = 0;
         mem.disp = mem.con;
         mem.base = RBP;
         sib = 0;
         if (rex) B(0x40 | rex);
         goto EmitMem;
      }
      rex |= mem.base >> 3; /* REX.B */
      if (mem.t != EN_M)
         rex |= (reg >> 3) << 2; /* REX.R */
      if (rex) B(0x40 | rex);
      else if (en->r8 && in_range(reg, RSP, RDI)) B(0x40);
      if (mem.index == NOINDEX && mem.shift == 0) sib = 0;
      else sib = 1;
      mod = !mem.disp ? 0                    /* disp = 0 -> mod = 00 */
          : (uint)(mem.disp + 128) < 256 ? 1 /* disp8 -> mod = 01 */
          : 2;                               /* disp32 -> mod = 10 */
      if (mod == 0 && (mem.base == RBP || mem.base == R13)) mod = 1;
      if (mem.base == RSP || mem.base == R12) sib = 1;
   EmitMem:
      D(opc, nopc);
      B(mod << 6 | (reg & 7) << 3 | (sib ? 4 : mem.base));
      if (sib)
         B(mem.shift << 6 | (mem.index & 7) << 3 | (mem.base & 7));
      if (mod == 1) B(mem.disp);
      else if (mod == 2 || (mod == 0 && mem.base == RBP/*RIP-rel*/)) {
         if (mem.t == OCONR) {
            objreloc(xcon2sym(mem.con), REL_PCREL32, Stext, *pcode - objout.textbegin, -4);
            mem.disp = 0;
         }
         I32(mem.disp);
      }
      if (en->operenc == EN_MI8) B(src.imm);
      if (en->operenc == EN_MI16) I16(src.imm);
      if (en->operenc == EN_MI32) I32(src.imm);
      break;
   case EN_R: case EN_RI32: case EN_RI8:
      rex |= (dst.reg >> 3) << 0; /* REX.B */
      if (rex) B(0x40 | rex);
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
   case EN_I32:
      if (rex) B(0x40 | rex);
      D(opc, nopc);
      I32(src.imm);
      break;
   case EN_R32:
      if (rex) B(0x40 | rex);
      D(opc, nopc);
      assert(dst.t == OCONR);
      objreloc(xcon2sym(dst.con), REL_PCREL32, Stext, *pcode - objout.textbegin, -4);
      I32(0);
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
)
DEFINSTR2(Xmovw,
   {-1, PMEM, PGPR, "\x66\x89", EN_MR},   /* MOV m16, r16 */
   {-1, PMEM, PI16, "\x66\xC7", EN_MI16}, /* MOV m16, imm16 */
)
static void Xmov(uchar **pcode, enum irclass k, struct oper dst, struct oper src)
{
   static const struct desc all[] = {
      {4  , PGPR, PI32, "\xB8",     EN_OI}, /* MOV r32, imm */
      {4|8, PGPR, PGPR, "\x8B",     EN_RR}, /* MOV r32/64, r32/64 */
      {4|8, PMEM, PGPR, "\x89",     EN_MR}, /* MOV m32/64, r32/64 */
      {4|8, PGPR, PMEM, "\x8B",     EN_RM}, /* MOV r32/64, m32/64 */
      {  8, PGPR, PU32, "\xB8",     EN_OI, .norexw=1}, /* MOV r64, uimm */
      {  8, PGPR, PI32, "\xC7",     EN_RI32}, /* MOV r64, imm */
      {4, PFPR, PFPR, "\xF3\x0F\x10", EN_RR}, /* MOVSS xmm, xmm */
      {4, PFPR, PMEM, "\xF3\x0F\x10", EN_RM}, /* MOVSS xmm, m32 */
      {4, PMEM, PFPR, "\xF3\x0F\x10", EN_MR}, /* MOVSS m32, xmm */
      {8, PFPR, PFPR, "\xF2\x0F\x10", EN_RR}, /* MOVSD xmm, xmm */
      {8, PFPR, PMEM, "\xF2\x0F\x10", EN_RM}, /* MOVSD xmm, m64 */
      {8, PMEM, PFPR, "\xF2\x0F\x11", EN_MR}, /* MOVSS m64, xmm */
   };
   static const uchar k2off[] = {
      [KI4] = 0,
      [KI8] = 1, [KPTR] = 1,
      [KF4] = 6,
      [KF8] = 9,
   };
   encode(pcode, all + k2off[k], arraylength(all) - k2off[k], k, dst, src);
}
DEFINSTR2(Xmovsxl,
   {8, PGPR, PMEM, "\x63", EN_RM}, /* MOVSXD r64, m32 */
   {8, PGPR, PGPR, "\x63", EN_RR}, /* MOVSXD r64, r32 */
   {4, PGPR, PMEM, "\x8B", EN_RM}, /* MOV r32, m32 */
   {4, PGPR, PGPR, "\x89", EN_RR}, /* MOV r32, r32 */
)
DEFINSTR2(Xmovsxw,
   {4|8, PGPR, PMEM, "\x66\x0F\xBF", EN_RM}, /* MOVSX r64, m16 */
   {4|8, PGPR, PGPR, "\x66\x0F\xBF", EN_RR}, /* MOVSX r64, r16 */
)
DEFINSTR2(Xmovsxb,
   {4|8, PGPR, PMEM, "\x0F\xBE", EN_RM}, /* MOVSX r64, m8 */
   {4|8, PGPR, PGPR, "\x0F\xBE", EN_RR, .r8=1}, /* MOVSX r64, r8 */
)
DEFINSTR2(Xmovzxw,
   {4|8, PGPR, PMEM, "\x66\x0F\xB7", EN_RM}, /* MOVZX r64, m16 */
   {4|8, PGPR, PGPR, "\x66\x0F\xB7", EN_RR}, /* MOVZX r64, r16 */
)
DEFINSTR2(Xmovzxb,
   {4|8, PGPR, PMEM, "\x0F\xB6", EN_RM}, /* MOVZX r64, m8 */
   {4|8, PGPR, PGPR, "\x0F\xB6", EN_RR, .r8=1}, /* MOVZX r64, r8 */
)
DEFINSTR2(Xlea,
   {4|8, PGPR, PMEM, "\x8D", EN_RM}, /* LEA r32/64,m32/64 */
)
DEFINSTR2(Xadd,
   {4|8, PGPR, PGPR, "\x03", EN_RR},   /* ADD r32/64, r32/64 */
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
DEFINSTR2(Xxor,
   {4|8, PGPR, PGPR, "\x33", EN_RR},           /* XOR r32/64, r32/64 */
   {4|8, PGPR, PI8,  "\x83", EN_RI8, .ext=6},  /* XOR r32/64, imm8 */
   {4|8, PRAX, PI32, "\x35", EN_I32},          /* XOR eax/rax, imm */
   {4|8, PGPR, PI32, "\x81", EN_RI32, .ext=6}, /* XOR r32/64, imm */
   {  8, PGPR, PMEM, "\x33", EN_RM},           /* XOR r64, m64 */
   {4|8, PFPR, PFPR, "\x0F\x57", EN_RR}, /* XORPS xmm, xmm */
)
DEFINSTR2(Xshl,
   {4|8, PGPR, P1,   "\xD1", EN_R, .ext=4},   /* SHL r32/64, 1 */
   {4|8, PGPR, PI32, "\xC1", EN_RI8, .ext=4}, /* SHL r32/64, imm */
   {4|8, PGPR, PRCX, "\xD3", EN_R, .ext=4},   /* SHL r32/64, CL */
)
DEFINSTR1(Xinc,
   {4|8, PGPR, 0, "\xFF", EN_R, .ext=0} /* INC r32/64 */
)
DEFINSTR1(Xdec,
   {4|8, PGPR, 0, "\xFF", EN_R, .ext=1} /* DEC r32/64 */
)
DEFINSTR1(Xidiv,
   {4|8, PGPR, 0, "\xF7", EN_R, .ext=7}, /* IDIV r32/64 */
   {4|8, PMEM, 0, "\xF7", EN_M, .ext=7}, /* IDIV m32/64 */
)
DEFINSTR1(Xcall,
   {-1, PSYM, 0, "\xE8", EN_R32, .norexw=1}, /* CALL rel32 */
   {-1, PGPR, 0, "\xFF", EN_R, .ext=2, .norexw=1}, /* CALL r64 */
   {-1, PMEM, 0, "\xFF", EN_M, .ext=2, .norexw=1}, /* CALL m64 */
)

static void
Xpush(uchar **pcode, enum reg reg)
{
   assert(in_range(reg, RAX, R15));
   if (reg >> 3) B(0x44); /* REX.R */
   B(0x50 + (reg & 7));
}

static void
Xpop(uchar **pcode, enum reg reg)
{
   assert(in_range(reg, RAX, R15));
   if (reg >> 3) B(0x44); /* REX.R */
   B(0x58 + (reg & 7));
}

/* Copy dst = val, with some peephole optimizations */
static void
gencopy(uchar **pcode, enum irclass cls, struct oper dst, union ref val)
{
   assert(dst.t == OREG);
   if (val.t == RMORE) {
      /* this is a LEA, but maybe it can be lowered to a 2-address instruction */
      const struct addr *addr = &addrht[val.i];
      if (addr->base.t && dst.reg == mkregoper(addr->base).reg) { /* base = dst */
         if (addr->index.t && !addr->disp && !addr->shift){
            /* lea Rx, [Rx + Ry] -> add Rx, Ry */
            Xadd(pcode, cls, dst, mkregoper(addr->index));
            return;
         } else if (!addr->index.t) {
            if (!addr->disp) /* lea Rx, [Rx] -> mov Rx, Rx */
               Xmov(pcode, cls, dst, dst);
            else /* lea Rx, [Rx + Imm] -> add Rx, Imm */
               Xadd(pcode, cls, dst, mkoper(OIMM, .imm = addr->disp));
            return;
         }
      } else if (addr->index.t && dst.reg == mkregoper(addr->index).reg) { /* index = dst */
         if (addr->base.t && !addr->disp && !addr->shift) {
            /* lea Rx, [Ry + Rx] -> add Rx, Ry */
            Xadd(pcode, cls, dst, mkregoper(addr->base));
            return;
         } else if (!addr->base.t) {
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
      Xlea(pcode, cls, dst, ref2oper(val));
   } else if (val.t == RICON && val.i == 0 && dst.t == OREG) {
      /* dst = 0 -> xor dst, dst */
      Xxor(pcode, cls, dst, dst);
   } else if (val.t == RXCON && conht[val.i].isdat && !conht[val.i].deref) {
      Xlea(pcode, cls, dst, mkoper(OCONR, .con = val.i));
   } else {
      struct oper src = mkimmdatregoper(val);
      if (memcmp(&dst, &src, sizeof dst) != 0)
         Xmov(pcode, cls, dst, src);
   }
}

static void
emitinstr(uchar **pcode, struct function *fn, struct block *blk, int ii, struct instr *ins)
{
   struct oper dst, src;
   enum irclass cls = ins->cls;
   void (*X)(uchar **, enum irclass, struct oper, struct oper) = NULL;
   void (*X1)(uchar **, enum irclass, struct oper) = NULL;

   switch (ins->op) {
   default: assert(!"nyi ins");
   case Onop: break;
   case Ostore1: cls = KI4, X = Xmovb; goto Store;
   case Ostore2: cls = KI4, X = Xmovw; goto Store;
   case Ostore4: cls = KI4, X = Xmov; goto Store;
   case Ostore8: cls = KI8, X = Xmov;
   Store:
      src = mkimmregoper(ins->r);
      if (cls == KI4 && src.t == OREG && src.reg >= XMM0) cls = KF4;
      if (cls == KI8 && src.t == OREG && src.reg >= XMM0) cls = KF8;
      X(pcode, cls, mkmemoper(ins->l), src);
      break;
   case Oexts1:  src = mkregoper(ins->l); goto Movsxb;
   case Oextu1:  src = mkregoper(ins->l); goto Movzxb;
   case Oexts2:  src = mkregoper(ins->l); goto Movsxw;
   case Oextu2:  src = mkregoper(ins->l); goto Movzxw;
   case Oexts4:  src = mkregoper(ins->l); goto Movsxl;
   case Oextu4:  src = mkregoper(ins->l); goto Movzxl;
   case Oloads1: src = mkmemoper(ins->l); Movsxb: Xmovsxb(pcode, cls, reg2oper(ins->reg-1), src); break;
   case Oloadu1: src = mkmemoper(ins->l); Movzxb: Xmovzxb(pcode, cls, reg2oper(ins->reg-1), src); break;
   case Oloads2: src = mkmemoper(ins->l); Movsxw: Xmovsxw(pcode, cls, reg2oper(ins->reg-1), src); break;
   case Oloadu2: src = mkmemoper(ins->l); Movzxw: Xmovzxw(pcode, cls, reg2oper(ins->reg-1), src); break;
   case Oloads4: src = mkmemoper(ins->l); Movsxl: Xmovsxl(pcode, cls, reg2oper(ins->reg-1), src); break;
   case Oloadu4: src = mkmemoper(ins->l); Movzxl: Xmov(pcode, KI4, reg2oper(ins->reg-1), src); break;
   case Oloadf4: case Oloadf8: Xmov(pcode, cls, reg2oper(ins->reg-1), mkmemoper(ins->l)); break;
   case Oloadi8: Xmov(pcode, KI8, reg2oper(ins->reg-1), mkmemoper(ins->l)); break;
   case Oadd:
      dst = mkregoper(ins->l);
      if (kisflt(cls)) {
         Xaddf(pcode, cls, dst, mkimmdatregoper(ins->r));
      } else if (ins->reg-1 == dst.reg) { /* two-address add */
         Xadd(pcode, cls, dst, mkimmdatregoper(ins->r));
      } else { /* three-address add (lea) */
         struct oper mem = { OMEM, .base = NOBASE, .index = NOINDEX };
         dst = reg2oper(ins->reg-1);
         addmemoper(&mem, ref2oper(ins->l));
         addmemoper(&mem, ref2oper(ins->r));
         Xlea(pcode, cls, dst, mem);
      }
      break;
   case Osub: X = kisint(cls) ? Xsub : Xsubf; goto ALU2;
   case Oshl: X = Xshl; goto ALU2;
   ALU2:
      dst = mkregoper(ins->l);
      assert(ins->reg-1 == dst.reg);
      X(pcode, cls, dst, mkimmdatregoper(ins->r));
      break;
   case Oxinc: X1 = Xinc; goto ALU1;
   case Oxdec: X1 = Xdec; goto ALU1;
   ALU1:
      dst = mkregoper(ins->l);
      assert(ins->reg-1 == dst.reg);
      X1(pcode, cls, dst);
      break;
   case Odiv:
      switch (cls) {
      default: assert(0);
      case KPTR:
      case KI8: B(0x48); /* REX.W */
      case KI4: B(0x99); /* CDQ/CQO */
                assert(mkregoper(ins->l).reg == RAX);
                Xidiv(pcode, cls, mkdatregoper(ins->r));
                break;
      case KF4: case KF8: assert(!"nyi");
      }
      break;
   case Omove:
      dst = ref2oper(ins->l);
      gencopy(pcode, cls, dst, ins->r);
      break;
   case Ocopy:
      dst = reg2oper(ins->reg-1);
      gencopy(pcode, cls, dst, ins->l);
      break;
   case Ocall:
      Xcall(pcode, KPTR, ref2oper(ins->l));
      break;
   }
   if (ins->reg) ioper[ins - instrtab] = reg2oper(ins->reg-1);
}

static void
calleesave(uchar **pcode, struct function *fn)
{
   if (bstest(fn->regusage, RBX)) Xpush(pcode, RBX);
   for (int r = R12; r <= R15; ++r)
      if (bstest(fn->regusage, r))
            Xpush(pcode, r);
}

static void
calleerestore(uchar **pcode, struct function *fn)
{
   for (int r = R15; r >= R12; --r)
      if (bstest(fn->regusage, r))
            Xpop(pcode, r);
   if (bstest(fn->regusage, RBX)) Xpop(pcode, RBX);
}

/* align code using NOPs */
static void
aligncode(uchar **pcode, int align)
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
   uchar *start;

   aligncode(pcode, 16);
   start = *pcode;

   /** prologue **/
   if (fn->stksiz != 0)
      /* push rbp; mov rbp, rsp */
      DS("\x55\x48\x89\xE5");
   calleesave(pcode, fn);
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
      for (int i = 0; i < blk->ins.n; ++i) {
         emitinstr(pcode, fn, blk, i, &instrtab[blk->ins.p[i]]);
      }
      if (blk->jmp.t == Jret) {
         /* epilogue */
         calleerestore(pcode, fn);
         if (fn->stksiz) B(0xC9); /* leave */
         B(0xC3); /* ret */
      }
   } while ((blk = blk->lnext) != fn->entry);
   objdeffunc(fn->name, fn->globl, start - objout.textbegin, *pcode - start);
}

void
amd64_emit(struct function *fn)
{
   fn->stksiz = alignup(fn->stksiz, 16);
   if (fn->stksiz > 1<<24) error(NULL, "'%s' stack frame too big", fn->name);
   emitbin(fn);
}

/* vim:set ts=3 sw=3 expandtab: */
