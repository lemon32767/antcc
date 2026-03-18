#include "antcc.h"
#include "c_type.h"

enum irclass {
   KXXX,
   KI32, KI64, KPTR,
   KF32, KF64,
};

#define kisint(k) in_range((k), KI32, KPTR)
#define kisflt(k) in_range((k), KF32, KF64)

typedef struct IRDat {
   uchar align : 6, globl : 1;
   uchar section;
   Type ctype;
   uint siz;
   uint off;
   internstr name;
} IRDat;

enum symflags {
   SLOCAL = 1,
   SFUNC  = 2,
};
typedef struct IRCon {
   bool issym, isdat, deref;
   uchar cls;
   uchar flag;
   union {
      internstr sym;
      int dat;
      vlong i;
      double f;
   };
} IRCon;

typedef union IRType {
   struct { ushort _     : 1, cls : 15; };
   struct { ushort isagg : 1, dat : 15; };
   ushort bits;
} IRType;

typedef struct ABIArg {
   IRType ty;
   union {
      struct { ushort _     : 1, reg : 15; };
      struct { ushort isstk : 1, stk : 15; };
   };
} ABIArg;

typedef struct IRCall {
   IRType ret;
   ushort narg;
   short vararg; /* first variadic arg or -1 */
   ushort argstksiz;
   ABIArg *abiarg;
   ABIArg abiret[2];
   uchar r2off;
} IRCall;

enum refkind {
   RXXX,  /* used for empty ref (zeros), undef, and the special args of call,phi,etc */
   RTMP,  /* reference to another instruction's result */
   RREG,  /* machine register */
   RICON, /* small integer constants */
   RXCON, /* other constants (incl. external symbols) */
   RADDR, /* target-specific addressing mode */
   RTYPE, /* irtype */
   RSTACK, /* stack base offset */
};

typedef union Ref {
   struct { unsigned t : 3; signed i : 29; };
   uint bits;
} Ref;
static_assert(sizeof(Ref) == 4);

typedef struct IRAddr {
   Ref base, index;
   int shift, disp;
} IRAddr;

#define insrescls(ins) (oiscmp((ins).op) ? KI32 : (ins).cls)
#define NOREF ((Ref) {0})
#define UNDREF ((Ref) {{ 0, -1 }})
#define ZEROREF ((Ref) {{ RICON, 0 }})
#define mkref(t, x) ((Ref) {{ (t), (x) }})
#define mktyperef(t) ((Ref) {{ RTYPE, (t).bits }})
#define ref2type(r) ((IRType) {.bits = (r).i})
#define rswap(a,b) do { Ref _t = (a); (a) = (b); (b) = _t; } while (0)

enum op {
   Oxxx,
#define _(o,...) O##o,
#include "ir_op.def"
#undef _
   NOPER,
};

#define oiscmp(o) in_range(o, Oequ, Ougte)
#define oisarith(o) in_range(o, Oneg, Ougte)
#define oisalloca(o) in_range(o, Oalloca1, Oalloca16)
#define oisstore(o) in_range(o, Ostorei8, Ostoref64)
#define oisload(o) in_range(o, Oloads8, Oloadf64)
extern const char *opnames[];
extern const uchar opnarg[];

enum intrin {
   INxxx,
#define _(b,...) IN##b,
#include "ir_intrin.def"
#undef _
};

typedef struct Instr {
   uchar op,
         cls; /* operation data class; also result class except for cmp ops (always i32) */
   uchar skip : 1, /* ignore during codegen: forms part of one machine instruction */
         keep : 1; /* for codegen, keep instr even if result seems unused */
   uchar inplace : 1; /* set (by isel) for instructions which modify its first arg in place */
   uchar reg; /* 0 -> no reg; else reg + 1 */
   Ref l, r; /* args */
} Instr;
static_assert(sizeof(Instr) == 4*3);

enum jumpkind { JXXX, Jb, Jret, Jtrap, };

typedef struct Block Block;
struct Block {
   int id;
   int npred;
   int visit;
   ushort loop;
   int inumstart;
   union {
      Block *_pred0;
      Block **_pred;
   };
   Block *lprev, *lnext;
   Block *s1, *s2;
   Block *idom;
   vec_of(ushort) phi;
   vec_of(ushort) ins;
   struct { uchar t; Ref arg[2]; } jmp;
};

