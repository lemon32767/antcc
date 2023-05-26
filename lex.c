#include "common.h"
#include "parse.h"
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

static void
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
      else if (kwtab[i].cstd <= ccopt.cstd) {
         tk->t = kwtab[i].t;
         tk->ident = kwtab[i].s;
         return;
      } else break;
   }
ident:
   tk->t = TKIDENT;
   tk->ident = intern(s);
}

static int
next0(struct parser *pr)
{
   bool trigraph = ccopt.trigraph;
   int n, c;

   while (!memcmp(pr->dat+pr->idx, "\\\n", n = 2)
     || (trigraph && !memcmp(pr->dat+pr->idx, "\?\?/\n", n = 4))) {
      pr->idx += n;
      addfileline(pr->fileid, pr->idx);
   }
   if (pr->idx >= pr->ndat)
      return TKEOF;
   if (trigraph && !memcmp(pr->dat+pr->idx, "??", 2)) {
      switch (pr->dat[pr->idx+2]) {
      case '=':  pr->idx += 3; return '#';
      case '(':  pr->idx += 3; return '[';
      case ')':  pr->idx += 3; return ']';
      case '!':  pr->idx += 3; return '|';
      case '<':  pr->idx += 3; return '{';
      case '>':  pr->idx += 3; return '}';
      case '-':  pr->idx += 3; return '~';
      case '/':  pr->idx += 3; return '\\';
      case '\'': pr->idx += 3; return '^';
      }
   }
   if ((c = pr->dat[pr->idx++]) == '\n') {
      addfileline(pr->fileid, pr->idx);
   }
   return c;
}

static int
next(struct parser *pr)
{
   int c;

   if (pr->npeekchr) {
      int c = pr->peekchr[0];
      pr->chridx = pr->peekcidx[0];
      memmove(pr->peekchr, pr->peekchr + 1, --pr->npeekchr * sizeof *pr->peekchr);
      memmove(pr->peekcidx, pr->peekcidx + 1, pr->npeekchr * sizeof *pr->peekcidx);
      pr->eof = c == TKEOF;
      return c;
   }
   c = next0(pr);
   pr->eof = c == TKEOF;
   pr->chridx = pr->idx;
   return c;
}

static int
peek(struct parser *pr, int off)
{
   assert(off < arraylength(pr->peekchr));
   while (pr->npeekchr < off+1) {
      pr->peekchr[pr->npeekchr] = next0(pr);
      pr->peekcidx[pr->npeekchr++] = pr->idx;
   }
   return pr->peekchr[off];
}

static bool
match(struct parser *pr, int c)
{
   if (!pr->eof && peek(pr, 0) == c) {
      next(pr);
      return 1;
   }
   return 0;
}

static bool
aissep(int c) {
   if (!aisprint(c) || aisspace(c))
      return 1;
   switch (c)
   case '(': case ')': case '[': case ']':
   case '{': case '}': case '.': case ',':
   case ';': case '?': case '+': case '-':
   case '*': case '/': case '&': case '|':
   case '^': case '~': case '=': case '\'':
   case '"': case '<': case '>': case ':':
   case '@': case '#': case '%': case '\\':
   case '`':
      return 1;
   return 0;
}

