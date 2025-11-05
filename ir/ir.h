#include "../common.h"

enum irclass {
   KXXX,
   KI4, KI8, KPTR,
   KF4, KF8,
};

#define kisint(k) in_range((k), KI4, KPTR)
#define kisflt(k) in_range((k), KF4, KF8)

union irtype {
   struct { ushort _     : 1, cls : 15; };
   struct { ushort isagg : 1, dat : 15; };
   ushort bits;
};

struct irdat {
   uchar align : 6, globl : 1;
   uchar section;
   uint siz;
   uint off;
   const char *name;
};

struct xcon {
   bool issym, isdat, deref;
   uchar cls;
   union {
      const char *sym;
      int dat;
      vlong i;
      double f;
   };
};

struct abiarg {
   union irtype ty;
   union {
      short reg; /* >= 0 */
      short stk; /* < 0 */
   };
};

struct call {
   union irtype ret;
   ushort narg;
   short vararg; /* first variadic arg or -1 */
   struct abiarg *abiarg;
   struct abiarg abiret[2];
};

enum refkind {
   RXXX,  /* used for empty ref (zeros), undef, and the special args of call,phi,etc */
   RTMP,  /* reference to another instruction's result */
   RREG,  /* machine register */
   RICON, /* small integer constants */
   RXCON, /* other constants (incl. external symbols) */
   RADDR, /* target-specific addressing mode */
   RTYPE, /* irtype */
};

union ref {
   struct { unsigned t : 3; signed i : 29; };
   uint bits;
};

struct addr {
   union ref base, index;
   int shift, disp;
};

#define insrescls(ins) (oiscmp((ins).op) ? KI4 : (ins).cls)
#define NOREF ((union ref) {0})
#define UNDREF ((union ref) {{ 0, -1 }})
#define ZEROREF ((union ref) {{ RICON, 0 }})
#define mkref(t, x) ((union ref) {{ (t), (x) }})
#define mktyperef(t) ((union ref) {{ RTYPE, (t).bits }})
#define ref2type(r) ((union irtype) {.bits = (r).i})
#define rswap(a,b) do { union ref _t = (a); (a) = (b); (b) = _t; } while (0)

enum op {
   Oxxx,
#define _(o,...) O##o,
#include "op.def"
#undef _
};

#define oiscmp(o) in_range(o, Oequ, Ougte)
#define oisarith(o) in_range(o, Oneg, Ougte)
#define oisalloca(o) in_range(o, Oalloca1, Oalloca16)
#define oisstore(o) in_range(o, Ostore1, Ostore8)
#define oisload(o) in_range(o, Oloads1, Oloadf8)
extern const char *opnames[];
extern const uchar opnarg[];

enum intrin {
   INxxx,
#define _(b,...) IN##b,
#include "intrin.def"
#undef _
};

struct instr {
   uchar op,
         cls; /* operation data class; also result class except for cmp ops (always i4) */
   uchar skip : 1, /* ignore during codegen: forms part of one machine instruction */
         keep : 1; /* for codegen, keep instr even if result seems unused */
   uchar inplace : 1; /* set (by isel) for instructions which modify its first arg in place */
   uchar reg; /* 0 -> no reg; else reg + 1 */
   union ref l, r; /* args */
};

enum jumpkind { JXXX, Jb, Jret, };

struct block {
   int id;
   int npred;
   int visit;
   int inumstart;
   union {
      struct block *_pred0;
      struct block **_pred;
   };
   struct block *lprev, *lnext;
   struct block *s1, *s2;
   vec_of(ushort) phi;
   vec_of(ushort) ins;
   struct { uchar t; union ref arg[2]; } jmp;
};

#define blkpred(blk, i) 0[(blk)->npred < 2 ? &(blk)->_pred0 : &(blk)->_pred[i]]

enum { USERJUMP = 0xFFFF };
struct use { struct block *blk; ushort u; };

enum { MAXREGS = 64 };

struct function {
   struct arena **arena;
   const char *name;
   struct block *entry, *curblk;
   struct use *use;
   short *nuse;
   union type fnty, retty;
   struct abiarg *abiarg, abiret[2];
   uint nblk;
   int stksiz;
   ushort nabiarg, nabiret;
   bool globl;
   bool isleaf;
   regset regusage;
};

enum objkind { OBJELF };
enum mcisa { ISamd64 };

struct mctarg {
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
   enum mcisa isa;
   /* abiret: lower return type:
    *  scalar/small struct -> returns number of regs (1..2),
    *                         r & cls filled with reg and irclass of each scalar return
    *  big struct -> returns 0, is passed via hidden pointer argument,
    *                r[0] contains register for returning said pointer or -1,
    *                r[1] contains register for hidden pointer argument,
    *                     *ni is set to 1 if said register is the first ABI integer argument
    */
   int (*abiret)(short r[2], uchar cls[2], int *ni, union irtype);
   /* abiarg: lower argument type:
    *  scalar/small struct -> returns number of regs (1..2),
    *                         r & cls filled with reg and irclass of each scalar arg
    *                         if reg == -1 -> stack
    *  big struct -> returns 0,
    *                if passed in stack   cls[0] == 0, r[0] == negative SP offset
    *                if passed by pointer cls[0] == KPTR, r[0] contains integer register
    *                                                     or negative SP offset if stack
    */
   int (*abiarg)(short r[2], uchar cls[2], int *ni, int *nf, int *ns, union irtype);

