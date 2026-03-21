#include "c_lex.h"
#include "version.h"
#include "u_bits.h"
#include <string.h>
#include <stdlib.h>

/* fill internal circular character buffer with input after translation phase 1 & 2
 * (trigraph substitution and backslash-newline deletion */
static void
fillchrbuf(Lexer *lx)
{
   const uchar *p = lx->dat + lx->idx;
   int i = lx->chrbuf0, idx = lx->idx;
   int rem = countof(lx->chrbuf) - i;
   assert(rem >= 0);
   if (rem > 0) {
      memmove(lx->chrbuf, lx->chrbuf+i, rem * sizeof *lx->chrbuf);
      memmove(lx->chridxbuf, lx->chridxbuf+i, rem * sizeof *lx->chridxbuf);
   }
   lx->chrbuf0 = 0;
   i = rem;

   for (; i < countof(lx->chrbuf); ++i) {
      uchar c;
      /* skip backslash-newline* */
      for (;;) {
         if (p[0] == '\\') {
            if (p[1] == '\n') {
               idx += 2;
               p += 2;
            } else if (p[1] == '\r' && p[2] == '\n') {
               idx += 3;
               p += 3;
            } else break;
         } else if (ccopt.trigraph && !memcmp(p, "\?\?/\n", 4)) {
            idx += 4;
            p += 4;
         } else if (ccopt.trigraph && !memcmp(p, "\?\?/\r\n", 5)) {
            idx += 5;
            p += 5;
         } else break;
         addfileline(lx->fileid, idx);
      }

      if (idx >= lx->ndat) {
         c = 0;
      } else if (ccopt.trigraph && ((p[0] == '?') & (p[1] == '?'))) {
         switch (p[2]) {
         case '=':  c = '#'; break;
         case '(':  c = '['; break;
         case ')':  c = ']'; break;
         case '!':  c = '|'; break;
         case '<':  c = '{'; break;
         case '>':  c = '}'; break;
         case '-':  c = '~'; break;
         case '/':  c = '\\'; break;
         case '\'': c = '^'; break;
         default: goto NoTrigraph;
         }
         p += 3;
         idx += 3;
      } else {
      NoTrigraph:
         ++idx;
         if ((c = *p++) == '\n')
            addfileline(lx->fileid, idx);
      }
      lx->chrbuf[i] = c;
      lx->chridxbuf[i] = idx;
   }
   lx->idx = idx;
}

static uchar
next(Lexer *lx)
{
   if (lx->chrbuf0 >= countof(lx->chrbuf))
      fillchrbuf(lx);
   lx->chridx = lx->chridxbuf[lx->chrbuf0];
   uchar c = lx->chrbuf[lx->chrbuf0];
   lx->eof = lx->chridx >= lx->ndat;
   ++lx->chrbuf0;
   return c;
}

static uchar
peek(Lexer *lx, int off)
{
   assert(off < countof(lx->chrbuf));
   if (lx->chrbuf0 + off >= countof(lx->chrbuf))
      fillchrbuf(lx);
   return lx->chrbuf[lx->chrbuf0 + off];
}

static bool
match(Lexer *lx, uchar c)
{
   if (!lx->eof && peek(lx, 0) == c) {
      next(lx);
      return 1;
   }
   return 0;
}

static bool
aissep(int c) {
   static const bool tab[] = {
      ['('] = 1, [')'] = 1, ['['] = 1, [']'] = 1,
      ['{'] = 1, ['}'] = 1, ['.'] = 1, [','] = 1,
      [';'] = 1, ['?'] = 1, ['+'] = 1, ['-'] = 1,
      ['*'] = 1, ['/'] = 1, ['&'] = 1, ['|'] = 1,
      ['^'] = 1, ['~'] = 1, ['='] = 1, ['\''] = 1,
      ['"'] = 1, ['<'] = 1, ['>'] = 1, [':'] = 1,
      ['@'] = 1, ['#'] = 1, ['%'] = 1, ['\\'] = 1,
      ['`'] = 1, ['!'] = 1,
   };
   if (!aisprint(c) || aisspace(c))
      return 1;
   return (uint)c < sizeof(tab) && tab[c];
}

enum typetag
parsenumlit(uvlong *outi, double *outf, const Token *tk, bool ispp)
{
   if (tk->t == TKCHRLIT) {
      uvlong n = 0;
      if (!tk->wide) {
         for (int i = 0; i < tk->len; ++i)
            n = n << 8 | (uchar)tk->s[i];
      } else if (tk->wide == 1) {
         n = tk->ws16[0];
      } else {
         assert(tk->wide == 2);
         n = tk->ws32[0];
      }
      if (outi) *outi = n;
      return TYINT;
   } else if (memchr(tk->s, '.', tk->len)) {
      extern double strtod(const char *, char **);
      double f;
      char buf[80], *suffix;
   Float: /* float literal */
      assert(tk->len < sizeof buf - 1 && "numlit too big");
      memcpy(buf, tk->s, tk->len);
      buf[tk->len] = 0;
      f = strtod(buf, &suffix);
      if (suffix == buf)
         return 0;
      if (!*suffix) {
         if (outf) *outf = f;
         return TYDOUBLE;
      } else if ((suffix[0]|0x20) == 'f' && !suffix[1]) {
         if (outf) *outf = f;
         return TYFLOAT;
      } else if ((suffix[0]|0x20) == 'l' && !suffix[1]) {
         if (outf) *outf = f;
         return TYLDOUBLE;
      }
      return 0;
   } else { /* int literal */
      static uvlong max4typ[TYUVLONG-TYINT+1];
      uvlong n = 0;
      int base = 10, nsx;
      bool dec, u = 0, longlongok = ccopt.cstd >= STDC99 || !ccopt.pedant;
      enum typetag ty = 0;
      const char *sx; /*suffix*/
      char c;

      if (!max4typ[0])
         for (ty = TYINT; ty <= TYUVLONG; ++ty)
            max4typ[ty-TYINT] = ((1ull << (8*targ_primsizes[ty]-1))-1) << isunsignedt(ty) | 1;

      sx = tk->s;
      if (tk->len > 2 && sx[0] == '0') {
         if ((sx[1]|32) == 'x') sx += 2, base = 16; /* 0x.. */
         else if ((sx[1]|32) == 'b') sx += 2, base = 2; /* 0b.. */
         else base = 8; /* 0.. */
      }
      for (; sx < tk->s + tk->len; ++sx) {
         if (base < 16) {
            if (!in_range(c = *sx, '0', '0'+base-1)) break;
            n = n*base + c - '0';
         } else {
            if (in_range(c = *sx, '0', '9')) n = n*base + c - '0';
            else if (in_range(c|32, 'a', 'f')) n = n*base + 0xa + (c|32) - 'a';
            else break;
         }
      }
      dec = base == 10;
      nsx = tk->len - (sx - tk->s);

      if (nsx == 0) /* '' */ {}
      else if ((sx[0]|32) == 'u') {
         u = 1;
         if (nsx == 1) /* 'u' */ {}
         else if ((sx[1]|32) == 'l') {
            if (nsx == 2) /* 'ul' */ goto L;
            if (sx[1] == sx[2] && nsx == 3) /* 'ull' */ goto LL;
            return 0;
         } else return 0;
      } else if ((sx[0]|32) == 'l') {
         if (nsx == 1) /* 'l' */ goto L;
         if ((sx[1]|32) == 'u' && nsx == 2) /* 'lu' */ { u=1; goto L; }
         if (sx[1] == sx[0]) {
            if (nsx == 2) /* 'll' */ goto LL;
            if ((sx[2]|32) == 'u' && nsx == 3) /* 'llu' */ { u=1; goto LL; }
         }
         return 0;
      } else if ((sx[0]|32) == 'e' || (sx[0]|32) == 'p')
         goto Float;
      else return 0;

#define I(T) if (n <= max4typ[T - TYINT]) { ty = T; goto Ok; }
      I(TYINT)
      if (u || !dec) I(TYUINT)
   L:
      I(TYLONG)
      if (u || !dec || !longlongok) I(TYULONG)
      if (longlongok) {
      LL:
         I(TYVLONG)
         if (u || !dec) I(TYUVLONG)
      }
      if (ispp) { ty = TYUVLONG; goto Ok; }
#undef I
      /* too big */
      if (outi) *outi = n;
      return 0;
   Ok:
      if (u && issignedt(ty)) ++ty; /* make unsigned */
      if (outi) *outi = n;
      if (ispp) {
         if (u) return TYUVLONG;
         else if (n <= max4typ[TYVLONG-TYINT]) return TYVLONG;
      }
      if (ty >= TYVLONG && !longlongok)
         warn(&tk->span, "'long long' in %M is an extension");
      return ty;
   }
}

static void
readstrchrlit(Lexer *lx, Token *tk, char delim, int wide)
{
   int c, i;
   uchar tmp[200];
   vec_of(uchar) b = VINIT(tmp, sizeof tmp);
   Span span = {0};
   uint n, beginoff, idx;
   beginoff = idx = lx->chridx;

   while ((c = next(lx)) != delim) {
      static uint wmax[] = {0xFF, 0xFFFF, 0xFFFFFFFFu};
      if (c == '\n' || c == TKEOF) {
      Noterm:
         span.sl = (Span0) { idx, lx->chridx - idx, lx->fileid };
         error(&span, "missing terminating %c character", delim);
         break;
      } else if (c == '\\') {
         span.sl = (Span0) { idx, lx->chridx - idx, lx->fileid };
         switch (c = next(lx)) {
         case '\n': case TKEOF:
            goto Noterm;
         case '\'': c = '\''; break;
         case '\\': c = '\\'; break;
         case '"':  c = '"';  break;
         case '?':  c = '?';  break;
         case 'a':  c = '\a'; break;
         case 'b':  c = '\b'; break;
         case 'f':  c = '\f'; break;
         case 'n':  c = '\n'; break;
         case 'r':  c = '\r'; break;
         case 't':  c = '\t'; break;
         case 'v':  c = '\v'; break;
         case 'x': case 'X': /* hex */
            n = 0;
            if (!aisxdigit(peek(lx, 0))) goto Badescseq;
            do {
               c = next(lx);
               if (c-'0' < 10) n = n<<4 | (c-'0');
               else            n = n<<4 | (10 + (c|0x20)-'a');
            } while (aisxdigit(peek(lx, 0)));
            if (n > wmax[wide]) {
               span.sl.len = lx->chridx - span.sl.off;
               error(&span, "hex escape sequence out of range");
            }
            c = n;
            break;
         default:
            if (aisodigit(c)) { /* octal */
               n = c-'0';
               for (i = 2; i--;) {
                  if (!aisodigit(peek(lx, 0))) break;
                  n = n<<3 | ((c = next(lx))-'0');
               }
               if (n > wmax[wide]) {
                  span.sl.len = lx->chridx - span.sl.off;
                  error(&span, "octal escape sequence out of range");
               }
               c = n;
               break;
            }
         Badescseq:
            span.sl.len = lx->chridx - span.sl.off;
            error(&span, "invalid escape sequence");
         }
      }
      if (!wide || c <= 0xFF) {
         vpush(&b, c);
      } else {
         /* XXX this doesn't work for non-utf sequences, UTF-16 surrogates, etc
          * the source utf8 -> utf16/32 conversion should be done on the fly, then
          * these can also be appended directly, rather than doing the conversion at the end */
         char p[4];
         int n = utf8enc(p, c);
         vpushn(&b, p, n);
      }
      idx = lx->chridx;;
   }
   if (delim == '"') {
      tk->t = TKSTRLIT;
      tk->len = b.n;
      if ((tk->wide = wide)) {
         tk->litlit = 0;
         if (wide == 1)
            tk->ws16 = utf8to16(&tk->len, lx->tmparena, b.p, b.n);
         else
            tk->ws32 = utf8to32(&tk->len, lx->tmparena, b.p, b.n);
      } else if (lx->chridx - beginoff == tk->len + 1) {
         tk->litlit = 1;
         tk->s = (char *)&lx->dat[beginoff];
      } else {
         tk->litlit = 0;
         vpush(&b, 0);
         tk->s = alloccopy(lx->tmparena, b.p, b.n, 1);
      }
   } else {
      if (b.n == 0) {
         span.sl = (Span0) { idx, lx->chridx - idx, lx->fileid };
         error(&span, "empty character literal");
      } else if (b.n > targ_primsizes[TYINT]) {
         span.sl = (Span0) { idx, lx->chridx - idx, lx->fileid };
         error(&span, "multicharacter literal too long");
      }
      tk->t = TKCHRLIT;
      tk->len = b.n;
      if ((tk->wide = wide)) {
         tk->litlit = 0;
         if (wide == 1)
            tk->ws16 = utf8to16(&tk->len, lx->tmparena, b.p, b.n);
         else
            tk->ws32 = utf8to32(&tk->len, lx->tmparena, b.p, b.n);
      } else if (lx->chridx - beginoff == tk->len + 1) {
         tk->litlit = 1;
         tk->s = (char *)&lx->dat[beginoff];
      } else {
         tk->litlit = 0;
         tk->s = alloccopy(lx->tmparena, b.p, tk->len, 1);
      }
   }
   vfree(&b);
}

