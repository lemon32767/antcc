#define _POSIX_C_SOURCE 200112L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "z_miniz.h"
#undef assert // miniz includes <assert.h>
#undef static_assert
#include "antcc.h"
#include "version.h"
#include "obj.h"
#include "a_sys.h"

void ccompbuf(const char *name, const char *data, uint size);

static Arena *arena;

static const char *
cacheroot(void) {
   const char *d = getenv("ANTCC_CACHE_DIR");
   if (d && *d) return d;
   const char *home = getenv("HOME");
   if (!home || !*home) return NULL;
   return afmt(&arena, "%s/.cache/antcc", home);
}

static const char *
packdirpath(const char *root, const char *targ) {
   if (!root) return NULL;
   return afmt(&arena, "%s/%s", root, targ);
}

static const char RTPACK_META_MARKER[] = ">>>ANTCC-META>>>";

/* binary-safe strstr: archives contain NULs */
static const char *
memfind(const char *p, size_t n, const char *needle) {
   size_t m = strlen(needle);
   if (m > n) return NULL;
   for (size_t i = 0; i + m <= n; ++i)
      if (p[i] == needle[0] && !memcmp(p + i, needle, m))
         return p + i;
   return NULL;
}

static bool
metamatch(const char *path, const char *ver, const char *hash, bool log) {
   int fd = open(path, O_RDONLY);
   if (fd < 0) return 0;
   char buf[16<<10];
   size_t n = 0;
   while (n < sizeof buf) {
      ssize_t r = read(fd, buf + n, sizeof buf - n);
      if (r < 0 && errno == EINTR) continue;
      if (r <= 0) break;
      n += r;
   }
   close(fd);
   bool ok = 0;
   const char *p = memfind(buf, n, RTPACK_META_MARKER);
   char v[256], h[256];
   if (!p) goto End;
   p += sizeof RTPACK_META_MARKER - 1;
   const char *sep = memchr(p, ':', n - (p - buf));
   if (!sep || sep-p >= sizeof v) goto End;
   memcpy(v, p, sep-p), v[sep-p] = 0;
   p = sep+1;
   sep = memchr(p, '\n', n - (p - buf));
   if (!sep || sep-p >= sizeof h) goto End;
   memcpy(h, p, sep-p), h[sep-p] = 0;
   if (!strcmp(ver, v) && !strcmp(hash, h)) ok = 1;
End:
   if (log && !ok) vrbfmt(" # existing library at %'s is stale, rebuilding\n", path);
   return ok;
}

static bool
mkmetaobj(const char *meta, const char *objpath) {
   char objbuf[16<<10];
   WriteBuf wbuf = MEMBUF(objbuf, sizeof objbuf);
   objmkmeta(&wbuf, meta, strlen(meta)+1);
   assert(!wbuf.err);
   FILE *f = fopen(objpath, "wb");
   if (!f) return warn(NULL, "fopen(%s): %s", objpath, strerror(errno)), 0;
   bool ok = fwrite(objbuf, 1, wbuf.len, f) == wbuf.len;
   fclose(f);
   return ok;
}

static int
rtcc1(const char *name, const char *out, const char *data, uint size) {
   extern int nerror;

   vrbfmt("rt cc1(in: %'s, out: %'s)\n", name, out);
   /* compilation must be deterministic here and not depend on invoked compiler flags */
   memset(&ccopt, 0, sizeof ccopt);
   ccopt.cstd = STDC11;
   ccopt.pic = 1;
   ccopt.o = OPT2;

   objini(name, out);
   ccompbuf(name, data, size);
   if (nerror) return 1;
   objfini(1);
   return 0;
}

static bool
ar(const char *out, char **objs, int nobj) {
   int ncmd = 3 + nobj + 1;
   const char **cmd = alloc(&arena, ncmd * sizeof *cmd, 0);

   cmd[0] = "ar", cmd[1] = "-rcs", cmd[2] = out;
   for (int i = 0; i < nobj; ++i)
      cmd[3 + i] = objs[i];
   cmd[ncmd-1] = NULL;
   return runproc(cmd, &(ProcSpawn){.log = ccopt.verbose}) == 0;
}

