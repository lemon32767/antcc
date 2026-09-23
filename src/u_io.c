#define _DEFAULT_SOURCE
#include "c_lex.h"
#include "u_hash.h"
#include "ir.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

WriteBuf bstdout, bstderr;

void
ioinit(void) {
   bstdout._fp = stdout;
   bstdout.isfp = 1;
   bstderr._fp = stderr;
   bstderr.isfp = 1;
}

#define b_fp(b) ((FILE *)(b)->_fp)

void
iowrite(WriteBuf *buf, const void *Src, int n) {
   const uchar *src = Src;

   if (buf->isfp) {
      fwrite(Src, 1, n, b_fp(buf));
      buf->err = ferror(b_fp(buf)) != 0;
      return;
   }
   while (n > 0) {
      int avail = buf->cap - buf->len;
      int amt = avail < n ? avail : n;

      memcpy(buf->buf + buf->len, src, amt);
      n -= amt;
      src += amt;
      buf->len += amt;
      if (n > 0 && buf->len == buf->cap) {
         if (buf->fd < 0) {
            buf->err = 1;
            return;
         }
         ioflush(buf);
      }
   }
}

void
ioflush(WriteBuf *buf) {
   int i, ret;

   if (buf->isfp) {
      fflush(b_fp(buf));
      buf->err = ferror(b_fp(buf)) != 0;
      return;
   }
   buf->err = 0;
   if (buf->fd < 0) {
      buf->len = 0;
      return;
   }
   for (i = 0; buf->len > 0;) {
      ret = write(buf->fd, buf->buf + i, buf->len);
      if (ret > 0) {
         assert(ret <= buf->len);
         buf->len -= ret;
         i += ret;
      } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
         continue;
      } else {
         buf->err = 1;
         break;
      }
   }
}

void
ioputc(WriteBuf *buf, uchar c) {
   if (buf->isfp) {
      buf->err = fputc(c, b_fp(buf)) != EOF;
      return;
   }
   if (buf->len == buf->cap) {
      if (buf->fd < 0) {
         buf->err = 1;
         return;
      }
      ioflush(buf);
   }
   buf->buf[buf->len++] = c;
}

static int
putquoted(WriteBuf *buf, uchar c, uchar qchar, int next) {
   if (c == qchar || c == '\\' || !aisprint(c)) {
      int n = (ioputc(buf, '\\'), 1);
      uchar cseq;

      switch (c) {
      case '\\':
      case '\'':
      case '"':
         cseq = c;
      Charseq:
         n += (ioputc(buf, cseq), 1);
         break;
      case '\a': cseq = 'a'; goto Charseq;
      case '\b': cseq = 'b'; goto Charseq;
      case '\f': cseq = 'f'; goto Charseq;
      case '\r': cseq = 'r'; goto Charseq;
      case '\t': cseq = 't'; goto Charseq;
      case '\v': cseq = 'v'; goto Charseq;
      case '\n': cseq = 'n'; goto Charseq;
      default:
          if (!next || in_range(next, '0', '7'))
              n += bfmt(buf, "%.3o", c);
          else
              n += bfmt(buf, "%o", c);
      }
      return n;
   }
   if (c == '?' && (!next || next == '?')) {
      return ioputc(buf, c), ioputc(buf, '\\'), 2;
   }
   return ioputc(buf, c), 1;
}

static int
putuint(WriteBuf *buf, u64int x, int base, bool lower) {
   uchar tmp[64];
   uchar *end = tmp + sizeof(tmp);
   uchar *s = end;
   switch (base) {
   case 2:
      do *--s = '0' + x%2; while (x >>= 1);
      break;
   case 8:
      do *--s = '0' + x%8; while (x >>= 3);
      break;
   case 10:
      do *--s = '0' + x%10; while (x /= 10);
      break;
   case 16:
      do *--s = "0123456789ABCDEF"[x&15] | (-lower & 0x20); while (x >>= 4);
      break;
   default:
      assert(0&&"base");
   }
   iowrite(buf, s, end - s);
   return end - s;
}

static void
fmterr(const char *fmt, ...) {
   va_list ap;

   efmt("fmt Error: ");
   va_start(ap, fmt);
   vbfmt(&bstderr, fmt, ap);
   va_end(ap);
   ioputc(&bstderr, '\n');
   ioflush(&bstderr);
   ioflush(&bstdout);
   abort();
}

#define bwriteS(buf, S) (iowrite(buf, S, sizeof S - 1), sizeof S - 1)
#define bputc(B, C) (ioputc(B, C), 1)