/* for #include directive, read "header" or <header> */
static void
readheadername(Lexer *lx, Token *tk, char delim)
{
   int c;
   uchar tmp[200];
   vec_of(uchar) b = VINIT(tmp, sizeof tmp);
   Span span = {0};
   uint beginoff, idx;
   beginoff = idx = lx->chridx;

   while ((c = next(lx)) != delim) {
      if (c == '\n' || lx->eof) {
         span.sl = (Span0) { idx, lx->chridx - idx, lx->fileid };
         error(&span, "missing terminating %c character", delim);
         break;
      }
      vpush(&b, c);
      idx = lx->chridx;;
   }
   tk->t = delim == '"' ? TKPPHDRQ : TKPPHDRH;
   tk->len = b.n;
   if (lx->chridx - beginoff == tk->len + 1) {
      tk->litlit = 1;
      tk->s = (char *)&lx->dat[beginoff];
   } else {
      tk->litlit = 0;
      vpush(&b, 0);
      tk->s = alloccopy(lx->tmparena, b.p, b.n, 1);
   }
   vfree(&b);
}

/* matches "<digit> | <identifier-nondigit> | '.' | ([eEpP][+-])" */
static bool
isppnum(char prev, char c)
{
   if (!aissep(c) || c == '.')
     return 1;
   if (c == '+' || c == '-')
     return (prev|0x20) == 'e' || (prev|0x20) == 'p';
   return 0;
}

enum { MAXLITLEN = 256 }; /* maximum length of num literals and identifiers */
static int
lex0(Lexer *lx, Token *tk, bool includeheader)
{
   int idx,q;
   bool space = 0;
Begin:
   idx = lx->chridx;
   if (lx->chrbuf0+4 >= countof(lx->chrbuf))
      fillchrbuf(lx);
   lx->chridx = lx->chridxbuf[lx->chrbuf0];
   uchar *p = &lx->chrbuf[lx->chrbuf0++],
         c = p[0];
   switch (c) {

#define RET(t_) do { tk->t = (t_); goto End; } while (0)
#define TK2(c2,t) if (p[1] == c2) {            \
      lx->chridx = lx->chridxbuf[lx->chrbuf0]; \
      ++lx->chrbuf0;                           \
      RET(t);                                  \
   }
#define TK3(c2,c3,t) if (p[1] == c2 && p[2] == c3) { \
      lx->chridx = lx->chridxbuf[++lx->chrbuf0];     \
      ++lx->chrbuf0;                                 \
      RET(t);                                        \
   }

   case ' ': case '\t': case '\f': case '\v': case '\r':
      space = 1;
      goto Begin;
      break;
   case '(': case ')': case ',': case ':':
   case ';': case '?': case '[': case ']':
   case '{': case '}': case '~': case '$':
   case '@': case '`': case '\\': case '\n':
      RET(c);
   case '!':
      TK2('=', TKNEQ);
      RET(c);
   case '#':
      TK2('#', TKPPCAT);
      RET(c);
   case '+':
      TK2('+', TKINC);
      TK2('=', TKSETADD);
      RET(c);
   case '-':
      TK2('-', TKDEC);
      TK2('=', TKSETSUB);
      TK2('>', TKARROW);
      RET(c);
   case '*':
      TK2('=', TKSETMUL);
      RET(c);
   case '/':
      TK2('=', TKSETDIV);
      if (match(lx, '/')) {
         /* // single line comment */
         for (;;) {
            do {
               if (lx->chrbuf[lx->chrbuf0] == '\n') {
                  lx->chridx = lx->chridxbuf[lx->chrbuf0++];
                  lx->eof = lx->chridx >= lx->ndat;
                  RET('\n');
               } else if (lx->eof) RET(TKEOF);
            } while (++lx->chrbuf0 < countof(lx->chrbuf));
            fillchrbuf(lx);
            lx->chridx = lx->chridxbuf[lx->chrbuf0];
            lx->eof = lx->chridx >= lx->ndat;
         }
      } else if (match(lx, '*')) {
         // /* multi line comment */
         if (lx->chrbuf0+1 >= countof(lx->chrbuf)) fillchrbuf(lx);
         for (;;) {
            do {
               if (lx->chrbuf[lx->chrbuf0] == '*' && lx->chrbuf[lx->chrbuf0+1] == '/') {
                  lx->chridx = lx->chridxbuf[lx->chrbuf0+1];
                  lx->chrbuf0 += 2;
                  lx->eof = lx->chridx >= lx->ndat;
                  space = 1;
                  goto Begin;
               }
            } while (++lx->chrbuf0+1 < countof(lx->chrbuf));
            fillchrbuf(lx);
            lx->chridx = lx->chridxbuf[lx->chrbuf0];
            if ((lx->eof = (lx->chridx >= lx->ndat))) {
               Span span = {{ idx, lx->chridx - idx, lx->fileid }};
               fatal(&span, "unterminated comment");
            }
         }
      }
      RET(c);
   case '%':
      TK2('=', TKSETREM);
      RET(c);
   case '^':
      TK2('=', TKSETXOR);
      RET(c);
   case '=':
      TK2('=', TKEQU);
      RET(c);
   case '<':
      if (includeheader) {
         readheadername(lx, tk, '>');
         goto End;
      }
      TK2('=', TKLTE);
      TK3('<','=', TKSETSHL)
      TK2('<', TKSHL);
      RET(c);
   case '>':
      TK2('=', TKGTE);
      TK3('>','=', TKSETSHR)
      TK2('>', TKSHR);
      RET(c);
   case '&':
      TK2('&', TKLOGAND);
      TK2('=', TKSETAND);
      RET(c);
   case '|':
      TK2('|', TKLOGIOR);
      TK2('=', TKSETIOR);
      RET(c);
   case '"':
      if (includeheader) {
         readheadername(lx, tk, '"');
      } else {
   case '\'':
         tk->wideuni = 0;
         readstrchrlit(lx, tk, c, 0);
      }
      goto End;
   case '.':
      TK3('.','.',TKDOTS)
      if (aisdigit(p[1])) goto Numlit;
      RET(c);
   case 'L':
      if (match(lx, (q = '\'')) || match(lx, (q = '"'))) {
         tk->wideuni = 0;
         readstrchrlit(lx, tk, q, /* wide */ targ_primsizes[targ_wchartype] == 2 ? 1 : 2);
         goto End;
      }
      /* fallthru */
   default:
      if (aisdigit(c)) Numlit: {
         --lx->chrbuf0;
         if (lx->chrbuf0 + MAXLITLEN >= countof(lx->chrbuf))
            fillchrbuf(lx);
         int n = 1;
         uchar *p = &lx->chrbuf[lx->chrbuf0];
         for (; isppnum(p[n-1], p[n]); ++n) {
            if (n >= MAXLITLEN) {
               lx->chridx = lx->chridxbuf[lx->chrbuf0+n-1];
            TooLong:
               fatal(&(Span) {{ idx, lx->chridx - idx, lx->fileid }},
                     "token is too long");
            }
         }
         tk->len = n;
         lx->chridx = lx->chridxbuf[(lx->chrbuf0 += n) - 1];
         if (n == lx->chridx - idx) {
            tk->litlit = 1;
            tk->s = (char *)&lx->dat[idx];
         } else {
            tk->litlit = 0;
            tk->s = alloccopy(lx->tmparena, p, n, 1);
         }
         RET(TKNUMLIT);
      } else if (c == '_' || aisalpha(c)) {
         --lx->chrbuf0;
         if (lx->chrbuf0 + MAXLITLEN >= countof(lx->chrbuf))
            fillchrbuf(lx);
         uchar *p = &lx->chrbuf[lx->chrbuf0];
         int n = 1;
         for (; !aissep(p[n]); ++n) {
            if (n >= MAXLITLEN) {
               lx->chridx = lx->chridxbuf[lx->chrbuf0+n-1];
               goto TooLong;
            }
         }
         tk->blue = 0;
         tk->len = n;
         tk->name = intern_((char *)p, n);
         lx->chridx = lx->chridxbuf[(lx->chrbuf0 += n) - 1];
         RET(TKIDENT);
      }
      /* fallthru */
   case 0: if (lx->idx >= lx->ndat) RET(TKEOF);
#undef TK2
   }
   fatal(&(Span) {{ idx, lx->chridx - idx, lx->fileid }},
         "unexpected character %'c at %d (%d)", c, idx, lx->idx);
End:
   tk->space = space;
   tk->span.sl.file = lx->fileid;
   tk->span.sl.off = idx;
   tk->span.sl.len = lx->chridx - idx;
   tk->span.ex = tk->span.sl;
   return tk->t;
#undef RET
}

/****************/
/* PREPROCESSOR */
/****************/

static bool
tokequ(const Token *a, const Token *b)
{
   if (a == b) return 1;
   if (!a || !b) return 0;
   if (a->t != b->t) return 0;
   if (a->t == TKNUMLIT || a->t == TKSTRLIT || a->t == TKCHRLIT) {
      if (a->len != b->len) return 0;
      return !memcmp(a->s, b->s, a->len);
   } else if (a->t == TKIDENT) {
      return a->name == b->name;
   } else if (a->t == TKPPMACARG || a->t == TKPPMACSTR) {
      return a->argidx == b->argidx;
   }
   return 1;
}

