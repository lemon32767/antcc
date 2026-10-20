/* This file handles lazy compiler runtime library compilation.
 * At build time, libantrt1/ sources get zipped and embedded in the binary.
 * rtpack_getlibantrt1 compiles and archives or reuses the cached static library,
 * and returns its path for the driver to use for linker.
 * The cache is in '<cache root>/<target name>/rt/libantrt1.a', for example
 * '~/.cache/antcc/x86_64-linux-gnu/rt/libantrt1.a'
 * The archive stores a metadata object that contains a string with the compiler version
 * and runtime library hash as computed in tool/mkpack.c; this is the cache key. If
 * the file doesn't exist/cache key mismatch/can't find the cache key => compile.
 */

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
rtpackdirpath(const char *root, const char *targ) {
   return root ? afmt(&arena, "%s/%s/rt", root, targ) : NULL;
}

/* used to find the metadata in the archive; it is immediately followed
 * by the actual metadata: "version:hash\n..." */
static const char ALIB_META_MARKER[] = "\xf0\x0f<ANTCCMETAv1>:";
/* the metadata should be in the beginning of the file */
enum { ALIB_META_MAX_OFFSET = 8<<10 };

static char *
findmeta(char *p, size_t n) {
   char c0 = *ALIB_META_MARKER;
   const size_t m = sizeof ALIB_META_MARKER - 1;
   for (char *q; (q = memchr(p, c0, n)); p = q + 1, --n) {
      n -= q - p;
      if (m > n) return NULL;
      if (!memcmp(q+1, ALIB_META_MARKER+1, m-1)) return q + m;
   }
   return NULL;
}

/* look for the marker to find the metadata and check if ver:hash match */
static bool
metamatch(const char *path, const char *ver, const char *hash, bool log) {
   FILE *fp = fopen(path, "rb");
   if (!fp) return 0;

   char buf[ALIB_META_MAX_OFFSET + 100];
   size_t n = fread(buf, 1, sizeof buf, fp);
   fclose(fp);

   char *meta = findmeta(buf, n);
   if (!meta) goto No;

   /* parse "{v}:{h}\n" */
   char *v = meta, *sep, *h;
   if (!(sep = memchr(meta, ':', n - (meta - buf)))) goto No;
   *sep = 0;
   h = sep + 1;
   if (!(sep = memchr(h, '\n', n - (h - buf)))) goto No;
   *sep = 0;
   if (!strcmp(ver, v) && !strcmp(hash, h))
      return 1;
No:
   if (log) vrbfmt(" # existing library at %'s is stale, rebuilding\n", path);
   return 0;
}

/* the actual meta file in the archive has to be a valid object (ELF) file, or else
 * ld.lld complains, annoyingly. create an object with the minimal amount
 * of stuff for it to be valid (basically headers, .shstrtab and .data sections) */
static bool
mkmetaobj(const char *meta, const char *objpath) {
   char objbuf[16<<10];
   WriteBuf wbuf = MEMBUF(objbuf, sizeof objbuf);
   objmkmeta(&wbuf, meta, strlen(meta)+1);
   assert(!wbuf.err);
   FILE *fp = fopen(objpath, "wb");
   if (!fp) return warn(NULL, "fopen(%s): %s", objpath, strerror(errno)), 0;
   bool ok = fwrite(objbuf, 1, wbuf.len, fp) == wbuf.len;
   if (ferror(fp)) warn(NULL, "fwrite(%s): %s", objpath, strerror(errno));
   fclose(fp);
   return ok;
}

/* this is invoked in a child process */
static int
rtcc1(const char *name, const char *out, const char *data, uint size) {
   extern int nerror;
   void ccompbuf(const char *name, const char *data, uint size);

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

static const char *arbin;

static bool
ar(const char *out, char **objs, int nobj) {
   int ncmd = 3 + nobj + 1;
   const char **cmd = alloc(&arena, ncmd * sizeof *cmd, 0);

   cmd[0] = arbin, cmd[1] = "-rcs", cmd[2] = out;
   memcpy(cmd+3, objs, nobj * sizeof *cmd);
   cmd[ncmd-1] = NULL;
   return runproc(cmd, &(ProcSpawn){.log = ccopt.verbose}) == 0;
}

static bool
compileaziplib(const char *dir, const char *ver, const char *hash,
               const char *path, const char *meta, mz_zip_archive *zip) {
   int lockfd = -1;
   char *tmp = NULL, *databuf = NULL;
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
   tmp = afmt(&arena, "%s.tmp", path);
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
      if (!mz_zip_reader_extract_to_mem(zip, i, (char *)file.s, st.m_uncomp_size, 0))
         goto UnzipErr;
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
         /* no include dirs except the embedded headers + these runtime headers */
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

   char *metameta = afmt(&arena, "%s%s:%s\n%s", ALIB_META_MARKER, ver, hash, meta);
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
   assert(metamatch(path, ver, hash, 0) && "fresh .a can't match meta?");
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
rtpack_getlibantrt1(const char *ar) {
   extern unsigned pack_libantrt1_size;
   extern const char pack_libantrt1_data[],
                     pack_libantrt1_meta[],
                     pack_libantrt1_hash[];
   enum { N = 64<<10 };
   union {Arena a; char mem[sizeof(Arena)+N];} amem;
   *(arena = &amem.a) = (Arena){.cap = N};

   const char *root = cacheroot(), *dir = NULL, *path = NULL, *targ;
   if (!root) goto End;
   targ = targ_canon_triple();
   dir = rtpackdirpath(root, targ);
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
   arbin = ar;
   if (!compileaziplib(dir, ANTCC_VERSION_STR, pack_libantrt1_hash, path, pack_libantrt1_meta, &zip))
      path = NULL;
   mz_zip_reader_end(&zip);
End:
   freearena(&arena);
   return path;
}

/* vim:set ts=3 sw=3 expandtab: */
