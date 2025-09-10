#include "lex.h"
#include <string.h>

const char *
intern(const char *s)
{
   static vec_of(char) mem;
   static uint ht[1<<10];
   uint h, i, n = arraylength(ht);

   if (!mem.p) vinit(&mem, NULL, 1<<10);
   
   i = h = hashs(0, s);
   for (;; ++i) {
      i &= arraylength(ht) - 1;
      if (!ht[i]) {
         ht[i] = mem.n+1;
         return vpushn(&mem, s, strlen(s)+1);
      } else if (!strcmp(s, &mem.p[ht[i]-1])) {
         return &mem.p[ht[i]-1];
      }
      assert(--n > 0 && "intern full");
   }   
}

static bool
identkeyword(struct token *tk, const char *s, int len)
{
   static const struct { const char *s; enum toktag t; enum cstd cstd; } kwtab[] = {
#define _(kw, cstd) { #kw, TKW##kw, cstd },
#include "keywords.def"
#undef _
   };
   int l = 0, h = arraylength(kwtab) - 1, i, cmp;

   if (len > TKWMAXLEN_) goto ident;
   /* binary search over sorted array */
   while (l <= h) {
      i = (l + h) / 2;
      cmp = strcmp(kwtab[i].s, s);
      if (cmp < 0) l = i + 1;
      else if (cmp > 0) h = i - 1;
      else if (kwtab[i].cstd <= ccopt.cstd || kwtab[i].s[0] == '_') {
         /* allow future keywords but only if they begin with _ */
         tk->t = kwtab[i].t;
         tk->s = kwtab[i].s;
         return kwtab[i].cstd <= ccopt.cstd;
      } else break;
   }
ident:
   tk->t = TKIDENT;
   tk->s = intern(s);
   return 1;
}


/* fill internal circular character buffer with input after translation phase 1 & 2
 * (trigraph substitution and backslash-newline deletion */
static void
fillchrbuf(struct lexer *lx)
{
   bool trigraph = ccopt.trigraph;
   const uchar *p = lx->dat + lx->idx;
   int i = lx->chrbuf0, idx = lx->idx, c;

   while (lx->nchrbuf < arraylength(lx->chrbuf)) {
      int n;
      while (!memcmp(p, "\\\n", n = 2) || (trigraph && !memcmp(p, "\?\?/\n", n = 4))) {
         idx += n;
         p += n;
         addfileline(lx->fileid, idx);
      }
      if (idx >= lx->ndat)
         c = TKEOF;
      else if (trigraph && ((p[0] == '?') & (p[1] == '?'))) {
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
      lx->chrbuf[i % arraylength(lx->chrbuf)] = c;
      lx->chridxbuf[i % arraylength(lx->chrbuf)] = idx;
      ++lx->nchrbuf;
      ++i;
   }
   lx->idx = idx;
}

static int
next(struct lexer *lx)
{
   int c;

   if (lx->nchrbuf == 0)
      fillchrbuf(lx);
   lx->chridx = lx->chridxbuf[lx->chrbuf0];
   c = lx->chrbuf[lx->chrbuf0];
   lx->eof = c == TKEOF;
   lx->chrbuf0 = (lx->chrbuf0 + 1) % arraylength(lx->chrbuf);
   --lx->nchrbuf;
   return c;
}

static int
peek(struct lexer *lx, int off)
{
   assert(off < arraylength(lx->chrbuf));
   if (lx->nchrbuf < off+1)
      fillchrbuf(lx);
   return lx->chrbuf[(lx->chrbuf0 + off) % arraylength(lx->chrbuf)];
}

static bool
match(struct lexer *lx, int c)
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
   return (uint)c < sizeof(tab) ? tab[c] : 0;
}