static vec_of(Token) mtoksbuf, /* buffers for macro replacement list tokens */
                     mdyntoksbuf; /* for function-like macros after parameter substitution */

typedef struct TokList TokList;
typedef struct Macro {
   internstr *param;
   Span0 span;
   uchar nparam;
   bool predef : 1,
        special : 1,
        fnlike : 1,
        variadic : 1;
   short id;
   union {
      void (*handler)(Lexer *, Token *); /* special && !fnlike */
      void (*handlerfn)(Lexer *, Token *, const Token *, int); /* special && fnlike */
      const Token *single; /* predef && !special */
      struct TokList {
         uint off; /* mtoksbuf[] */
         int n;
      } rl;
   };
} Macro;

static bool
macroequ(const Macro *a, const Macro *b)
{
   if (a->special != b->special) return 0;
   if (a->fnlike != b->fnlike || a->variadic != b->variadic) return 0;
   if (a->fnlike) {
      if (a->nparam != b->nparam) return 0;
      for (int i = 0; i < a->nparam; ++i)
         if (a->param[i] != b->param[i])
            return 0;
   }
   if (a->special) return a->handler == b->handler;
   if (a->predef || b->predef) {
      if (a->predef && b->predef)
         return tokequ(a->single, b->single);
      if (a->predef && !b->predef) {
         return (!!a->single) == b->rl.n
            && (!a->single || tokequ(a->single, &mtoksbuf.p[b->rl.off]));
      }
      return (!!b->single) == a->rl.n
         && (!b->single || tokequ(b->single, &mtoksbuf.p[a->rl.off]));
   }
   if (a->rl.n != b->rl.n) return 0;
   const Token *tka = &mtoksbuf.p[a->rl.off], *tkb = &mtoksbuf.p[b->rl.off];
   for (int i = 0; i < a->rl.n; ++i) {
      if (!tokequ(&tka[i], &tkb[i]))
         return 0;
      if (i > 0 && tka[i].space != tkb[i].space)
         return 0;
   }
   return 1;
}

static void
freemac(Macro *mac)
{
   if (mac->special) return;
   free(mac->param);
}

static pmap_of(Macro) macroht;

static void
putmac(internstr name, Macro *mac)
{
   static short id;
   if (!macroht.v) pmap_init(&macroht, 1<<10);
   Macro *slot = pmap_get(&macroht, name);
   mac->id = id++;
   if (slot) {
      if (!macroequ(slot, mac)) {
         if (slot->predef)
            warn(&(Span){mac->span}, "redefining builtin macro");
         else {
            warn(&(Span){mac->span}, "redefining macro");
            note(DGWARN, &(Span){slot->span}, "previous definition:");
         }
         freemac(slot);
         *slot = *mac;
      } else {
         freemac(mac);
      }
   } else {
      pmap_set(&macroht, name, *mac);
   }
}

static void
delmac(internstr name)
{
   Macro *slot = pmap_get(&macroht, name);
   if (!slot) return;
   freemac(slot);
   pmap_del(&macroht, name);
}

static inline internstr
macname(Macro *mac)
{
   return macroht.mb.k[mac - macroht.v];
}

static inline Macro *
findmac(internstr name)
{
   return pmap_get(&macroht, name);
}

static void popmac(Lexer *, bool all);

typedef struct MacroStack MacroStack;
static struct MacroStack {
   struct {
      union {
         uint off; /* mtoksbuf[]/mdyntoksbuf[] */
         const Token *p;
      };
      int n;
   } rl;
   Span0 exspan;
   int idx;
   short macid; /* -1 for argument undergoing expansion */
   bool space : 1, stop : 1, dyn;
} mstk[1200];

static void NORETURN
lxfatal(Lexer *lx, const Span *span, const char *fmt, ...)
{
   if (fmt) {
      va_list ap;
      va_start(ap, fmt);
      vdiag(span, DGERROR, fmt, ap);
      va_end(ap);
   }
   int n = lx->macstk ? lx->macstk - mstk : 0, i = 0;
   for (MacroStack *l = lx->macstk; l && l > mstk; --l, ++i) {
      if (i < 4 || i > n - 5) {
         note(DGERROR, &(Span){l->exspan}, "expanded from here");
      } else if (i == 5) {
         efmt(" (...) \n");
      }
   }
   for (Lexer *sv = lx->save; sv; sv = sv->save) {
      int line;
      const char *f = getfilepos(&line, NULL, sv->fileid, sv->chridx-2);
      note(DGERROR, NULL, "in file included from %s:%d", f, line);
   }
   if (!fmt || span) efmt("Aborting due to previous error.\n");
   exit(1);
}

static void
ppskipline(Lexer *lx)
{
   while (lx->macstk) popmac(lx, 1);
   for (int c; (c = peek(lx, 0)) != '\n' && !lx->eof; next(lx)) {
      if (c == '/' && peek(lx, 1) == '*') { /* comment */
         next(lx), next(lx);
         bool done = 0;
         while (!((c = peek(lx, 0)) == '*' && peek(lx, 1) == '/')) {
            if (lx->eof) {
               Span span = {{ lx->idx, lx->chridx - lx->idx, lx->fileid }};
               lxfatal(lx, &span, "unterminated comment");
            }
            done = c == '\n';
            next(lx);
         }
         next(lx);
         if (done) return;
      }
   }
}

#define isppident(tk) in_range((tk).t, TKIDENT, TKWEND_)

static bool
tokpaste(Lexer *lx, Token *dst, const Token *l, const Token *r)
{
   int t;
   if (isppident(*l) && (isppident(*r) || r->t == TKNUMLIT)) {
      /* foo ## bar  ;   foo ## 123 */
      t = TKIDENT;
   } else if (l->t == TKNUMLIT && (isppident(*r) || r->t == TKNUMLIT)) {
      /* 0x ## abc   ;   213 ## 456 */
      t = TKNUMLIT;
   } else if (l->t && !r->t) {
      if (dst) *dst = *l;
      return 1;
   } else if (!l->t && r->t) {
      if (dst) *dst = *r;
      return 1;
   } else {
      static const struct { char s[2]; char t; } tab[] = {
         {"==", TKEQU}, {"!=", TKNEQ}, {"<=", TKLTE}, {">=", TKGTE},
         {">>", TKSHR}, {"<<", TKSHL}, {"++", TKINC}, {"--", TKDEC},
         {"->", TKARROW}, {"##", TKPPCAT}, {"&&", TKLOGAND}, {"||", TKLOGIOR},
         {"+=", TKSETADD}, {"-=", TKSETSUB}, {"*=", TKSETMUL}, {"/=", TKSETDIV},
         {"%=", TKSETREM}, {"|=", TKSETIOR}, {"^=", TKSETXOR}, {"&=", TKSETAND},
         {{TKSHL,'='}, TKSETSHL}, {{TKSHR,'='}, TKSETSHR}
      };
      for (int i = 0; i < countof(tab); ++i) {
         if (tab[i].s[0] == l->t && tab[i].s[1] == r->t) {
            if (dst) dst->t = tab[i].t;
            return 1;
         }
      }

      if (dst) {
         error(&l->span, "pasting %'tk and %'tk does not form a valid preprocessing token", l, r);
         note(DGERROR, &r->span, "right-hand side");
      }
      return 0;
   }

   if (!dst) return 1;
   char buf[200];
   memset(dst, 0, sizeof *dst);
   dst->span = l->span;
   if (dst->span.ex.file == r->span.ex.file && dst->span.ex.off < r->span.ex.off)
      joinspan(&dst->span.ex, r->span.ex);
   dst->t = t;
   dst->len = l->len + r->len;
   char *s = (isppident(*dst) && dst->len + 1 < sizeof buf) ? buf : alloc(lx->tmparena, dst->len + 1, 1);
   memcpy(s, l->s, l->len);
   memcpy(s + l->len, r->s, r->len);
   s[dst->len] = 0;
   dst->space = l->space;
   if (isppident(*dst)) {
      dst->blue = 0;
      dst->name = intern(s);
   } else {
      dst->s = s;
   }
   return 1;
}

enum { MAXMACROARGS = 128 };

static void
ppdefine(Lexer *lx)
{
   Token tk0, tk;
   internstr mname;
   Macro mac = {0};
   BitSet usedparams[BSSIZE(MAXMACROARGS)] = {0};

   lex0(lx, &tk0, 0);
   if (tk0.t != TKIDENT) {
      error(&tk0.span, "macro name missing");
      ppskipline(lx);
      return;
   }
   mname = tk0.name;
   mac.span = tk0.span.sl;

   if (match(lx, '(')) {
      /* gather params for function-like macro */
      vec_of(internstr) params = {0};
      vinit(&params, NULL, 4);
      mac.fnlike = 1;
      while (lex0(lx, &tk, 0) != ')') {
         if (mac.variadic) {
            error(&tk.span, "expected `)' after `...'");
            if (tk.t == TKEOF || tk.t == '\n') return;
            break;
         }
         if (params.n > 0) {
            if (tk.t == TKDOTS) { /* GNU extension 'args...' */
               mac.variadic = 1;
               continue;
            } if (tk.t != ',') {
               error(&tk.span, "expected `,' or `)'");
               if (tk.t == TKEOF || tk.t == '\n') return;
               break;
            }
            lex0(lx, &tk, 0);
         }
         if (tk.t == TKIDENT)
            vpush(&params, tk.name);
         else if (tk.t == TKDOTS) {
            mac.variadic = 1;
            vpush(&params, intern("__VA_ARGS__"));
         } else {
            error(&tk.span, "expected parameter name or `)'");
            if (tk.t == TKEOF || tk.t == '\n') return;
            break;
         }
      }
      if (!params.n) vfree(&params);
      mac.param = params.p;
      mac.nparam = params.n;
   }

   /* gather replacement list */
   mac.rl.off = mtoksbuf.n;
   for (int n = 0; lex0(lx, &tk, 0) != '\n' && tk.t != TKEOF;) {
      if (n == 0 && !tk.space && !mac.fnlike)
         warn(&tk.span, "no whitespace after macro name");
      Token *prev = n ? &mtoksbuf.p[mtoksbuf.n-1] : NULL;
      if (mac.fnlike && tk.t == TKIDENT) {
         for (int i = 0; i < mac.nparam; ++i) {
            if (tk.name == mac.param[i]) {
               bsset(usedparams, i);
               tk.argidx = i;
               if (prev && prev->t == '#') {
                  tk.t = TKPPMACSTR;
                  *prev = tk;
                  goto Next;
               } else {
                  tk.t = TKPPMACARG;
                  break;
               }
            }
         }
      }
      if (n > 1 && prev->t == TKPPCAT) {
         Token new;
         if (prev[-1].t != TKPPMACARG && tk.t != TKPPMACARG
          && tokpaste(lx, &new, &prev[-1], &tk))
         {
            /* trivial concatenations */
            prev[-1] = new;
            --mtoksbuf.n;
            --n;
            continue;
         }
      }
      if (in_range(tk.t, TKNUMLIT, TKSTRLIT) && !tk.litlit)
         tk.s = alloccopy(&globarena, tk.s, tk.len << tk.wide, 1);
      vpush(&mtoksbuf, tk);
      ++n;
   Next:;
   }
   mac.rl.n = mtoksbuf.n - mac.rl.off;
   /* mark unused params as such by nulling out param name,
    * this way they aren't expanded when unused in the macro body */
   for (uint i = 0; bsiterzr(&i, usedparams, countof(usedparams)) && i < mac.nparam; ++i) {
      mac.param[i] = NULL;
   }
   putmac(mname, &mac);
}