static void
strtonum(struct token *tk, const char *s)
{
   extern uvlong strtoull(const char *, char **, int);
   extern double strtod(const char *, char **);
   char *sx; /*suffix*/

   tk->ty = TYXXX;
   if (strchr(s, '.')) { /* float literal */
   Float:
      tk->f = strtod(s, &sx);
      if (sx == s)
         return;
      if (!*sx)
         tk->ty = TYDOUBLE;
      else if ((sx[0]|0x20) == 'f' && !sx[1]) {
         tk->ty = TYFLOAT;
         tk->f = (float) tk->f;
      } else tk->ty = TYXXX;
   } else { /* int literal */
      static uvlong max4typ[TYUVLONG-TYINT+1];
      enum typetag t;
      bool u = 0, dec = s[0] != '0';
      bool c99 = ccopt.cstd >= STDC99;

      tk->u = strtoull(s, &sx, 0);
      if (sx == s)
         return;

      if (!max4typ[0])
         for (t = TYINT; t <= TYUVLONG; ++t)
            max4typ[t-TYINT] = ((1ull << (8*targ_primsizes[t]-1))-1) << isunsignedt(t) | 1;

      if (!*sx) /* '' */ {}
      else if ((sx[0]|0x20) == 'u') {
         u = 1;
         if (!sx[1]) /* 'u' */ {}
         else if ((sx[1]|0x20) == 'l') {
            if (!sx[2]) /* 'ul' */ goto L;
            if (c99 && sx[1] == sx[2] && !sx[3]) /* 'ull' */ goto LL;
            return;
         } else return;
      } else if ((sx[0]|0x20) == 'l') {
         if (!sx[1]) /* 'l' */ goto L;
         if ((sx[1]|0x20) == 'u' && !sx[2]) /* 'lu' */ { u=1; goto L; }
         if (c99 && sx[1] == sx[0]) {
            if (!sx[2]) /* 'll' */ goto LL;
            if ((sx[2]|0x20) == 'u' && !sx[3]) /* 'llu' */ { u=1; goto LL; }
         }
         return;
      } else if ((sx[0]|0x20) == 'e' || (sx[0]|0x20) == 'p')
         goto Float;
      else return;

#define I(T) if (tk->u <= max4typ[T - TYINT]) { t = T; goto Ok; }
      I(TYINT)
      if (u || !dec) I(TYUINT)
   L:
      I(TYLONG)
      if (u || !dec || !c99) I(TYULONG)
      if (c99) {
      LL:
         I(TYVLONG)
         if (u || !dec) I(TYUVLONG)
      }
#undef I
      /* too big */
      return;
   Ok:
      if (u && issignedt(t)) ++t; /* make unsigned */
      tk->ty = t;
   }
}

static void
readstrchrlit(struct parser *pr, struct token *tk, char delim)
{
   int c, i;
   uchar tmp[80];
   vec_of(uchar) b = VINIT(tmp, sizeof tmp);
   struct span span = {0};
   uint n, idx = pr->chridx;

   while ((c = next(pr)) != delim) {
      if (c == '\n' || c == TKEOF) {
      Noterm:
         span.sl = (struct span0) { idx, pr->chridx - idx, pr->fileid };
         error(&span, "missing terminating %c character", delim);
         break;
      } else if (c == '\\') {
         span.sl = (struct span0) { idx, pr->chridx - idx, pr->fileid };
         switch (c = next(pr)) {
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
            if (!aisxdigit(peek(pr, 0))) goto Badescseq;
            do {
               c = next(pr);
               if (c-'0' < 10) n = n<<4 | (c-'0');
               else            n = n<<4 | (10 + (c|0x20)-'a');
            } while (aisxdigit(peek(pr, 0)));
            if (n > 0xFF) {
               span.sl.len = pr->chridx - span.sl.off;
               error(&span, "hex escape sequence out of range");
            }
            c = n & 0xFF;
            break;
         default:
            if (aisodigit(c)) { /* octal */
               n = c-'0';
               for (i = 2; i--;) {
                  if (!aisodigit(peek(pr, 0))) break;
                  n = n<<3 | ((c = next(pr))-'0');
               }
               if (n > 0377) {
                  span.sl.len = pr->chridx - span.sl.off;
                  error(&span, "hex escape sequence out of range");
               }
               c = n;
               break;
            }
         Badescseq:
            span.sl.len = pr->chridx - span.sl.off;
            error(&span, "invalid escape sequence");
         }
      }
      vpush(&b, c);
      idx = pr->chridx;;
   }
   if (delim == '"') {
      vpush(&b, 0);
      tk->t = TKSTRLIT;
      tk->s.p = alloc(&pr->exarena, b.n, 1);
      memcpy(tk->s.p, b.p, tk->s.n = b.n);
   } else {
      if (b.n == 0) {
         span.sl = (struct span0) { idx, pr->chridx - idx, pr->fileid };
         error(&span, "empty character literal");
      } else if (b.n > targ_primsizes[TYINT]) {
         span.sl = (struct span0) { idx, pr->chridx - idx, pr->fileid };
         error(&span, "multicharacter literal too long");
      }
      tk->t = TKNUMLIT;
      tk->ty = TYINT;
      tk->u = 0;
      for (i = 0; i < b.n; ++i)
         tk->u = tk->u<<8 | b.p[i];
   }
   vfree(&b);
}

