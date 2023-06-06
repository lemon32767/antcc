#include "all.h"
#include "../obj.h"

static inline void
wr32le(uchar *p, uint x)
{
   p[0] = x >>  0; p[1] = x >>  8;
   p[2] = x >> 16; p[3] = x >> 24;
}

#define B(b) (*(*pcode)++ = (b))
#define D(xs, N) (memcpy(*pcode, (xs), (N)), (*pcode) += (N))
#define I32(w) (wr32le(*pcode, (w)), *pcode += 4)
#define DS(S) D(S, sizeof S - 1)

enum operkind { ONONE, OMEM, OGPR, OFPR, OIMM };
enum size { Z8, Z16, Z32, Z64 };
static struct oper {
   uchar siz : 4, t : 4;
   struct { uchar shift, index, base; }; /* OMEM */
   union {
      uchar reg; /* OGPR/FPR */
      int disp; /* OMEM */
      int imm; /* OIMM */
   };
} ioper[MAXINSTR];
#define mkoper(t, sz, ...) ((struct oper){(sz), (t), __VA_ARGS__})
#define reg2oper(R, sz) mkoper(OGPR + ((R) >= XMM0), sz, .reg = (R))
static const char size4cls[] = {Z64, [KI4] = Z32, [KI8] = Z64, [KPTR] = Z64, [KF4] = Z32, [KF8] = Z64};

static struct oper
ref2oper(union ref r)
{
   extern struct xcon conht[];
   switch (r.t) {
   case RTMP: return ioper[r.i];
   case RICON: return mkoper(OIMM, Z32, .imm = r.i);
   case RXCON:
      assert(conht[r.i].cls == KI4);
      return mkoper(OIMM, Z32, .imm = conht[r.i].i4);
   default: assert(0);
   }
}

enum { NOBASE = RBP, NOINDEX = RSP };

static struct oper
addmemoper(struct oper mem, struct oper add)
{
   assert(mem.t == OMEM);
   if (add.t == OIMM) {
      mem.disp += add.imm;
   } else if (add.t == OGPR) {
      assert(mem.index == NOINDEX);
      mem.index = add.reg;
   }
   return mem;
}

enum operenc {
   EN_RR = 1, /* reg, reg with /r */
   EN_MR,     /* mem, reg with /r */
   EN_RM,     /* reg, mem with /r */
   EN_RI32,   /* reg, imm32 with /0 */
   EN_OI,     /* reg, imm32 with op + reg */
};
struct desc {
   uchar siz;
   uchar td, ts;
   const char *opc;
   uchar ext;
   uchar operenc;
};

static void
encode(uchar **pcode, const struct desc *tab, int ntab, struct oper dst, struct oper src)
{
   const char *opc;
   int nopc, mod, rex;
   bool sib = 0;
   uchar reg;
   struct oper mem;
   const struct desc *en = NULL;
  
   for (int i = 0; i < ntab; ++i) {
      // efmt("<%d, %d>, <%d, %d>, <%d, %d>\n", tab[i].siz, dst.siz, tab[i].td, dst.t, tab[i].ts, src.t);
      if (dst.siz == tab[i].siz && tab[i].td == dst.t && tab[i].ts == src.t) {
         en = &tab[i];
         break;
      }
   }
   assert(en && "no match for instr");

   rex = (en->siz == Z64) << 3; /* REX.W */
   opc = en->opc;
   nopc = strlen(opc);
   switch (en->operenc) {
   case EN_RR: /* mod = 11; reg = dst; rm = src */
      rex |= (dst.reg >> 3) << 2; /* REX.R */
      rex |= (src.reg >> 3) << 0; /* REX.B */
      if (rex) B(0x40 | rex);
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
   Mem:
      rex |= (     reg >> 3) << 2; /* REX.R */
      rex |= (mem.base >> 3) << 0; /* REX.B */
      if (rex) B(0x40 | rex);
      else if (en->siz == Z8 && in_range(reg, RSP, RDI)) 
         /* /r8 needs REX to encode SP,BP,SI,DI (otherwise -> AH..BH) */
         B(0x40);
      if (mem.index == NOINDEX && mem.shift == 0) sib = 0;
      mod = !mem.disp ? 0                    /* disp = 0 -> mod = 00 */
          : (uint)(mem.disp + 128) < 256 ? 1 /* disp8 -> mod = 01 */
          : 2;                               /* disp32 -> mod = 10 */
      if (mod == 0 && (mem.base == RBP || mem.base == R13)) mod = 1;
      if (mem.base == RSP || mem.base == R12) sib = 1;
      D(opc, nopc);
      B(mod << 6 | (reg & 7) << 3 | (sib ? 4 : mem.base));
      if (sib)
         B(mem.shift << 6 | (mem.index & 7) << 3 | (mem.base & 7));
      if (mod == 1) B(mem.disp);
      else if (mod == 2) I32(mem.disp);
      break;
   case EN_RI32:
      rex |= (dst.reg >> 3) << 0; /* REX.B */
      if (rex) B(0x40 | rex);
      D(opc, nopc);
      B(0300 | en->ext << 3 | (dst.reg & 7));
      I32(src.imm);
      break;
   case EN_OI:
      rex |= (dst.reg >> 3) << 0; /* REX.B */
      if (rex) B(0x40 | rex);
      B(*opc++ + (dst.reg & 7));
      D(opc, nopc - 1);
      I32(src.imm);
      break;
   }
}

