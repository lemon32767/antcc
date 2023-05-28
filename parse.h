#include "common.h"

/*************/
/** PARSING **/
/************/

static inline bool
joinspan(struct span0 *dst, struct span0 snd)
{
   if (dst->file != snd.file) return 0;
   assert(dst->off <= snd.off);
   dst->len = snd.off + snd.len - dst->off;
   return 1;
}

enum toktag { /* single-character tokens' tag value is the character itself */
   TKEOF = -1,
   TKXXX,
   TKNUMLIT,
   TKCHRLIT,
   TKSTRLIT,
   TKEQU = '@', /* == */
   TKNEQ, /* != */
   TKLTE, /* <= */
   TKGTE, /* >= */
   TKSHR, /* >> */
   TKSHL, /* << */
   TKINC, /* ++ */
   TKDEC, /* -- */
   TKDOTS, /* ... */
   TKARROW, /* -> */
   TKPPCAT, /* ## */
   TKLOGAND, /* && */
   TKLOGIOR, /* || */
   TKSETADD, /* += */
   TKSETSUB, /* -= */
   TKSETMUL, /* *= */
   TKSETDIV, /* /= */
   TKSETREM, /* %= */
   TKSETIOR, /* |= */
   TKSETXOR, /* ^= */
   TKSETAND, /* &= */
   TKSETSHL, /* <<= */
   TKSETSHR, /* >>= */
   TKIDENT = 0x80,
#define _(kw, stdc) TKW##kw,
#include "keywords.def"
#undef _
};

struct token {
   short t; /* toktag */
   bool litlit;
   uint len;
   struct span span;
   const char *s; 
   /* for (multi-)character tokens s & len are unused
    * for keywords, s is constant cstring, len = strlen(s)
    * for idents, s is interned cstring, len = strlen(s)
    * for strlit and chrlit:
    *  when litlit : s points to start of string within file buffer (after the ")
    *                len == span.sl.len - 2 (string data appears literally in source code)
    *  otherwise s is heap allocated buffer of len bytes
    * for numlit:
    *  when litlit : s points to start of token within file buffer (normal case)
    *                len == span.sl.len (number literal appears literally in source code)
    *  otherwise s is heap allocated buffer of len bytes
    */
};

struct macro {
   const char *name; /* interned */
   const char **param;
   struct span0 span;
   uchar nparam;
   bool fnlike, variadic;
   struct rlist {
     struct token *tk;
     int n;
   } rlist;
};

struct macrostack {
   struct macrostack *link;
   struct rlist *args;
   struct span0 exspan;
   int mac;
   int idx;
};

extern int nerror;
struct parser {
   struct parser *save;
   short fileid;
   const uchar *dat;
   uint ndat;
   uint idx, chridx;
   short peekchr[2];
   uint peekcidx[2];
   short npeekchr;
   struct macrostack *macstk;
   struct token peektok;
   bool eof, err;
   struct env *env;
   struct arena *fnarena, *exarena;
   struct span fnblkspan;
};

const char *intern(const char *);
int lex(struct parser *, struct token *);
int lexpeek(struct parser *, struct token *);
enum typetag parsenumlit(uvlong *, double *, const struct token *, bool ispp);
void initparser(struct parser *, const char *file);
void parse(struct parser *);

/***********/
/** CTYPE **/
/***********/

#define aisprint(c) in_range(c, ' ', '~')
#define aisdigit(c) in_range(c, '0', '9')
#define aisodigit(c) in_range(c, '0', '7')
#define aisalpha(c) in_range((c)|0x20, 'a', 'z')
static inline bool aisspace(int c) { return c == ' ' || in_range(c, '\t', '\r'); }
static inline bool aisxdigit(int c) { return aisdigit(c) || in_range(c|0x20, 'a', 'f'); }

/************/
/* ANALYSIS */
/************/

enum exprkind {
   EXXX, ENUMLIT, ESTRLIT, ESYM, EINIT, EGETF, ECALL, ECOND,
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
     struct expr *sub;
     uvlong u; vlong i; double f; /* ENUMLIT */
     struct bytes s; /* ESTRLIT */
     struct decl *sym; /* ESYM */
     struct initializer *ini; /* EINIT */
     struct { /* EGETF */
       const char *name;
       uint off;
       uchar bitsiz, bitoff;
     } fld;
     char dummy;
   };
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
   uchar qual;
   ushort align;
   struct span span;
   const char *name;
   int id;
};

enum evalmode {
   EVINTCONST,
   EVARITH,
   EVSTATICINI,
};

bool eval(struct expr *, enum evalmode);

/* vim:set ts=3 sw=3 expandtab: */