static bool
isppnum(char prev, char c)
{
   if (!aissep(c) || c == '.')
     return 1;
   if (c == '+' || c == '-')
     return (prev|0x20) == 'e' || (prev|0x20) == 'p';
   return 0;
}

static int
lex0(struct parser *pr, struct token *tk)
{
   int idx, c;

#define RET(t_) do { tk->t = (t_); goto End; } while (0)

Begin:
   idx = pr->chridx;
   switch (c = next(pr)) {
   case ' ': case '\r': case '\t':
      goto Begin;
      break;
   case '!': case '(': case ')': case ',':
   case ':': case ';': case '?': case '[':
   case ']': case '{': case '}': case '~':
   case '$': case '@': case '`': case '\\': case TKEOF: case '\n':
      RET(c);
   case '#':
      if (match(pr, '#')) RET(TKPPCAT);
      RET(c);
   case '+':
      if (match(pr, '+')) RET(TKINC);
      if (match(pr, '=')) RET(TKSETADD);
      RET(c);
   case '-':
      if (match(pr, '-')) RET(TKDEC);
      if (match(pr, '=')) RET(TKSETSUB);
      if (match(pr, '>')) RET(TKARROW);
      RET(c);
   case '*':
      if (match(pr, '=')) RET(TKSETMUL);
      RET(c);
   case '/':
      if (match(pr, '=')) RET(TKSETDIV);
      if (match(pr, '/')) {
         /* // comment */
         while (!pr->eof && !match(pr, '\n'))
            next(pr);
         goto Begin;
      }
      if (match(pr, '*')) {
         /* comment */
         while (peek(pr, 0) != '*' || peek(pr, 1) != '/') {
            if (next(pr) == TKEOF) {
               struct span span = {{ idx, pr->chridx - idx, pr->fileid }};
               fatal(&span, "unterminated multiline comment");
            }
         }
         next(pr), next(pr);
         goto Begin;
      }
      RET(c);
   case '%':
      if (match(pr, '=')) RET(TKSETMOD);
      RET(c);
   case '^':
      if (match(pr, '=')) RET(TKSETXOR);
      RET(c);
   case '=':
      if (match(pr, '=')) RET(TKEQU);
      RET(c);
   case '<':
      if (match(pr, '=')) RET(TKLTE);
      if (match(pr, '<')) RET(match(pr, '=') ? TKSETSHL : TKSHL);
      RET(c);
   case '>':
      if (match(pr, '=')) RET(TKGTE);
      if (match(pr, '>')) RET(match(pr, '=') ? TKSETSHR : TKSHR);
      RET(c);
   case '&':
      if (match(pr, '&')) RET(TKLOGAND);
      if (match(pr, '=')) RET(TKSETAND);
      RET(c);
   case '|':
      if (match(pr, '|')) RET(TKLOGIOR);
      if (match(pr, '=')) RET(TKSETIOR);
      RET(c);
   case '\'':
   case '"':
      readstrchrlit(pr, tk, c);
      goto End;
   case '.':
      if (peek(pr, 0) == '.' && peek(pr, 1) == '.') {
         next(pr), next(pr);
         RET(TKDOTS);
      } else if (aisdigit(peek(pr, 0))) {
         goto Numlit;
      }
      RET(c);
   default:
      if (aisdigit(c)) Numlit: {
         char tmp[70];
         int n = 0;
         tmp[n++] = c;
         while (isppnum(tmp[n-1], peek(pr, 0))) {
            assert(n < arraylength(tmp)-1 && "too big");
            tmp[n++] = next(pr);
         }
         tmp[n] = 0;
         strtonum(tk, tmp);
         RET(TKNUMLIT);
      } else if (c == '_' || aisalpha(c)) {
         char tmp[70];
         int n = 0;
         tmp[n++] = c;
         while (!aissep(c = peek(pr, 0))) {
            assert(n < arraylength(tmp)-1 && "too big");
            tmp[n++] = next(pr);
         }
         tmp[n] = 0;
         identkeyword(tk, tmp, n);
         goto End;
      }
   }
   fatal(&(struct span) {{ idx, pr->chridx - idx, pr->fileid }},
         "unexpected character %'c at %d", c, idx);
End:
   tk->span.sl.file = pr->fileid;
   tk->span.sl.off = idx;
   tk->span.sl.len = pr->chridx - idx;
   tk->span.ex.file = pr->fileid;
   tk->span.ex.off = idx;
   tk->span.ex.len = pr->chridx - idx;
   return tk->t;
#undef RET
}

