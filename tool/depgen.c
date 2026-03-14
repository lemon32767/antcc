#define USAGE "depgen [-MP | -MT target | -MF output] <file>"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *
alloc1(size_t n)
{
   static char mem[64*1<<10], *pmem = mem;
   assert(mem+sizeof mem - pmem >= n && "oom");
   pmem += n;
   return pmem - n;
}

static char *
sdup(const char *s, size_t n)
{
   return memcpy(alloc1(n+1), s, n);
}

static char *
catpath(const char *s1, const char *s2)
{
   if (!s1) return strcpy(alloc1(strlen(s2)+1), s2);
   size_t n1 = strlen(s1), n2 = strlen(s2), n;
   char *x = alloc1(n = n1+1+n2+1);
   memcpy(x, s1, n1);
   x[n1] = '/';
   memcpy(x+n1+1, s2, n2);
   x[n1+n2+1] = 0;
   for (char *p = x+1; *p; ++p) {
      if (!memcmp(p, "/../", 4)) {
         char *next = p+4, *q;
         for (q = p - 1; q > x && *q != '/'; --q);
         if (q == x) x = p = next;
         else {
            memmove(q+1, next, strlen(next)+1);
            p = q-1;
         }
      }
   }
   return x;
}

static const char *
fileext(const char *path)
{
   assert(path && *path && "empty");
   const char *dot = NULL;
   for (++path; *path; ++path) {
      if (*path == '.') dot = path;
   }
   return dot ? dot+1 : "";
}

static const char *
withext(const char *path, const char *ext)
{
   const char *oext, *file = path;
   assert(*file && "no filename");
   oext = fileext(file);
   size_t len;
   if (!*oext)
      len = strlen(file);
   else
      len = oext - file - 1;
   char *res = memcpy(alloc1(len + 1 + (ext ? strlen(ext) + 1 : 0)), file, len);
   if (ext) {
      res[len] = '.';
      memcpy(res + len + 1, ext, strlen(ext));
   }
   return res;
}

static char *
dirname(const char *path)
{
   const char *end = path + strlen(path);
   while (end > path && *end != '/') --end;
   return end == path ? NULL : sdup(path, end - path);
}

static FILE *out;

enum { MAXFILES = 200, MAXLINE = 2048 };
static const char *files[MAXFILES];
static int nfiles;
static int
dofile(const char *f)
{
   int ret = 0;
   assert(nfiles < MAXFILES);
   files[nfiles++] = f;
   FILE *fp = fopen(f, "r");
   if (!fp) {
      perror(f);
      return 1;
   }
   char line[MAXLINE], hdr[MAXLINE];
   while (fgets(line, sizeof line, fp)) {
      if (sscanf(line, " # include \"%[^\"]\"", hdr) == 1) {
         const char *abspath = catpath(dirname(f), hdr);
         for (int i = 0; files[i] && i < MAXFILES; ++i) {
            if (!strcmp(files[i], abspath)) goto Skip;
         }
         fprintf(out, " %s", abspath);
         ret |= dofile(abspath);
      }
   Skip:;
   }
   if (ferror(fp)) perror(f);
   fclose(fp);
   return ret;
}

int
main(int argc, char **argv)
{
#define DIE(...) return fprintf(stderr, "? "__VA_ARGS__), fputc('\n', stderr), 1
   const char *src = NULL, *targ = NULL, *mf = NULL;
   int mp = 0;
   for (int i = 1; i < argc; ++i) {
      if (*argv[i] != '-') {
         if (src) DIE("too many input files");
         else src = argv[i];
      } else if (!strcmp(argv[i], "-MT")) {
         if (!(targ = argv[++i])) DIE("-MT missing arg");
      } else if (!strcmp(argv[i], "-MF")) {
         if (mf) DIE("too many output files\n");
         if (!(mf = argv[++i])) DIE("-MF missing arg");
      } else if (!strcmp(argv[i], "-MP")) {
         mp = 1;
      } else {
         DIE("invalid arg '%s'", argv[i]);
      }
   }
   if (!src) return fprintf(stderr, "Usage: "USAGE"\n"), 1;
   if (mf && !(out = fopen(mf, "w"))) return perror(mf), 1;
   else if (!out) out = stdout;
   fprintf(out, "%s: %s", targ ? targ : withext(src, "o"), src);
   int ret = dofile(src);
   if (mp) for (int i = 1; files[i]; ++i) {
      fprintf(out, "\n%s:", files[i]);
   }
   fputc('\n', out);
   if (ferror(out)) ret += 4;
   fclose(out);
   return ret;
}

/* vim:set ts=3 sw=3 expandtab: */