#define DEFINSTR2(X, ...)                               \
   static void                                          \
   X(uchar **pcode, struct oper dst, struct oper src)   \
   {                                                    \
      static const struct desc tab[] = { __VA_ARGS__ }; \
      encode(pcode, tab, arraylength(tab), dst, src);   \
   }

DEFINSTR2(Xmov,
   {Z8,  OMEM, OGPR, "\x88", 0,     EN_MR}, /* MOV m8, r8 */
   {Z16, OMEM, OGPR, "\x66\x89", 0, EN_MR}, /* MOV m16, r16 */
   {Z32, OGPR, OGPR, "\x89", 0,     EN_RR}, /* MOV r32, r32 */
   {Z32, OMEM, OGPR, "\x89", 0,     EN_MR}, /* MOV m32, r32 */
   {Z32, OGPR, OMEM, "\x8B", 0,     EN_RM}, /* MOV r32, m32 */
   {Z32, OGPR, OIMM, "\xB8", 0,     EN_OI}, /* MOV r32, imm */
   {Z64, OGPR, OGPR, "\x89", 0,     EN_RR}, /* MOV r64, r64 */
   {Z64, OMEM, OGPR, "\x89", 0,     EN_MR}, /* MOV m64, r64 */
   {Z64, OGPR, OMEM, "\x8B", 0,     EN_RM}, /* MOV r64, m64 */
   {Z32, OGPR, OIMM, "\xB8", 0,     EN_OI}, /* MOV r64, imm */
)
DEFINSTR2(Xlea,
   {Z32, OGPR, OMEM, "\x8D", 0,     EN_RM}, /* LEA r32,m32 */
   {Z64, OGPR, OMEM, "\x8D", 0,     EN_RM}, /* LEA r64,m64 */
)
DEFINSTR2(Xadd,
   {Z32, OGPR, OGPR, "\x03", 0,     EN_RR},
   {Z32, OGPR, OIMM, "\x81", 0,     EN_RI32},
   {Z64, OGPR, OGPR, "\x03", 0,     EN_RR},
   {Z64, OGPR, OIMM, "\x81", 0,     EN_RI32},
)

static bool /* stack frame size <= 128? */
smallstack(struct function *fn)
{
   uint stktop = 0;
   struct block *blk = fn->entry;
   do {
      for (int i = 0; i < blk->ins.n; ++i) {
         struct instr *ins = &instrtab[blk->ins.p[i]];
         if (oisalloca(ins->op)) {
            uint align = 1 << (ins->op - Oalloca1);
            uint siz = ins->l.i * align;
            stktop = alignup(stktop + siz, align);
            if (alignup(stktop, 16) > 128) return 0;
         }
      }
   } while ((blk = blk->lnext) != fn->entry);
   return 1;
}

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
      case 3: DS("\x0f\1f\00"); break;
      case 2: B(0x66);
      case 1: B(0x90); break;
      }
   }
}

#define ismemref(ref) ((ref).t == RTMP && ioper[(ref).i].t == OMEM)
#define isregref(ref) ((ref).t == RTMP && in_range(ioper[(ref).i].t, OGPR, OFPR))