static void
expecteol(Lexer *lx, const char *ppname)
{
   Token tk;
   assert(!lx->macstk);
   if (lex0(lx, &tk, 0) != '\n' && tk.t != TKEOF) {
      (ccopt.pedant ? error : warn)(&tk.span, "extra tokens after #%s", ppname);
      ppskipline(lx);
   }
}
static void
ppundef(Lexer *lx)
{
   Token tk;

   lex0(lx, &tk, 0);
   if (tk.t != TKIDENT) {
      error(&tk.span, "macro name missing");
      ppskipline(lx);
      return;
   }
   expecteol(lx, "undef");
   delmac(tk.name);
}

static void
pushmacstk(Lexer *lx, const Span *span, const MacroStack *m)
{
   MacroStack *l = lx->macstk;
   if (!l) l = mstk;
   else if ((++l == mstk+countof(mstk))) lxfatal(lx, span, "macro expansion depth limit reached");
   *l = *m;
   l->idx = 0;
   l->exspan = span->ex;
   lx->macstk = l;
}

static void
popmac(Lexer *lx, bool all)
{
   MacroStack *stk;

   assert(stk = lx->macstk);
   do {
      if (stk->dyn)
         mdyntoksbuf.n -= stk->rl.n;
      if (lx->macstk == mstk) lx->macstk = NULL;
      else --lx->macstk;
      if (!all) break;
   } while ((stk = lx->macstk) && stk->idx >= stk->rl.n && !stk->stop);
}


static inline const Token *
stkgetrl(MacroStack *s)
{
   if (s->macid < 0) return s->rl.p;
   return (s->dyn ? mdyntoksbuf.p : mtoksbuf.p) + s->rl.off;
}

static void expandfnmacro(Lexer *lx, Span *span, internstr mname, Macro *mac);

static enum expandres { EXPNONE, EXPINL, EXPSTACK }
tryexpand(Lexer *lx, Token *tk)
{
   Span span = tk->span;
   Macro *mac = NULL;
   internstr mname = tk->name;

   if (tk->t != TKIDENT || tk->blue || !(mac = findmac(mname)))
      return EXPNONE;

   /* prevent infinite recursion */
   for (MacroStack *l = lx->macstk; l && l+1 > mstk; --l) {
      if (l->macid == mac->id) {
         tk->blue = 1;
         return EXPNONE;
      }
   }

   MacroStack *stkprev = lx->macstk;
   if (mac->special && !mac->fnlike) {
      mac->handler(lx, tk);
      return EXPINL;
   } else if (mac->fnlike) {
      /* look if there is a '(' token ahead, expand if so */
      MacroStack *s = lx->macstk;
      if (s && s->idx >= s->rl.n && !s->stop) {
         popmac(lx, 1);
         s = lx->macstk;
      }
      if (!s) { /* top-level context: looking ahead in file data */
         Token tk;
         int t;
         for (;;) { /* skip whitespace and comments */
            if (aisspace(t = peek(lx, 0))) next(lx);
            else if (t == '/') {
               int idx = lx->chridx;
               switch (peek(lx, 1)) {
               case '/':
                  while (!lx->eof && next(lx) != '\n') ;
                  continue;
               case '*':
                  next(lx), next(lx);
                  while (peek(lx, 0) != '*' || peek(lx, 1) != '/') {
                     if (lx->eof) {
                        Span span = {{ idx, lx->chridx - idx, lx->fileid }};
                        lxfatal(lx, &span, "unterminated comment");
                     }
                     next(lx);
                  }
                  next(lx), next(lx);
                  continue;
               }
               break;
            } else break;
         }
         if (t != '(') return 0;
         lex0(lx, &tk, 0);
      } else { /* expansion context: look ahead in macro stack */
         if (s->idx >= s->rl.n || stkgetrl(s)[s->idx].t != '(') return 0;
         ++s->idx;
      }
      expandfnmacro(lx, &span, mname, mac);
   } else if (mac->predef && mac->single) {
      Span span = tk->span;
      *tk = *mac->single;
      tk->span = span;
      return EXPINL;
   } else if (mac->rl.n) {
      pushmacstk(lx, &span, &(MacroStack){
         .rl = { .off = mac->rl.off, .n = mac->rl.n },
         .macid = mac->id,
         .space = tk->space,
      });
   }
   if (lx->macstk != stkprev) {
      lx->macstk->space = tk->space;
   }
   return EXPSTACK;
}

static bool
advancemacstk(Lexer *lx, Token *tk)
{
   MacroStack *s = lx->macstk;
   assert(s != NULL);
   if (s->idx >= s->rl.n) {
      if (s->stop) {
         tk->t = TKEOF;
         return 1;
      }
      popmac(lx, 1);
      return 0;
   }
   *tk = stkgetrl(s)[s->idx];
   if (s->idx == 0) {
      /* the first token of the replaced expansion gets its space from the
       * context in which it is expanded */
      tk->space = s->space;
   }
   ++s->idx;
   assert(tk->t && tk->t != TKEOF);
   tk->span.ex = s->exspan;
   return tryexpand(lx, tk) != EXPSTACK;
}