static int
priquals(WriteBuf *buf, int q) {
   const char s[] = " const volatile", *p = s;
   int m = sizeof s - 1;
   if (!q) return 0;
   else if (q == QCONST) m -= 9;
   else if (q == QVOLATILE) p += 6, m -= 6;
   else assert(q == (QCONST | QVOLATILE));
   iowrite(buf, p, m);
   return m;
}
static int
pritypebefore(WriteBuf *buf, Type ty, int qual) {
   const char *s, *s2;
   Type chld;
   int n;
   if (isprim(ty)) {
      n = bfmt(buf, "%s", primtypenames[ty.t]);
      return n + priquals(buf, qual);
   }
   switch (ty.t) {
   case TYPTR:
      chld = typechild(ty);
      n = pritypebefore(buf, chld, ty.flag & TFCHLDQUAL);
      if (!isptrcvtt(chld.t))
         n += bputc(buf, ' ');
      if (chld.t == TYARRAY || chld.t == TYFUNC)
         n += bputc(buf, '(');
      n += bputc(buf, '*');
      n += priquals(buf, qual);
      return n;
   case TYARRAY:
      return pritypebefore(buf, typechild(ty), ty.flag & TFCHLDQUAL);
   case TYFUNC:
      return pritypebefore(buf, typedata[ty.dat].ret, 0);
   case TYSTRUCT:
      s = "struct";
   Tagged:
      n = bfmt(buf, "%s %s", s, (s2 = (char *)tagtypetags[typedata[ty.dat].id]) ? s2 : "(anonymous)");
      return n + priquals(buf, qual);
   case TYUNION:
      s = "union";
      goto Tagged;
   case TYENUM:
      s = "enum";
      goto Tagged;
   default:
      return bfmt(buf, "?\?%d?",ty.t);
   }
}

static int
pritypeafter(WriteBuf *buf, Type ty, int qual) {
   const TypeData *td;
   int n = 0;
   switch (ty.t) {
   case TYPTR:
      if (typechild(ty).t == TYARRAY || typechild(ty).t == TYFUNC)
         n += bputc(buf, ')');
      n += pritypeafter(buf, typechild(ty), ty.flag & TFCHLDQUAL);
      break;
   case TYARRAY:
      n += bputc(buf, '[');
      if (isvla(ty))
         n += bputc(buf, '?');
      else if (typearrlen(ty))
         n += bfmt(buf, "%u", typearrlen(ty));
      n += bputc(buf, ']');
      n += pritypeafter(buf, typechild(ty), ty.flag & TFCHLDQUAL);
      break;
   case TYFUNC:
      td = &typedata[ty.dat];
      n += bputc(buf, '(');
      for (int i = 0; i < td->nmemb; ++i) {
         n += bfmt(buf, "%ty", td->param[i]);
         if (i < td->nmemb - 1 || td->variadic)
            n += bwriteS(buf, ", ");
      }
      if (td->variadic) n += bwriteS(buf, "...");
      else if (td->nmemb == 0 && !td->kandr) n += bwriteS(buf, "void");
      n += bwriteS(buf, ")");
      n += pritypeafter(buf, td->ret, 0);
      break;
   }
   return n;
}

static int
fmttype(WriteBuf *buf, Type ty, int qual) {
   int n = pritypebefore(buf, ty, qual);
   n += pritypeafter(buf, ty, qual);
   return n;
}

static int
putdouble(WriteBuf *buf, double x) {
   char tmp[200];
   int n = snprintf(tmp, sizeof tmp, "%f", x);
   if (n >= sizeof tmp-1) n = snprintf(tmp, sizeof tmp, "%g", x);
   assert(n < sizeof tmp-1);
   iowrite(buf, tmp, n);
   return n;
}

static int
putstr(WriteBuf *buf, const char *s, int len, bool quote, bool lower) {
   int n = 0;
   if (len < 0) { /* nullterminated */
      if (quote) {
         if (!s) return bwriteS(buf, "(null)");
         n += bputc(buf, '"');
         for (; *s; ++s)
            n += putquoted(buf, lower && aisalpha(*s) ? *s|32 : *s, '"', s[1]);
         n += bputc(buf, '"');
      } else {
         assert(s && "%s null!");
         for (; *s; ++s)
            n += bputc(buf, lower && aisalpha(*s) ? *s|32 : *s);
      }
   } else {
      if (quote) {
         if (!s) return bwriteS(buf, "(null)");
         n += bputc(buf, '"');
         for (; len-->0; ++s)
            n += putquoted(buf, lower && aisalpha(*s) ? *s|32 : *s, '"', len ? s[1] : -1);
         n += bputc(buf, '"');
      } else {
         n += len;
         for (; len-->0; ++s)
            ioputc(buf, lower && aisalpha(*s) ? *s|32 : *s);
      }
   }
   return n;
}

