#include "lex.h"
#include <string.h>

/* fill internal circular character buffer with input after translation phase 1 & 2
 * (trigraph substitution and backslash-newline deletion */
static void
fillchrbuf(struct lexer *lx)
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
next(struct lexer *lx)
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
peek(struct lexer *lx, int off)
{
   assert(off < countof(lx->chrbuf));
   if (lx->chrbuf0 + off >= countof(lx->chrbuf))
      fillchrbuf(lx);
   return lx->chrbuf[lx->chrbuf0 + off];
}

static bool
match(struct lexer *lx, uchar c)
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
parsenumlit(uvlong *outi, double *outf, const struct token *tk, bool ispp)
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
readstrchrlit(struct lexer *lx, struct token *tk, char delim, int wide)
{
   int c, i;
   uchar tmp[200];
   vec_of(uchar) b = VINIT(tmp, sizeof tmp);
   struct span span = {0};
   uint n, beginoff, idx;
   beginoff = idx = lx->chridx;

   while ((c = next(lx)) != delim) {
      static uint wmax[] = {0xFF, 0xFFFF, 0xFFFFFFFFu};
      if (c == '\n' || c == TKEOF) {
      Noterm:
         span.sl = (struct span0) { idx, lx->chridx - idx, lx->fileid };
         error(&span, "missing terminating %c character", delim);
         break;
      } else if (c == '\\') {
         span.sl = (struct span0) { idx, lx->chridx - idx, lx->fileid };
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
         span.sl = (struct span0) { idx, lx->chridx - idx, lx->fileid };
         error(&span, "empty character literal");
      } else if (b.n > targ_primsizes[TYINT]) {
         span.sl = (struct span0) { idx, lx->chridx - idx, lx->fileid };
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
readheadername(struct lexer *lx, struct token *tk, char delim)
{
   int c;
   uchar tmp[200];
   vec_of(uchar) b = VINIT(tmp, sizeof tmp);
   struct span span = {0};
   uint beginoff, idx;
   beginoff = idx = lx->chridx;

   while ((c = next(lx)) != delim) {
      if (c == '\n' || lx->eof) {
         span.sl = (struct span0) { idx, lx->chridx - idx, lx->fileid };
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

/* special mode to parse header path for #include */
static bool lexingheadername = 0;

enum { MAXLITLEN = 256 }; /* maximum length of num literals and identifiers */
static int
lex0(struct lexer *lx, struct token *tk)
{
   int idx,q;
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
               }
            } while (++lx->chrbuf0 < countof(lx->chrbuf));
            fillchrbuf(lx);
            lx->chridx = lx->chridxbuf[lx->chrbuf0];
            if ((lx->eof = (lx->chridx >= lx->ndat))) {
               struct span span = {{ idx, lx->chridx - idx, lx->fileid }};
               fatal(&span, "unterminated comment");
            }
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
                  goto Begin;
               }
            } while (++lx->chrbuf0+1 < countof(lx->chrbuf));
            fillchrbuf(lx);
            lx->chridx = lx->chridxbuf[lx->chrbuf0];
            if ((lx->eof = (lx->chridx >= lx->ndat))) {
               struct span span = {{ idx, lx->chridx - idx, lx->fileid }};
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
      if (lexingheadername) {
         readheadername(lx, tk, '>');
         lexingheadername = 0;
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
      if (lexingheadername) {
         readheadername(lx, tk, '"');
         lexingheadername = 0;
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
               fatal(&(struct span) {{ idx, lx->chridx - idx, lx->fileid }},
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
   fatal(&(struct span) {{ idx, lx->chridx - idx, lx->fileid }},
         "unexpected character %'c at %d (%d)", c, idx, lx->idx);
End:
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

struct macro {
   internstr *param;
   struct span0 span;
   uchar nparam;
   bool predef : 1,
        special : 1,
        fnlike : 1,
        variadic : 1;
   short id;
   union {
      void (*handler)(struct lexer *, struct token *);
      struct rlist {
        const struct token *tk;
        int n;
      } rlist;
      void (*handlerfn)(struct lexer *, struct token *ret, struct rlist arg);
   };
};

static bool
tokequ(const struct token *a, const struct token *b)
{
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

static bool /* whitespace separating tokens? */
wsseparated(const struct token *l, const struct token *r)
{
   if (l->span.sl.file != r->span.sl.file) return 1;
   return l->span.sl.off + l->span.sl.len != r->span.sl.off;
}

static bool
macroequ(const struct macro *a, const struct macro *b)
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
   if (a->rlist.n != b->rlist.n) return 0;
   for (int i = 0; i < a->rlist.n; ++i) {
      const struct token *tka = a->rlist.tk, *tkb = b->rlist.tk;
      if (!tokequ(&tka[i], &tkb[i]))
         return 0;
      if (i > 0 && wsseparated(&tka[i-1], &tka[i]) != wsseparated(&tkb[i-1], &tkb[i]))
         return 0;
   }
   return 1;
}

static void
freemac(struct macro *mac)
{
   if (mac->special) return;
   free(mac->param);
   free((void *)mac->rlist.tk);
}

static pmap_of(struct macro) macroht;

static void
putmac(internstr name, struct macro *mac)
{
   static short id;
   if (!macroht.v) pmap_init(&macroht, 1<<10);
   struct macro *slot = pmap_get(&macroht, name);
   mac->id = id++;
   if (slot) {
      if (!macroequ(slot, mac)) {
         if (slot->predef)
            warn(&(struct span){mac->span}, "redefining builtin macro");
         else {
            warn(&(struct span){mac->span}, "redefining macro");
            note(&(struct span){slot->span}, "previous definition:");
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
   struct macro *slot = pmap_get(&macroht, name);
   if (!slot) return;
   freemac(slot);
   pmap_del(&macroht, name);
}

static struct macro *
findmac(internstr name)
{
   return pmap_get(&macroht, name);
}

static void popmac(struct lexer *);

static void
ppskipline(struct lexer *lx)
{
   while (lx->macstk) popmac(lx);
   for (int c; (c = peek(lx, 0)) != '\n' && !lx->eof; next(lx)) {
      if (c == '/' && peek(lx, 1) == '*') { /* comment */
         next(lx), next(lx);
         bool done = 0;
         while (!((c = peek(lx, 0)) == '*' && peek(lx, 1) == '/')) {
            if (lx->eof) {
               struct span span = {{ lx->idx, lx->chridx - lx->idx, lx->fileid }};
               fatal(&span, "unterminated comment");
            }
            done = c == '\n';
            next(lx);
         }
         next(lx);
         if (done) return;
      }
   }
}

static bool
tokpaste(struct lexer *lx, struct token *dst, const struct token *l, const struct token *r)
{
   memset(dst, 0, sizeof *dst);
   dst->span = l->span;
   if (dst->span.ex.file == r->span.ex.file && dst->span.ex.off < r->span.ex.off)
      joinspan(&dst->span.ex, r->span.ex);
   if (l->t == TKIDENT && (r->t == TKIDENT || r->t == TKNUMLIT)) {
      /* foo ## bar  ;   foo ## 123 */
      dst->t = TKIDENT;
   } else if (l->t == TKNUMLIT && (r->t == TKIDENT || r->t == TKNUMLIT)) {
      /* 0x ## abc   ;   213 ## 456 */
      dst->t = TKNUMLIT;
   } else if (l->t && !r->t) {
      *dst = *l;
      return 1;
   } else if (!l->t && r->t) {
      *dst = *r;
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
      struct span span = l->span;

      for (int i = 0; i < countof(tab); ++i)
         if (tab[i].s[0] == l->t && tab[i].s[1] == r->t)
            return dst->t = tab[i].t, 1;

      joinspan(&span.ex, r->span.ex);
      error(&span, "pasting %'tk and %'tk does not form a valid preprocessing token", l, r);
      return 0;
   }

   char buf[200];
   dst->len = l->len + r->len;
   char *s = (dst->t == TKIDENT && dst->len + 1 < sizeof buf) ? buf : alloc(lx->tmparena, dst->len + 1, 1);
   memcpy(s, l->s, l->len);
   memcpy(s + l->len, r->s, r->len);
   s[dst->len] = 0;
   if (dst->t == TKIDENT) {
      dst->blue = 0;
      dst->name = intern(s);
   } else {
      dst->s = s;
   }
   return 1;
}

static void
ppdefine(struct lexer *lx)
{
   struct token tk0, tk;
   internstr mname;
   struct macro mac = {0};
   vec_of(struct token) rlist = {0};

   lex0(lx, &tk0);
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
      while (lex0(lx, &tk) != ')') {
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
            lex0(lx, &tk);
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
   while (lex0(lx, &tk) != '\n' && tk.t != TKEOF) {
      if (!rlist.n && !wsseparated(&tk0, &tk))
         warn(&tk.span, "no whitespace after macro name");
      if (mac.fnlike && tk.t == TKIDENT) {
         for (int i = 0; i < mac.nparam; ++i) {
            if (tk.name == mac.param[i]) {
               tk.argidx = i;
               if (rlist.n > 0 && rlist.p[rlist.n - 1].t == '#') {
                  tk.t = TKPPMACSTR;
                  rlist.p[rlist.n - 1] = tk;
                  goto Next;
               } else {
                  tk.t = TKPPMACARG;
                  break;
               }
            }
         }
      }
      if (rlist.n > 1 && rlist.p[rlist.n-1].t == TKPPCAT) {
         struct token new;
         if (rlist.p[rlist.n-2].t != TKPPMACARG && tk.t != TKPPMACARG
          && tokpaste(lx, &new, &rlist.p[rlist.n-2], &tk))
         {
            /* trivial concatenations */
            rlist.p[rlist.n-2] = new;
            --rlist.n;
            continue;
         }
      }
      if (in_range(tk.t, TKNUMLIT, TKSTRLIT) && !tk.litlit)
         tk.s = alloccopy(&globarena, tk.s, tk.len << tk.wide, 1);
      vpush(&rlist, tk);
   Next:;
   }
   mac.rlist.tk = rlist.p;
   mac.rlist.n = rlist.n;
   putmac(mname, &mac);
}

static void
expecteol(struct lexer *lx, const char *ppname)
{
   struct token tk;
   assert(!lx->macstk);
   if (lex0(lx, &tk) != '\n' && tk.t != TKEOF) {
      (ccopt.pedant ? error : warn)(&tk.span, "extra tokens after #%s", ppname);
      ppskipline(lx);
   }
}
static void
ppundef(struct lexer *lx)
{
   struct token tk;

   lex0(lx, &tk);
   if (tk.t != TKIDENT) {
      error(&tk.span, "macro name missing");
      ppskipline(lx);
      return;
   }
   expecteol(lx, "undef");
   delmac(tk.name);
}

static struct macrostack {
   struct rlist rlist;
   struct span0 exspan;
   int idx;
   short macid;
   bool stop;
   bool dofree;
} mstk[64];

static void
pushmacstk(struct lexer *lx, const struct span *span, const struct macrostack *m)
{
   struct macrostack *l = lx->macstk;
   if (!l) l = mstk;
   else if ((++l == mstk+countof(mstk))) fatal(span, "macro depth limit reached");
   *l = *m;
   l->idx = 0;
   l->exspan = span->ex;
   lx->macstk = l;
}

static void
popmac(struct lexer *lx)
{
   struct macrostack *stk;

   assert(stk = lx->macstk);
   do {
      if (stk->dofree)
         free((void *)stk->rlist.tk);
      if (lx->macstk == mstk) lx->macstk = NULL;
      else --lx->macstk;
   } while ((stk = lx->macstk) && stk->idx >= stk->rlist.n && !stk->stop);
}

static void expandfnmacro(struct lexer *lx, struct span *span, internstr mname, struct macro *mac);

static bool
tryexpand(struct lexer *lx, struct token *tk)
{
   struct span span = tk->span;
   struct macro *mac = NULL;
   internstr mname = tk->name;

   if (tk->t != TKIDENT || !(mac = findmac(mname)) || tk->blue)
      return 0;

   /* prevent infinite recursion */
   for (struct macrostack *l = lx->macstk; l && l+1 > mstk; --l) {
      if (l->macid == mac->id) {
         tk->blue = 1;
         return 0;
      }
   }

   if (mac->special && !mac->fnlike) {
      mac->handler(lx, tk);
      pushmacstk(lx, &span, &(struct macrostack){
         .rlist = { alloccopy(lx->tmparena, tk, sizeof *tk, 0), 1 },
         .macid = -1,
         .idx = 0,
      });
   } else if (mac->fnlike) {
      /* look if there is a '(' token ahead, expand if so */
      struct macrostack *s = lx->macstk;
      if (s && s->idx >= s->rlist.n && !s->stop) {
         popmac(lx);
         s = lx->macstk;
      }
      if (!s) {
         struct token tk;
         int t;
         while (aisspace(t = peek(lx, 0))) next(lx);
         if (t != '(') return 0;
         lex0(lx, &tk);
      } else {
         if (s->idx >= s->rlist.n || s->rlist.tk[s->idx].t != '(') return 0;
         ++s->idx;
      }
      expandfnmacro(lx, &span, mname, mac);
   } else if (mac->rlist.n) {
      pushmacstk(lx, &span, &(struct macrostack){
         .rlist = mac->rlist,
         .macid = mac->id,
         .idx = 0,
      });
   }
   return 1;
}

static void
expandfnmacro(struct lexer *lx, struct span *span, internstr mname, struct macro *mac)
{
   vec_of(struct token) argsbuf = {0}, /* argument tokens pre-expansion */
                        rlist2 = {0}; /* macro replacement list with arguments subsituted */
   struct argtks { int idx, n; } args[100]; /* index,n into argsbuf */
   struct span excessspan;
   int cur, len, i, bal, narg;
   struct token tk;
   bool toomany = 0;

   /* we push all arg tokens to buffer, each of args[i] is a slice (idx..idx+n) of the vector;
    * while we're building the list, args[i].tk points to &tk + idx, because rlist.p can move,
    * then we fix them up in the end to point to rlist.p + idx */

   cur = i = bal = len = narg = 0;
   for (struct macrostack *s = lx->macstk;;) {
      if (!s) do lex0(lx, &tk); while (tk.t == '\n');
      else {
         tk = s->idx < s->rlist.n ? s->rlist.tk[s->idx++] : (struct token){TKEOF};
      }
      if (((tk.t == ')' && bal == 0) || tk.t == TKEOF)) break;
      if (tk.t == ',' && bal == 0) {
         ++narg;
         if (i == mac->nparam-1 && !mac->variadic) {
            excessspan = tk.span;
            toomany = 1;
         } else if (i < mac->nparam - mac->variadic) {
            assert(i < countof(args) || "too many args in fn-like macro");
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
         if (tk.t == '(' || tk.t == '[') ++bal;
         else if (tk.t == ')' || tk.t == ']') --bal;
         vpush(&argsbuf, tk);
         ++len;
      }
   }

   if (tk.t == TKEOF) {
      joinspan(&span->ex, tk.span.ex);
      fatal(span, "unterminated function-like macro invocation");
   } else if (i < mac->nparam) {
      ++narg;
      args[i].idx = cur;
      args[i].n = len;
      cur = argsbuf.n;
      len = 0;
      ++i;
   }
   joinspan(&span->ex, tk.span.ex);
   if (narg < mac->nparam) {
      warn(span, "macro `%s' passed %d arguments, but takes %d", mname, narg, mac->nparam);
      for (int i = narg; i < mac->nparam; ++i)
         args[i].n = 0;
   } else if (toomany) {
      joinspan(&excessspan.ex, tk.span.ex);
      warn(&excessspan, "macro `%s' passed %d arguments, but takes just %d", mname, narg, mac->nparam);
   }
   if (mac->special) {
      mac->handlerfn(lx, &tk, (struct rlist){argsbuf.p, argsbuf.n});
      pushmacstk(lx, span, &(struct macrostack){
         .rlist = { alloccopy(lx->tmparena, &tk, sizeof tk, 0), 1 },
         .macid = mac->id,
      });
   } else if (mac->nparam) { /* make new rlist with args replaced */
      bool rhsargpaste = 0;
      for (int i = 0; i < mac->rlist.n; ++i) {
         struct argtks *arg;
         tk = mac->rlist.tk[i];
         if (tk.t == TKPPCAT) {
            if (i > 0 && i < mac->rlist.n-1) {
               const struct token *lhs = &mac->rlist.tk[i-1],
                                  *rhs = &mac->rlist.tk[i+1];
               struct token new;
               if (lhs->t != TKPPMACARG && rhs->t != TKPPMACARG) {
                  /* trivial case should have been handled when defining */
                  assert(0 && "## ?");
               } else if (rhs->t != TKPPMACARG) {
                  if (rlist2.n == 0) {
                     vpush(&rlist2, *rhs);
                     ++i;
                  } else {
                     lhs = &rlist2.p[rlist2.n-1];
                     if (tokpaste(lx, &new, lhs, rhs)) {
                        rlist2.p[rlist2.n-1] = new;
                        ++i;
                     }
                  }
                  continue;
               } else {
                  rhsargpaste = 1;
                  continue;
               }
            }
         }
         if (tk.t != TKPPMACARG && tk.t != TKPPMACSTR) {
            vpush(&rlist2, tk);
            continue;
         }

         arg = &args[tk.argidx];
         if (tk.t == TKPPMACARG) {
            struct macrostack *l;
            if (arg->n == 0) {
               rhsargpaste = 0;
               continue;
            }
            pushmacstk(lx, &tk.span, &(struct macrostack) {
               .rlist = {argsbuf.p + arg->idx, arg->n},
               .macid = -1,
               .idx = 0,
               .stop = 1,
            });
            l = lx->macstk;
            if (rhsargpaste && rlist2.n > 0) {
               struct token new;
               rhsargpaste = 0;
               if (tokpaste(lx, &new, &rlist2.p[rlist2.n-1], &l->rlist.tk[0])) {
                  l->idx = 1;
                  rlist2.p[rlist2.n-1] = new;
               }
            }
            while (lex(lx, &tk) != TKEOF)
               vpush(&rlist2, tk);
            assert(lx->macstk == l);
            popmac(lx);
         } else { /* PPMACSTR */
            char tmp[200];
            struct wbuf buf = MEMBUF(tmp, sizeof tmp);
            int n = 0;

            // XXX this is wrong bc the string literal produced should be re-parsed later
            // i.e. stringifying the token sequence '\n' should ultimately produce a
            // string with an actual newline, not {'\\','n'}
         Redo:
            for (int i = 0; i < arg->n; ++i) {
               struct token *tk = &argsbuf.p[arg->idx + i];
               if (i > 0 && wsseparated(tk-1, tk))
                  n += bfmt(&buf, " ");
               n += bfmt(&buf, "%tk", tk);
            }
            ioputc(&buf, 0);
            if (buf.err) {
               struct wbuf new = MEMBUF(alloc(lx->tmparena, n+1, 1), n+1);
               assert(buf.buf == tmp);
               memcpy(&buf, &new, sizeof buf);
               goto Redo;
            }
            tk.t = TKSTRLIT;
            tk.wide = 0;
            tk.s = buf.buf != tmp ? buf.buf : alloccopy(lx->tmparena, buf.buf, buf.len, 1);
            tk.len = buf.len-1;
            vpush(&rlist2, tk);
         }
      }

      if (rlist2.n) {
         pushmacstk(lx, span, &(struct macrostack){
            .rlist = { rlist2.p, rlist2.n },
            .macid = mac->id,
            .dofree = 1,
         });
      }
   } else if (mac->rlist.n) {
      pushmacstk(lx, span, &(struct macrostack){
         .rlist = mac->rlist,
         .macid = mac->id,
      });
   }
   vfree(&argsbuf);
}

static bool
advancemacro(struct lexer *lx, struct token *tk)
{
   struct rlist rl;
   assert(lx->macstk);
   rl = lx->macstk->rlist;
   if (lx->macstk->idx >= rl.n) {
      if (lx->macstk->stop) {
         tk->t = TKEOF;
         return 1;
      }
      popmac(lx);
      return 0;
   }
   *tk = rl.tk[lx->macstk->idx++];
   assert(tk->t && tk->t != TKEOF);
   tk->span.ex = lx->macstk->exspan;
   if (tryexpand(lx, tk))
      return 0;
   return 1;
}

static struct token epeektk;
static int
elex(struct lexer *lx, struct token *tk)
{
   assert(tk);
   if (epeektk.t) {
      int tt = epeektk.t;
      if (tk) *tk = epeektk;
      epeektk.t = 0;
      return tt;
   }
   if (lx->macstk) {
      if (!advancemacro(lx, tk))
         return elex(lx, tk);
      return tk->t;
   }

   lex0(lx, tk);
   return tk->t;
}

static int
epeek(struct lexer *lx, struct token *tk)
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
expr(struct lexer *lx, bool *pu, int prec, bool ignore)
{
   struct token tk;
   enum typetag ty;
   char unops[16];
   int nunop = 0;
   vlong x, y;
   bool xu = 0, yu; /* x unsigned?; y unsigned? */

Unary:
   switch (elex(lx, &tk)) {
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
            lex0(lx, &tk);
            if ((paren = tk.t == '(')) lex0(lx, &tk);
            if (tk.t != TKIDENT && !in_range(tk.t, TKWBEGIN_, TKWEND_)) {
               error(&tk.span, "expected macro name");
               goto Err;
            }
            if (paren && lex0(lx, &tk) != ')') {
               error(&tk.span, "expected `)'");
               goto Err;
            }
            x = findmac(tk.name) != NULL;
         } else {
            if (tryexpand(lx, &tk))
               goto Unary;
            /* non defined pp name -> 0 */
            x = 0;
         }
         break;
      }
      error(&tk.span, "expected preprocessor integer expression");
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
         struct span span = tk.span;
         vlong m = expr(lx, &xu, 1, ignore || !x);
         if (elex(lx, &tk) != ':') {
            error(&tk.span, "expected ':'");
            note(&span, "to match conditional expression here");
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
static struct ppcnd {
   struct span0 ifspan;
   int filedepth;
   uchar cnd;
   bool elsep;
} ppcndstk[32];
static int nppcnd;

static int includedepth;

static void
ppif(struct lexer *lx, const struct span *span)
{
   vlong v = expr(lx, NULL, 0, 0);
   assert(nppcnd < countof(ppcndstk) && "too many nested #if");
   ppcndstk[nppcnd].ifspan = span->sl;
   ppcndstk[nppcnd].filedepth = includedepth;
   ppcndstk[nppcnd].cnd = v ? PPCNDTRUE : PPCNDFALSE;
   ppcndstk[nppcnd++].elsep = 0;
}

static void
ppifxdef(struct lexer *lx, bool defp, const struct span *span)
{
   struct token tk;

   lex0(lx, &tk);
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
ppelif(struct lexer *lx, const struct span *span)
{
   vlong v;
   struct ppcnd *cnd;

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
ppelifxdef(struct lexer *lx, bool defp, const struct span *span)
{
   struct token tk;
   struct ppcnd *cnd;

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
   lex0(lx, &tk);
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
ppendif(struct lexer *lx, const struct span *span)
{
   expecteol(lx, "endif");
   if (!nppcnd) {
      error(span, "#endif without matching #if");
      return;
   }
   --nppcnd;
}

static void
ppelse(struct lexer *lx, const struct span *span)
{
   struct ppcnd *cnd;
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
tryinclude(struct lexer *lx, const struct span *span, char *path)
{
   struct lexer new;
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
      memcpy(new.save, lx, sizeof *lx);
      *lx = new;

      if (++includedepth == MAXINCLUDE)
         fatal(span, "Maximum nested include depth of %d reached", includedepth);
      break;
   case LXFILESKIP:
      xbfree(path);
      break;
   }
   return 1;
}

static void
ppinclude(struct lexer *lx, const struct span *span0)
{
   struct token tk;
   struct span span = *span0;

   lexingheadername = 1;
   if (in_range(lex0(lx, &tk), TKPPHDRH, TKPPHDRQ)) {
      char *path = NULL;
      const char *base, *end;

      expecteol(lx, "include");
      joinspan(&span.ex, tk.span.ex);
      if (tk.t == TKPPHDRQ) {
         /* try raw path: absolute or relative to working dir */
         xbgrow(&path, tk.len + 1);
         memcpy(path, tk.s, tk.len);
         path[tk.len] = 0;
         if (tryinclude(lx, &span, path)) return;
         if (tk.s[0] == '/') goto NotFound;

         /* try relative to current file's directory */
         base = getfilename(lx->fileid, 0);
         for (end = base; *end != 0; ++end) {}
         for (--end; *end != '/' && end != base; --end) {}
         if (*end == '/') ++end;
         xbgrow(&path, end - base + tk.len + 1);
         memcpy(path, base, end - base);
         memcpy(path + (end - base), tk.s, tk.len);
         path[end - base + tk.len] = 0;
         if (tryinclude(lx, &span, path)) return;
      }
      /* try system paths */
      for (struct inclpaths *p = cinclpaths; p; p = p->next) {
         int ndir = strlen(p->path);
         xbgrow(&path, ndir + tk.len + 2);
         memcpy(path, p->path, ndir);
         path[ndir++] = '/';
         memcpy(path + ndir, tk.s, tk.len);
         path[ndir + tk.len] = 0;
         if (tryinclude(lx, &span, path)) return;
      }
      /* try embedded files pseudo-path */
      xbgrow(&path, tk.len + 3);
      path[0] = '@', path[1] = ':';
      memcpy(path+2, tk.s, tk.len);
      path[tk.len+2] = 0;
      if (tryinclude(lx, &span, path)) return;
   NotFound:
      error(&tk.span, "file not found: %'S", tk.s, tk.len);
   } else {
      error(&tk.span, "expected \"header\" or <header>");
      ppskipline(lx);
      return;
   }
}

static void
ppline(struct lexer *lx, struct token *tk0)
{
   struct token tk, tks[2];
   int ntk = 0;
   struct span span = tk0->span;
   bool ext = 0;
   if (tk0->t == TKNUMLIT) { /* handles GNU-style post preprocessing directive '# n ...' */
      tks[ntk++] = *tk0;
      ext = 1;
   }
   while (ntk < 2) {
      if (lx->macstk && advancemacro(lx, &tk)) {
         tks[ntk++] = tk;
         if (lx->macstk->idx >= lx->macstk->rlist.n) popmac(lx);
      } else if (!lx->macstk && (lex0(lx, &tk) == '\n' || tk.t == TKEOF)) {
         break;
      } else if (tk.t == TKIDENT && tryexpand(lx, &tk)) {
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
pppragma(struct lexer *lx, const struct span *span0)
{
   struct token tk;
   struct span span = *span0;
   if (lex0(lx, &tk) == TKIDENT && !strcmp(tk.s, "once")) {
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
ppdiag(struct lexer *lx, const struct span *span0, bool err)
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
findppcmd(const struct token *tk)
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
identkeyword(struct token *tk)
{
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-braces"
#endif
   static const struct {
      const char *s;
      struct kw { uchar t, cstd : 4, ext : 1; } kw;
      const char *alias[2];
   } kwtab[] = {
#define _(kw, cstd, ...) { #kw, {TKW##kw, cstd}, __VA_ARGS__ },
#include "keywords.def"
#undef _
   };
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
   static pmap_of(struct kw) kwmap;
   if (!kwmap.v) {
      pmap_init(&kwmap, 128);
      for (int i = 0; i < countof(kwtab); ++i) {
         struct kw kw = kwtab[i].kw;
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
   struct kw *kw = pmap_get(&kwmap, tk->name);
   if (kw) {
      tk->t = kw->t;
      tk->extwarn = kw->ext;
   }
}

int
lex(struct lexer *lx, struct token *tk_)
{
   struct token tkx[1], *tk;
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
      if (!advancemacro(lx, tk))
         goto Begin;
      if (tk->t == TKIDENT) identkeyword(tk);
      return tk->t;
   }
   bool linebegin = 1,
        skip = nppcnd ? ppcndstk[nppcnd-1].cnd != PPCNDTRUE : 0;
   enum directive lastcmd = 0;
   for (;;) {
      while ((t = lex0(lx, tk)) == '\n') linebegin = 1;
      if (t == '#' && linebegin) {
         if (lex0(lx, tk) == '\n') { }
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
               case PPINCLUDE:  ppinclude(lx, &tk->span); break;
               case PPLINE:     ppline(lx, tk); break;
               case PPPRAGMA:   pppragma(lx, &tk->span); break;
               case PPWARNING:  ppdiag(lx, &tk->span, 0); break;
               case PPERROR:    ppdiag(lx, &tk->span, 1); break;
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
         if (tryexpand(lx, tk))
            goto Begin;
         if (t == TKEOF && nppcnd && ppcndstk[nppcnd-1].filedepth == includedepth) {
            struct span span = { ppcndstk[nppcnd-1].ifspan };
            error(&span, "#if is not matched by #endif");
         }
         if (t == TKEOF && lx->save) {
            /* end of #include'd file, restore previous state */
            if (lastcmd == PPENDIF && lx->inclguard) {
               markfileonce(lx->fileid, lx->inclguard);
            }
            struct lexer *sv = lx->save;
            if (sv->inclnerror != nerror) {
               int line;
               const char *f = getfilepos(&line, NULL, sv->fileid, sv->chridx-2);
               note(NULL, "in file included from %s:%d", f, line);
            }
            memcpy(lx, sv, sizeof *lx);
            free(sv);
            --includedepth;
            linebegin = 1;
            lx->firstdirective = 0;
         } else {
            if (nppcnd == lx->nppcnd0) lx->inclguard = NULL;
            if (t == TKIDENT) identkeyword(tk);
            return tk->t;
         }
      }
   }
   assert(0);
}

int
lexpeek(struct lexer *lx, struct token *tk_)
{
   struct token tkx[1], *tk;
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

static vec_of(uchar) ppcmdline;

void
cpppredef(bool undef, const char *cmd)
{
   const char *sep = strchr(cmd, '='), *body = sep ? sep+1 : "1";
   uint namelen = sep ? sep - cmd : strlen(cmd);
   char line[1024];
   struct wbuf wbuf = MEMBUF(line, sizeof line);
   if (!ppcmdline.p) vinit(&ppcmdline, NULL, 1<<10);
   int n = bfmt(&wbuf, "%s %S %s\n", undef ? "#undef" : "#define", cmd, namelen, body);
   assert(n <= sizeof line);
   vpushn(&ppcmdline, line, n);
}

static void
mac__file__handler(struct lexer *lx, struct token *tk)
{
   tk->t = TKSTRLIT;
   tk->s = getfilename(lx->fileid, lx->chridx);
   tk->wide = 0;
   tk->len = strlen(tk->s);
}

static void
mac__line__handler(struct lexer *lx, struct token *tk)
{
   char buf[40];
   int line;
   struct wbuf wbuf = MEMBUF(buf, sizeof buf);
   getfilepos(&line, NULL, lx->fileid, lx->chridx);
   bfmt(&wbuf, "%d", line), buf[wbuf.len++] = 0;
   tk->t = TKNUMLIT;
   tk->s = alloccopy(lx->tmparena, buf, wbuf.len, 1);
   tk->len = strlen(tk->s);
}

#include <time.h>

static void
mac__date__handler(struct lexer *lx, struct token *tk)
{
   char buf[20];
   struct wbuf wbuf = MEMBUF(buf, sizeof buf);
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
mac__time__handler(struct lexer *lx, struct token *tk)
{
   char buf[20];
   struct wbuf wbuf = MEMBUF(buf, sizeof buf);
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
mac__has_builtin(struct lexer *lx, struct token *tk, struct rlist arg)
{
   extern bool hasbuiltin(const char *, uint n);
   bool has = 0;
   tk->t = TKNUMLIT, tk->len = 1;
   if (arg.n == 1) {
      if (arg.tk->t == TKIDENT)
         has = hasbuiltin(arg.tk->s, arg.tk->len);
      else if (in_range(arg.tk->t, TKWBEGIN_, TKWEND_))
         has = arg.tk->len >= sizeof "__builtin_" && !memcmp(arg.tk->s, "__builtin_", 10);
   }
   tk->s = &"01"[has];
}

static const struct token tok_1 = { TKNUMLIT, .s = "1", .len = 1, .litlit = 1 };
static void
putdef1(const char *name)
{
   putmac(intern(name), &(struct macro) {
      .predef = 1,
      .rlist = { &tok_1, 1},
   });
}

static void
putdefs1(const char *s)
{
   for (; *s; s += strlen(s) + 1) putdef1(s);
}

static void
addpredefmacros(struct arena **tmparena)
{
   static struct token tok_ver = { TKNUMLIT };
   static struct { const char *name; struct macro m; } macs[] = {
      { "__FILE__", { .predef = 1, .special = 1, .handler = mac__file__handler }},
      { "__LINE__", { .predef = 1, .special = 1, .handler = mac__line__handler }},
      { "__DATE__", { .predef = 1, .special = 1, .handler = mac__date__handler }},
      { "__TIME__", { .predef = 1, .special = 1, .handler = mac__time__handler }},
      { "__has_builtin", { .predef = 1, .nparam = 1, .fnlike = 1, .special = 1, .handlerfn = mac__has_builtin }},
      { "__STDC_VERSION__", { .predef = 1, .rlist = { &tok_ver, 1 } }},
   };
   static const char
      cpredefs[] =
         "__antcc__\0__STDC__\0__STDC_NO_ATOMICS__\0__STDC_NO_COMPLEX__\0__STDC_NO_THREADS__\0__STDC_NO_VLA__\0",
      *ospredefs[] = {
         [OSlinux] = "__linux\0__linux__\0linux\0unix\0__unix\0__unix__\0"
      }, *archpredefs[] = {
         [ISx86_64] = "__x86_64__\0__x86_64\0",
         [ISaarch64] = "__aarch64__\0__aarch64\0",
      }, cstdver[][8] = {
         [STDC89] = "199409L",
         [STDC99] = "199901L",
         [STDC11] = "201112L",
         [STDC23] = "202311L",
      };

   tok_ver.s = cstdver[ccopt.cstd];
   tok_ver.len = 7;
   for (int i = 0; i < countof(macs); ++i)
      putmac(intern(macs[i].name), &macs[i].m);
   putdefs1(cpredefs);
   if (target.os != OSunknown) putdef1("__STDC_HOSTED__");
   putdefs1(ospredefs[target.os]);
   putdefs1(archpredefs[target.arch]);

   if (ppcmdline.n) {
      struct memfile *f;
      struct lexer lx[1] = {0};
      lx->fileid = getpredeffile(&f, "<command line>");
      assert(!f->p);
      lx->ndat = f->n = ppcmdline.n;
      vpushn(&ppcmdline, "\0\0\0\0\0\0", 6);
      lx->dat = f->p = ppcmdline.p;
      lx->tmparena = tmparena;
      lx->chrbuf0 = countof(lx->chrbuf);
      lx->firstdirective = 1;
      while (lex(lx, NULL) != TKEOF) ;
   }
}

enum initlexer
initlexer(struct lexer *lx, const char **err, const char *file)
{
   enum { NARENA = 1<<12 };
   static union { char m[sizeof(struct arena) + NARENA]; struct arena *_align; } amem;
   static struct arena *tmparena = (void *)amem.m;

   if (!tmparena->cap) tmparena->cap = NARENA;
   if (!macroht.v) addpredefmacros(&tmparena);

   struct memfile *f;
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
lexerfreetemps(struct lexer *lx)
{
   if (!lx->macstk) {
      /* some of the tokens could be somewhere in the macro stack */
      freearena(lx->tmparena);
   }
}

void
lexerdump(struct lexer *lx, struct wbuf *out)
{
   struct token prev = {0}, tok;
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
      } else if (prev.t && wsseparated(&prev, &tok)) {
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