static void
expandfnmacro(Lexer *lx, Span *span, internstr mname, Macro *mac)
{
   Token _argsbuf[30];
   vec_of(Token) argsbuf = VINIT(_argsbuf, countof(_argsbuf)); /* buffer for argument tokens */
   Span excessspan;
   int cur, len, i, bal, narg;
   Token tk;
   bool toomany = 0;
   struct ArgToks {
      int idx, n; /* slices of argsbuf */
      int idx2, n2;
      ushort nfirstx, /* for concatenation to work properly with expanded arguments, */
             nlastx;  /* length of expanded first and last tokens of the unexpanded argument */
   } _args0[4],
     *args = mac->nparam < countof(_args0) ? _args0 : alloc(lx->tmparena, sizeof *args * mac->nparam, 0);

   cur = i = bal = len = narg = 0;
   for (MacroStack *s = lx->macstk;;) {
      if (!s) {
         bool nl = 0;
         for (;; nl = 1) {
            lex0(lx, &tk, 0);
            if (tk.t != '\n') break;
         }
         tk.space |= nl;
      }
      else {
         tk = s->idx < s->rl.n ? stkgetrl(s)[s->idx++] : (Token){TKEOF};
      }
      if (((tk.t == ')' && bal == 0) || tk.t == TKEOF)) break;
      if (tk.t == ',' && bal == 0) {
         ++narg;
         if (i == mac->nparam-1 && !mac->variadic) {
            excessspan = tk.span;
            toomany = 1;
         } else if (i < mac->nparam - mac->variadic) {
            assert(i < MAXMACROARGS);
            args[i].idx = cur;
            args[i].n = len;
            cur = argsbuf.n;
            len = 0;
            ++i;
         } else if (mac->variadic) {
            vpush(&argsbuf, tk);
            ++len;
         }
      } else if (!toomany) {
         if (tk.t == '(') ++bal;
         else if (tk.t == ')') --bal;
         vpush(&argsbuf, tk);
         ++len;
      }
   }

   if (tk.t == TKEOF) {
      joinspan(&span->ex, tk.span.ex);
      lxfatal(lx, span, "unterminated function-like macro invocation");
   } else if (i < mac->nparam) {
      ++narg;
      args[i].idx = cur;
      args[i].n = len;
      cur = argsbuf.n;
      len = 0;
      ++i;
   }
   joinspan(&span->ex, tk.span.ex);
   int expargs0 = argsbuf.n;
   for (int i = 0; i < mac->nparam; ++i) {
      struct ArgToks *arg = &args[i];
      if (i >= narg) {
         memset(arg, 0, sizeof *arg);
      } else if (!mac->param || (mac->param[i] && arg->n > 0)) {
         /* expand args used in the macro body */
         pushmacstk(lx, &tk.span, &(MacroStack) {
            .rl = { .p = argsbuf.p + arg->idx, .n = arg->n },
            .macid = -1,
            .stop = 1,
         });
         MacroStack *l = lx->macstk;
         arg->idx2 = argsbuf.n;
         arg->nfirstx = arg->nlastx = 1;
         int ilastx = -1;
         for (bool pad = 0;;) {
            MacroStack *sprev = lx->macstk;
            if (!advancemacstk(lx, &tk)) {
               pad |= tk.space && lx->macstk == sprev; /* preserve whitespace empty macro */
               if (lx->macstk == l && l->idx == 1)
                  arg->nfirstx = argsbuf.n - arg->idx2;
               if (lx->macstk == l+1 && lx->macstk->idx == 0 && l->idx == l->rl.n)
                  ilastx = argsbuf.n - arg->idx2;
               continue;
            }
            if (tk.t == TKEOF) break;
            size_t off = l->rl.p - argsbuf.p;
            tk.space |= pad;
            vpush(&argsbuf, tk);
            l->rl.p = argsbuf.p + off;
            pad = 0;
         }
         arg->n2 = argsbuf.n - arg->idx2;
         arg->nlastx = ilastx < 0 ? 1 : args->n2 - ilastx;
         assert(lx->macstk == l);
         popmac(lx, 0);
      } else {
         memset(arg, 0, sizeof *arg);
      }
   }
   if (narg < mac->nparam - mac->variadic) {
      warn(span, "macro `%s' passed %d arguments, but takes %d", mname, narg, mac->nparam);
   } else if (toomany) {
      joinspan(&excessspan.ex, tk.span.ex);
      warn(&excessspan, "macro `%s' passed %d arguments, but takes just %d", mname, narg, mac->nparam);
   }
   if (mac->special) {
      mac->handlerfn(lx, &tk, argsbuf.p+expargs0, argsbuf.n-expargs0);
      vpush(&mdyntoksbuf, tk);
      pushmacstk(lx, span, &(MacroStack){
         .rl = { .off = mdyntoksbuf.n-1, .n = 1 },
         .dyn = 1,
         .macid = mac->id,
      });
   } else if (mac->nparam > 0) { /* make new rlist with args replaced */
      bool vaoptskip = 0, spacepad = 0;
      int vaoptbal = 0;
      uint off = mdyntoksbuf.n;
      for (int i = 0; i < mac->rl.n; ++i) {
         struct ArgToks *arg;
         const Token *tki = &mtoksbuf.p[mac->rl.off+i];
         if (vaoptskip) {
            assert(vaoptbal > 0);
            if (tki->t == '(') ++vaoptbal;
            else if (tki->t == ')') {
               if (--vaoptbal == 0) vaoptskip = 0;
            }
            continue;
         }
         if (tki->t == TKPPCAT && i > 0 && i < mac->rl.n-1) { /* concatenation */
            const Token *lhs = tki-1,
                               *rhs = tki+1;
            bool space = lhs->space | spacepad;
            if (lhs->t == ',' && mac->variadic
             && rhs->t == TKPPMACARG && rhs->argidx == mac->nparam-1) {
               /* handle GNU extension:  ', ## __VA_ARGS__' */
               arg = &args[rhs->argidx];
               if (narg < mac->nparam) { /* no vaargs -> skip comma */
                  assert(arg->n == 0);
                  --mdyntoksbuf.n;
               } else { /* otherwise put comma and substitute vaargs */
                  vpushn(&mdyntoksbuf, argsbuf.p+arg->idx2, arg->n2);
                  mdyntoksbuf.p[mdyntoksbuf.n - arg->n2].space |= rhs->space | tk.space;
               }
               ++i; /* we already handled rhs (__VA_ARGS__) */
               continue;
            }
            if (i > 2 && tki[-2].t == TKPPCAT) {
               /* handles chained concatenations: xyz ## arg ## c
                *                                        lhs ^  rhs */
               lhs = (off < mdyntoksbuf.n) ? &mdyntoksbuf.p[--mdyntoksbuf.n] : NULL;
            } else if (lhs->t == TKPPMACARG) {
               arg = &args[lhs->argidx];
               lhs = arg->n ? &argsbuf.p[arg->idx + arg->n-1] : NULL;
               if (lhs && arg->n > 1) space |= lhs->space;
            } else {
               --mdyntoksbuf.n;
            }
            if (rhs->t == TKPPMACARG) {
               arg = &args[rhs->argidx];
               rhs = arg->n ? &argsbuf.p[arg->idx] : NULL;
            } else {
               ++i;
            }
            if (!lhs && !rhs) continue;
            spacepad = 0;
            if (!lhs) vpush(&mdyntoksbuf, *rhs);
            else if (!rhs) vpush(&mdyntoksbuf, *lhs);
            else {
               Token new;
               if (tokpaste(lx, &new, lhs, rhs)) {
                  new.span.sl = tki->span.sl;
               }
               vpush(&mdyntoksbuf, new);
            }
            mdyntoksbuf.p[mdyntoksbuf.n-1].space = space;
         } else if (tki->t != TKPPMACARG && tki->t != TKPPMACSTR) { /* regular token */
            if (tki->t == TKIDENT && mac->variadic) {
               /* handle GNUC __VA_OPT__(...) */
               static internstr istr_vaopt;
               if (!istr_vaopt) istr_vaopt = intern("__VA_OPT__");
               if (tki->name == istr_vaopt && i+2 < mac->rl.n && tki[1].t == '(') {
                  vaoptbal = 1;
                  vaoptskip = args[mac->nparam-1].n == 0;
                  ++i; /* skip open paren */
                  continue;
               }
            }
            if (vaoptbal) {
               if (tki->t == '(') ++vaoptbal;
               else if (tki->t == ')') {
                  /* skip closing paren of __VA_OPT__ invocation */
                  if (--vaoptbal == 0) continue;
               }
            }
            vpush(&mdyntoksbuf, *tki);
            mdyntoksbuf.p[mdyntoksbuf.n-1].space |= spacepad;
            spacepad = 0;
         } else if (tki->t == TKPPMACARG) {
            arg = &args[tki->argidx];
            if (arg->n == 0) {
               spacepad = 1;
               continue;
            }
            Token *rl = argsbuf.p + arg->idx2;
            int n = arg->n2;
            bool skipfirst = 0;
            if (i > 0 && tki[-1].t == TKPPCAT) {
               /* skip first unexpanded token, was pasted */
               rl += arg->nfirstx;
               n -= arg->nfirstx;
               skipfirst = 1;
            }
            if (i < mac->rl.n-2 && tki[1].t == TKPPCAT) {
               /* skip last unexpanded token, will be pasted */
               n -= arg->nlastx;
            }
            if (n > 0) {
               vpushn(&mdyntoksbuf, rl, n);
               if (!skipfirst)
                  /* the first token of the expanded body gets its space from the replacement list */
                  mdyntoksbuf.p[mdyntoksbuf.n - n].space = tki->space | spacepad;
            }
            spacepad = 0;
         } else { /* PPMACSTR */
            char tmp[200];
            WriteBuf buf = MEMBUF(tmp, sizeof tmp);
            int n = 0;

            arg = &args[tki->argidx];
            // XXX this is wrong bc the string literal produced should be re-parsed later
            // i.e. stringifying the token sequence '\n' should ultimately produce a
            // string with an actual newline, not {'\\','n'}
         Redo:
            for (int i = 0; i < arg->n; ++i) {
               Token *tk = &argsbuf.p[arg->idx + i];
               if (i > 0 && tk->space)
                  n += bfmt(&buf, " ");
               n += bfmt(&buf, "%tk", tk);
            }
            ioputc(&buf, 0);
            if (buf.err) {
               WriteBuf new = MEMBUF(alloc(lx->tmparena, n+1, 1), n+1);
               assert(buf.buf == tmp);
               memcpy(&buf, &new, sizeof buf);
               goto Redo;
            }
            vpush(&mdyntoksbuf, ((Token) {
               .t = TKSTRLIT,
               .wide = 0,
               .space = tki->space | spacepad,
               .s = buf.buf != tmp ? buf.buf : alloccopy(lx->tmparena, buf.buf, buf.len, 1),
               .len = buf.len-1,
            }));
            spacepad = 0;
         }
      }
      uint n = mdyntoksbuf.n - off;

      if (n) {
         pushmacstk(lx, span, &(MacroStack){
            .rl = { .off = off, .n = n },
            .macid = mac->id,
            .dyn = 1,
         });
      }
   } else if (mac->rl.n) {
      pushmacstk(lx, span, &(MacroStack){
         .rl = { .off = mac->rl.off, .n = mac->rl.n },
         .macid = mac->id,
      });
   }
   vfree(&argsbuf);
}

static Token epeektk;
static int
elex(Lexer *lx, Token *tk)
{
   assert(tk);
   if (epeektk.t) {
      int tt = epeektk.t;
      if (tk) *tk = epeektk;
      epeektk.t = 0;
      return tt;
   }
   if (lx->macstk) {
      if (!advancemacstk(lx, tk))
         return elex(lx, tk);
      return tk->t;
   }

   lex0(lx, tk, 0);
   return tk->t;
}

static int
epeek(Lexer *lx, Token *tk)
{
   if (!epeektk.t) elex(lx, &epeektk);
   if (tk) *tk = epeektk;
   return epeektk.t;
}

static int
tkprec(int tt)
{
   static const char tab[] = {
      ['*']   = 12, ['/']   = 12, ['%']  = 12,
      ['+']   = 11, ['-']   = 11,
      [TKSHL] = 10, [TKSHR] = 10,
      ['<']   =  9, ['>']   = 9, [TKLTE] =  9, [TKGTE] = 9,
      [TKEQU] =  8, [TKNEQ] = 8,
      ['&']   =  7,
      ['^']   =  6,
      ['|']   =  5,
      [TKLOGAND] = 4,
      [TKLOGIOR] = 3,
      ['?']   = 2,
   };
   if ((uint)tt < countof(tab))
      return tab[tt] - 1;
   return -1;
}

static vlong
expr(Lexer *lx, bool *pu, int prec, bool ignore)
{
   Token tk;
   enum typetag ty;
   char unops[16];
   int nunop = 0;
   vlong x, y;
   bool xu = 0, yu; /* x unsigned?; y unsigned? */

Unary:
   elex(lx, &tk);
Switch:
   switch (tk.t) {
   case '-': case '~': case '!':
      unops[nunop++] = tk.t;
      if (nunop >= countof(unops)) {
         x = expr(lx, &xu, 999, ignore);
         break;
      }
      /* fallthru */
   case '+': goto Unary;
   case '(':
      x = expr(lx, &xu, 1, ignore);
      if (elex(lx, &tk) != ')') {
         error(&tk.span, "expected ')'");
         goto Err;
      }
      break;
   case TKNUMLIT:
   case TKCHRLIT:
      ty = parsenumlit((uvlong *)&x, NULL, &tk, 1);
      if (!ty) {
         error(&tk.span, "bad number literal");
         goto Err;
      } else if (isfltt(ty)) {
         error(&tk.span, "float literal in preprocessor expresion");
         goto Err;
      }
      xu = isunsignedt(ty);
      break;
   default:
      if (tk.t == TKIDENT) {
         xu = 0;
         if (!strcmp(tk.s, "defined")) {
            /* 'defined' ppident */
            bool paren = 0;
            lex0(lx, &tk, 0);
            if ((paren = tk.t == '(')) lex0(lx, &tk, 0);
            if (!isppident(tk)) {
               error(&tk.span, "expected macro name");
               goto Err;
            }
            if (paren && lex0(lx, &tk, 0) != ')') {
               error(&tk.span, "expected `)'");
               goto Err;
            }
            x = findmac(tk.name) != NULL;
         } else {
            switch (tryexpand(lx, &tk)) {
            case EXPSTACK: goto Unary;
            case EXPINL: goto Switch;
            case EXPNONE: x = 0; break; /* non defined pp name -> 0 */
            }
         }
         break;
      }
      error(&tk.span, "expected preprocessor integer expression (near %'tk)", &tk);
      goto Err;
   }

   while (nunop > 0) switch (unops[--nunop]) {
      case '-': x = -(uvlong)x; break;
      case '~': x = ~x; break;
      case '!': x = !x; break;
      default: assert(0);
   }

   for (int opprec; (opprec = tkprec(epeek(lx, &tk))) >= prec;) {
      elex(lx, &tk);
      if (tk.t == TKLOGAND) {
         x = !!x & !!expr(lx, &yu, opprec+1, ignore || !x);
         xu = 0;
      } else if (tk.t == TKLOGIOR) {
         x = !!x | !!expr(lx, &yu, opprec+1, ignore || x);
         xu = 0;
      } else if (tk.t == '?') {
         Span span = tk.span;
         vlong m = expr(lx, &xu, 1, ignore || !x);
         if (elex(lx, &tk) != ':') {
            error(&tk.span, "expected ':'");
            note(DGERROR, &span, "to match conditional expression here");
            goto Err;
         }
         y = expr(lx, &yu, 1, ignore || x);
         x = x ? m : y;
         xu |= yu;
      } else {
         y = expr(lx, &yu, opprec + 1, ignore);
         bool u = xu | yu;
         switch ((int) tk.t) {
         case   '+': x += (uvlong) y; break;
         case   '-': x -= (uvlong) y; break;
         case   '*': x = u ? (uvlong) x * y : x * y; break;
         case   '&': x &= y; break;
         case   '^': x ^= y; break;
         case   '|': x |= y; break;
         case   '/': if (y) x = u ? (uvlong) x / y : x / y;
                     else if (ignore) x = 0;
                     else goto Div0;
                     break;
         case   '%': if (y) x = u ? (uvlong) x % y : x % y;
                     else if (ignore) x = 0;
                     else Div0: error(&tk.span, "division by zero");
                     break;
         case TKSHL: if ((uvlong)y < 64) x <<= y;
                     else if (ignore) x = 0;
                     else goto BadShift;
                     break;
                     u = xu;
         case TKSHR: if ((uvlong)y < 64) x = u ? (uvlong) x >> y : x >> y;
                     else if (ignore) x = 0;
                     else BadShift: error(&tk.span, "bad shift by %ld", y);
                     u = xu;
                     break;
         case   '<': x = u ? (uvlong) x < y  : x < y;  u = 0; break;
         case   '>': x = u ? (uvlong) x > y  : x > y;  u = 0; break;
         case TKLTE: x = u ? (uvlong) x <= y : x <= y; u = 0; break;
         case TKGTE: x = u ? (uvlong) x >= y : x >= y; u = 0; break;
         case TKEQU: x = x == y; u = 0; break;
         case TKNEQ: x = x != y; u = 0; break;
         default: assert(0);
         }
         xu = u;
      }
   }
   if (!prec) { /* not a sub expr */
      if (elex(lx, &tk) != '\n' && tk.t != TKEOF) {
         error(&tk.span, "extra tokens after preprocessor expression");
         ppskipline(lx);
      }
   }
   if (pu) *pu = xu;
   return x;

Err:
   ppskipline(lx);
   if (pu) *pu = xu;
   return 0;
}

