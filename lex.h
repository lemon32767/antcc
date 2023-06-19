#include "common.h"

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
struct lexer {
   struct lexer *save;
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
   struct arena **tmparena;
};

const char *intern(const char *);
int lex(struct lexer *, struct token *);
int lexpeek(struct lexer *, struct token *);
enum typetag parsenumlit(uvlong *, double *, const struct token *, bool ispp);
void initlexer(struct lexer *, const char *file, struct arena **);

/* vim:set ts=3 sw=3 expandtab: */
