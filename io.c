#include "lex.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static char stdoutbuf[1024], stderrbuf[1024];
struct wbuf bstdout = FDBUF(stdoutbuf, sizeof stdoutbuf, 1),
            bstderr = FDBUF(stderrbuf, sizeof stderrbuf, 2);

void
iowrite(struct wbuf *buf, const void *Src, int n)
{
   const uchar *src = Src;

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
ioflush(struct wbuf *buf)
{
   int i, ret;

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
ioputc(struct wbuf *buf, uchar c)
{
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
putquoted(struct wbuf *buf, uchar c, uchar qchar, int next)
{
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
          if (in_range(next, '0', '7'))
              n += bfmt(buf, "%.3o", c);
          else
              n += bfmt(buf, "%o", c);
      }
      return n;
   }
   if (c == '?' && next == '?') {
      return ioputc(buf, c), ioputc(buf, '\\'), 2;
   }
   return ioputc(buf, c), 1;
}

static int
putuint(struct wbuf *buf, uvlong x, int base, bool lower)
{
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
fmterr(const char *fmt, ...)
{
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
priquals(struct wbuf *buf, int q)
{
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
pritypebefore(struct wbuf *buf, union type ty, int qual)
{
   const char *s, *s2;
   union type chld;
   int n;
   switch (ty.t) {
   case TYVOID:   s = "void"; Prim: n = bfmt(buf, "%s", s); return n + priquals(buf, qual);
   case TYBOOL:   s = "bool"; goto Prim;
   case TYCHAR:   s = "char"; goto Prim;
   case TYSCHAR:  s = "signed char"; goto Prim;
   case TYUCHAR:  s = "unsigned char"; goto Prim;
   case TYSHORT:  s = "short"; goto Prim;
   case TYUSHORT: s = "unsigned short"; goto Prim;
   case TYINT:    s = "int"; goto Prim;
   case TYUINT:   s = "unsigned int"; goto Prim;
   case TYLONG:   s = "long"; goto Prim;
   case TYULONG:  s = "unsigned long"; goto Prim;
   case TYVLONG:  s = "long long"; goto Prim;
   case TYUVLONG: s = "unsigned long long"; goto Prim;
   case TYFLOAT:  s = "float"; goto Prim;
   case TYDOUBLE: s = "double"; goto Prim;
   case TYLDOUBLE:s = "long double"; goto Prim;
   case TYVALIST: s = "va_list"; goto Prim;
   case TYPTR:
      chld = typechild(ty);
      n = pritypebefore(buf, chld, ty.flag & TFCHLDQUAL);
      //if (chld.t != TYPTR) n += ioputc(buf, ' ');
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
      n = bfmt(buf, "%s %s", s, (s2 = ttypenames[typedata[ty.dat].id]) ? s2 : "(anonymous)");
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
pritypeafter(struct wbuf *buf, union type ty, int qual)
{
   const struct typedata *td;
   int n = 0;
   switch (ty.t) {
   case TYPTR:
      if (typechild(ty).t == TYARRAY || typechild(ty).t == TYFUNC)
         n += bputc(buf, ')');
      n += pritypeafter(buf, typechild(ty), ty.flag & TFCHLDQUAL);
      break;
   case TYARRAY:
      n += bputc(buf, '[');
      if (typearrlen(ty))
         n += bfmt(buf, "%u", typearrlen(ty));
      n += bputc(buf, ']');
      n += pritypeafter(buf, typechild(ty), ty.flag & TFCHLDQUAL);
      break;
   case TYFUNC:
      td = &typedata[ty.dat];
      n += bputc(buf, '(');
      for (int i = 0; i < td->nmemb; ++i) {
         n += bfmt(buf, "%tq", td->param[i], tdgetqual(td->quals, i));
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
fmttype(struct wbuf *buf, union type ty, int qual)
{
   int n = pritypebefore(buf, ty, qual);
   n += pritypeafter(buf, ty, qual);
   return n;
}

static int
putdouble(struct wbuf *buf, double x, vlong prec)
{
   int n = 0;
   if (x < 0) {
      n += bputc(buf, '-');
      x = -x;
   }

   x += 0.5 / prec;  // round last decimal
   if (x >= (double)(-1ULL>>1)) { // out of range?
      n += bwriteS(buf, "inf");
   } else {
      vlong integral = x;
      vlong fractional = (x - integral)*prec;
      n += putuint(buf, integral, 10, 0);
      n += bputc(buf, '.');
      for (vlong i = prec/10; i > 1; i /= 10) {
         if (i > fractional) {
            n += bputc(buf, '0');
         }
      }
      n += putuint(buf, fractional, 10, 0);
   }
   return n;
}

int
vbfmt(struct wbuf *out, const char *fmt, va_list ap)
{
   bool quote, umod, lmod, lower;
   int base;
   vlong i;
   int pad, prec, q;
   const char *s;
   void *p;
   struct token *tok;
   union type ty;
   double f;
   char tmpbuf1[70], tmpbuf2[70];
   struct wbuf tmp1 = MEMBUF(tmpbuf1, sizeof tmpbuf1);
   struct wbuf tmp2 = MEMBUF(tmpbuf2, sizeof tmpbuf2);
   struct wbuf *buf = out;
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
         if (quote) {
            if (!s) {
               n += bwriteS(buf, "(null)");
               break;
            }
         QuotedStr:
            n += bputc(buf, '"');
            if (lmod) /* lower */
               for (; *s; ++s) n += putquoted(buf, aisalpha(*s) ? *s|32 : *s, '"', s[1]);
            else
               for (; *s; ++s) n += putquoted(buf, *s, '"', s[1]);
            n += bputc(buf, '"');
         } else {
            assert(s && "%s null!");
            if (lmod) /* lower */
               for (; *s; ++s) n += bputc(buf, aisalpha(*s) ? *s|32 : *s);
            else
               while (*s) n += bputc(buf, *s++);
         }
         break;
      case 'S':  /* string ptr + len */
         s = va_arg(ap, const char *);
         i = va_arg(ap, uint);
         assert(s && "%S null");
         if (quote) {
            n += bputc(buf, '"');
            for (; i--; ++s) n += putquoted(buf, *s, '"', i ? s[1] : -1);
            n += bputc(buf, '"');
         } else {
            iowrite(buf, s, i);
            n += i;
         }
         break;
      case 'y': /* symbol: print string literally if valid identifier, or quote it */
         s = va_arg(ap, const char *);
         assert(s && "%y null");
         for (i = 0; s[i]; ++i) {
            if (aisalpha(s[i]) || s[i] == '_' || (i > 0 && aisdigit(s[i])))
               continue;
            goto QuotedStr;
         }
         /* valid identifier */
         while (*s) n += bputc(buf, *s++);
         break;
      case 'd': /* decimal */
         base = 10;
      Int:
         if (base != 10) umod = 1;
         i = lmod ? va_arg(ap, vlong) : (umod ? va_arg(ap, uint) : (vlong)va_arg(ap, int));
         tmp2.len = 0;
         if (quote) {
            switch (base) {
            case  2: n += bwriteS(buf, "0b"); break;
            case  8: n += bwriteS(buf, "0"); break;
            case 16: n += bwriteS(buf, "0x"); break;
            }
         }
         if (!umod && i < 0) {
            n += bputc(buf, '-');
            i = -i;
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
            n += putuint(prec > 0 ? &tmp2 : buf, (uvlong)p, 16, 1);
            if (prec > 0) {
               int fil = prec - tmp2.len;
               while (fil-- > 0) n += bputc(buf, '0');
               iowrite(buf, tmp2.buf, tmp2.len);
            }
         }
         break;
      case 'f': /* float */
         f = va_arg(ap, double);
         n += putdouble(buf, f, prec <= 0 ? 10e6 : prec);
         break;
      case 't': /* token/tokentag/type */
         switch (*fmt++) {
         case 'k': /* tk token */
            tok = va_arg(ap, struct token *);
         Tok:
            switch (tok->t) {
            case TKXXX:
               n += bwriteS(buf, "\?\?\?");
               break;
            case TKNUMLIT:
               if (quote) n += bputc(buf, '`');
               n += bfmt(buf, "%S", tok->s, tok->len);
               if (quote) n += bputc(buf, '\'');
               break;
            case TKCHRLIT:
               n += bputc(buf, '\'');
               for (int i = 0; i < tok->len; ++i)
                  n += putquoted(buf, tok->s[i], '\'', i < tok->len - 1 ? tok->s[i+1] : -1);
               n += bputc(buf, '\'');
               break;
            case TKSTRLIT:
               n += bfmt(buf, "%'S", tok->s, tok->len);
               break;
            case TKIDENT:
               n += bfmt(buf, "`%s'", tok->s);
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
                  n += bfmt(buf, "%s", tok->s);
               } else if (aisprint(tok->t)) {
                  n += bputc(buf, tok->t);
               } else {
                  n += bwriteS(buf, "??");
               }
               if (quote) n += bputc(buf, '\'');
               break;
            }
            break;
         case 't': /* tt token tag */
            tok = &(struct token) { va_arg(ap, int) };
            switch (tok->t) {
            case TKNUMLIT:
               n += bfmt(buf, "numeric literal");
               break;
            case TKSTRLIT:
               n += bfmt(buf, "string literal");
               break;
            case TKIDENT:
               n += bfmt(buf, "identifier");
               break;
            case TKEOF:
               n += bfmt(buf, "<end-of-file>");
               break;
            default:
               if (tok->t >= TKWBEGIN_ && tok->t <= TKWEND_) {
                  static const char *tab[] = {
                  #define _(kw, c) #kw,
                  #include "keywords.def"
                  #undef _
                  };
                  n += bfmt(buf, "%s", tab[tok->t - TKWBEGIN_]);
                  break;
               }
               goto Tok;
            }
            break;
         case 'y': /* ty type */
            ty = va_arg(ap, union type);
            n += fmttype(buf, ty, 0);
            break;
         case 'q': /* tq qualified type */
            ty = va_arg(ap, union type);
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
         iowrite(out, buf->buf, buf->len);
         out->err |= buf->err;
      } else if (pad < 0) { /* right pad */
         int len = n - prevn;
         while (pad++ < -len)
            n += bputc(out, ' ');
      }
   }
   return buf->err ? -1 : n;
}

int
bfmt(struct wbuf *buf, const char *fmt, ...)
{
   va_list ap;
   int ret;

   va_start(ap, fmt);
   ret = vbfmt(buf, fmt, ap);
   va_end(ap);
   return ret;
}

static uint pagesiz;

struct memfile
mapopen(const char **err, const char *path)
{
   struct stat stat;
   int fd = -1;
   void *p = NULL;
   struct memfile f = {0};
   uint mapsiz;

   assert("nullp" && err && path);
 
   if (!pagesiz) pagesiz = sysconf(_SC_PAGESIZE);
   *err = NULL;

   if ((fd = open(path, O_RDONLY)) < 0)
      goto Err;
   if (fstat(fd, &stat) != 0)
      goto Err;

   if (!S_ISREG(stat.st_mode)) {
      uint cap = 0;
      int ret;

      do {
         enum { CHUNKSIZ = 1<<10 };
         if (f.n + CHUNKSIZ >= cap && (cap += CHUNKSIZ) < CHUNKSIZ) {
            /* overflow */
            free(p);
            goto Big;
         }
         if (!(f.p = p ? realloc(p, cap) : malloc(cap))) {
            free(p);
            goto Err;
         }
         p = (void *)f.p;
         ret = read(fd, p, CHUNKSIZ);
         if (ret >= 0)
            f.n += ret;
         else if (errno != EAGAIN && errno != EWOULDBLOCK)
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
   }

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
Err:
   if (fd >= 0) close(fd);
   if (!*err) *err = strerror(errno);
   return f;
}

void
mapclose(struct memfile *f)
{
   assert(f->p);
   munmap((void *)f->p, alignup(f->n, pagesiz) + pagesiz);
   memset(f, 0, sizeof *f);
}

void *
mapzeros(uint N)
{
   void *p = mmap(NULL, N, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
   return p == MAP_FAILED ? NULL : p;
}

void
_assertfmt(const char *file, int line, const char *func, const char *expr)
{
   ioflush(&bstdout);
   efmt("%s:%d: %s: Assertion `%s' failed.\n", file, line, func, expr);
   ioflush(&bstderr);
}

static struct file {
   const char *path;
   struct memfile f;
   vec_of(uint) lineoffs;
} files[256];
static int nfiles;

int
openfile(const char **err, struct memfile **pf, const char *path)
{
   struct file *f;

   assert(nfiles + 1 < arraylength(files) && "too many files");
   f = &files[nfiles];
   f->path = path;
   f->f = mapopen(err, path);
   if (*err) return -1;
   vinit(&f->lineoffs, NULL, 50);
   vpush(&f->lineoffs, 0);
   *pf = &f->f;
   return nfiles++;
}

const char *
getfilename(int id)
{
   assert(id < nfiles);
   return files[id].path;
}

struct memfile *
getfile(int id)
{
   assert(id < nfiles);
   return &files[id].f;
}

void
addfileline(int id, uint off)
{
   vec_of(uint) *lineoffs;

   assert(id < nfiles);
   lineoffs = (void *)&files[id].lineoffs;
   if (lineoffs->n) assert(off > lineoffs->p[lineoffs->n-1]);
   vpush(lineoffs, off);
}

void
getfilepos(int *line, int *col, int id, uint off)
{
   uint *offs, n;
   int l = 0, h, i = 0;

   assert(id < nfiles);
   offs = files[id].lineoffs.p;
   n = files[id].lineoffs.n;
   h = n - 1;
   
   /* binary search over offsets array */
   while (l <= h) {
      i = (l + h) / 2;
      if (offs[i] < off) l = i + 1;
      else if (offs[i] > off) h = i - 1;
      else break;
   }
   i -= offs[i] > off;
   *line = i + 1;
   *col = off - offs[i] + 1;
}

void
closefile(int id)
{
   assert(id >= 0 && id < nfiles);
   mapclose(&files[id].f);
}

enum diagkind { DGERROR, DGWARN, DGNOTE, };

void
vdiag(const struct span *span, enum diagkind kind, const char *fmt, va_list ap)
{
   static const char *label[] = { "error", "warning", "note" };
   static const char *color[] = { "%g1;31.", "%g1;35.", "%g1;36." };
   int line, col;
   struct memfile *f;
   const struct span0 *loc;

   if (span) {
      loc = span->ex.len ? &span->ex : &span->sl;
      f = getfile(loc->file);
      getfilepos(&line, &col, loc->file, loc->off);
      efmt("%s:%d:%d: ", getfilename(loc->file), line, col);
   }
   efmt(color[kind]);
   efmt("%s: %g.", label[kind]);
   vbfmt(&bstderr, fmt, ap);
   efmt("\n");
   if (span) {
      uint i;
      int j, nmark;
      char mark = '^';

      /* find start of line */
      for (i = loc->off - 1; i + 1 > 0 && f->p[i] != '\n'; --i) ;
      if (i || f->p[i] == '\n') ++i;

      nmark = loc->len;
      while (i < loc->off + loc->len) {
         int end;
         int curoff = efmt("%5d | ", line);
         for (end = 0; f->p[i] != '\n' && i < f->n; ++i, ++end)
            ioputc(&bstderr,  f->p[i]);
         ++i;
         ioputc(&bstderr,  '\n');

         for (j = 0; j < curoff + col - 1; ++j)
            ioputc(&bstderr, j == curoff-2 ? '|' : ' ');
         efmt(color[kind]);
         j -= curoff;
         do {
            ioputc(&bstderr, mark);
            mark = '~';
         } while (--nmark && ++j < end);
         col = 1;
         ++line;
         efmt("%g.\n");
      }
      ioputc(&bstderr,  '\n');
   }

   if (span && loc == &span->ex && span->sl.len)
      if (span->ex.file != span->sl.file || !((uint) span->sl.off - span->ex.off < span->ex.len))
         note(&(struct span){ span->sl }, "expanded from here");
}

void
fatal(const struct span *span, const char *fmt, ...)
{
   va_list ap;

   va_start(ap, fmt);
   vdiag(span, DGERROR, fmt, ap);
   va_end(ap);
   efmt("Aborting due to previous error.\n");
   exit(1);
}

int nerror;

void
error(const struct span *span, const char *fmt, ...)
{
   va_list ap;

   ++nerror;
   va_start(ap, fmt);
   vdiag(span, DGERROR, fmt, ap);
   va_end(ap);
}

void
warn(const struct span *span, const char *fmt, ...)
{
   va_list ap;

   va_start(ap, fmt);
   vdiag(span, DGWARN, fmt, ap);
   va_end(ap);
}

void
note(const struct span *span, const char *fmt, ...)
{
   va_list ap;

   va_start(ap, fmt);
   vdiag(span, DGNOTE, fmt, ap);
   va_end(ap);
}

/* vim:set ts=3 sw=3 expandtab: */
