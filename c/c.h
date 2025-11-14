#include "../common.h"

/*************/
/* EXPR TREE */
/*************/

enum exprkind {
   EXXX, ENUMLIT, ESTRLIT, ESYM, EVAARG, EINIT, EGETF, ECALL, ECOND,
   /* unary */
   EPLUS, ENEG, ECOMPL, ELOGNOT, EDEREF, EADDROF, ECAST,
   EPREINC, EPOSTINC, EPREDEC, EPOSTDEC,
   /* binary */
   EADD, ESUB, EMUL, EDIV, EREM, EBAND, EBIOR, EXOR, ESHL, ESHR,
   ELOGAND, ELOGIOR,
   EEQU, ENEQ, ELTH, EGTH, ELTE, EGTE,
   ESET, ESETADD, ESETSUB, ESETMUL, ESETDIV, ESETREM, ESETAND, ESETIOR, ESETXOR, ESETSHL, ESETSHR,
   ESEQ,
};
#define isunop(t) in_range(t, EPLUS, EPOSTDEC)
#define isbinop(t) in_range(t, EADD, ESEQ)
#define isassign(t) in_range(t, ESET, ESETSHR)
#define assigntobinop(t) ((t) - ESETADD + EADD)

struct expr {
   uchar t;
   uchar qual;
   ushort narg; /* ECALL */
   union type ty;
   struct span span;
   union {
     struct {
        struct expr *sub;
        struct exgetfld {
           ushort off;
           uchar bitsiz, bitoff;
        } fld; /* EGETF */
     };
     uvlong u; vlong i; double f; /* ENUMLIT */
     struct {
        union {
           uchar *p;
           ushort *w16;
           uint *w32;
        };
        uint n;
     } s; /* ESTRLIT */
     struct decl *sym; /* ESYM */
     struct init *init; /* EINIT */
   };
};

struct init {
   struct bitset zero[1]; /* bytes to zero out up to 64 */
   struct initval {
      struct initval *next;
      uint off;
      uchar bitoff, bitsiz;
      struct expr ex;
   } *vals, **tail;
};

/** C compiler state **/
struct comp {
   struct lexer *lx;
   struct env *env;
   struct arena *fnarena, *exarena;
   struct span fnblkspan;
   uint loopdepth, switchdepth;
   struct block *breakto, *loopcont;
   struct switchstmt *switchstmt;
   struct label *labels;
};

enum storageclass {
   SCNONE,
   SCTYPEDEF = 1<<0,
   SCEXTERN = 1<<1,
   SCSTATIC = 1<<2,
   SCTHREADLOCAL = 1<<3,
   SCAUTO = 1<<4,
   SCREGISTER = 1<<5,
};

struct decl {
   union type ty;
   uchar scls;
   uchar qual : 2,
         isenum : 1,
         isdef : 1,
         isbuiltin : 1;
   struct span span;
   const char *name;
   union {
      struct { ushort align; int id; };
      vlong value;
      const struct builtin *builtin;
   };
};

extern union type cvalistty;
struct function;
struct decl *envadddecl(struct env *env, const struct decl *d);
bool assigncheck(union type t, const struct expr *src);
union ref expraddr(struct function *, const struct expr *);
union ref compileexpr(struct function *, const struct expr *, bool discard);

/** builtin.c **/
struct builtin {
   bool (*sema)(struct comp *, struct expr *);
   union ref (*comp)(struct function *, struct expr *, bool discard);
};
void putbuiltins(struct env *);
union ref builtin_va_arg_comp(struct function *, const struct expr *, bool discard);

/** eval.c **/
enum evalmode {
   EVNONE,
   EVINTCONST,
   EVARITH,
   EVSTATICINI,
   EVFOLD,
};

bool eval(struct expr *, enum evalmode);

/* vim:set ts=3 sw=3 expandtab: */