int
vbfmt(WriteBuf *out, const char *fmt, va_list ap) {
   bool quote, umod, lmod, zmod, lower, possign;
   int base;
   s64int i;
   int pad, prec, q;
   const char *s;
   void *p;
   Token *tok;
   Type ty;
   double f;
   char tmpbuf1[70], tmpbuf2[70];
   WriteBuf tmp1 = MEMBUF(tmpbuf1, sizeof tmpbuf1);
   WriteBuf tmp2 = MEMBUF(tmpbuf2, sizeof tmpbuf2);
   WriteBuf *buf = out;
   int n = 0, prevn;

   while (*fmt) {
      buf = out;
      if (*fmt++ != '%') {
         n += bputc(buf, fmt[-1]);
         continue;
      }
      if (*fmt == '%') {
         n += bputc(buf, *fmt++);
         continue;
      }
      fmt += quote = *fmt == '\'';
      fmt += possign = *fmt == '+';
      pad = 0;
      if (aisdigit(*fmt)) { /* left pad */
         for (; aisdigit(*fmt); ++fmt)
            pad = pad*10 + *fmt-'0';
         if (pad) {
            tmp1.len = 0;
            buf = &tmp1;
         }
      } else if (*fmt == '-') { /* right pad */
         if (!aisdigit(*++fmt))
            fmterr("padding amount expected");
         for (; aisdigit(*fmt); ++fmt)
            pad = pad*10 - (*fmt-'0');
      }
      prec = -1;
      if (*fmt == '.') {
         if (!aisdigit(*++fmt))
            fmterr("precision expected");
         prec = 0;
         for (; aisdigit(*fmt); ++fmt)
            prec = prec*10 + *fmt-'0';
      }
      fmt += umod = *fmt == 'u';
      if ((zmod = *fmt == 'z'))
         ++fmt, lmod = 0;
      else
         fmt += lmod = *fmt == 'l';
      lower = 0;
      prevn = n;
      switch (*fmt++) {
      case 'c': /* character */
         if (quote) {
            n += bputc(buf, '\'');
            n += putquoted(buf, va_arg(ap, int), '\'', -1);
            n += bputc(buf, '\'');
         } else {
            n += bputc(buf, va_arg(ap, int));
         }
         break;
      case 's': /* nullterminated string */
         s = va_arg(ap, const char *);
         n += putstr(buf, s, -1, quote, lmod);
         break;
      case 'S':  /* string ptr + len */
         s = va_arg(ap, const char *);
         i = va_arg(ap, int);
         assert(i >= 0);
         n += putstr(buf, s, i, quote, lmod);
         break;
      case 'y': /* symbol: print string literally if valid identifier, or quote it */
         s = va_arg(ap, const char *);
         assert(s && "%y null");
         for (i = 0; s[i]; ++i) {
            if (!(aisalpha(s[i]) || s[i] == '_' || (i > 0 && aisdigit(s[i])))) {
               quote = 1;
               break;
            }
         }
         n += putstr(buf, s, -1, quote, lmod);
         break;
      case 'd': /* decimal */
         base = 10;
      Int:
         if (base != 10) umod = 1;
         i = lmod ? va_arg(ap, s64int)
           : umod ? va_arg(ap, uint)
           : zmod && sizeof(&i-&i) > sizeof(int) ? va_arg(ap, s64int)
           : (s64int)va_arg(ap, int);
         tmp2.len = 0;
         if (!umod && i < 0) {
            n += bputc(buf, '-');
            i = -(u64int)i;
         } else if (possign) {
            n += bputc(buf, '+');
         }
         if (quote) {
            switch (base) {
            case  2: n += bwriteS(buf, "0b"); break;
            case  8: n += bwriteS(buf, "0"); break;
            case 16: n += bwriteS(buf, "0x"); break;
            }
         }
         n += putuint(prec > 0 ? &tmp2 : buf, i, base, lower);
         if (prec > 0) {
            int fil = prec - tmp2.len;
            while (fil-- > 0) n += bputc(buf, '0');
            iowrite(buf, tmp2.buf, tmp2.len);
         }
         break;
      case 'o': /* octal */
         base = 8;
         goto Int;
      case 'b': /* binary */
         base = 2;
         goto Int;
      case 'x': case 'X': /* hexadecimal */
         base = 16;
         lower = fmt[-1] == 'x';
         goto Int;
      case 'p': /* pointer */
         p = va_arg(ap, void *);
         if (!p && quote) {
            n += bwriteS(buf, "NULL");
         } else {
            n += bwriteS(buf, "0x");
            tmp2.len = 0;
            n += putuint(prec > 0 ? &tmp2 : buf, (u64int)p, 16, 1);
            if (prec > 0) {
               int fil = prec - tmp2.len;
               while (fil-- > 0) n += bputc(buf, '0');
               iowrite(buf, tmp2.buf, tmp2.len);
            }
         }
         break;
      case 'f': /* float */
         f = va_arg(ap, double);
         n += putdouble(buf, f);
         break;
      case 't': /* token/tokentag/type */
         switch (*fmt++) {
         case 'k': /* tk token */
            tok = va_arg(ap, Token *);
         Tok:
            switch (tok->t) {
            case TKXXX:
               n += bwriteS(buf, "\?\?\?");
               break;
            case TKNUMLIT:
               if (quote) {
                  n += bputc(buf, '`');
                  iowrite(buf, tok->s, tok->len);
                  n += tok->len;
                  n += bputc(buf, '\'');
               } else {
                  n += putstr(buf, tok->s, tok->len, 0, 0);
               }
               break;
            case TKCHRLIT:
               if (tok->wide) n += bputc(buf, tok->wideuni ? tok->wide == 1 ? 'u' : 'U' : 'L');
               n += bputc(buf, '\'');
               if (tok->wide == 0)
                  for (int i = 0; i < tok->len; ++i)
                     n += putquoted(buf, tok->s[i], '\'', i < tok->len - 1 ? tok->s[i+1] : -1);
               else {
                  char p[4];
                  uint c = tok->wide == 1 ? tok->ws16[0] : tok->ws32[0];
                  int l = utf8enc(p, c);
                  if (l == 1)
                     n += putquoted(buf, *p, '\'', -1);
                  else
                     n += (iowrite(buf, p, l), l);
               }
               n += bputc(buf, '\'');
               break;
            case TKSTRLIT:
               if (tok->wide == 0) {
                  n += putstr(buf, tok->s, tok->len, /*quote*/1, 0);
               } else {
                  n += bputc(buf, tok->wideuni ? tok->wide == 1 ? 'u' : 'U' : 'L');
                  n += bputc(buf, '\"');
                  for (int i = 0; i < tok->len; ++i) {
                     char p[4];
                     uint c = tok->wide == 1 ? tok->ws16[i] : tok->ws32[i];
                     int l = utf8enc(p, c);
                     if (l == 1)
                        n += putquoted(buf, *p, '\"', 0);
                     else
                        n += (iowrite(buf, p, l), l);
                  }
                  n += bputc(buf, '\"');
               }
               break;
            case TKPPMACSTR:
               if (quote) n += bputc(buf, '`');
               n += bfmt(buf, "#%s", tok->s);
               if (quote) n += bputc(buf, '\'');
               break;
            case TKPPMACARG:
            case TKIDENT:
               if (quote) n += bputc(buf, '`');
               n += bfmt(buf, "%s", tok->name);
               if (quote) n += bputc(buf, '\'');
               break;
            case TKPPPRAGMA:
               if (tok->pragmakind == 0) {
                  n += bwriteS(buf, "#pragma ");
                  n += putstr(buf, tok->s, tok->len, 0, 0);
               } else {
                  n += bwriteS(buf, "_Pragma(");
                  n += putstr(buf, tok->s, tok->len, 1, 0);
                  n += bputc(buf, ')');
               }
               break;
            case TKEOF:
               n += bwriteS(buf, "<end-of-file>");
               break;
            case TKEQU: s = "=="; C2: iowrite(buf, s, 2); n += 2; break;
            case TKNEQ: s = "!="; goto C2;
            case TKLTE: s = "<="; goto C2;
            case TKGTE: s = ">="; goto C2;
            case TKSHR: s = ">>"; goto C2;
            case TKSHL: s = "<<"; goto C2;
            case TKINC: s = "++"; goto C2;
            case TKDEC: s = "--"; goto C2;
            case TKDOTS: n += bwriteS(buf, "..."); break;
            case TKARROW: s = "->"; goto C2;
            case TKPPCAT: s = "##"; goto C2;
            case TKLOGAND: s = "&&"; goto C2;
            case TKLOGIOR: s = "||"; goto C2;
            case TKSETADD: s = "+="; goto C2;
            case TKSETSUB: s = "-="; goto C2;
            case TKSETMUL: s = "*="; goto C2;
            case TKSETDIV: s = "/="; goto C2;
            case TKSETREM: s = "%="; goto C2;
            case TKSETIOR: s = "|="; goto C2;
            case TKSETXOR: s = "^="; goto C2;
            case TKSETAND: s = "&="; goto C2;
            case TKSETSHR: n += bwriteS(buf, ">>="); break;
            case TKSETSHL: n += bwriteS(buf, "<<="); break;
            default:
               if (quote) n += bputc(buf, '`');
               if (in_range(tok->t, TKWBEGIN_, TKWEND_)) {
                  iowrite(buf, tok->name, tok->len);
                  n += tok->len;
               } else if (aisprint(tok->t)) {
                  n += bputc(buf, tok->t);
               } else if (tok->t == '\n') {
                  n += bwriteS(buf, "<newline>");
               } else {
                  n += bwriteS(buf, "??");
               }
               if (quote) n += bputc(buf, '\'');
               break;
            }
            break;
         case 't': /* tt token tag */
            tok = &(Token) { va_arg(ap, int) };
            switch (tok->t) {
            case TKNUMLIT:
               n += bwriteS(buf, "numeric literal");
               break;
            case TKSTRLIT:
               n += bwriteS(buf, "string literal");
               break;
            case TKIDENT:
               n += bwriteS(buf, "identifier");
               break;
            case TKEOF:
               n += bwriteS(buf, "<end-of-file>");
               break;
            default:
               if (tok->t >= TKWBEGIN_ && tok->t <= TKWEND_) {
                  static const char *tab[] = {
                  #define _(kw, c, ...) #kw,
                  #include "c_keywords.def"
                  #undef _
                  };
                  tok->s = tab[tok->t - TKWBEGIN_];
                  tok->len = strlen(tok->s);
               }
               goto Tok;
            }
            break;
         case 'y': /* ty type */
            ty = va_arg(ap, Type);
            n += fmttype(buf, ty, 0);
            break;
         case 'q': /* tq qualified type */
            ty = va_arg(ap, Type);
            q = va_arg(ap, int);
            n += fmttype(buf, ty, q);
            break;
         default:
            if (fmt[-1] == ' ' || !aisprint(fmt[-1]))
               fmterr("expected format specifier");
            else
               fmterr("unknown format specifier 't%c'", fmt[-1]);
         }
         break;
      case 'g': /* graphics rendition (color) */
         if (!ccopt.nocolor) n += bwriteS(buf, "\033[");
         while (*fmt++ != '.') {
            if (ccopt.nocolor) continue;
            n += bputc(buf, fmt[-1]);
         }
         if (!ccopt.nocolor) n += bputc(buf, 'm');
         break;
      case 'M': /* cc mode */
         iowrite(buf, &"C89\0C99\0C11\0C23"[ccopt.cstd*4], 3);
         n += 3;
         n += bwriteS(buf, " mode");
         break;
      case 'k': /* enum irclass */
         i = va_arg(ap, int);
         n += putstr(buf, (uint)i < countof(clsname) ? clsname[i] : "?", -1, 0, 0);
         break;
      case 'r': /* ir ref */
         n += dumpref(buf, 0, va_arg(ap, Ref));
         break;
      case '?': /* callback, ptr */
         {
            int (*f)(WriteBuf *, void *) = va_arg(ap, int(*)(WriteBuf *, void *));
            n += f(buf, va_arg(ap, void *));
         }
         break;
      default:
         if (umod || lmod) {
            --fmt;
            base = 10;
            goto Int;
         }
         if (fmt[-1] == ' ' || !aisprint(fmt[-1]))
            fmterr("expected format specifier");
         else
            fmterr("unknown format specifier '%c'", fmt[-1]);
      }
      if (pad > 0) { /* left pad */
         while (pad-- > buf->len)
            n += bputc(out, ' ');
         assert(buf != out);
         iowrite(out, buf->buf, buf->len);
         out->err |= buf->err;
      } else if (pad < 0) { /* right pad */
         int len = n - prevn;
         while (pad++ < -len)
            n += bputc(out, ' ');
      }
   }
   return n;
}

