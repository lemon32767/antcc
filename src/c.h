#pragma once

#include "antcc.h"
#include "c_type.h"

enum exprkind {
   EXXX, ENUMLIT, ESTRLIT, ESYM, ESSYMREF, EVAARG, EINIT, EGETF, ECALL, ECOND,
   /* unary */
   EPLUS, ENEG, ECOMPL, ELOGNOT, EDEREF, EADDROF, ECAST,
   EPREINC, EPOSTINC, EPREDEC, EPOSTDEC,
   /* binary */
   EADD, ESUB, EMUL, EDIV, EREM, EBAND, EBIOR, EXOR, ESHL, ESHR,
   ELOGAND, ELOGIOR,
   EEQU, ENEQ, ELTH, EGTH, ELTE, EGTE,
   ESET, ESETADD, ESETSUB, ESETMUL, ESETDIV, ESETREM, ESETAND, ESETIOR, ESETXOR, ESETSHL, ESETSHR,
   ESEQ,
   EIRVALUE,
};
#define isunop(t) in_range(t, EPLUS, EPOSTDEC)
#define isbinop(t) in_range(t, EADD, ESEQ)
#define isassign(t) in_range(t, ESET, ESETSHR)
#define assigntobinop(t) ((t) - ESETADD + EADD)

typedef struct Expr Expr;
typedef struct ExprGetFld ExprGetFld;
typedef struct Init Init;
typedef struct InitElem InitElem;
struct Expr {
   uchar t;
   uchar qual;
   ushort narg; /* ECALL */
   Type ty;
   Span span;
   union {
      struct {
         Expr *sub; /* child(ren) */
         struct ExprGetFld {
            uint off;
            uchar bitsiz, bitoff;
         } fld; /* EGETF */
      };
      u64int u; s64int i; double f; /* ENUMLIT */
      struct {
         union {
            uchar *p;
            ushort *w16;
            uint *w32;
         };
         uint n;
      } s; /* ESTRLIT */
      int decl; /* ESYM, index into declsbuf */
      internstr implicitsym; /* ESYM (undeclared) */
      struct {
         internstr sym;
         int off;
         bool func : 1, local : 1;
      } ssym; /* ESSYMREF (static symbol addr + off) */
      Init *init; /* EINIT */
      struct { uint bits; } irref; /* EIRVALUE */
   };
};

struct Init {
   BitSet zero[BSSIZE(64)]; /* bytes to zero out up to 64 */
   struct InitElem {
      InitElem *next;
      uint off;
      uchar bitoff, bitsiz;
      Expr ex;
   } *vals, **tail;
};

typedef struct Env Env;
typedef struct Builtin Builtin;

/** C compiler state **/
typedef struct {
   struct Lexer *lx;
   Env *env;
   Arena *fnarena, *exarena;
   Span fnblkspan;
   uint loopdepth, switchdepth;
   struct Block *breakto, *loopcont;
   struct SwitchStmt *switchstmt;
   struct Label *labels;
   struct Function *fn;
} CComp;

enum storageclass {
   SCNONE,
   SCTYPEDEF = 1<<0,
   SCEXTERN = 1<<1,
   SCSTATIC = 1<<2,
   SCTHREADLOCAL = 1<<3,
   SCAUTO = 1<<4,
   SCREGISTER = 1<<5,
};

#define LIST_ATTRS(_)          \
   /*name,               f, v, t */ \
   _(noreturn,           1, 0, 0)   \
   _(returns_twice,      1, 0, 0)   \
   _(noinline,           1, 0, 0)   \
   _(always_inline,      1, 0, 0)   \
   _(flatten,            1, 0, 0)   \
   _(pure,               1, 0, 0)   \
   _(const,              1, 0, 0)   \
   _(nothrow,            1, 0, 0)   \
   _(constructor,        1, 0, 0)   \
   _(destructor,         1, 0, 0)   \
   _(used,               1, 1, 1)   \
   _(unused,             1, 1, 1)   \
   _(deprecated,         1, 1, 1)   \
   _(weak,               1, 1, 0)   \
   _(weakref,            1, 1, 0)   \
   _(malloc,             1, 0, 0)   \
   _(alias,              1, 0, 0)   \
   _(warn_unused_result, 1, 0, 0)   \
   _(gnu_inline,         1, 0, 0)   \
   _(externally_visible, 1, 1, 0)   \
   _(common,             0, 1, 0)   \
   _(nocommon,           0, 1, 0)   \
   _(packed,             0, 1, 1)   \
   _(transparent_union,  0, 0, 1)   \
   _(may_alias,          0, 0, 1)   \
   _(aligned,            0, 1, 1)   \
   _(format,             1, 0, 0)   \
   _(nonnull,            1, 1, 0)   \

enum attr {
   ATTRxxx,
#define DEF_ATTR(x,...) ATTR##x,
   LIST_ATTRS(DEF_ATTR)
#undef DEF_ATTR
};
typedef struct Attrs {
   int align;
   uint bset;
} Attrs;
#define setattr(attrs,A) ((attrs)->bset |= 1<<(A))
#define hasattr(attrs,A) ((attrs)->bset & 1<<(A))

typedef struct Decl {
   Type ty;
   uchar scls;
   uchar qual  : 2,
         inlin : 1,
         isdef : 1,
         isenumconst : 1,
         isbuiltin   : 1;
   Span span;
   internstr name;
   Attrs attr;
   union {
      internstr sym; /* static/extern scls */
      int id; /* local var */
      s64int value; /* enum constant */
      const Builtin *builtin; /* .isbuiltin */
   };
} Decl;

/** c.c **/
extern struct declsbuf {vec_of(Decl);} declsbuf;
extern Type cvalistty;
int envadddecl(Env *env, const Decl *d);
bool assigncheck(Type t, const Expr *src);
struct Function;
union Ref expraddr(struct Function *, const Expr *);
union Ref scalarcvt(struct Function *, Type to, Type from, union Ref);
union Ref compileexpr(struct Function *, const Expr *, bool discard);
void dumpexpr(const Expr *, bool printtypes);

struct Builtin {
   bool (*sema)(CComp *, Expr *);
   union Ref (*comp)(struct Function *, Expr *, bool discard);
};

/** builtin.c **/
void putbuiltins(Env *);
union Ref builtin_va_arg_comp(struct Function *, const Expr *, bool discard);

enum evalmode {
   EVNONE,
   EVINTCONST,
   EVARITH,
   EVSTATICINI,
   EVFOLD,
};

/** eval.c **/
bool eval(Expr *, enum evalmode);

/* vim:set ts=3 sw=3 expandtab: */
