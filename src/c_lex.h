#pragma once

#include "antcc.h"
#include "c_type.h"

static inline bool
joinspan(Span0 *dst, Span0 snd) {
   if (dst->file != snd.file) return 0;
   if (dst->off > snd.off) return 0;
   dst->len = snd.off + snd.len - dst->off;
   return 1;
}

enum toktag { /* single-character tokens' tag value is the character itself */
   TKEOF = 0xFF,
   TKXXX = 0,
   TKNUMLIT,
   TKCHRLIT,
   TKSTRLIT,
   TKPPHDRH, /* <hdr> (for #include) */
   TKPPHDRQ, /* "hdr" (for #include) */
   TKPPMACARG, /* macro param, in repl list */
   TKPPMACSTR, /* stringify macro param, in repl list */
   TKPPPRAGMA, /* #pragma ... / _Pragma("...") : contains string as in TKSTRLIT */
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
#define _(kw, stdc, ...) TKW##kw,
#include "c_keywords.def"
#undef _
   NTOKTAG,
};
static_assert(NTOKTAG < 256);

typedef struct Token {
   uchar t; /* toktag */
   uchar litlit : 1,
        blue : 1, /* preprocessor token painted blue */
        extwarn : 1, /* warn this keyword token is an extension */
        wide : 2, /* for CHRLIT & STRLIT; 1 -> 16bit, 2 -> 32bit */
        wideuni : 1, /* ditto, 0 -> 'L', 1 -> 'u'/'U' (C11) */
        space : 1, /* preceded by whitespace? */
        pragmakind : 1; /* 0-> #pragma, 1-> _Pragma */
   union {
      uint len;
      ushort argidx;
   };
   Span span;
   union {
      internstr name;
      const char *s;
      const ushort *ws16;
      const uint   *ws32;
   };
   /* for (multi-)character tokens s & len are unused
    * for idents/keywords, s is interned cstring, len = strlen(s)
    * for strlit and chrlit:
    *  when litlit : s points to start of string within file buffer (after the ")
    *                len == span.sl.len - 2 (string data appears literally in source code)
    *  otherwise s is heap allocated buffer of len bytes
    *  when wide, litlit = 0 and use ws16/ws32
    * for numlit:
    *  when litlit : s points to start of token within file buffer (normal case)
    *                len == span.sl.len (number literal appears literally in source code)
    *  otherwise s is heap allocated buffer of len bytes
    * for macro arg/stringify:
    *  s is like keyword/ident
    *  argidx is index in macro param list,
    *  macidx is macro id of which it is a parameter
    */
} Token;

typedef struct PragmaState {
   bool weak;
   struct { schar stk[21], top; } pack, visibility;
   internstr weakalias;
} PragmaState;

extern int nerror, nwarn;
typedef struct Lexer {
   struct Lexer *save;
   short fileid;
   const uchar *dat;
   PragmaState *pragma;
   uint ndat;
   uint idx;
   uint curlineoff;
   uint curline;
   struct MacroStack *macstk;
   Token peektok;
   bool eof, err, virtlines;
   Arena **tmparena;
   bool firstdirective;
   short nppcnd0;
   short inclnerror, inclnwarn;
   short src_incdiridx;
   internstr inclguard;
} Lexer;

enum initlexer {
   LXOK,
   LXFILESEEN,
   LXFILESKIP,
   LXERR,
};

int lex(Lexer *, Token *);
int lexpeek(Lexer *, Token *);
enum typetag parsenumlit(u64int *, double *, const Token *, bool ispp);
enum initlexer initlexer(Lexer *, const char **err, const char *file);
void lexerdump(Lexer *, WriteBuf *out);
void lexerfreetemps(Lexer *);

/* vim:set ts=3 sw=3 expandtab: */