/****************/
/* PREPROCESSOR */
/****************/

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
   char tmpbuf[100];
   struct wbuf tmp = MEMBUF(tmpbuf, sizeof tmpbuf);
   if (a->t != b->t) return 0;
   if (a->t == TKNUMLIT) {
      const char *s1 = tmp.buf, *s2;
      int n1, n2;

      if (a->ty != b->ty) return 0;
      n1 = bfmt(&tmp, "%tk", a);
      s2 = tmp.buf + tmp.len;
      n2 = bfmt(&tmp, "%tk", b);
      if (tmp.err) return 0;
      return n1 == n2 && !memcmp(s1, s2, n1);
   } else if (a->t == TKIDENT) {
      return a->ident == b->ident;
   } else if (a->t == TKSTRLIT) {
      return a->s.n == b->s.n && !memcmp(a->s.p, b->s.p, a->s.n);
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
ppskipline(struct parser *pr)
{
   while (peek(pr, 0) != '\n' && peek(pr, 0) != TKEOF)
      next(pr);
}

static void
ppdefine(struct parser *pr)
{
   struct token tk0, tk;
   struct macro mac = {0};
   vec_of(struct token) rlist = {0};

   lex0(pr, &tk0);
   if (!isppident(tk0)) {
      error(&tk0.span, "macro name missing");
      ppskipline(pr);
      return;
   }
   mac.name = tk0.ident;
   mac.span = tk0.span.sl;
   
   if (peek(pr, 0) == '(') {
      mac.fnlike = 1;
   }

   while (lex0(pr, &tk) != '\n' && tk.t != TKEOF) {
      if (!wsseparated(&tk0, &tk))
         warn(&tk.span, "no whitespace after macro name");
      vpush(&rlist, tk);
   }
   mac.rlist.tk = rlist.p;
   mac.rlist.n = rlist.n;
   putmac(&mac);
}

static struct token epeektk;
static int
elex(struct parser *pr, struct token *tk)
{
   if (epeektk.t) {
      int tt = epeektk.t;
      if (tk) *tk = epeektk;
      epeektk.t = 0;
      return tt;
   }
   return lex0(pr, tk);
}

static int
epeek(struct parser *pr, struct token *tk)
{
   if (!epeektk.t) elex(pr, &epeektk);
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
expr(struct parser *pr, bool *pu, int prec)
{
   vlong x, y;
   struct token tk;
   int opprec;
   char unops[16];
   int nunop = 0;
   bool xu = 0, yu; /* x unsigned?; y unsigned? */

Unary:
   switch (elex(pr, &tk)) {
   case '-': case '~': case '!':
      unops[nunop++] = tk.t;
      if (nunop >= arraylength(unops)) {
         x = expr(pr, &xu, 999);
         break;
      }
      /* fallthru */
   case '+': goto Unary;
   case '(':
      x = expr(pr, &xu, 1);
      if (elex(pr, &tk) != ')') {
         error(&tk.span, "expected ')'");
         goto Err;
      }
      break;
   case TKNUMLIT:
      if (!tk.ty) {
         error(&tk.span, "bad number literal");
         goto Err;
      } else if (isfltt(tk.ty)) {
         error(&tk.span, "float literal in preprocessor expresion");
         goto Err;
      }
      x = tk.i;
      xu = isunsignedt(tk.ty);
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
      case '-': x = -x; break;
      case '~': x = ~x; break;
      case '!': x = !x; break;
      default: assert(0);
      }

   while ((opprec = tkprec(epeek(pr, &tk))) >= prec) {
      elex(pr, &tk);
      if (tk.t != '?') {
         bool u;
         y = expr(pr, &yu, opprec + 1);
         u = xu | yu;
         switch ((int) tk.t) {
         case   '+': x += y; break;
         case   '-': x -= y; break;
         case   '*': x *= y; break;
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
         vlong m = expr(pr, &xu, 1);
         if (elex(pr, &tk) != ':') {
            error(&tk.span, "expected ':'");
            note(&span, "to match conditional expression here");
            goto Err;
         }
         y = expr(pr, &yu, 1);
         efmt("%ld ? %ld : %ld\n", x, m, y);
         x = x ? m : y;
         xu |= yu;
      }
   }
   if (!prec) /* not a sub expr */
      if (elex(pr, &tk) != '\n' && tk.t != TKEOF) {
         error(&tk.span, "garbage after preprocessor expression");
         ppskipline(pr);
      }
   if (pu) *pu = xu;
   return x;

Err:
   ppskipline(pr);
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
ppif(struct parser *pr, const struct span *span)
{
   vlong v = expr(pr, NULL, 0);
   assert(nppcnd < arraylength(ppcndstk) && "too many nested #if");
   ppcndstk[nppcnd].ifspan = span->sl;
   ppcndstk[nppcnd].cnd = v ? PPCNDTRUE : PPCNDFALSE;
   ppcndstk[nppcnd++].elsep = 0;
}

static void
ppelif(struct parser *pr, const struct span *span)
{
   vlong v;
   struct ppcnd *cnd;

   if (!nppcnd) {
      error(span, "#elif without matching #if");
      ppif(pr, span);
      return;
   }
   v = expr(pr, NULL, 0);
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
ppendif(struct parser *pr, const struct span *span)
{
   struct token tk;
   if (lex0(pr, &tk) != '\n' && tk.t != TKEOF) {
      error(&tk.span, "garbage after #endif");
      ppskipline(pr);
   }
   if (!nppcnd) {
      error(span, "#endif without matching #if");
      return;
   }
   --nppcnd;
}

static void
ppelse(struct parser *pr, const struct span *span)
{
   struct token tk;
   struct ppcnd *cnd;
   if (lex0(pr, &tk) != '\n' && tk.t != TKEOF) {
      error(&tk.span, "garbage after #else");
      ppskipline(pr);
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

static struct macrostack mstk[64], *mfreelist;
static bool
tryexpand(struct parser *pr, const struct token *tk)
{
   static bool inimstk;
   struct macro *mac;
   struct macrostack *l;
   int macidx, i;

   if (!isppident(*tk) || !(mac = findmac(tk->ident)))
      return 0;

   if (!inimstk) {
      inimstk = 1;
      for (i = 0; i < arraylength(mstk); ++i) {
         mstk[i].link = mfreelist;
         mfreelist = &mstk[i];
      }
   }

   macidx = mac - macros.p;
   /* prevent infinite recursion */
   for (l = pr->macstk; l; l = l->link)
      if (l->mac == macidx)
         return 0;

   if (mac->fnlike) {
   }
   if (mac->rlist.n) {
      if (!(l = mfreelist)) fatal(&tk->span, "macro depth limit reached");
      l = mfreelist;
      mfreelist = l->link;
      l->link = pr->macstk;
      l->mac = macidx;
      l->idx = 0;
      l->exspan = tk->span.ex;
      pr->macstk = l;
   }
   return 1;
}

static void
popmac(struct parser *pr)
{
   struct macrostack *stk;
   
   assert(stk = pr->macstk);
   do {
      pr->macstk = stk->link;
      stk->link = mfreelist;
      mfreelist = stk;
   } while ((stk = pr->macstk) && stk->idx >= macros.p[stk->mac].rlist.n);
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
   const char *s = tk->ident;

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
lex(struct parser *pr, struct token *tk_)
{
   struct token tkx[1], *tk;
   int t;
   bool linebegin, skip;

   assert(tk_ != &pr->peektok);
   tk = tk_ ? tk_ : tkx;
   if (pr->peektok.t) {
      *tk = pr->peektok;
      memset(&pr->peektok, 0, sizeof pr->peektok);
      return tk->t;
   }

   if (pr->macstk) {
      struct macro *mac = &macros.p[pr->macstk->mac];
      struct rlist rl = mac->rlist;
      *tk = rl.tk[pr->macstk->idx++];
      assert(tk->t);
      tk->span.ex = pr->macstk->exspan;
      if (tryexpand(pr, tk))
         return lex(pr, tk_);
      if (pr->macstk->idx == rl.n)
         popmac(pr);
      return tk->t;
   }

   skip = nppcnd ? ppcndstk[nppcnd-1].cnd != PPCNDTRUE : 0;
   for (linebegin = 0;;) {
      while ((t = lex0(pr, tk)) == '\n') linebegin = 1;
      if (t == '#' && linebegin) {
         if (lex0(pr, tk) == '\n') { }
         else if (isppident(*tk)) {
            if (!skip) {
               switch (findppcmd(tk)) {
               case PPXXX: goto BadPP;
               case PPDEFINE: ppdefine(pr); break;
               case PPIF:     ppif(pr, &tk->span); break;
               case PPELIF:   ppelif(pr, &tk->span); break;
               case PPENDIF:  ppendif(pr, &tk->span); break;
               case PPELSE:   ppelse(pr, &tk->span); break;
               default: assert(0&&"nyi");
               }
            } else {
               switch (findppcmd(tk)) {
               case PPIF: /* increment nesting level */
                  assert(nppcnd < arraylength(ppcndstk) && "too many nested #if");
                  ppcndstk[nppcnd].ifspan = tk->span.sl;
                  ppcndstk[nppcnd].cnd = PPCNDTAKEN;
                  ppcndstk[nppcnd++].elsep = 0;
                  break;
               case PPELIF:  ppelif(pr, &tk->span); break;
               case PPENDIF: ppendif(pr, &tk->span); break;
               case PPELSE:  ppelse(pr, &tk->span); break;
               default: ppskipline(pr); break;
               }
            }
            skip = nppcnd ? ppcndstk[nppcnd-1].cnd != PPCNDTRUE : 0;
         } else {
            if (!skip) {
            BadPP:
               error(&tk->span, "invalid preprocessor directive");
            }
            ppskipline(pr);
         }
      } else {
         linebegin = 0;
         if (skip && tk->t != TKEOF) continue;
         if (tryexpand(pr, tk))
            return lex(pr, tk_);
         if (t == TKEOF && nppcnd) {
            struct span span = { ppcndstk[nppcnd-1].ifspan };
            error(&span, "#if is not matched by #endif");
         }
         return t;
      }
   }
   assert(0);
}

int
lexpeek(struct parser *pr, struct token *tk_)
{
   struct token tkx[1], *tk;
   uint t;

   tk = tk_ ? tk_ : tkx;
   if ((t = pr->peektok.t)) {
      *tk = pr->peektok;
      return t;
   }
   t = lex(pr, tk);
   pr->peektok = *tk;
   return t;
}

/* vim:set ts=3 sw=3 expandtab: */