enum {
   PPCNDFALSE, /* the condition was zero, skip until #else/#elif */
   PPCNDTRUE, /* the condition was non-zero, emit until #else/#elif */
   PPCNDTAKEN /* some branch was already taken, skip until #else */
};
typedef struct PPCond PPCond;
static struct PPCond {
   Span0 ifspan;
   int filedepth;
   uchar cnd;
   bool elsep;
} ppcndstk[32];
static int nppcnd;

static int includedepth;

static void
ppif(Lexer *lx, const Span *span)
{
   vlong v = expr(lx, NULL, 0, 0);
   assert(nppcnd < countof(ppcndstk) && "too many nested #if");
   ppcndstk[nppcnd].ifspan = span->sl;
   ppcndstk[nppcnd].filedepth = includedepth;
   ppcndstk[nppcnd].cnd = v ? PPCNDTRUE : PPCNDFALSE;
   ppcndstk[nppcnd++].elsep = 0;
}

static void
ppifxdef(Lexer *lx, bool defp, const Span *span)
{
   Token tk;

   lex0(lx, &tk, 0);
   if (tk.t != TKIDENT) {
      error(&tk.span, "macro name missing");
      ppskipline(lx);
      return;
   }
   expecteol(lx, defp ? "ifdef" : "ifndef");
   if (!defp && lx->firstdirective) lx->inclguard = tk.name;
   assert(nppcnd < countof(ppcndstk) && "too many nested #if");
   ppcndstk[nppcnd].ifspan = span->sl;
   ppcndstk[nppcnd].filedepth = includedepth;
   ppcndstk[nppcnd].cnd = (findmac(tk.name) == NULL) ^ defp ? PPCNDTRUE : PPCNDFALSE;
   ppcndstk[nppcnd++].elsep = 0;
}

static void
ppelif(Lexer *lx, const Span *span)
{
   vlong v;
   PPCond *cnd;

   if (!nppcnd) {
      error(span, "#elif without matching #if");
      ppif(lx, span);
      return;
   }
   v = expr(lx, NULL, 0, 0);
   cnd = &ppcndstk[nppcnd-1];
   if (cnd->elsep) {
      error(span, "#elif after #else");
      return;
   }
   switch (cnd->cnd) {
   case PPCNDTRUE: cnd->cnd = PPCNDTAKEN; break;
   case PPCNDFALSE: cnd->cnd = v ? PPCNDTRUE : PPCNDFALSE; break;
   }
}
static void
ppelifxdef(Lexer *lx, bool defp, const Span *span)
{
   Token tk;
   PPCond *cnd;

   if (!nppcnd) {
      error(span, "#elif%sdef without matching #if", &"n"[defp]);
      ppif(lx, span);
      return;
   }
   cnd = &ppcndstk[nppcnd-1];
   if (cnd->elsep) {
      error(span, "#elif%sdef after #else", &"n"[defp]);
      return;
   }
   lex0(lx, &tk, 0);
   if (tk.t != TKIDENT) {
      error(&tk.span, "macro name missing");
      ppskipline(lx);
      return;
   }
   expecteol(lx, defp ? "elifdef" : "elifndef");
   switch (cnd->cnd) {
   case PPCNDTRUE: cnd->cnd = PPCNDTAKEN; break;
   case PPCNDFALSE: cnd->cnd = (findmac(tk.name) == NULL) ^ defp ? PPCNDTRUE : PPCNDFALSE; break;
   case PPCNDTAKEN: assert(0);
   }
}

static void
ppendif(Lexer *lx, const Span *span)
{
   expecteol(lx, "endif");
   if (!nppcnd) {
      error(span, "#endif without matching #if");
      return;
   }
   --nppcnd;
}

static void
ppelse(Lexer *lx, const Span *span)
{
   PPCond *cnd;
   expecteol(lx, "else");
   if (!nppcnd) {
      error(span, "#else without matching #if");
      return;
   }
   cnd = &ppcndstk[nppcnd-1];
   if (cnd->elsep)
      error(span, "#else after #else");
   switch (cnd->cnd) {
   case PPCNDFALSE: cnd->cnd = PPCNDTRUE; break;
   case PPCNDTRUE: cnd->cnd = PPCNDTAKEN; break;
   }
   cnd->elsep = 1;
}

enum { MAXINCLUDE = 200 };
static bool
tryincludepath(Lexer *lx, const Span *span, char *path)
{
   Lexer new;
   const char *err;
   switch (initlexer(&new, &err, path)) {
   default: assert(0);
   case LXERR: return 0;
   case LXFILESEEN:
      xbfree(path);
      /* fallthru */
   case LXOK:
      new.save = xmalloc(sizeof *new.save);
      lx->inclnerror = nerror;
      lx->inclnwarn = nwarn;
      memcpy(new.save, lx, sizeof *lx);
      *lx = new;

      if (++includedepth == MAXINCLUDE)
         lxfatal(lx, span, "Maximum nested include depth of %d reached", includedepth);
      break;
   case LXFILESKIP:
      xbfree(path);
      break;
   }
   return 1;
}

static bool
doinclude(Lexer *lx, const Span *span, bool quote, const char *str, size_t slen)
{
   char *path = NULL;
   const char *base, *end;
   if (quote) {
      if (str[0] == '/') {
         /* try absolute path */
         xbgrow(&path, slen + 1);
         memcpy(path, str, slen);
         path[slen] = 0;
         if (tryincludepath(lx, span, path)) return 1;
         goto NotFound;
      }

      /* try relative to current file's directory */
      base = getfilename(lx->fileid, 0);
      for (end = base; *end != 0; ++end) {}
      for (--end; *end != '/' && end != base; --end) {}
      if (*end == '/') ++end;
      xbgrow(&path, end - base + slen + 1);
      memcpy(path, base, end - base);
      memcpy(path + (end - base), str, slen);
      path[end - base + slen] = 0;
      if (tryincludepath(lx, span, path)) return 1;
   }
   /* try system paths. order:
    * 1. -iquote
    * 2. -I
    * 3. -isystem
    * 4. embedded include files
    * 5. standard system includes
    * 6. -idirafter
    */
   for (int i = quote ? CINCL_iquote : CINCL_I; i < countof(cinclpaths); ++i) {
      for (CInclPath *p = cinclpaths[i]; p; p = p->next) {
         if (i == CINCLsys) {
            /* try embedded files pseudo-path */
            xbgrow(&path, slen + 3);
            path[0] = '@', path[1] = ':';
            memcpy(path+2, str, slen);
            path[slen+2] = 0;
            if (tryincludepath(lx, span, path)) return 1;
         }
         int ndir = strlen(p->path);
         xbgrow(&path, ndir + slen + 2);
         memcpy(path, p->path, ndir);
         path[ndir++] = '/';
         memcpy(path + ndir, str, slen);
         path[ndir + slen] = 0;
         if (tryincludepath(lx, span, path)) return 1;
      }
   }
NotFound:
   error(span, "file not found: %'S", str, slen);
   xbfree(path);
   return 0;
}

static bool
ppinclude(Lexer *lx, const Span *span0)
{
   Token tk;
   Span span = *span0;

   if (in_range(lex0(lx, &tk, 1), TKPPHDRH, TKPPHDRQ)) {
      expecteol(lx, "include");
      joinspan(&span.ex, tk.span.ex);
      return doinclude(lx, &span, tk.t == TKPPHDRQ, tk.s, tk.len);
   } else if (tk.t == '\n' || tk.t == TKEOF) {
      goto BadSyntax;
   } else {
      /* '#include pp-tokens'
       * gather and expand pp-tokens */
      Token tksbuf[8];
      vec_of(Token) tks = VINIT(tksbuf, countof(tksbuf));
      for (;;) {
         if (!lx->macstk) {
            if (tryexpand(lx, &tk) == EXPSTACK) continue;
            vpush(&tks, tk);
         } else if (advancemacstk(lx, &tk)) {
            vpush(&tks, tk);
            continue;
         }
         if (lex0(lx, &tk, 0) == '\n' || tk.t == TKEOF) break;
      }
      if (tks.n >= 1 && tks.p[0].t == TKSTRLIT) { /* "header.h" */
         if (tks.n > 1)
            (ccopt.pedant ? error : warn)(&tks.p[1].span, "extra tokens after #include");
         joinspan(&span.ex, tks.p[0].span.ex);
         return doinclude(lx, &span, 1, tks.p[0].s, tks.p[0].len);
      } else if (tks.n > 2 && tks.p[0].t == '<' && tks.p[tks.n-1].t == '>') { /* <header.h> */
         /* this is multiple tokens, concatenate them together */
         char buf[4096];
         WriteBuf wbuf = MEMBUF(buf, sizeof buf);
         for (int i = 1; i < tks.n-1; ++i) {
            Token *tk = &tks.p[i];
            bfmt(&wbuf, &" %tk"[!tk->space], tk);
         }
         joinspan(&span.ex, tks.p[tks.n-1].span.ex);
         if (wbuf.err) error(&span, "path too long");
         else {
            return doinclude(lx, &span, 0, buf, wbuf.len);
         }
      } else {
      BadSyntax:
         error(&tk.span, "expected \"header\" or <header>");
         ppskipline(lx);
      }
      vfree(&tks);
   }
   return 1;
}