static bool
compileaziplib(const char *dir, const char *ver, const char *hash,
               const char *path, const char *meta, mz_zip_archive *zip) {
   int lockfd = -1;
   char *tmp = NULL, *databuf = NULL;
   struct hdrbuf { char *p; size_t n; };
   DEF_SVEC(EmbedFile, 32, headers);
   DEF_SVEC(char *, 32, objs);
   bool ok = 0;

   /* acquire lock */
   char *lockpath = afmt(&arena, "%s/.lock", dir);
   if ((lockfd = open(lockpath, O_CREAT|O_RDWR, 0600)) < 0)
      return 0;
   fcntl(lockfd, F_SETFD, FD_CLOEXEC);
   if (fcntl(lockfd, F_SETLKW, &(struct flock){.l_type = F_WRLCK, .l_whence = SEEK_SET}) != 0)
      goto End;
   if (isfile(path) && metamatch(path, ver, hash, 0)) {
      /* another process may have built it while we waited */
      vrbfmt(" # found @ %s\n", path);
      ok = 1;
      goto End;
   }
   vrbfmt(" # begin lock %s\n", lockpath);

   /* use a temp so other processes don't take the .a mid write */
   tmp = afmt(&arena, "%s/.libantrt1.a.tmp", dir);
   unlink(tmp);

   mz_zip_archive_file_stat st;
   uint nfiles = mz_zip_reader_get_num_files(zip);

   const uint pad = 8;

   /* extract headers */
   for (uint i = 0; i < nfiles; ++i) {
      if (!mz_zip_reader_file_stat(zip, i, &st) || mz_zip_reader_is_file_a_directory(zip, i))
         continue;
      const char *ext = fileext(st.m_filename);
      if (strcmp(ext, "h") != 0 && strcmp(ext, "inc") != 0) continue;
      EmbedFile file = {
         alloccopy(&arena, st.m_filename, strlen(st.m_filename)+1, 0),
         alloc(&arena, st.m_uncomp_size + pad, 0),
         st.m_uncomp_size
      };
      memset((char *)file.s + st.m_uncomp_size, 0, pad);
      if (!mz_zip_reader_extract_to_mem(zip, i, (char *)file.s, st.m_uncomp_size, 0)) goto UnzipErr;
      vpush(&headers, file);
   }
   vpush(&headers, (EmbedFile){0});

   /* the metadata entry has to be first for metamatch() to find it (quickly) */
   char *metaobj = afmt(&arena, "%s/__.ANTCCMETA", dir);
   vpush(&objs, metaobj);
   /* compile each obj in zip */
   for (uint i = 0; i < nfiles; ++i) {
      if (!mz_zip_reader_file_stat(zip, i, &st) || mz_zip_reader_is_file_a_directory(zip, i))
         continue;

      const char *name = strrchr(st.m_filename, '/');
      if (strcmp(fileext(st.m_filename), "c") != 0) continue;
      name = name ? name + 1 : st.m_filename;
      const char *dot = strrchr(name, '.');
      int namelen = dot ? dot - name : strlen(name);
      char *obj = afmt(&arena, "%s/%S.o", dir, name, namelen);

      assert(st.m_uncomp_size < 1<<20 && "?");
      uint size = st.m_uncomp_size;
      databuf = databuf ? xrealloc(databuf, size + pad) : xmalloc(size + pad);
      if (!mz_zip_reader_extract_to_mem(zip, i, databuf, size, 0)) UnzipErr: {
         warn(NULL, "unzip: %s", mz_zip_get_error_string(mz_zip_get_last_error(zip)));
         goto End;
      }
      memset(databuf + size, 0, pad);

      ioflush(&bstdout), ioflush(&bstderr);
      pid_t p = fork();
      if (p < 0) {
         warn(NULL, "fork: %s", strerror(errno));
         goto End;
      } else if (p == 0) {
         /* remove all include dirs except the embedded headers + these runtime headers */
         memset(cinclpaths, 0, sizeof cinclpaths);
         extern EmbedFile *dynamic_cincludes;
         dynamic_cincludes = headers.p;

         _exit(rtcc1(st.m_filename, obj, databuf, size));
      }
      int wst;
      if (waitpid(p, &wst, 0) < 0 || !WIFEXITED(wst) || WEXITSTATUS(wst) != 0) {
         warn(NULL, "rtpack: failed to compile %'s", st.m_filename);
         goto End;
      }

      vpush(&objs, obj);
   }

   char *metameta = afmt(&arena, "%s%s:%s\n%s", RTPACK_META_MARKER, ver, hash, meta);
   if (!mkmetaobj(metameta, metaobj)) {
      unlink(metaobj);
      goto End;
   }
   assert(objs.n > 0);

   /* make static lib */
   if (!ar(tmp, objs.p, objs.n)) {
      warn(NULL, "rtpack: ar failed");
      goto End;
   }

   /* atomic move tmp .a to dest path */
   if (rename(tmp, path) != 0) {
      warn(NULL, "rtpack: rename: %s", strerror(errno));
      goto End;
   }
   vrbfmt(" # %s OK\n", path);
   ok = 1;
End:
   free(databuf);
   if (!ok && tmp)
      unlink(tmp);
   for (int i = 0; i < objs.n; ++i)
      unlink(objs.p[i]);
   vfree(&objs);
   vfree(&headers);
   if (lockfd >= 0) {
      fcntl(lockfd, F_SETLKW, &(struct flock){.l_type = F_UNLCK, .l_whence = SEEK_SET});
      close(lockfd);
   }
   return ok;
}

const char *
rtpack_getlibantrt1(void) {
   extern unsigned pack_libantrt1_size;
   extern const char pack_libantrt1_data[],
                     pack_libantrt1_meta[],
                     pack_libantrt1_hash[];

   enum { N = 64<<10 };
   union {Arena a; char mem[sizeof(Arena)+N];} amem;
   *(arena = &amem.a) = (Arena){.cap = N};

   const char *root = cacheroot(), *dir = NULL, *path = NULL, *targ;
   if (!root)
      goto End;
   targ = targ_canon_triple();
   dir = packdirpath(root, targ);
   path = afmt(&globarena, "%s/libantrt1.a", dir);
   vrbfmt("rt cache dir: %s\n", dir);
   if (isfile(path) && metamatch(path, ANTCC_VERSION_STR, pack_libantrt1_hash, 1)) {
      vrbfmt(" # found @ %s\n", path);
      goto End;
   }

   if (!mkdir_p(dir)) {
      warn(NULL, "runtime pack: cannot write to %'s, set ANTCC_CACHE_DIR", dir);
      path = NULL;
      goto End;
   }

   mz_zip_archive zip = {0};
   if (!mz_zip_reader_init_mem(&zip, pack_libantrt1_data, pack_libantrt1_size, 0)) {
      warn(NULL, "runtime pack: %s", mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
      path = NULL;
      goto End;
   }
   if (!compileaziplib(dir, ANTCC_VERSION_STR, pack_libantrt1_hash, path, pack_libantrt1_meta, &zip))
      path = NULL;
   mz_zip_reader_end(&zip);
End:
   freearena(&arena);
   return path;
}

/* vim:set ts=3 sw=3 expandtab: */