int
bfmt(WriteBuf *buf, const char *fmt, ...) {
   va_list ap;
   int ret;

   va_start(ap, fmt);
   ret = vbfmt(buf, fmt, ap);
   va_end(ap);
   return ret;
}

void
gpritype(Type ty) {
   efmt("%ty\n", ty);
   ioflush(&bstderr);
}

static uint pagesiz;

typedef struct EmbedFile {
   const char *name;
   const char *s;
   size_t len;
} EmbedFile;
extern EmbedFile embedfilesdir[];

MemFile
mapopen(const char **err, const char *path) {
   struct stat stat;
   int fd = -1;
   void *p = NULL;
   MemFile f = {0};
   uint mapsiz;

   assert("nullp" && err && path);

   if (!pagesiz) pagesiz = sysconf(_SC_PAGESIZE);
   *err = NULL;

   if (*path == '@' && path[1] == ':') {
      for (EmbedFile *e = embedfilesdir; e->name; ++e) {
         if (!strcmp(e->name, path+2)) {
            return (MemFile) { (const uchar *)e->s, e->len, .statik = 1 };
         }
      }
   }

   if ((fd = open(path, O_RDONLY)) < 0)
      goto Err;
   if (fstat(fd, &stat) != 0)
      goto Err;

   if (S_ISREG(stat.st_mode)) {
      if (stat.st_size > UINT_MAX) {
      Big:
         errno = EFBIG;
         goto Err;
      }
      mapsiz = alignup(stat.st_size, pagesiz);
      if ((p = mmap(NULL, mapsiz + pagesiz, PROT_READ, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0)) == MAP_FAILED)
         goto Err;
      if (mapsiz > 0 && mmap(p, mapsiz, PROT_READ, MAP_FIXED|MAP_PRIVATE, fd, 0) == MAP_FAILED)
         goto Err;

      close(fd);
      f.p = p;
      f.n = stat.st_size;
      return f;
   } else if (S_ISFIFO(stat.st_mode) || S_ISCHR(stat.st_mode)) {
      uint cap = 0;
      enum {MAX = 1<<28};
      int ret;

      do {
         enum { CHUNKSIZ = 1<<10 };
         if ((cap += CHUNKSIZ) > MAX) {
            free(p);
            goto Big;
         }
         cap += CHUNKSIZ;
         if (!(f.p = p ? realloc(p, cap) : malloc(cap))) {
            free(p);
            goto Err;
         }
         p = (void *)f.p;
         ret = read(fd, (char *)p + f.n, CHUNKSIZ);
         if (ret >= 0) {
            assert((uint)(f.n + ret) >= f.n);
            f.n += ret;
         } else if (errno != EAGAIN && errno != EWOULDBLOCK)
            goto Err;
      } while (ret != 0);

      close(fd);
      fd = -1;
      mapsiz = alignup(f.n, pagesiz);
      if ((f.p = mmap(NULL, mapsiz + pagesiz, PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0)) == MAP_FAILED) {
         free(p);
         goto Err;
      }
      memcpy((void *)f.p, p, f.n);
      free(p);
      mprotect((void *)f.p, mapsiz + pagesiz, PROT_READ);
      return f;
   } else {
      *err = "Not a file";
   }

Err:
   if (fd >= 0) close(fd);
   if (!*err) *err = strerror(errno);
   return f;
}