enum typetag
parsenumlit(uvlong *outi, double *outf, const struct token *tk, bool ispp)
{
   if (tk->t == TKCHRLIT) {
      uvlong n = 0;
      for (int i = 0; i < tk->len; ++i)
         n = n << 8 | (uchar)tk->s[i];
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
            n = n * base + c - '0';
         } else {
            n *= base;
            if (in_range(c = *sx, '0', '9')) n += c - '0';
            else if (in_range(c|32, 'a', 'f')) n += 0xa + (c|32) - 'a';
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
readstrchrlit(struct lexer *lx, struct token *tk, char delim)
{
   int c, i;
   uchar tmp[80];
   vec_of(uchar) b = VINIT(tmp, sizeof tmp);
   struct span span = {0};
   uint n, beginoff, idx;
   beginoff = idx = lx->chridx;

   while ((c = next(lx)) != delim) {
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
            if (n > 0xFF) {
               span.sl.len = lx->chridx - span.sl.off;
               error(&span, "hex escape sequence out of range");
            }
            c = n & 0xFF;
            break;
         default:
            if (aisodigit(c)) { /* octal */
               n = c-'0';
               for (i = 2; i--;) {
                  if (!aisodigit(peek(lx, 0))) break;
                  n = n<<3 | ((c = next(lx))-'0');
               }
               if (n > 0377) {
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
      vpush(&b, c);
      idx = lx->chridx;;
   }
   if (delim == '"') {
      tk->t = TKSTRLIT;
      tk->len = b.n;
      if (lx->chridx - beginoff == tk->len + 1) {
         tk->litlit = 1;
         tk->s = (char *)&lx->dat[beginoff];
      } else {
         tk->litlit = 0;
         vpush(&b, 0);
         tk->s = alloc(lx->tmparena, b.n, 1);
         memcpy((char *)tk->s, b.p, b.n);
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
      if (lx->chridx - beginoff == tk->len + 1) {
         tk->litlit = 1;
         tk->s = (char *)&lx->dat[beginoff];
      } else {
         tk->litlit = 0;
         tk->s = alloc(lx->tmparena, tk->len, 1);
         memcpy((char *)tk->s, b.p, tk->len);
      }
   }
   vfree(&b);
}

/* for #include directive, read "header" or <header> */
static void
readheadername(struct lexer *lx, struct token *tk, char delim)
{
   int c;
   uchar tmp[80];
   vec_of(uchar) b = VINIT(tmp, sizeof tmp);
   struct span span = {0};
   uint beginoff, idx;
   beginoff = idx = lx->chridx;

   while ((c = next(lx)) != delim) {
      if (c == '\n' || c == TKEOF) {
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
      tk->s = alloc(lx->tmparena, b.n, 1);
      memcpy((char *)tk->s, b.p, b.n);
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

static int
lex0(struct lexer *lx, struct token *tk)
{
   int idx, c;

#define RET(t_) do { tk->t = (t_); goto End; } while (0)

Begin:
   idx = lx->chridx;
   switch (c = next(lx)) {
   case ' ': case '\r': case '\t':
      goto Begin;
      break;
   case '(': case ')': case ',': case ':':
   case ';': case '?': case '[': case ']':
   case '{': case '}': case '~': case '$':
   case '@': case '`': case '\\': case TKEOF: case '\n':
      RET(c);
   case '!':
      if (match(lx, '=')) RET(TKNEQ);
      RET(c);
   case '#':
      if (match(lx, '#')) RET(TKPPCAT);
      RET(c);
   case '+':
      if (match(lx, '+')) RET(TKINC);
      if (match(lx, '=')) RET(TKSETADD);
      RET(c);
   case '-':
      if (match(lx, '-')) RET(TKDEC);
      if (match(lx, '=')) RET(TKSETSUB);
      if (match(lx, '>')) RET(TKARROW);
      RET(c);
   case '*':
      if (match(lx, '=')) RET(TKSETMUL);
      RET(c);
   case '/':
      if (match(lx, '=')) RET(TKSETDIV);
      if (match(lx, '/')) {
         /* // comment */
         while (!lx->eof && !match(lx, '\n'))
            next(lx);
         goto Begin;
      }
      if (match(lx, '*')) {
         /* comment */
         while (peek(lx, 0) != '*' || peek(lx, 1) != '/') {
            if (next(lx) == TKEOF) {
               struct span span = {{ idx, lx->chridx - idx, lx->fileid }};
               fatal(&span, "unterminated multiline comment");
            }
         }
         next(lx), next(lx);
         goto Begin;
      }
      RET(c);
   case '%':
      if (match(lx, '=')) RET(TKSETREM);
      RET(c);
   case '^':
      if (match(lx, '=')) RET(TKSETXOR);
      RET(c);
   case '=':
      if (match(lx, '=')) RET(TKEQU);
      RET(c);
   case '<':
      if (lexingheadername) {
         readheadername(lx, tk, '>');
         lexingheadername = 0;
         goto End;
      }
      if (match(lx, '=')) RET(TKLTE);
      if (match(lx, '<')) RET(match(lx, '=') ? TKSETSHL : TKSHL);
      RET(c);
   case '>':
      if (match(lx, '=')) RET(TKGTE);
      if (match(lx, '>')) RET(match(lx, '=') ? TKSETSHR : TKSHR);
      RET(c);
   case '&':
      if (match(lx, '&')) RET(TKLOGAND);
      if (match(lx, '=')) RET(TKSETAND);
      RET(c);
   case '|':
      if (match(lx, '|')) RET(TKLOGIOR);
      if (match(lx, '=')) RET(TKSETIOR);
      RET(c);
   case '"':
      if (lexingheadername) {
         readheadername(lx, tk, '"');
         lexingheadername = 0;
      } else {
   case '\'':
         readstrchrlit(lx, tk, c);
      }
      goto End;
   case '.':
      if (peek(lx, 0) == '.' && peek(lx, 1) == '.') {
         next(lx), next(lx);
         RET(TKDOTS);
      } else if (aisdigit(peek(lx, 0))) {
         goto Numlit;
      }
      RET(c);
   default:
      if (aisdigit(c)) Numlit: {
         char tmp[70];
         int n = 0;
         tmp[n++] = c;
         while (isppnum(tmp[n-1], peek(lx, 0))) {
            assert(n < arraylength(tmp)-1 && "too big");
            tmp[n++] = next(lx);
         }
         tmp[n] = 0;
         tk->len = n;
         if (n == lx->chridx - idx) tk->s = (char *)&lx->dat[idx];
         else {
            tk->s = alloc(lx->tmparena, n, 1);
            memcpy((char *)tk->s, tmp, n);
         }
         RET(TKNUMLIT);
      } else if (c == '_' || aisalpha(c)) {
         char tmp[70];
         int n = 0;
         tmp[n++] = c;
         while (!aissep(c = peek(lx, 0))) {
            assert(n < arraylength(tmp)-1 && "too big");
            tmp[n++] = next(lx);
         }
         tmp[n] = 0;
         if (!identkeyword(tk, tmp, n) && ccopt.pedant)
            warn(&(struct span) {{ idx, lx->chridx - idx, lx->fileid }},
                  "%'tk in %M is an extension", tk);
         goto End;
      }
   }
   fatal(&(struct span) {{ idx, lx->chridx - idx, lx->fileid }},
         "unexpected character %'c at %d", c, idx);
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

#define isppident(tk) (in_range((tk).t, TKIDENT, TKWEND_))

static vec_of(struct macro) macros;
static ushort macroht[1<<10];

static struct macro *
findmac(const char *name)
{
   uint h, i, n = arraylength(macroht);
   
   i = h = ptrhash(name);
   for (; n--; ++i) {
      i &= arraylength(macroht) - 1;
      if (!macroht[i]) {
         return NULL;
      } else if (macros.p[macroht[i]-1].name == name) {
         return &macros.p[macroht[i]-1];
      }
   }
   return NULL;
}

static void
freemac(struct macro *mac)
{
   free(mac->param);
   free(mac->rlist.tk);
}

static bool
tokequ(const struct token *a, const struct token *b)
{
   if (a->t != b->t) return 0;
   if (a->t == TKNUMLIT || a->t == TKSTRLIT || a->t == TKCHRLIT) {
      if (a->len != b->len) return 0;
      return !memcmp(a->s, b->s, a->len);
   } else if (a->t == TKIDENT) {
      return a->s == b->s;
   }
   return 1;
}

static bool /* whitespace separating tokens? */
wsseparated(const struct token *l, const struct token *r)
{
   assert(l->span.sl.file == r->span.sl.file);
   return l->span.sl.off + l->span.sl.len < r->span.sl.off;
}

static bool
macroequ(const struct macro *a, const struct macro *b)
{
   int i;
   if (a->name != b->name) return 0;
   if (a->fnlike != b->fnlike || a->variadic != b->variadic) return 0;
   if (a->fnlike) {
      if (a->nparam != b->nparam) return 0;
      for (i = 0; i < a->nparam; ++i)
         if (a->param[i] != b->param[i])
            return 0;
   }
   if (a->rlist.n != b->rlist.n) return 0;
   for (i = 0; i < a->rlist.n; ++i) {
      struct token *tka = a->rlist.tk, *tkb = b->rlist.tk;
      if (!tokequ(&tka[i], &tkb[i]))
         return 0;
      if (i && wsseparated(&tka[i-1], &tka[i]) != wsseparated(&tkb[i-1], &tkb[i]))
         return 0;
   }
   return 1;
}

static struct macro *
putmac(struct macro *mac)
{
   uint h, i, n = arraylength(macroht);
   struct macro *slot;
   
   i = h = ptrhash(mac->name);
   for (;; ++i) {
      i &= arraylength(macroht) - 1;
      if (!macroht[i]) {
         macroht[i] = macros.n+1;
         vpush(&macros, *mac);
         return &macros.p[macros.n - 1];
      } else if ((slot = &macros.p[macroht[i]-1])->name == mac->name) {
         if (!macroequ(slot, mac)) {
            warn(&(struct span){mac->span}, "redefining macro");
            note(&(struct span){slot->span}, "previous definition:");
            freemac(slot);
            *slot = *mac;
         } else {
            freemac(mac);
         }
         return slot;
      }
      assert(--n && "macro limit");
   }
}

static void
ppskipline(struct lexer *lx)
{
   while (peek(lx, 0) != '\n' && peek(lx, 0) != TKEOF)
      next(lx);
}

static void
ppdefine(struct lexer *lx)
{
   struct token tk0, tk;
   struct macro mac = {0};
   vec_of(struct token) rlist = {0};
   vec_of(const char *) params = {0};

   lex0(lx, &tk0);
   if (!isppident(tk0)) {
      error(&tk0.span, "macro name missing");
      ppskipline(lx);
      return;
   }
   mac.name = tk0.s;
   mac.span = tk0.span.sl;
   
   if (match(lx, '(')) {
      mac.fnlike = 1;
      while (lex0(lx, &tk0) != ')') {
         if (params.n > 0) {
            if (tk0.t != ',')
               error(&tk0.span, "expected `,' or `)'");
            if (tk0.t == TKEOF) return;
            lex0(lx, &tk0);
         }
         if (isppident(tk0))
            vpush(&params, tk0.s);
         else {
            error(&tk0.span, "expected parameter name or `)'");
            if (tk0.t == TKEOF)
               return;
         }
      }
      mac.param = params.p;
      mac.nparam = params.n;
   }

   while (lex0(lx, &tk) != '\n' && tk.t != TKEOF) {
      if (!wsseparated(&tk0, &tk))
         warn(&tk.span, "no whitespace after macro name");
      if (mac.fnlike && isppident(tk)) for (int i = 0; i < mac.nparam; ++i) {
         if (tk.s == mac.param[i]) {
            tk.t = TKPPMACARG;
            tk.argidx = i;
            break;
         }
      }
      vpush(&rlist, tk);
   }
   mac.rlist.tk = rlist.p;
   mac.rlist.n = rlist.n;
   putmac(&mac);
}

static struct token epeektk;
static int
elex(struct lexer *lx, struct token *tk)
{
   if (epeektk.t) {
      int tt = epeektk.t;
      if (tk) *tk = epeektk;
      epeektk.t = 0;
      return tt;
   }
   return lex0(lx, tk);
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
   if ((uint)tt < arraylength(tab))
      return tab[tt] - 1;
   return -1;
}

static vlong
expr(struct lexer *lx, bool *pu, int prec)
{
   vlong x, y;
   struct token tk;
   enum typetag ty;
   int opprec;
   char unops[16];
   int nunop = 0;
   bool xu = 0, yu; /* x unsigned?; y unsigned? */

Unary:
   switch (elex(lx, &tk)) {
   case '-': case '~': case '!':
      unops[nunop++] = tk.t;
      if (nunop >= arraylength(unops)) {
         x = expr(lx, &xu, 999);
         break;
      }
      /* fallthru */
   case '+': goto Unary;
   case '(':
      x = expr(lx, &xu, 1);
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
      if (in_range(tk.t, TKWBEGIN_, TKWEND_)) {
      case TKIDENT:
         x = 0;
         xu = 0;
         break;
      }
      error(&tk.span, "expected preprocessor integer expression");
      goto Err;
   }

   while (nunop > 0)
      switch (unops[--nunop]) {
      case '-': x = -(uvlong)x; break;
      case '~': x = ~x; break;
      case '!': x = !x; break;
      default: assert(0);
      }

   while ((opprec = tkprec(epeek(lx, &tk))) >= prec) {
      elex(lx, &tk);
      if (tk.t != '?') {
         bool u;
         y = expr(lx, &yu, opprec + 1);
         u = xu | yu;
         switch ((int) tk.t) {
         case   '+': x += (uvlong) y; break;
         case   '-': x -= (uvlong) y; break;
         case   '*': x = u ? (uvlong) x * y : x * y; break;
         case   '&': x &= y; break;
         case   '^': x ^= y; break;
         case   '|': x |= y; break;
         case   '/': if (y) x = u ? (uvlong) x / y : x / y;
                     else goto Div0;
                     break;
         case   '%': if (y) x = u ? (uvlong) x % y : x % y;
                     else Div0: error(&tk.span, "division by zero");
                     break;
         case TKSHL: if ((uvlong)y < 64) x <<= y;
                     else goto BadShift;
                     break;
         case TKSHR: if ((uvlong)y < 64) x = u ? (uvlong) x >> y : x >> y;
                     else BadShift: error(&tk.span, "bad shift by %ld", y);
                     break;
         case   '<': x = u ? (uvlong) x < y  : x < y;  goto BoolRes;
         case   '>': x = u ? (uvlong) x > y  : x > y;  goto BoolRes;
         case TKLTE: x = u ? (uvlong) x <= y : x <= y; goto BoolRes;
         case TKGTE: x = u ? (uvlong) x >= y : x >= y; goto BoolRes;
         case TKEQU: x = x == y; goto BoolRes;
         case TKNEQ: x = x != y; goto BoolRes;
         case TKLOGAND: x = x && y; goto BoolRes;
         case TKLOGIOR: x = x || y; BoolRes: u = 0; break;
         default: assert(0);
         }
         xu = u;
      } else {
         struct span span = tk.span;
         vlong m = expr(lx, &xu, 1);
         if (elex(lx, &tk) != ':') {
            error(&tk.span, "expected ':'");
            note(&span, "to match conditional expression here");
            goto Err;
         }
         y = expr(lx, &yu, 1);
         x = x ? m : y;
         xu |= yu;
      }
   }
   if (!prec) /* not a sub expr */
      if (elex(lx, &tk) != '\n' && tk.t != TKEOF) {
         error(&tk.span, "garbage after preprocessor expression");
         ppskipline(lx);
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
   uchar cnd;
   bool elsep;
} ppcndstk[32];
static int nppcnd;

static void
ppif(struct lexer *lx, const struct span *span)
{
   vlong v = expr(lx, NULL, 0);
   assert(nppcnd < arraylength(ppcndstk) && "too many nested #if");
   ppcndstk[nppcnd].ifspan = span->sl;
   ppcndstk[nppcnd].cnd = v ? PPCNDTRUE : PPCNDFALSE;
   ppcndstk[nppcnd++].elsep = 0;
}

static void
ppifxdef(struct lexer *lx, bool defp, const struct span *span)
{
   struct token tk;

   lex0(lx, &tk);
   if (!isppident(tk)) {
      error(&tk.span, "macro name missing");
      ppskipline(lx);
      return;
   }
   assert(nppcnd < arraylength(ppcndstk) && "too many nested #if");
   ppcndstk[nppcnd].ifspan = span->sl;
   ppcndstk[nppcnd].cnd = (findmac(tk.s) == NULL) ^ defp ? PPCNDTRUE : PPCNDFALSE;
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
   v = expr(lx, NULL, 0);
   cnd = &ppcndstk[nppcnd-1];
   if (cnd->elsep) {
      error(span, "#elif after #else");
      return;
   }
   switch (cnd->cnd) {
   case PPCNDTRUE: cnd->cnd = PPCNDTAKEN; break;
   case PPCNDFALSE: cnd->cnd = v ? PPCNDTRUE : PPCNDFALSE; break;
   case PPCNDTAKEN: assert(0);
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
   if (!isppident(tk)) {
      error(&tk.span, "macro name missing");
      ppskipline(lx);
      return;
   }
   switch (cnd->cnd) {
   case PPCNDTRUE: cnd->cnd = PPCNDTAKEN; break;
   case PPCNDFALSE: cnd->cnd = (findmac(tk.s) == NULL) ^ defp ? PPCNDTRUE : PPCNDFALSE; break;
   case PPCNDTAKEN: assert(0);
   }
}



static void
ppendif(struct lexer *lx, const struct span *span)
{
   struct token tk;
   if (lex0(lx, &tk) != '\n' && tk.t != TKEOF) {
      error(&tk.span, "garbage after #endif");
      ppskipline(lx);
   }
   if (!nppcnd) {
      error(span, "#endif without matching #if");
      return;
   }
   --nppcnd;
}

static void
ppelse(struct lexer *lx, const struct span *span)
{
   struct token tk;
   struct ppcnd *cnd;
   if (lex0(lx, &tk) != '\n' && tk.t != TKEOF) {
      error(&tk.span, "garbage after #else");
      ppskipline(lx);
   }
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

static int includedepth;
enum { MAXINCLUDE = 200 };

static void
ppinclude(struct lexer *lx, const struct span *span0)
{
   char *path;
   struct lexer new;
   struct token tk;
   struct span span = *span0;

   lexingheadername = 1;
   if (in_range(lex0(lx, &tk), TKPPHDRH, TKPPHDRQ)) {
      const char *base, *end;

      /* build relative path */
      base = getfilename(lx->fileid);
      for (end = base; *end != 0; ++end) {}
      for (--end; *end != '/' && end != base; --end) {}
      if (*end == '/') ++end;
      path = xcalloc(end - base + tk.len + 1);
      memcpy(path, base, end - base);
      memcpy(path + (end - base), tk.s, tk.len);
      path[end - base + tk.len] = 0;
   } else {
      error(&tk.span, "garbage after #include");
      ppskipline(lx);
      return;
   }
   //efmt(">include %'s\n", path);
   joinspan(&span.ex, tk.span.ex);
   initlexer(&new, &span, path, lx->tmparena);
   new.save = xcalloc(sizeof *new.save);
   memcpy(new.save, lx, sizeof *lx);
   *lx = new;

   if (++includedepth == MAXINCLUDE)
      fatal(&span, "Maximum nested include depth of %d reached", includedepth);
}

static struct macrostack {
   struct macrostack *link;
   struct rlist *args;
   const struct rlist *rlist;
   struct span0 exspan;
   int macno;
   int idx;
} mstk[64], *mfreelist;

static void
pushmacstk(struct lexer *lx, const struct span *span, const struct macrostack *m)
{
   struct macrostack *l;
   if (!(l = mfreelist)) fatal(span, "macro depth limit reached");
   l = mfreelist;
   mfreelist = l->link;
   l->link = lx->macstk;
   assert(m->rlist);
   l->rlist = m->rlist;
   l->macno = m->macno;
   l->args = m->args;
   l->idx = 0;
   l->exspan = span->ex;
   lx->macstk = l;
}

static bool
tryexpand(struct lexer *lx, const struct token *tk)
{
   static bool inimstk;
   int macidx, i;
   struct span span = tk->span;
   struct macrostack *l;
   struct macro *mac = NULL;
   struct rlist *args = NULL;

   if (!inimstk) {
      inimstk = 1;
      for (i = 0; i < arraylength(mstk); ++i) {
         mstk[i].link = mfreelist;
         mfreelist = &mstk[i];
      }
   }

   if (tk->t == TKPPMACARG) {
      struct rlist *arg;
      l = lx->macstk;
      arg = &l->args[tk->argidx];
      if (arg->n) {
         pushmacstk(lx, &span, &(struct macrostack){
            .idx = 0,
            .rlist = arg,
            .macno = -1,
         });
      }
      return 1;
   } else if (!isppident(*tk) || !(mac = findmac(tk->s)))
      return 0;

   macidx = mac - macros.p;
   /* prevent infinite recursion */
   for (l = lx->macstk; l; l = l->link)
      if (l->macno == macidx)
         return 0;


   if (mac->fnlike) {
      vec_of(struct token) rlist = {0};
      bool toomany = 0;
      struct span endspan;
      int cur, n, i, bal;
      struct token tk;

      if (lexpeek(lx, &tk) != '(')
         return 0;
      lex(lx, &tk);
      args = xcalloc((mac->nparam + mac->variadic) * sizeof *args);

      for (cur = 0, i = 0, bal = 0, n = 0; (lex(lx, &tk) != ')' || bal != 0) && tk.t != TKEOF; ) {
         if (tk.t == ',' && bal == 0) {
            if (i == mac->nparam-1) {
               if (!mac->variadic) {
                  endspan = tk.span;
                  toomany = 1;
               }
            } else if (i < mac->nparam) {
               args[i].tk = &tk + cur;
               args[i].n = n;
               cur = rlist.n;
               n = 0;
               ++i;
            }
         } else if (!toomany) {
            if (tk.t == '(' || tk.t == '[') ++bal;
            else if (tk.t == ')' || tk.t == ']') --bal;
            vpush(&rlist, tk);
            ++n;
         }
      }
      if (tk.t == TKEOF)
         error(&span, "unterminated function-like macro invocation");
      else if (i < mac->nparam) {
         args[i].tk = &tk + cur;
         args[i].n = n;
         cur = rlist.n;
         n = 0;
         ++i;
      }
      joinspan(&span.ex, tk.span.ex);
      if (i < mac->nparam)
         error(&span, "not enough arguments in function-like macro invocation");
      else if (toomany) {
         joinspan(&endspan.ex, tk.span.ex);
         error(&endspan, "excess arguments in function-like macro invocation");
      }
      for (int i = 0; i < mac->nparam + mac->variadic; ++i) {
         int idx = args[i].tk ? args[i].tk - &tk : rlist.n;
         args[i].tk = rlist.p + idx;
      }
   }

   if (mac->rlist.n) {
      pushmacstk(lx, &span, &(struct macrostack){
         .rlist = &mac->rlist,
         .macno = macidx,
         .args = args,
         .idx = 0,
      });
   }
   return 1;
}

static void
popmac(struct lexer *lx)
{
   struct macrostack *stk;
   
   assert(stk = lx->macstk);
   do {
      if (stk->args) {
         free(stk->args->tk);
         free(stk->args);
      }
      lx->macstk = stk->link;
      stk->link = mfreelist;
      mfreelist = stk;
   } while ((stk = lx->macstk) && stk->idx >= stk->rlist->n);
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
   int l = 0, h = arraylength(tab) - 1, i, cmp;
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

int
lex(struct lexer *lx, struct token *tk_)
{
   struct token tkx[1], *tk;
   int t;
   bool linebegin, skip;

   assert(tk_ != &lx->peektok);
   tk = tk_ ? tk_ : tkx;
   if (lx->peektok.t) {
      *tk = lx->peektok;
      memset(&lx->peektok, 0, sizeof lx->peektok);
      return tk->t;
   }

   if (lx->macstk) {
      const struct rlist *rl = lx->macstk->rlist;
      *tk = rl->tk[lx->macstk->idx++];
      assert(tk->t);
      tk->span.ex = lx->macstk->exspan;
      if (tryexpand(lx, tk))
         return lex(lx, tk_);
      if (lx->macstk->idx == rl->n)
         popmac(lx);
      return tk->t;
   }

   skip = nppcnd ? ppcndstk[nppcnd-1].cnd != PPCNDTRUE : 0;
   for (linebegin = 1;;) {
      while ((t = lex0(lx, tk)) == '\n') linebegin = 1;
      if (t == '#' && linebegin) {
         if (lex0(lx, tk) == '\n') { }
         else if (isppident(*tk)) {
            if (!skip) {
               switch (findppcmd(tk)) {
               case PPXXX: goto BadPP;
               case PPDEFINE:   ppdefine(lx); break;
               case PPIF:       ppif(lx, &tk->span); break;
               case PPIFDEF:    ppifxdef(lx, 1, &tk->span); break;
               case PPIFNDEF:   ppifxdef(lx, 0, &tk->span); break;
               case PPELIF:     ppelif(lx, &tk->span); break;
               case PPELIFDEF:  ppelifxdef(lx, 1, &tk->span); break;
               case PPELIFNDEF: ppelifxdef(lx, 0, &tk->span); break;
               case PPELSE:     ppelse(lx, &tk->span); break;
               case PPENDIF:    ppendif(lx, &tk->span); break;
               case PPINCLUDE:  ppinclude(lx, &tk->span); break;
               default: assert(0&&"nyi");
               }
            } else {
               switch (findppcmd(tk)) {
               case PPIF: /* increment nesting level */
               case PPIFDEF:
               case PPIFNDEF:
                  assert(nppcnd < arraylength(ppcndstk) && "too many nested #if");
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
         linebegin = 0;
         if (skip && tk->t != TKEOF) continue;
         if (tryexpand(lx, tk))
            return lex(lx, tk_);
         if (t == TKEOF && nppcnd) {
            struct span span = { ppcndstk[nppcnd-1].ifspan };
            error(&span, "#if is not matched by #endif");
         }
         if (t == TKEOF && lx->save) {
            /* end of #include'd file, restore previous state */
            struct lexer *sv = lx->save;
            memcpy(lx, lx->save, sizeof *lx);
            free(sv);
            --includedepth;
         } else {
            return t;
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

void
initlexer(struct lexer *lx, const struct span *span, const char *file, struct arena **tmparena)
{
   const char *error;
   struct memfile *f;

   memset(lx, 0, sizeof *lx);
   lx->fileid = openfile(&error, &f, file);
   if (lx->fileid < 0)
      fatal(span, "Cannot open %'s: %s", file, error);
   lx->dat = f->p;
   lx->ndat = f->n;
   lx->tmparena = tmparena;
}

/* vim:set ts=3 sw=3 expandtab: */