   void (*isel)(struct function *);
   void (*emit)(struct function *);
};

enum { MAXINSTR = 1<<14 };

/** ir.c **/
extern uchar type2cls[];
extern uchar cls2siz[];
extern const uchar siz2intcls[];
extern struct instr instrtab[];
extern struct use *instruse[];
extern short instrnuse[];
extern struct xcon conht[];
extern struct calltab {vec_of(struct call);} calltab;
extern struct phitab {vec_of(union ref *);} phitab;
extern struct dattab {vec_of(struct irdat);} dattab;
extern struct addr addrht[];
extern int visitmark;
#define mkinstr(O, C, ...) ((struct instr) { .op = (O), .cls = (C), .reg=0, __VA_ARGS__ })
#define mkarginstr(ty, x) mkinstr(Oarg, 0, mktyperef(ty), (x))
void irinit(struct function *);
void irfini(struct function *);
#define cls2type(k) ((union irtype){.cls=(k)})
union irtype mkirtype(union type);
union ref mkintcon(enum irclass, vlong);
union ref mkfltcon(enum irclass, double);
#define iscon(r) in_range((r).t, RICON, RXCON)
#define concls(r) ((r).t == RICON ? KI4 : conht[(r).i].cls)
#define isintcon(r) (iscon(r) && kisint(concls(r)))
#define isfltcon(r) ((r).t == RXCON && kisflt(conht[(r).i].cls))
#define isnumcon(r) ((r).t == RICON || ((r).t == RXCON && conht[(r).i].cls))
#define isaddrcon(r) ((r).t == RXCON && !conht[(r).i].cls && !conht[(r).i].deref)
#define intconval(r) ((r).t == RICON ? (r).i : conht[(r).i].i)
#define fltconval(r) ((r).t == RICON ? (r).i : conht[(r).i].f)
union ref mksymref(const char *);
union ref mkdatref(const char *name, uint siz, uint align, const void *, uint n, bool deref);
const char *xcon2sym(int ref);
struct instr mkalloca(uint siz, uint align);
union ref mkcallarg(union irtype ret, uint narg, int vararg);
#define mkintrin(B, C, N) mkinstr(Ointrin, C, {.t=RICON,B}, mkcallarg((union irtype){{0}},N,-1))
union ref mkaddr(struct addr);
void addpred(struct block *blk, struct block *p);

struct block *newblk(struct function *);
void freeblk(struct function *, struct block *);
struct block *insertblk(struct function *, struct block *pred, struct block *subst);
void adduse(struct block *ublk, int ui, union ref r);
union ref insertinstr(struct block *, int idx, struct instr);
union ref insertphi(struct block *, enum irclass cls);
void replcuses(union ref from, union ref to);
bool deluse(struct block *ublk, int ui, union ref r);
void deluses(int ins);
void delinstr(struct block *, int idx);
void delphi(struct block *, int idx);
void delnops(struct block *blk);
void fillblkids(struct function *);
#define startbbvisit() (void)(++visitmark)
#define wasvisited(blk) ((blk)->visit == visitmark)
#define markvisited(blk) ((blk)->visit = visitmark)
void numberinstrs(struct function *);

/** builder.c **/
union ref irbinop(struct function *, enum op, enum irclass, union ref lhs, union ref rhs);
union ref irunop(struct function *, enum op, enum irclass, union ref);
union ref addinstr(struct function *, struct instr);
union ref addphi(struct function *, enum irclass, union ref []);
void useblk(struct function *, struct block *);
void putbranch(struct function *, struct block *);
void putcondbranch(struct function *, union ref arg, struct block *t, struct block *f);
void putreturn(struct function *, union ref r0, union ref r1);

/** fold.c **/
bool foldbinop(union ref *to, enum op, enum irclass, union ref l, union ref r);
bool foldunop(union ref *to, enum op, enum irclass, union ref);

/** irdump.c **/
void irdump(struct function *);

/** ssa.c **/
void filluses(struct function *);
void copyopt(struct function *);

/** cfg.c **/
void sortrpo(struct function *fn);

/** abi0.c **/
void abi0(struct function *);
void abi0_call(struct function *, struct instr *, struct block *blk, int *curi);

/** optmem.c **/
void mem2reg(struct function *);

/** intrin.c **/
void lowerintrin(struct function *);

/** regalloc.c **/
void regalloc(struct function *);

/* vim:set ts=3 sw=3 expandtab: */