#define blkpred(blk, i) 0[(blk)->npred < 2 ? &(blk)->_pred0 : &(blk)->_pred[i]]

enum { USERJUMP = 0xFFFF };
typedef struct IRUse {
   struct IRUse *next;
   Block *blk;
   ushort u;
} IRUse;

enum { MAXREGS = 64 };
/** register set **/
typedef uvlong regset;
#define BIT(x) (1ull<<(x))
#define rsset(pS, r) (*(pS) |= 1ull << (r))
#define rsclr(pS, r) (*(pS) &=~ (1ull << (r)))
#define rstest(S, r) ((S) >> (r) & 1)
static inline bool
rsiter(int *i, uvlong rs)
{
   if (*i > 63) return 0;
   uvlong mask = -(1ull << *i);
   if ((rs & mask) == 0) return 0;
   *i = lowestsetbit(rs & mask);
   return 1;
}

enum {
   FNBLKID = 1<<0,
   FNUSE   = 1<<1,
   FNRPO   = 1<<2,
   FNDOM   = 1<<3,
};
typedef struct Function {
   Arena **arena, **passarena;
   internstr name;
   Block *entry, *curblk;
   IRUse *use;
   short *nuse;
   Type fnty, retty;
   ABIArg *abiarg, abiret[2];
   uint prop;
   uint nblk;
   int stksiz;
   ushort nabiarg, nabiret;
   bool globl;
   bool isleaf;
   bool inlin;
   regset regusage;
} Function;

#define FREQUIRE(_prop) assert((fn->prop & (_prop)) == (_prop) && "preconditions not met")

typedef struct MCTarg {
   short gpr0, /* first gpr */
         ngpr, /* gpr count */
         bpr,  /* frame/base pointer reg */
         gprscratch, fprscratch, /* scratch registers for regalloc */
         fpr0, /* first fpr */
         nfpr; /* fpr count */
   regset rcallee, /* callee-saved */
          rglob;   /* globally live (never used for regalloc) */
   const char (*rnames)[6];
   enum objkind objkind;
   /* abiret: lower return type:
    *  scalar/small struct -> returns number of regs (1..2),
    *                         r & cls filled with reg and irclass of each scalar return,
    *                         for struct, r2off filled with byte offset within struct for 2nd reg
    *  big struct -> returns 0, is passed via hidden pointer argument,
    *                r[0] contains register for returning said pointer or -1,
    *                r[1] contains register for hidden pointer argument,
    *                     *ni is set to 1 if said register is the first ABI integer argument
    */
   int (*abiret)(short r[2], uchar cls[2], uchar *r2off, int *ni, IRType);
   /* abiarg: lower argument type:
    *  scalar/small struct -> returns number of regs (1..2),
    *                         r & cls filled with reg and irclass of each scalar arg
    *                         for struct, r2off filled with byte offset within struct for 2nd reg
    *                         if reg == -1 -> stack
    *  big struct -> returns 0,
    *                if passed in stack   cls[0] == 0, r[0] == negative SP offset
    *                if passed by pointer cls[0] == KPTR, r[0] contains integer register
    *                                                     or negative SP offset if stack
    */
   int (*abiarg)(short r[2], uchar cls[2], uchar *r2off, int *ni, int *nf, int *ns, IRType);
   void (*vastart)(Function *, Block *, int *curi);
   void (*vaarg)(Function *, Block *, int *curi);
   void (*isel)(Function *);
   void (*emit)(Function *);
} MCTarg;

enum { MAXINSTR = 1<<15 };