static void
ppline(Lexer *lx, Token *tk0)
{
   Token tk, tks[2];
   int ntk = 0;
   Span span = tk0->span;
   bool ext = 0;
   if (tk0->t == TKNUMLIT) { /* handles GNU-style post preprocessing directive '# n ...' */
      tks[ntk++] = *tk0;
      ext = 1;
   }
   while (ntk < 2) {
      if (lx->macstk && advancemacstk(lx, &tk)) {
         tks[ntk++] = tk;
         if (lx->macstk->idx >= lx->macstk->rl.n) popmac(lx, 1);
      } else if (!lx->macstk && (lex0(lx, &tk, 0) == '\n' || tk.t == TKEOF)) {
         break;
      } else if (tk.t == TKIDENT && tryexpand(lx, &tk) == EXPSTACK) {
         continue;
      } else {
         tks[ntk++] = tk;
      }
   }
   uvlong lineno = 0;
   char *file = NULL;
   if (ntk > 0 && tks[0].t == TKNUMLIT) {
      if (!parsenumlit(&lineno, NULL, &tks[0], 1) || (lineno == 0 && !ext))
         goto BadNum;
      if (lineno >= 1<<(32-SPANFILEBITS)) {
         warn(&tks[0].span, "ignoring #line number that is too big");
         lineno = 0;
         goto Err;
      }
   } else {
   BadNum:
      error(ntk ? &tks[0].span : &span, "#line requires a positive integer argument");
   Err:
      if (lx->macstk || (tk.t != '\n' && tk.t != TKEOF)) ppskipline(lx);
      return;
   }
   if (ntk > 1) {
      if (tks[1].t == TKSTRLIT && !tks[1].wide) {
         file = alloc(&globarena, tks[1].len+1, 0);
         memcpy(file, tks[1].s, tks[1].len);
         file[tks[1].len] = 0;
      } else {
         error(&tks[1].span, "invalid filename for #line directive");
      }
   }
   if (lineno) setfileline(lx->fileid, lx->chridx, lineno, file);
   if (lx->macstk) {
      span.sl.off = span.ex.off = lx->chridx;
      span.sl.len = span.ex.len = 1;
      ppskipline(lx);
      if (!ext)
         (ccopt.pedant ? error : warn)(&span, "extra tokens after #line");
   } else if (tk.t != '\n' && tk.t != TKEOF) {
      if (ext) ppskipline(lx);
      else expecteol(lx, "line");
   }
}

static void
pppragma(Lexer *lx, const Span *span0)
{
   Token tk;
   Span span = *span0;
   if (lex0(lx, &tk, 0) == TKIDENT && !strcmp(tk.s, "once")) {
      markfileonce(lx->fileid, NULL);
   } else {
      joinspan(&span.ex, tk.span.ex);
      warn(&span, "unknown pragma ignored");
      ppskipline(lx);
      return;
   }
   expecteol(lx, "pragma");
}

static void
ppdiag(Lexer *lx, const Span *span0, bool err)
{
   const uchar *p = getfile(lx->fileid)->p;
   uint off = lx->chridx, end;
   ppskipline(lx);
   end = lx->chridx;
   while (off < end && aisspace(p[off])) ++off;
   (err ? error : warn)(span0, "%S", p + off, end - off);
}

enum directive {
   PPXXX,
   /* !sorted */
   PPDEFINE,
   PPELIF,
   PPELIFDEF,
   PPELIFNDEF,
   PPELSE,
   PPENDIF,
   PPERROR,
   PPIF,
   PPIFDEF,
   PPIFNDEF,
   PPINCLUDE,
   PPLINE,
   PPPRAGMA,
   PPUNDEF,
   PPWARNING,
};

static enum directive
findppcmd(const Token *tk)
{
   static const char *tab[] = {
      /* !sorted */
      "define",
      "elif",
      "elifdef",
      "elifndef",
      "else",
      "endif",
      "error",
      "if",
      "ifdef",
      "ifndef",
      "include",
      "line",
      "pragma",
      "undef",
      "warning",
   };
   int l = 0, h = countof(tab) - 1, i, cmp;
   const char *s = tk->s;

   if (tk->t == TKWif) return PPIF;
   if (tk->t == TKWelse) return PPELSE;
   /* binary search over sorted array */
   while (l <= h) {
      i = (l + h) / 2;
      cmp = strcmp(tab[i], s);
      if (cmp < 0) l = i + 1;
      else if (cmp > 0) h = i - 1;
      else return i + 1;
   }
   return PPXXX;
}

static void
identkeyword(Token *tk)
{
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-braces"
#endif
   static const struct {
      const char *s;
      struct Kw { uchar t, cstd : 4, ext : 1; } kw;
      const char *alias[2];
   } kwtab[] = {
#define _(kw, cstd, ...) { #kw, {TKW##kw, cstd}, __VA_ARGS__ },
#include "c_keywords.def"
#undef _
   };
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
   static pmap_of(struct Kw) kwmap;
   if (!kwmap.v) {
      pmap_init(&kwmap, 128);
      for (int i = 0; i < countof(kwtab); ++i) {
         struct Kw kw = kwtab[i].kw;
         /* allow future keywords but only if they begin with _ */
         if (kw.cstd <= ccopt.cstd || kwtab[i].s[0] == '_') {
            kw.ext = kw.cstd > ccopt.cstd;
            pmap_set(&kwmap, intern(kwtab[i].s), kw);
         }
         for (const char *const *palias = kwtab[i].alias, *const *end = palias+2;
              palias != end && *palias; ++palias)
         {
            pmap_set(&kwmap, intern(*palias), kw);
         }
      }
   }
   struct Kw *kw = pmap_get(&kwmap, tk->name);
   if (kw) {
      tk->t = kw->t;
      tk->extwarn = kw->ext;
   }
}

int
lex(Lexer *lx, Token *tk_)
{
   Token tkx[1], *tk;
   int t;

Begin:
   assert(tk_ != &lx->peektok);
   tk = tk_ ? tk_ : tkx;
   if (lx->peektok.t) {
      *tk = lx->peektok;
      memset(&lx->peektok, 0, sizeof lx->peektok);
      return tk->t;
   }

   if (lx->macstk) {
      if (!advancemacstk(lx, tk))
         goto Begin;
      if (tk->t == TKIDENT) identkeyword(tk);
      return tk->t;
   }
   bool linebegin = 1,
        skip = nppcnd ? ppcndstk[nppcnd-1].cnd != PPCNDTRUE : 0,
        inclerror = 0; /* set when #include header file not found: process other directives then abort */
   enum directive lastcmd = 0;
   for (;;) {
      while ((t = lex0(lx, tk, 0)) == '\n') linebegin = 1;
      if (t == '#' && linebegin) {
         if (lex0(lx, tk, 0) == '\n') { }
         else if (tk->t == TKNUMLIT || tk->t == TKIDENT) {
            lastcmd = tk->t == TKNUMLIT ? PPLINE : findppcmd(tk);
            if (nppcnd == lx->nppcnd0) lx->inclguard = NULL;
            if (!skip) {
               switch (lastcmd) {
               case PPXXX: goto BadPP;
               case PPDEFINE:   ppdefine(lx); break;
               case PPUNDEF:    ppundef(lx); break;
               case PPIF:       ppif(lx, &tk->span); break;
               case PPIFDEF:    ppifxdef(lx, 1, &tk->span); break;
               case PPIFNDEF:   ppifxdef(lx, 0, &tk->span); break;
               case PPELIF:     ppelif(lx, &tk->span); break;
               case PPELIFDEF:  ppelifxdef(lx, 1, &tk->span); break;
               case PPELIFNDEF: ppelifxdef(lx, 0, &tk->span); break;
               case PPELSE:     ppelse(lx, &tk->span); break;
               case PPENDIF:    ppendif(lx, &tk->span); break;
               case PPLINE:     ppline(lx, tk); break;
               case PPPRAGMA:   pppragma(lx, &tk->span); break;
               case PPWARNING:  ppdiag(lx, &tk->span, 0); break;
               case PPERROR:    ppdiag(lx, &tk->span, 1); break;
               case PPINCLUDE:  inclerror |= !ppinclude(lx, &tk->span); break;
               default: assert(0&&"nyi");
               }
            } else {
               switch (lastcmd) {
               case PPIF: /* increment nesting level */
               case PPIFDEF:
               case PPIFNDEF:
                  assert(nppcnd < countof(ppcndstk) && "too many nested #if");
                  ppcndstk[nppcnd].ifspan = tk->span.sl;
                  ppcndstk[nppcnd].cnd = PPCNDTAKEN;
                  ppcndstk[nppcnd++].elsep = 0;
                  break;
               case PPELIF:     ppelif(lx, &tk->span); break;
               case PPELIFDEF:  ppelifxdef(lx, 1, &tk->span); break;
               case PPELIFNDEF: ppelifxdef(lx, 0, &tk->span); break;
               case PPELSE:     ppelse(lx, &tk->span); break;
               case PPENDIF:    ppendif(lx, &tk->span); break;
               default: ppskipline(lx); break;
               }
            }
            if (lastcmd != PPINCLUDE)
               lx->firstdirective = 0;
            skip = nppcnd ? ppcndstk[nppcnd-1].cnd != PPCNDTRUE : 0;
         } else {
            if (!skip) {
            BadPP:
               error(&tk->span, "invalid preprocessor directive");
            }
            ppskipline(lx);
         }
         linebegin = 1;
      } else {
         lx->firstdirective = 0;
         linebegin = 0;
         if (skip && t != TKEOF)
            continue;
         if (tryexpand(lx, tk) == EXPSTACK)
            goto Begin;
         if (t == TKEOF && nppcnd && ppcndstk[nppcnd-1].filedepth == includedepth) {
            Span span = { ppcndstk[nppcnd-1].ifspan };
            error(&span, "#if is not matched by #endif");
         }
         if (t == TKEOF && lx->save) {
            /* end of #include'd file, restore previous state */
            if (lastcmd == PPENDIF && lx->inclguard) {
               markfileonce(lx->fileid, lx->inclguard);
            }
            Lexer *sv = lx->save;
            if (sv->inclnerror != nerror || sv->inclnwarn != nwarn) {
               int line;
               const char *f = getfilepos(&line, NULL, sv->fileid, sv->chridx-2);
               note(DGERROR, NULL, "in file included from %s:%d", f, line);
            }
            memcpy(lx, sv, sizeof *lx);
            free(sv);
            --includedepth;
            linebegin = 1;
            lx->firstdirective = 0;
         } else if (t == TKEOF && inclerror) {
            break;
         } else {
            if (nppcnd == lx->nppcnd0) lx->inclguard = NULL;
            if (t == TKIDENT) identkeyword(tk);
            if (!inclerror) return tk->t;
         }
      }
   }
   assert(inclerror);
   efmt("Aborting due to previous error(s).\n");
   exit(1);
   assert(0);
}

int
lexpeek(Lexer *lx, Token *tk_)
{
   Token tkx[1], *tk;
   uint t;

   tk = tk_ ? tk_ : tkx;
   if ((t = lx->peektok.t)) {
      *tk = lx->peektok;
      return t;
   }
   t = lex(lx, tk);
   lx->peektok = *tk;
   return t;
}

/* Predefined/builtin macros */

static char ppcmdlinebuf0[4000];
static WriteBuf ppcmdline = MEMBUF(ppcmdlinebuf0, sizeof ppcmdlinebuf0);

