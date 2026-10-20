/* mkpack.c - creates a zip archive of the regular files in a directory, non recursively,
 * and outputs C definitions for the zip data, and a reproducible hash.
 *
 * hash = SHA1(
 *    filename-1: nul-terminated char[]
 *    SHA1(file-1-data): byte[20]
 *    ...
 *    filename-n
 *    SHA1(file-n-data)
 * ) where files 1..n are sorted by their filenames, bytewise.
 *
 * Also outputs a string containing each file, with size and individual hash
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#define ANTCC_TOOL
#include "../src/z_miniz.h"

#define PATHMAX 4096

static const char * 
pslug(const char *path) {
   static char out[64];
   const char *base = path, *s = strrchr(base, '/');
   s = s ? s + 1 : base;
   size_t n;
   for (n = 0; n+1 < sizeof out && *s; ++s) {
      unsigned c = *s&0xFF;
      if (c-'A' <= 'Z'-'A' || c-'a' <= 'z'-'a' || (c-'0'<10 && s > base))
         out[n++] = c;
      else if (!n || out[n-1] != '_')
         out[n++] = '_';
   }
   out[n] = 0;
   return out;
}

struct sha1 {
   uint32_t h[5];
   uint64_t len;
   uint8_t buf[64];
};
static const struct sha1 SHA1INIT;
static void sha1feed(struct sha1 *, const void *, size_t);
static void sha1digest(uint8_t [20], struct sha1 *);
static void
puthex(FILE *out, uint8_t *buf, size_t n) {
   for (int i = 0; i < n; ++i) {
      fputc("0123456789abcdef"[buf[i]>>4], out);
      fputc("0123456789abcdef"[buf[i]&15], out);
   }
}

static int
dfilter(const struct dirent *e) {
    return !!strcmp(e->d_name, ".") && !!strcmp(e->d_name, "..");
}
static int
cmpname(const struct dirent **a, const struct dirent **b) {
   return strcmp((*a)->d_name, (*b)->d_name);
}

#define DIE(...) fprintf(stderr, "error! "__VA_ARGS__), exit(1)
#define ZERRS(z) mz_zip_get_error_string(mz_zip_get_last_error(z))

int
main(int argc, char **argv) {
   if (argc < 2) DIE("usage: %s [DIRECTORY PATH] [OUTPUT]\n", argv[0]);
   const char *dirpath = argv[1], *buf, *end;
   struct dirent **ents;
   int nents = scandir(dirpath, &ents, dfilter, cmpname);
   if (nents < 0) DIE("%s: %s\n", dirpath, strerror(errno));

   mz_zip_archive zip = {0};
   if (!mz_zip_writer_init_heap(&zip, 0, 0))
      DIE("zip init: %s\n", ZERRS(&zip));

   FILE *out = stdout;
   if (argc > 2 && !(out = fopen(argv[2], "w")))
      DIE("%s: %s\n", argv[2], strerror(errno));

   struct sha1 shafinal = SHA1INIT;
   char wd[PATHMAX];
   if (!getcwd(wd, sizeof wd)) DIE("getcwd %s", strerror(errno));
   if (chdir(dirpath) != 0) DIE("chdir %s", strerror(errno));
   const char *slug = pslug(dirpath);
   fprintf(out, "const char pack_%s_meta[] = \n", slug);
   for (int i = 0; i < nents; ++i) {
      const char *name = ents[i]->d_name;
      struct stat st;
      if (lstat(name, &st) != 0) DIE("stat %s: %s\n", name, strerror(errno));
      if (S_ISREG(st.st_mode)) {
         FILE *file = fopen(name, "r");
         uint8_t filehash[20];
         if (!file) DIE("fopen %s: %s\n", name, strerror(errno));
         else {
            struct sha1 sha = SHA1INIT;
            while (!feof(file)) {
               char buf[4096];
               size_t n = fread(buf, 1, sizeof buf, file);
               sha1feed(&sha, buf, n);
               if (ferror(file)) DIE("read %s: %s\n", name, strerror(errno));
            }
            fclose(file);
            sha1digest(filehash, &sha);
         }
         if (!mz_zip_writer_add_file(&zip, name, name, NULL, 0, MZ_BEST_COMPRESSION))
            DIE("zip add file %s: %s\n", name, ZERRS(&zip));
         fprintf(out, "  \"%s \t%d \t", name, (int)st.st_size), puthex(out, filehash, sizeof filehash), fputs("\\n\"\n", out);
         sha1feed(&shafinal, name, strlen(name)+1);
         sha1feed(&shafinal, filehash, sizeof filehash);
      }
      free(ents[i]);
   }
   fprintf(out, ";\n");
   free(ents);
   chdir(wd);

   void *zipbuf;
   size_t ziplen, n;
   if (!mz_zip_writer_finalize_heap_archive(&zip, &zipbuf, &ziplen))
      DIE("zip finalize: %s\n", ZERRS(&zip));
   if (!mz_zip_writer_end(&zip))
      DIE("zip end: %s\n", ZERRS(&zip));
   if (ziplen != (unsigned)ziplen) DIE("zip too big");

   fprintf(out, "unsigned pack_%s_size = %d;\n", slug, (int)ziplen);
   fprintf(out, "const char pack_%s_data[%d] = {", slug, (int)ziplen);
   for (n=999, buf = zipbuf, end = buf + ziplen; buf != end; ++buf) {
      if (n >= 97) n = fprintf(out, "\n   ")-1;
      n += fprintf(out, "%d,", *buf);
   }
   fprintf(out, "\n};\n");
   mz_free(zipbuf);
   uint8_t hash[20];
   sha1digest(hash, &shafinal);
   fprintf(out, "const char pack_%s_hash[] = \"", slug);
   puthex(out, hash, sizeof hash);
   fprintf(out, "\";\n");

   if (out != stdout) fclose(out);
}

static const struct sha1 SHA1INIT = {{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0}};

static void
sha1block(struct sha1 *s, const uint8_t *p) {
   uint32_t w[80], a, b, c, d, e, f, k, t;
   for (int i = 0; i < 16; ++i)
      w[i] = (uint32_t)p[4*i]<<24 | p[4*i+1]<<16 | p[4*i+2]<<8 | p[4*i+3];
   for (int i = 16; i < 80; ++i) {
      t = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
      w[i] = t<<1 | t>>31;
   }
   a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
   for (int i = 0; i < 80; ++i) {
      if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
      else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
      else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
      else f = b ^ c ^ d, k = 0xCA62C1D6;
      t = (a<<5 | a>>27) + f + e + k + w[i];
      e = d, d = c, c = b<<30 | b>>2, b = a, a = t;
   }
   s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e;
}

static void
sha1feed(struct sha1 *s, const void *p, size_t n) {
   const uint8_t *q = p;
   size_t used = s->len % sizeof s->buf;
   s->len += n;
   if (used) {
      size_t m = used + n < sizeof s->buf ? n : sizeof s->buf - used;
      memcpy(s->buf + used, q, m);
      q += m, n -= m;
      if (used + m < sizeof s->buf) return;
      sha1block(s, s->buf);
   }
   for (; n >= sizeof s->buf; q += sizeof s->buf, n -= sizeof s->buf)
      sha1block(s, q);
   memcpy(s->buf, q, n);
}

static void
sha1digest(uint8_t out[20], struct sha1 *s) {
   struct sha1 t = *s;
   uint64_t bits = t.len * 8;
   static const char bit1 = 0x80, zero[64];
   uint8_t len[8];
   sha1feed(&t, &bit1, 1);
   while (t.len % sizeof t.buf != 56)
      sha1feed(&t, zero, 1);
   for (int i = 0; i < 8; ++i)
      len[i] = bits >> (56 - 8*i);
   sha1feed(&t, len, sizeof len);
   for (int i = 0; i < 5; ++i) {
      out[4*i]   = t.h[i] >> 24;
      out[4*i+1] = t.h[i] >> 16;
      out[4*i+2] = t.h[i] >> 8;
      out[4*i+3] = t.h[i];
   }
}

#include "../src/z_miniz.c"