/** ir.c **/
extern uchar type2cls[];
extern uchar cls2siz[];
extern uchar cls2load[];
extern uchar cls2store[];
extern const uchar siz2intcls[];
extern Instr instrtab[];
extern IRUse *instruse[];
extern struct calltab {vec_of(IRCall);} calltab;
extern struct phitab {vec_of(Ref *);} phitab;
extern struct dattab {vec_of(IRDat);} dattab;
extern struct contab {vec_of(IRCon);} contab;
extern struct addrtab {vec_of(IRAddr);} addrtab;
extern int visitmark;
#define mkinstr(O, C, ...) ((Instr) { .op = (O), .cls = (C), .reg=0, __VA_ARGS__ })
#define mkarginstr(ty, x) mkinstr(Oarg, 0, mktyperef(ty), (x))
void irinit(Function *);
void irfini(Function *);
#define cls2type(k) ((IRType){.cls=(k)})
IRType mkirtype(Type);
Ref newxcon(const IRCon *);
Ref mkintcon(enum irclass, vlong);
Ref mkfltcon(enum irclass, double);
#define iscon(r) in_range((r).t, RICON, RXCON)
#define concls(r) ((r).t == RICON ? KI32 : contab.p[(r).i].cls)
#define isintcon(r) (iscon(r) && kisint(concls(r)))
#define isfltcon(r) ((r).t == RXCON && kisflt(contab.p[(r).i].cls))
#define isnumcon(r) ((r).t == RICON || ((r).t == RXCON && contab.p[(r).i].cls))
#define isaddrcon(r,derefok) ((r).t == RXCON && !contab.p[(r).i].cls && (derefok || !contab.p[(r).i].deref))
#define intconval(r) ((r).t == RICON ? (r).i : contab.p[(r).i].i)
#define fltconval(r) ((r).t == RICON ? (r).i : contab.p[(r).i].f)
Ref mksymref(internstr, enum symflags);
Ref mkdatref(internstr sym, Type ctype, uint siz, uint align,
                   const void *, uint n, bool deref, bool funclocal);
internstr xcon2sym(int ref);
Instr mkalloca(uint siz, uint align);
Ref mkcallarg(IRType ret, uint narg, int vararg);
#define mkintrin(B, C, N) mkinstr(Ointrin, C, {{.t=RICON,B}}, mkcallarg((IRType){{0}},N,-1))
Ref mkaddr(IRAddr);
void addpred(Block *blk, Block *p);

Block *newblk(Function *);
void freeblk(Function *, Block *);
Block *insertblk(Function *, Block *pred, Block *subst);
Block *blksplitafter(Function *, Block *, int idx);
void adduse(Block *ublk, int ui, Ref r);
int newinstr(Block *at, Instr ins);
Ref insertinstr(Block *, int idx, Instr);
Ref insertphi(Block *, enum irclass cls);
void replcuses(Ref from, Ref to);
bool deluse(Block *ublk, int ui, Ref r);
void deluses(int ins);
void filluses(Function *);
void delinstr(Block *, int idx);
void delphi(Block *, int idx);
void delnops(Block *blk);
void delpred(Block *blk, Block *p);
void fillblkids(Function *);
#define startbbvisit() (void)(++visitmark)
#define wasvisited(blk) ((blk)->visit == visitmark)
#define markvisited(blk) ((blk)->visit = visitmark)
uint numberinstrs(Function *);
bool blkreachable(Function *fn, Block *blk);

/** builder.c **/
Ref irbinop(Function *, enum op, enum irclass, Ref lhs, Ref rhs);
Ref irunop(Function *, enum op, enum irclass, Ref);
Ref addinstr(Function *, Instr);
Ref addphi(Function *, enum irclass, Ref []);
void useblk(Function *, Block *);
void putbranch(Function *, Block *);
void putcondbranch(Function *, Ref arg, Block *t, Block *f);
void putreturn(Function *, Ref r0, Ref r1);
void puttrap(Function *);

/** fold.c **/
bool foldbinop(Ref *to, enum op, enum irclass, Ref l, Ref r);
bool foldunop(Ref *to, enum op, enum irclass, Ref);

/** irdump.c **/
void irdump(Function *);

/** mem2reg.c **/
void mem2reg(Function *);

/** ssa.c **/
void copyopt(Function *);

/** cfg.c **/
void sortrpo(Function *);
void filldom(Function *);
void fillloop(Function *);

/** abi0.c **/
void abi0(Function *);
void abi0_call(Function *, Instr *, Block *blk, int *curi);

/** simpl.c **/
void simpl(Function *);

/** cselim.c **/
void cselim(Function *);

/** inliner.c **/
bool maybeinlinee(Function *);
void doinline(Function *);

/** intrin.c **/
void lowerintrin(Function *);

/** stack.c **/
void lowerstack(Function *);

/** regalloc.c **/
void regalloc(Function *);

/* vim:set ts=3 sw=3 expandtab: */