static void
ppcmdappendf(const char *fmt, ...)
{
   va_list ap;
   uint lensave = ppcmdline.len;
Redo:
   va_start(ap, fmt);
   uint n = vbfmt(&ppcmdline, fmt, ap);
   va_end(ap);
   if (ppcmdline.err) {
      uint newcap = ppcmdline.cap + n + 100;
      if (ppcmdline.buf == ppcmdlinebuf0) {
         ppcmdline.buf = xcalloc(newcap);
         memcpy(ppcmdline.buf, ppcmdlinebuf0, sizeof ppcmdlinebuf0);
      } else {
         ppcmdline.buf = xrealloc(ppcmdline.buf, newcap);
         memset(ppcmdline.buf + ppcmdline.cap, 0, newcap - ppcmdline.cap);
      }
      ppcmdline.cap = newcap;
      ppcmdline.len = lensave;
      ppcmdline.err = 0;
      goto Redo;
   }
}

static void
putdef1(const char *name)
{
   static const Token tok_1 = { TKNUMLIT, .s = "1", .len = 1, .litlit = 1 };
   putmac(intern(name), &(Macro) {
      .predef = 1,
      .single = &tok_1
   });
}

void
cpp0define(const char *name, const char *body)
{
   if (!body || !strcmp(body, "1"))
      putdef1(name);
   else
      ppcmdappendf("#define %s %s\n", name, body);
}

void
cpp0undef(const char *name)
{
   ppcmdappendf("#undef %s\n", name);
}

static void
mac__file__(Lexer *lx, Token *tk)
{
   tk->t = TKSTRLIT;
   tk->s = getfilename(lx->fileid, lx->chridx);
   tk->wide = 0;
   tk->len = strlen(tk->s);
}

static void
mac__line__(Lexer *lx, Token *tk)
{
   char buf[20];
   int line;
   WriteBuf wbuf = MEMBUF(buf, sizeof buf);
   getfilepos(&line, NULL, lx->fileid, lx->chridx);
   bfmt(&wbuf, "%d", line), buf[wbuf.len++] = 0;
   tk->t = TKNUMLIT;
   tk->s = alloccopy(lx->tmparena, buf, wbuf.len, 1);
   tk->len = wbuf.len-1;
}

#include <time.h>

static void
mac__date__(Lexer *lx, Token *tk)
{
   char buf[20];
   WriteBuf wbuf = MEMBUF(buf, sizeof buf);
   time_t tm = time(NULL);
   struct tm *ts = localtime(&tm);
   tk->t = TKSTRLIT;
   tk->wide = 0;
   tk->len = 11;
   if (ts) {
      bfmt(&wbuf, "%S %2d %4d%c",
           &"JanFebMarAprMayJunJulAugSepOctNovDec"[ts->tm_mon*3], 3,
           ts->tm_mday, 1900+ts->tm_year, 0);
      assert(wbuf.len == 11+1);
      tk->s = alloccopy(lx->tmparena, buf, wbuf.len, 1);
   } else {
      tk->s = "\?\?\? \?\? \?\?\?\?";
   }
}

static void
mac__time__(Lexer *lx, Token *tk)
{
   char buf[20];
   WriteBuf wbuf = MEMBUF(buf, sizeof buf);
   time_t tm = time(NULL);
   struct tm *ts = localtime(&tm);
   tk->t = TKSTRLIT;
   tk->wide = 0;
   tk->len = 8;
   if (ts) {
      bfmt(&wbuf, "%.2d:%.2d:%.2d%c", ts->tm_hour, ts->tm_min, ts->tm_sec, 0);
      tk->s = alloccopy(lx->tmparena, buf, wbuf.len, 1);
      assert(wbuf.len == 8+1);
   } else {
      tk->s = "\?\?:\?\?:\?\?";
   }
}

static void
mac__counter__(Lexer *lx, Token *tk)
{
   char buf[20];
   WriteBuf wbuf = MEMBUF(buf, sizeof buf);
   static int counter;
   bfmt(&wbuf, "%d", counter++), buf[wbuf.len++] = 0;
   tk->t = TKNUMLIT;
   tk->s = alloccopy(lx->tmparena, buf, wbuf.len, 1);
   tk->len = wbuf.len-1;
}

static void
mac__has_builtin(Lexer *lx, Token *tk, const Token *args, int narg)
{
   extern bool hasbuiltin(const char *, uint n);
   bool has = 0;
   tk->t = TKNUMLIT, tk->len = 1;
   if (narg >= 1) {
      if (args[0].t == TKIDENT)
         has = hasbuiltin(args[0].s, args[0].len);
      else if (in_range(args[0].t, TKWBEGIN_, TKWEND_))
         has = args[0].len >= sizeof "__builtin_" && !memcmp(args[0].s, "__builtin_", 10);
      else goto Bad;
      if (narg != 1)
         error(&args[1].span, "expected `)' after '%tk'", &args[0]);
   } else Bad: {
      error(narg ? &args[0].span : &tk->span, "'__has_builtin' requires an identifier");
   }
   tk->s = &"01"[has];
}

static void
putdefs1(const char *s)
{
   if (s) for (; *s; s += strlen(s) + 1) putdef1(s);
}

static void
addpredefmacros(Arena **tmparena)
{
   static Token tok_stdc = {TKNUMLIT},
                       tok_major = {TKNUMLIT, .s = XSTR(ANTCC_VERSION_MAJOR),
                                              .len = sizeof XSTR(ANTCC_VERSION_MAJOR) - 1},
                       tok_minor = {TKNUMLIT, .s = XSTR(ANTCC_VERSION_MINOR),
                                              .len = sizeof XSTR(ANTCC_VERSION_MINOR) - 1},
                       tok_patch = {TKNUMLIT, .s = XSTR(ANTCC_VERSION_PATCH),
                                              .len = sizeof XSTR(ANTCC_VERSION_PATCH) - 1};
   static struct { const char *name; Macro m; } macs[] = {
      { "__FILE__", { .predef = 1, .special = 1, .handler = mac__file__ }},
      { "__LINE__", { .predef = 1, .special = 1, .handler = mac__line__ }},
      { "__DATE__", { .predef = 1, .special = 1, .handler = mac__date__ }},
      { "__TIME__", { .predef = 1, .special = 1, .handler = mac__time__ }},
      { "__COUNTER__", { .predef = 1, .special = 1, .handler = mac__counter__ }},
      { "__has_builtin", { .predef = 1, .nparam = 1, .fnlike = 1, .special = 1, .handlerfn = mac__has_builtin }},
      { "__STDC_VERSION__", { .predef = 1, .single = &tok_stdc }},
      { "__antcc_major__", { .predef = 1, .single = &tok_major }},
      { "__antcc_minor__", { .predef = 1, .single = &tok_minor }},
      { "__antcc_patch__", { .predef = 1, .single = &tok_patch }},
      { "__extension__", { .predef = 1, .single = NULL }},
   };
   static const char
      cpredefs[] =
         "__antcc__\0__STDC__\0__STDC_NO_ATOMICS__\0__STDC_NO_COMPLEX__\0__STDC_NO_THREADS__\0__STDC_NO_VLA__\0",
      cstdver[][8] = {
         [STDC89] = "199409L",
         [STDC99] = "199901L",
         [STDC11] = "201112L",
         [STDC23] = "202311L",
      };

   tok_stdc.s = cstdver[ccopt.cstd];
   tok_stdc.len = 7;
   if (ccopt.cstd >= STDC99)
      putdef1("__GNUC_STDC_INLINE__");

   for (int i = 0; i < countof(macs); ++i)
      putmac(intern(macs[i].name), &macs[i].m);
   putdefs1(cpredefs);
   if (target.os != OSunknown) putdef1("__STDC_HOSTED__");

   if (ppcmdline.len) {
      MemFile *f;
      Lexer lx[1] = {0};
      lx->fileid = getpredeffile(&f, "<command line>");
      assert(!f->p);
      lx->ndat = f->n = ppcmdline.len;
      ppcmdappendf("%S", "\0\0\0\0\0\0", 6);
      lx->dat = f->p = (uchar *)ppcmdline.buf;
      lx->tmparena = tmparena;
      lx->chrbuf0 = countof(lx->chrbuf);
      lx->firstdirective = 1;
      while (lex(lx, NULL) != TKEOF) ;
   }
}

enum initlexer
initlexer(Lexer *lx, const char **err, const char *file)
{
   enum { NARENA = 1<<12 };
   static union { char m[sizeof(Arena) + NARENA]; Arena *_align; } amem;
   static Arena *tmparena = (void *)amem.m;
   static bool predefs;

   if (!tmparena->cap) tmparena->cap = NARENA;
   if (!mtoksbuf.p) vinit(&mtoksbuf, NULL, 1024);
   if (!mdyntoksbuf.p) vinit(&mdyntoksbuf, NULL, 256);
   if (!predefs) {
      addpredefmacros(&tmparena);
      predefs = 1;
   }

   MemFile *f;
   int fileid = openfile(err, &f, file);
   if (fileid < 0)
      return LXERR;
   internstr guard;
   if (isfileseen(fileid) && isoncefile(fileid, &guard) && (!guard || findmac(guard))) {
      //efmt("skipping %s .. guard %s\n", file, guard ? guard : "<none>");
      return LXFILESKIP;
   }
   memset(lx, 0, sizeof *lx);
   lx->fileid = fileid;
   markfileseen(fileid);

   lx->dat = f->p;
   lx->ndat = f->n;
   lx->tmparena = &tmparena;
   lx->chrbuf0 = countof(lx->chrbuf);
   lx->firstdirective = 1;
   lx->nppcnd0 = nppcnd;
   return getfilename(fileid, 0) != file ? LXFILESEEN : LXOK;
}

/* callback to let lexer release temp memory for arena allocated token data */
void
lexerfreetemps(Lexer *lx)
{
   if (!lx->macstk) {
      /* some of the tokens could be somewhere in the macro stack */
      freearena(lx->tmparena);
   }
}

void
lexerdump(Lexer *lx, WriteBuf *out)
{
   Token prev = {0}, tok;
   int file = lx->fileid, line = 1, col = 1;
   const char *lastfile = getfilename(file, 0);
   bfmt(out, "# %d %'s\n", 1, lastfile);
   while (lex(lx, &tok) != TKEOF) {
      int tkline, tkcol;
      const char *fname = getfilepos(&tkline, &tkcol, tok.span.ex.file, tok.span.ex.off);
      if (tok.span.ex.file != file || fname != lastfile) {
         file = tok.span.ex.file;
         bfmt(out, "\n# %d %'s\n", tkline, fname);
         col = 1;
         lexerfreetemps(lx);
         lastfile = fname;
      } else if (line < tkline && tkline - line < 5) {
         do
            ioputc(out, '\n');
         while (++line != tkline);
         col = 1;
      } else if (line != tkline) {
         bfmt(out, "\n# %d\n", tkline);
         line = tkline;
         col = 1;
         lexerfreetemps(lx);
      } else if (prev.t && (tok.space || tokpaste(lx, NULL, &prev, &tok))) {
         /* preserve whitespace & paste avoidance */
         ioputc(out, ' ');
         ++col;
      }
      if (col == 1)
         for (; col < tkcol; ++col)
            ioputc(out, ' ');
      line = tkline;
      bfmt(out, "%tk", &tok);
      col += tok.span.ex.len;
      prev = tok;
   }
   bfmt(out, "\n");
   ioflush(out);
}

/* vim:set ts=3 sw=3 expandtab: */