void
mapclose(MemFile *f) {
   assert(f->p);
   if (!f->statik)
      munmap((void *)f->p, alignup(f->n, pagesiz) + pagesiz);
   memset(f, 0, sizeof *f);
}

void *
mapzeros(uint N) {
   void *p = mmap(NULL, N, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
   return p == MAP_FAILED ? NULL : p;
}

void
_assertfmt(const char *file, int line, const char *func, const char *expr) {
   ioflush(&bstdout);
   efmt("%s:%d: %s: Assertion `%s' failed.\n", file, line, func, expr);
   ioflush(&bstderr);
}

typedef struct {
   int phys; /* pre-preprocessing line number */
   int toline; /* line number in directive */
   const char *tofile;
} LineMap;

typedef struct FileUID FileUID;
typedef struct File {
   struct FileUID {
      long dev;
      union {
         long ino;
         const char *str;
      };
   } uid;
   const char *path;
   MemFile f;
   vec_of(uint) lineoffs;
   uint nlscanidx;
   vec_of(LineMap) linemap; /* one entry per #line directive */
   bool seen;
   bool once; /* uses guard macro or #pragma once */
   internstr guardmac;
} File;
static File *fileht[1<<SPANFILEBITS];
static int nfiles;

int
getpredeffile(MemFile **pf, const char *name) {
   File *f;
   FileUID uid;
   uint h, id, n = countof(fileht);

   uid.dev = -11;
   uid.ino = hashs(0, name);
   for (id = h = uid.dev ^ uid.ino;; ++id) {
      id &= countof(fileht) - 1;
      f = fileht[id];
      if (f && f->uid.dev == uid.dev && f->uid.ino == uid.ino) {
         break;
      } else if (!f) {
         f = allocz(&globarena, sizeof *f, 0);
         f->uid = uid;
         f->path = name;
         f->f = (MemFile) { .statik = 1 };
         fileht[id] = f;
         vinit(&f->lineoffs, NULL, 10);
         vpush(&f->lineoffs, 0);
         ++nfiles;
         break;
      }
      assert(--n > 0 && "fileht full");
   }
   *pf = &f->f;
   return id;
}

int
openfile(const char **err, MemFile **pf, const char *path) {
   struct stat st;
   File *f;
   FileUID uid;
   size_t h, id, n = countof(fileht);

   if (*path == '@' && path[1] == ':') {
      uid.dev = -1;
      uid.str = path;
      h = hashs(0, path+2);
   } else {
      if (stat(path, &st) != 0) {
         *err = strerror(errno);
         return -1;
      }
      uid.dev = st.st_dev, uid.ino = st.st_ino;
      h = uid.dev ^ uid.ino;
   }
   for (id = h;; ++id) {
      id &= countof(fileht) - 1;
      f = fileht[id];
      if (f && f->uid.dev == uid.dev && (uid.dev >= 0 ? f->uid.ino == uid.ino : !strcmp(f->uid.str, uid.str))) {
         break;
      } else if (!f) {
         MemFile m;
         m = mapopen(err, path);
         if (*err) return -1;
         f = allocz(&globarena, sizeof *f, 0);
         f->uid = uid;
         f->path = path;
         f->f = m;
         fileht[id] = f;
         vinit(&f->lineoffs, NULL, 50);
         vpush(&f->lineoffs, 0);
         ++nfiles;
         break;
      }
      assert(--n > 0 && "fileht full");
   }
   *pf = &f->f;
   return id;
}

const char *
getfilename(int id, uint atoff) {
   assert((uint)id < countof(fileht) && fileht[id]);
   if (!fileht[id]->linemap.n || !atoff)
      return fileht[id]->path;
   return getfilepos(NULL, NULL, id, atoff);
}

MemFile *
getfile(int id) {
   assert((uint)id < countof(fileht) && fileht[id]);
   return &fileht[id]->f;
}

static bool // -> last?
filllineoffs(File *f, uint upto) {
   if (upto >= f->f.n) upto = f->f.n - 1;
   if (upto <= f->nlscanidx) return 0;
   const uchar *p = f->f.p + f->nlscanidx;
   for (uint i = f->nlscanidx; i <= upto; ++p, ++i) {
      if (*p == '\n')
         vpush(&f->lineoffs, i + 1);
   }
   f->nlscanidx = upto;
   return 1;
}

void
setfileline(int id, uint off, int line, const char *file) {
   assert((uint)id < countof(fileht) && fileht[id]);
   vec_of(LineMap) *linemap = (void *)&fileht[id]->linemap;
   vec_of(uint) *lineoffs = (void *)&fileht[id]->lineoffs;
   int phys = 2;
   filllineoffs(fileht[id], off);
   for (int i = lineoffs->n-1; i >= 0; --i) {
      if (lineoffs->p[i] < off) {
         phys = i+2;
         break;
      }
   }
   if (linemap->n > 0) {
      assert(linemap->p[linemap->n-1].phys < phys);
      if (!file) file = linemap->p[linemap->n-1].tofile;
   }
   vpush(linemap, ((LineMap){ phys, line, file }));
}

const char *
getfilepos(int *pline, int *pcol, int id, uint off) {
   assert((uint)id < countof(fileht) && fileht[id]);
   File *f = fileht[id];
   int line, col;
   if (filllineoffs(f, off)) {
      // fast path: belongs to last scanned line
      line = f->lineoffs.n;
      col = off - f->lineoffs.p[line - 1] + 1;
   } else {
      uint *offs = f->lineoffs.p;
      uint n = f->lineoffs.n;
      /* binary search over offsets array */
      int l = 0, h = n - 1, i = 0;
      while (l <= h) {
         i = (l + h) / 2;
         if (offs[i] < off) l = i + 1;
         else if (offs[i] > off) h = i - 1;
         else break;
      }
      i -= offs[i] > off;
      line = i + 1;
      col = off - offs[i] + 1;
   }
   const char *file = f->path;
   vec_of(LineMap) *linemap = (void *)&f->linemap;
   if (linemap->n) {
      /* binary search over linemap array */
      int l = 0, h = linemap->n - 1, i = 0;
      while (l <= h) {
         i = (l + h) / 2;
         if (linemap->p[i].phys < line) l = i + 1;
         else if (linemap->p[i].phys > line) h = i - 1;
         else break;
      }
      i -= linemap->p[i].phys > line;
      if (i >= 0) {
         line = linemap->p[i].toline + (line - linemap->p[i].phys);
         if (linemap->p[i].tofile) file = linemap->p[i].tofile;
      }
   }
   if (pline) *pline = line;
   if (pcol) *pcol = col;
   return file;
}

bool
isoncefile(int id, internstr *guard) {
   assert(id < countof(fileht) && fileht[id]);
   *guard = fileht[id]->guardmac;
   return fileht[id]->once;
}

void
markfileonce(int id, internstr guard) {
   assert(id < countof(fileht) && fileht[id]);
   fileht[id]->once = 1;
   fileht[id]->guardmac = guard;
}

void
markfileseen(int id) {
   assert(id < countof(fileht) && fileht[id]);
   fileht[id]->seen = 1;
}

bool
isfileseen(int id) {
   assert(id < countof(fileht) && fileht[id]);
   return fileht[id]->seen;
}

void
closefile(int id) {
   assert(id < countof(fileht) && fileht[id]);
   mapclose(&fileht[id]->f);
}

static int
safeputnonascii(WriteBuf *buf, const uchar *p) {
   if (aisspace(*p)) { /* don't print the silly whitespaces (carriage return, formfeed etc) */
      return 1;
   } else if (*p > 127) { /* pass utf-8, assume valid. it's fine if not, won't mangle term */
      ioputc(buf, *p);
      return 1;
   } else { /* control characters, avoid printing these */
      bfmt(buf, "%g7.^%c%g.", ('@' + *p) & 0x7F); /* background inverted ^x */
      return 1;
   }
}

#define isnewline(c) ((c) == '\n' || (c) == '\r')

void
vdiag(const Span *span, enum diagkind kind, const char *fmt, va_list ap) {
   /* to avoid concurrent invocations of the compiler mixing up the diagnostics
    * in the unbuffered stderr output, use a separate buffer here and write()
    * it all out bypassing stdio */
   static char ebuf[4096];
   static WriteBuf out = FDBUF(ebuf, sizeof ebuf, STDERR_FILENO);
   static int depth = 0; /* needed for nested note() calls */

   static const char *label[] = { "error", "warning", "note" };
   static const char color[][8] = { "%g1;31.", "%g1;35.", "%g1;36." };
   int line, col;
   MemFile *f;
   const Span0 *loc;

   bool stub = kind == DGSTUB;
   if (stub) kind = DGERROR;

   ++depth;
   if (span) {
      /* print "file:line:col " */
      loc = span->ex.len ? &span->ex : &span->sl;
      f = getfile(loc->file);
      const char *file = getfilepos(&line, &col, loc->file, loc->off);
      if (file[0] == '@' && file[1] == ':')
         bfmt(&out, "<%s>", file+2);
      else
         bfmt(&out, "%s", file);
      bfmt(&out, ":%d:%d: ", line, col);
   }
   bfmt(&out, color[kind]);
   bfmt(&out, "%s: %g.", label[kind]);
   if (stub) bfmt(&out, "STUB: ");
   vbfmt(&out, fmt, ap);
   bfmt(&out, "\n");
   if (span) {
      uint i;
      int nmark;
      char mark = '^';

      /* find start of line */
      for (i = loc->off - 1; i + 1 > 0 && !isnewline(f->p[i]); --i) ;
      if (i || isnewline(f->p[i])) ++i;

      nmark = loc->len;
      uint nsafechar = ~0u;
      while (i < loc->off + loc->len) {
         static const char spaces[8] = "        ";
         enum { NTABWIDTH = 8 };
         int j, end;
         int curoff = bfmt(&out, "%5d | ", line);
         const uchar *linep = &f->p[i];
         bool begintabs = 1;
         int ntabs = 0;
         for (end = 0; !isnewline(f->p[i]) && i < f->n; ++i, ++end) {
            uchar c = f->p[i];
            if (c == '\t') {
               if (begintabs) {
                  ++ntabs;
                  iowrite(&out, spaces, NTABWIDTH);
                  continue;
               }
               c = ' ';
            } else {
               begintabs = 0;
               if (!aisprint(c)) {
                  /* can't assume 1 cell per byte (cell-width of arbitrary unicode is non trivial) */
                  nsafechar = i;
                  /* also might be control characters needing spacing */
                  i += safeputnonascii(&out, &f->p[i]) - 1;
                  continue;
               }
            }
            ioputc(&out, c);
         }
         ioputc(&out, '\n');
         ++i;

         if (col >= nsafechar) continue;
         for (j = -curoff; j < 0; ++j)
            ioputc(&out, j == -2 ? '|' : ' ');
         for (begintabs = 1; j < col-1; ++j) {
            uchar c = *linep++;
            if (c == '\t') {
               if (begintabs) {
                  --ntabs;
                  iowrite(&out, spaces, NTABWIDTH);
                  continue;
               }
               c = ' ';
            } else {
               c = ' ';
               begintabs = 0;
            }
            ioputc(&out, c);
         }
         bfmt(&out, color[kind]);
         do {
            if (ntabs > 0) {
               iowrite(&out, spaces, NTABWIDTH);
               --ntabs;
               continue;
            }
            ioputc(&out, mark);
            mark = '~';
         } while (--nmark > 0 && ++j < end);
         col = 1;
         ++line;
         bfmt(&out, "%g.\n");
         --nmark;
      }
      ioputc(&out, '\n');
   }

   if (span && loc == &span->ex && span->sl.len)
      if (span->ex.file != span->sl.file || !((uint) span->sl.off - span->ex.off < span->ex.len))
         note(&(Span){ span->sl }, "expanded from here");

   if (--depth == 0) ioflush(&out);
}

void _Noreturn
fatal(const Span *span, const char *fmt, ...) {
   if (fmt) {
      va_list ap;
      va_start(ap, fmt);
      vdiag(span, DGERROR, fmt, ap);
      va_end(ap);
   }
   if (!fmt || span) efmt("Aborting due to previous error.\n");
   exit(1);
}

int nerror, nwarn;
enum { MAXERROR = 20 };

static void
counterror(void) {
   if (++nerror > MAXERROR) {
      efmt("Too many errors emitted, stopping now.\n");
      exit(1);
   }
}

bool
error(const Span *span, const char *fmt, ...) {
   va_list ap;

   va_start(ap, fmt);
   vdiag(span, DGERROR, fmt, ap);
   va_end(ap);
   counterror();
   return 1;
}

bool
warn(const Span *span, const char *fmt, ...) {
   va_list ap;

   if (ccopt.wnone) return 0;
   va_start(ap, fmt);
   vdiag(span, ccopt.werror ? DGERROR : DGWARN, fmt, ap);
   va_end(ap);
   if (ccopt.werror) counterror();
   else ++nwarn;
   return 1;
}

void
note(const Span *span, const char *fmt, ...) {
   va_list ap;

   va_start(ap, fmt);
   vdiag(span, DGNOTE, fmt, ap);
   va_end(ap);
}

void
stub(const Span *span, const char *fmt, ...) {
   va_list ap;

   va_start(ap, fmt);
   vdiag(span, DGSTUB, fmt, ap);
   va_end(ap);
   counterror();
}

/*** UTF util ***/

ushort *
utf8to16(uint *ulen, Arena **arena, const uchar *s, size_t len) {
   assert(0 && "nyi");
}

uint *
utf8to32(uint *ulen, Arena **arena, const uchar *s, size_t len) {
   uint *ret, *w;
   const uchar *p, *end;
   size_t n = 0;
   bool istrunc;

   assert(ulen && s);
   if (!len) {
      *ulen = 0;
      return NULL;
   }

   for (p = end = s; p < s + len; ++n) {
      end = p;
      if ((*p & 0xF8) == 0xF0) /* 11110xxx */
         p += 4;
      else if ((*p & 0xF0) == 0xE0) /* 1110xxxx */
         p += 3;
      else if ((*p & 0xE0) == 0xC0) /* 110xxxxx */
         p += 2;
      else p += 1;
   }
   istrunc = p > s+len;
   if (!istrunc) end += 1;

   ret = allocz(arena, n * sizeof *ret, sizeof *ret);
   for (w = ret, p = s; p < end; ++w) {
      if ((*p & 0xF8) == 0xF0) { /* 11110xxx */
         *w = (uint)(p[0] & 0x07) << 18
            | (uint)(p[1] & 0x3F) << 12
            | (uint)(p[2] & 0x3F) << 6
            | (uint)(p[3] & 0x3F);
         p += 4;
      } else if ((*p & 0xF0) == 0xE0) { /* 1110xxxx */
         *w = (uint)(p[0] & 0x07) << 12
            | (uint)(p[1] & 0x3F) << 6
            | (uint)(p[2] & 0x3F);
         p += 3;
      } else if ((*p & 0xE0) == 0xC0) { /* 110xxxxx */
         *w = (uint)(p[0] & 0x07) << 6
            | (uint)(p[1] & 0x3F);
         p += 2;
      } else {
         *w = *p;
         p += 1;
      }
   }
   if (istrunc) *w++ = 0xFFFD;
   *ulen = n;

   return ret;
}

int
utf8enc(char p[4], uint cp) {
   if ((cp & 0xffffff80) == 0) {
      p[0] = cp;
      return 1;
   } else if ((cp & 0xfffff800) == 0) {
      p[0] = 0xC0 | (cp >> 6 & 0x1F);
      p[1] = 0x80 | (cp & 0x3F);
      return 2;
   } else if ((cp & 0xffff0000) == 0) {
      p[0] = 0xE0 | (cp >> 12 & 0x0F);
      p[1] = 0x80 | (cp >>  6 & 0x3F);
      p[2] = 0x80 | (cp & 0x3F);
      return 3;
   } else {
      p[0] = 0xF0 | (cp >> 18 & 0x07);
      p[1] = 0x80 | (cp >> 12 & 0x3F);
      p[2] = 0x80 | (cp >>  6 & 0x3F);
      p[3] = 0x80 | (cp & 0x3F);
      return 4;
   }
}

/* vim:set ts=3 sw=3 expandtab: */