static void
emitbin(struct function *fn)
{
   struct block *blk;
   const struct instr *ins;
   uchar *rspdisp;
   uint stktop = 0;
   bool stack8 = smallstack(fn);
   uchar **pcode = &objout.code;

   aligncode(pcode, 16);

   /** prologue **/
   /* push rbp; mov rbp, rsp */
   DS("\x55\x48\x89\xE5");
   /* sub rsp, <stack size> */
   if (stack8)
      DS("\x48\x83\xEC"), rspdisp = *pcode, DS("\xAA");
   else
      DS("\x48\x81\xEC"), rspdisp = *pcode, DS("\xAA\xAA\xAA\xAA");

   blk = fn->entry;
   do {
      for (int i = 0; i < blk->ins.n; ++i) {
         struct oper dst, src;
         ins = &instrtab[blk->ins.p[i]];
         if (oisalloca(ins->op)) {
            uint alignlog2 = ins->op - Oalloca1;
            uint siz = ins->l.i << alignlog2;
            stktop += siz;
            stktop = alignup(stktop, 1 << alignlog2);
            ioper[ins - instrtab] = mkoper(OMEM, alignlog2, .base = RBP, .index = NOINDEX, .disp = -stktop);
         } else if (oisstore(ins->op)) {
            dst = ref2oper(ins->l);
            if (!ismemref(ins->l)) {
               assert(isregref(ins->l));
               dst = mkoper(OMEM, ins->op - Ostore1, .base = dst.reg, .index = NOINDEX);
            }
            if (ins->r.t == RPARAM) {
               struct abiarg abi = fn->abiarg[ins->r.i];
               assert(abi.reg >= 0 && "stk");
               Xmov(pcode, ref2oper(ins->l), mkoper(OGPR, ioper[ins->l.t].siz, .reg = abi.reg));
            } else {
               src = ref2oper(ins->r);
               Xmov(pcode, ref2oper(ins->l), ref2oper(ins->r));
            }
         } else if (oisload(ins->op)) {
            src = ref2oper(ins->l);
            if (src.t == OGPR) src = mkoper(OMEM, size4cls[ins->cls], .base = src.reg, .index = NOINDEX); 
            assert(ins->reg);
            assert(src.t == OMEM);
            if (in_range(ins->op, Oloads4, Oloadi8)) {
               Xmov(pcode, reg2oper(ins->reg-1, size4cls[ins->cls]), src);
            } else assert(0);
         } else switch (ins->op) {
         default: if (ins->reg) assert(!"nyi ins");
         case Oadd:
            if (ismemref(ins->l)) {
               assert(ins->reg);
               Xlea(pcode, reg2oper(ins->reg-1, size4cls[ins->cls]),
                           addmemoper(ioper[ins->l.i], ref2oper(ins->r)));
            } else {
               struct oper l = ref2oper(ins->l);
               assert(in_range(l.t, OGPR, OFPR) && ins->reg && ins->reg-1 == l.reg);
               Xadd(pcode, l, ref2oper(ins->r));
            }
            break;
         }
         if (ins->reg) ioper[ins - instrtab] = reg2oper(ins->reg-1, size4cls[ins->cls]);
      }
      if (blk->jmp.t == Jret) {
         if (blk->jmp.arg[0].t) {
            struct oper rval = ref2oper(blk->jmp.arg[0]);
            if (!in_range(rval.t, OGPR, OFPR) || rval.reg != fn->abiret[0].reg)
               Xmov(pcode, reg2oper(fn->abiret[0].reg, size4cls[fn->abiret[0].ty.cls]), rval);
         }
         /* epilogue */
         DS("\xC9\xC3"); /* leave; ret */
      }
   } while ((blk = blk->lnext) != fn->entry);

   stktop = alignup(stktop, 16);
   if (stack8) {
      assert(stktop <= 128);
      if (stktop < 128) *rspdisp = stktop;
      else {
         /* cannot encode `sub rsp, 128` with 8bit imm, turn into `add rsp, -128` */
         rspdisp[-1] = 0xC4;
         *rspdisp = -128;
      }
   } else {
      wr32le(rspdisp, alignup(stktop, 16));
   }
}

void
amd64_emit(struct function *fn)
{
   emitbin(fn);
}

/* vim:set ts=3 sw=3 expandtab: */
